/*
 * LeJIT's replacement for Psyco's merge-point analysis. It has three jobs:
 *  1. It is the opcode whitelist. A code object containing any opcode not
 *     handled in the switch gets Py_None and is never compiled.
 *  2. It marks merge points: bytecode positions where the state is compared
 *     with already-compiled code, and where a new code buffer (and the
 *     snapshot a respawn replays from) starts. As upstream, they go only
 *     where control flow needs them: the start, every position
 *     entered from two or more places, every backward-jump target (loop
 *     heads), every exception or finally handler, and at least every
 *     MAX_MP_DISTANCE instructions so that respawn replays stay short.
 *     Entries are counted per instruction from its successors; an
 *     instruction with EXTENDED_ARG prefixes counts as one, starting at
 *     the first prefix. Placement only affects how much code is compiled
 *     and shared: any instruction boundary may be a merge point.
 *  3. Liveness of locals (as upstream's FULL_CONTROL_FLOW): at each merge
 *     point, the locals never read again are listed first in its fatlist,
 *     and psyco_forget_dead_vars drops what is known about them before the
 *     state is compared, so states differing only in that share code.
 *     Their values stay, as CPython's frame keeps them. Successors are
 *     those of 2, made conservative where 3.7 bytecode doesn't say: an
 *     instruction may reach the handler of every SETUP_EXCEPT/FINALLY/WITH
 *     whose body contains it (the compiler lays the body out between the
 *     SETUP and its handler) or of every exception table entry whose range
 *     contains it, BREAK_LOOP every loop exit, END_FINALLY those
 *     and every CONTINUE_LOOP target.
 */
#include "mergepoints.h"
#include <Python.h>
#include "Python/pycinternal.h"
#include "bytecode.h"
#include "vcompiler.h"
#include "codemanager.h"

typedef struct {
    int flags;
    int fuse_stopped; /* psyco_fuse_stopped */
    Py_ssize_t mergepoint_count;
    Py_ssize_t unit_count;   /* code units in the bytecode */
    global_entries_t *joins; /* per code unit, or NULL: psyco_join_point */
    /* one per code unit: compiled code tests eval_breaker before that
       instruction. Allocated after the merge points. */
    unsigned char *breaker_checks;
    /*
    NULL terminated.
    on allocation we extend this array to count(mergepoints) + 1.
    */
    mergepoint_t mergepoints[1];
} co_report_t;

/* when the code object dies (lejit.c:code_extra_free) */
static void
report_free(co_report_t *report)
{
    Py_ssize_t i;
    for (i = 0; i < report->mergepoint_count; i++) {
        psyco_ge_changed(&report->mergepoints[i].entries);
        Py_XDECREF(report->mergepoints[i].entries.fatlist);
    }
    if (report->joins != NULL) {
        for (i = 0; i < report->unit_count; i++)
            Py_XDECREF(report->joins[i].fatlist);
        PyMem_Free(report->joins);
    }
    PyMem_Free(report);
}

static void
report_destructor(PyObject *capsule)
{
    report_free(PyCapsule_GetPointer(capsule, NULL));
}

#define _ENTER_INC(loc)             \
    do {                            \
        if (entry_count[loc] < 8) { \
            entry_count[loc]++;     \
        }                           \
    } while (0)

#define ENTER_NEXT()           \
    do {                       \
        _ENTER_INC(next_unit); \
    } while (0)

/* a jump target, in code units, checked to be in the code (where it starts
   an instruction is checked once all are known: JUMP_TARGET_CHECK) */
#define JUMP_TARGET(target)                        \
    ({                                             \
        Py_ssize_t _j = (target);                  \
        if (_j < 0 || _j >= entry_count_len - 1) { \
            bad_jump = 1;                          \
            _j = 0;                                \
        }                                          \
        is_target[_j] = 1;                         \
        _j;                                        \
    })

/* target: in code units. A backward jump is to a loop head: always a merge
   point, or compilation would unroll the loop. Where backward jumps have
   opcodes of their own, ceval tests eval_breaker at the jump (but not at
   JUMP_BACKWARD_NO_INTERRUPT), so compiled code tests before it; else at
   the loop head */
#define ENTER_JUMP_TO(target)                              \
    do {                                                   \
        Py_ssize_t _t = JUMP_TARGET(target);               \
        if (_t <= entry_count_index) {                     \
            entry_count[_t] = 8;                           \
            if (!LE_IS_JBACK(in.raw))                      \
                BREAKER_CHECK(_t);                         \
            else if (opcode != JUMP_BACKWARD_NO_INTERRUPT) \
                breaker_check[entry_count_index] = 1;      \
        }                                                  \
        else                                               \
            _ENTER_INC(_t);                                \
    } while (0)

/* the current instruction's jump target, in code units */
#define TARGET_UNIT() (in.target / LE_UNIT)

#define MAX_MP_DISTANCE 170

/* inlining (upstream's rule): one merge point (the start), no block, no
   LOAD_GLOBAL, and at most this many instructions that are not "light"
   (produce little or no code) */
#define INLINE_MAXIMUM_WEIGHT 6
#define IS_LIGHT_INSTR(op) (le_op_flags(op) & LE_OPF_LIGHT)

/* ceval tests eval_breaker before every instruction it reaches through
   DISPATCH(): after most opcodes, and at a frame's start. Compiled code
   tests it at every backward jump (ENTER_JUMP_TO), after calls (below) and
   LeCode_Run at the start; before one of BREAKER_SKIPS, the test is before
   the next instruction instead. */
#define BREAKER_CHECK(unit)              \
    do {                                 \
        int _p = (int)(unit) * LE_UNIT;  \
        if (BREAKER_SKIPS(le_op(c, _p))) \
            _p = le_after(c, _p);        \
        breaker_check[_p / LE_UNIT] = 1; \
    } while (0)

#define MP_HERE()                           \
    do {                                    \
        entry_count[entry_count_index] = 8; \
    } while (0)

/* one bit per local, per instruction start */
#define LIVE_WORDS(nlocals) (((nlocals) + 63) / 64)
#define LIVE_TEST(set, i) (((set)[(i) / 64] >> ((i) % 64)) & 1)

static inline __attribute__((always_inline)) int
_live_union(uint64_t *dst, const uint64_t *src, int words)
{
    int i, changed = 0;
    for (i = 0; i < words; i++) {
        uint64_t v = dst[i] | src[i];
        changed |= v != dst[i];
        dst[i] = v;
    }
    return changed;
}

DEFINEFN int
psyco_class_cell(PyCodeObject *co)
{
    Py_ssize_t fv;
    for (fv = 0; fv < le_code_nfrees(co); fv++)
        if (PyUnicode_CompareWithASCIIString(le_code_free_name(co, fv),
                                             "__class__") == 0)
            return (int)fv;
    return -1;
}

/* a read (load) or write of local in a set of live locals, going backward */
static inline __attribute__((always_inline)) void
_live_access(uint64_t *set, int local, int load, int nlocals)
{
    if (local >= nlocals)
        return;
    if (load)
        set[local / 64] |= (uint64_t)1 << (local % 64);
    else
        set[local / 64] &= ~((uint64_t)1 << (local % 64));
}

/* one backward pass of _find_dead_locals's dataflow: whether a set grew */
static inline __attribute__((always_inline)) int
_live_pass(Py_ssize_t n, const int *op, const int *arg, const int *nxt,
           const int *setup_at, const int *try_end, const int *handler_at,
           int nsetups, const int *break_to, int nbreak, const int *exit_to,
           int nexit, uint64_t *live, uint64_t *tmp, int nlocals, int words)
{
    Py_ssize_t i, k;
    int changed = 0;
    for (i = n - 1; i >= 0; i--) {
        int o = op[i], a = arg[i], f;
        uint64_t *in = live + i * words;
        if (o < 0)
            continue;
        f = le_op_flags(o);
        memset(tmp, 0, words * sizeof(uint64_t));
        for (k = 0; k < nsetups; k++)
            if (setup_at[k] <= i && i < try_end[k])
                _live_union(tmp, live + handler_at[k] * words, words);
        if (o == BREAK_LOOP)
            for (k = 0; k < nbreak; k++)
                _live_union(tmp, live + break_to[k] * words, words);
        else if (o == END_FINALLY) {
            for (k = 0; k < nexit; k++)
                _live_union(tmp, live + exit_to[k] * words, words);
            _live_union(tmp, live + nxt[i] * words, words);
        }
        /* its return, by END_FINALLY, is an exit_to */
        else if (o == CALL_FINALLY)
            _live_union(tmp, live + a * words, words);
        else if (!(f & LE_OPF_STOPS)) {
            if (f & (LE_OPF_JUMPS | LE_OPF_BRANCHES))
                _live_union(tmp, live + a * words, words);
            if (!(f & LE_OPF_JUMPS))
                _live_union(tmp, live + nxt[i] * words, words);
        }
        /* in = uses | (out - defs) */
        if ((o == STORE_FAST || o == DELETE_FAST ||
             o == LOAD_FAST_AND_CLEAR) &&
            a < nlocals)
            tmp[a / 64] &= ~((uint64_t)1 << (a % 64));
        if ((o == LOAD_FAST || o == DELETE_FAST || o == LOAD_FAST_CHECK ||
             o == LOAD_FAST_AND_CLEAR) &&
            a < nlocals)
            tmp[a / 64] |= (uint64_t)1 << (a % 64);
        /* two locals, the first in the high four bits: the second's access
           first, going backward */
        if (o == LOAD_FAST_LOAD_FAST || o == STORE_FAST_LOAD_FAST ||
            o == STORE_FAST_STORE_FAST) {
            _live_access(tmp, a & 15, o != STORE_FAST_STORE_FAST, nlocals);
            _live_access(tmp, a >> 4, o == LOAD_FAST_LOAD_FAST, nlocals);
        }
        changed |= _live_union(in, tmp, words);
    }
    return changed;
}

/* write the dead locals at each merge point into its fatlist (before any
   code buffer is added). Returns 0 on memory error, with nothing set.
   The first argument of code with a __class__ cell is never dead: a
   zero-argument super() reads it (psuper_noargs). Nor is a local that is
   also a cell, which the *_DEREF instructions read. */
static int
_find_dead_locals(PyCodeObject *co, co_report_t *report)
{
    le_code_t c = le_code(co);
    Py_ssize_t n = c.size / LE_UNIT;
    int nlocals = le_code_nlocals(co), words = LIVE_WORDS(le_code_nlocals(co));
    Py_ssize_t i, m;
    int *op, *nxt;
    int *arg; /* a jump's target, in code units; else the argument */
    int *setup_at, *try_end, *handler_at,
        nsetups = 0; /* try bodies: [setup_at, try_end) */
    int *break_to, *exit_to, nbreak = 0, nexit = 0;
    uint64_t *live, *tmp;
    unsigned char *jump_targets; /* 1: loop exit, 2: continue target */
    int ok = 0;
    int keep_first = co->co_argcount > 0 && psyco_class_cell(co) >= 0;

    if (nlocals == 0)
        return 1;
    /* one block: the sets first, for their alignment */
    live = PyMem_Calloc(1, ((n + 2) * words) * sizeof(uint64_t) +
                               8 * (n + 1) * sizeof(int) + (n + 1));
    if (live == NULL)
        goto done;
    tmp = live + (n + 1) * words;
    op = (int *)(tmp + words);
    arg = op + (n + 1);
    nxt = arg + (n + 1);
    setup_at = nxt + (n + 1);
    try_end = setup_at + (n + 1);
    handler_at = try_end + (n + 1);
    break_to = handler_at + (n + 1);
    exit_to = break_to + (n + 1);
    jump_targets = (unsigned char *)(exit_to + (n + 1));

    /* decode: op[i] = -1 except at instruction starts */
    for (i = 0; i < n; i++) op[i] = -1;
    op[n] = -1;
    for (i = 0; i < n;) {
        Py_ssize_t start = i;
        le_instr_t in = le_instr(c, (int)i * LE_UNIT);
        int o = in.op, j;
        i = in.next / LE_UNIT;
        j = in.target >= 0 ? in.target / LE_UNIT : -1;
        op[start] = o;
        arg[start] = j >= 0 ? j : in.arg;
        nxt[start] = (int)i;
        switch (o) {
            case SETUP_LOOP:
                jump_targets[j] |= 1;
                break;
            case CONTINUE_LOOP:
                jump_targets[j] |= 2;
                break;
            case CALL_FINALLY: /* END_FINALLY returns after it */
                jump_targets[i] |= 2;
                break;
            case SETUP_EXCEPT:
            case SETUP_FINALLY:
            case SETUP_WITH:
                setup_at[nsetups] = (int)i;
                try_end[nsetups] = j;
                handler_at[nsetups++] = j;
                break;
        }
    }
#if LE_HAS_EXCEPTION_TABLE
    {
        Py_ssize_t at = 0;
        le_handler_t h;
        while (le_handler_next(co, &at, &h)) {
            setup_at[nsetups] = h.start / LE_UNIT;
            try_end[nsetups] = h.end / LE_UNIT;
            handler_at[nsetups++] = h.handler / LE_UNIT;
        }
    }
#endif

    /* where BREAK_LOOP and END_FINALLY may go */
    for (i = 0; i < n; i++)
        if (op[i] >= 0 && jump_targets[i] != 0) {
            if (jump_targets[i] & 1)
                break_to[nbreak++] = (int)i;
            exit_to[nexit++] = (int)i;
        }

    /* backward dataflow to a fixed point; a separate copy for code with
       at most 64 locals, where a set is one word */
    if (words == 1)
        while (_live_pass(n, op, arg, nxt, setup_at, try_end, handler_at,
                          nsetups, break_to, nbreak, exit_to, nexit, live, tmp,
                          nlocals, 1));
    else
        while (_live_pass(n, op, arg, nxt, setup_at, try_end, handler_at,
                          nsetups, break_to, nbreak, exit_to, nexit, live, tmp,
                          nlocals, words));

    for (m = 0; m < report->mergepoint_count; m++) {
        mergepoint_t *mp = &report->mergepoints[m];
        uint64_t *in = live + (mp->bytecode_position / LE_UNIT) * words;
        int v;
        for (v = keep_first; v < nlocals; v++)
            if (!LIVE_TEST(in, v) && !le_code_local_is_cell(co, v)) {
                PyObject *o = PyLong_FromLong(v);
                if (o == NULL || PyList_Append(mp->entries.fatlist, o) < 0) {
                    Py_XDECREF(o);
                    goto done;
                }
                Py_DECREF(o);
            }
    }
    ok = 1;
done:
    PyMem_Free(live);
    return ok;
}

/* NULL with *bad set: a jump to no instruction's start, which LeJIT doesn't
   compile (a wrong unit would be a jump to the wrong place) */
static co_report_t *
_find_merge_points(PyCodeObject *co, int *bad)
{
    le_code_t c = le_code(co);
    le_instr_t in;
    int opcode;
    Py_ssize_t next_unit = 0, end_unit = c.size / LE_UNIT;
    Py_ssize_t mergepoint_count = 0;
    Py_ssize_t mergepoint_index = 0;
    co_report_t *report;
    int flags = 0;
    int weight = 0, loads_global = 0;
    Py_ssize_t entry_count_index = -1;
    Py_ssize_t entry_count_len = end_unit + 1;
    unsigned char entry_count[entry_count_len];
    unsigned char is_start[entry_count_len];
    unsigned char breaker_check[entry_count_len];
    unsigned char is_target[entry_count_len];
    int bad_jump = 0;
    memset(entry_count, 0, entry_count_len);
    memset(is_start, 0, entry_count_len);
    memset(breaker_check, 0, entry_count_len);
    memset(is_target, 0, entry_count_len);

    entry_count[0] = 2; /* start of code is always a merge point */
    while (next_unit < end_unit) {
        /* an instruction with EXTENDED_ARG prefixes is one unit starting at
           the first prefix: that is where jumps land, and pycompiler.c
           handles it in one step */
        entry_count_index = next_unit;
        in = le_instr(c, (int)next_unit * LE_UNIT);
        opcode = in.op;
        next_unit = in.next / LE_UNIT;
        is_start[entry_count_index] = 1;
        if (!IS_LIGHT_INSTR(opcode))
            weight++;
        if (opcode == LOAD_GLOBAL)
            loads_global = 1;
        /* after a call pycompiler.c tests at the end of the call itself,
           if it emitted a call that may block, except before an
           instruction ceval skips: then always, after that one */
        if ((le_op_flags(opcode) & LE_OPF_CALL) && next_unit < end_unit &&
            BREAKER_SKIPS(le_op(c, in.next)))
            BREAKER_CHECK(next_unit);
        extra_assert(
            (in.target >= 0) ==
            ((le_op_flags(opcode) & (LE_OPF_JUMPS | LE_OPF_BRANCHES)) != 0));
        switch (opcode) {
            /* a handler can be entered from anywhere in the block */
            case SETUP_EXCEPT:
                flags |= MP_FLAGS_HAS_EXCEPT;
                MP_HERE();
                entry_count[JUMP_TARGET(TARGET_UNIT())] = 8;
                ENTER_NEXT();
                break;
            case SETUP_FINALLY:
            case SETUP_WITH:
                flags |= MP_FLAGS_HAS_FINALLY;
                MP_HERE();
                entry_count[JUMP_TARGET(TARGET_UNIT())] = 8;
                ENTER_NEXT();
                break;
            /* BREAK_LOOP goes to the SETUP_LOOP target, counted there;
               CALL_FINALLY's successors are the finally block and the next
               instruction, where END_FINALLY leaves the block */
            default:
                if (le_op_flags(opcode) & LE_OPF_STOPS)
                    break;
                if (le_op_flags(opcode) & (LE_OPF_JUMPS | LE_OPF_BRANCHES))
                    ENTER_JUMP_TO(TARGET_UNIT());
                if (!(le_op_flags(opcode) & LE_OPF_JUMPS))
                    ENTER_NEXT();
        }
    }

#if LE_HAS_EXCEPTION_TABLE
    /* a handler can be entered from anywhere in its range */
    {
        Py_ssize_t at = 0;
        le_handler_t h;
        while (le_handler_next(co, &at, &h)) {
            flags |= MP_FLAGS_HAS_EXCEPT;
            entry_count[JUMP_TARGET(h.handler / LE_UNIT)] = 8;
        }
    }
#endif

    for (entry_count_index = 0; entry_count_index < entry_count_len;
         entry_count_index++)
        if (is_target[entry_count_index] && !is_start[entry_count_index])
            bad_jump = 1;
    if (bad_jump) {
        *bad = 1;
        return NULL;
    }

    /* cap the distance between merge points (in reachable instructions,
       in bytecode order) */
    {
        int distance = 0;
        for (entry_count_index = 0; entry_count_index < entry_count_len;
             entry_count_index++) {
            if (!is_start[entry_count_index] ||
                entry_count[entry_count_index] == 0)
                continue;
            if (entry_count[entry_count_index] > 1 ||
                ++distance > MAX_MP_DISTANCE) {
                entry_count[entry_count_index] = 8;
                distance = 0;
            }
        }
    }

    for (entry_count_index = 0; entry_count_index < entry_count_len;
         entry_count_index++) {
        if (entry_count[entry_count_index] > 1) {
            mergepoint_count++;
        }
    }

    report = (co_report_t *)PyMem_Malloc(
        sizeof(co_report_t) + (sizeof(mergepoint_t) * mergepoint_count) +
        entry_count_len);
    if (report == NULL)
        return NULL;
    report->breaker_checks =
        (unsigned char *)&report->mergepoints[mergepoint_count + 1];
    memcpy(report->breaker_checks, breaker_check, entry_count_len);
    /* the single merge point means no loop and no join: after the call
       enters the inlined body (pycompiler.c, EInline) the next merge point
       looked for is past the start, so inlined state is never snapshotted */
    if (mergepoint_count == 1 && flags == 0 && !loads_global &&
        weight <= INLINE_MAXIMUM_WEIGHT)
        flags |= MP_FLAGS_INLINABLE;
    report->flags = flags;
    report->fuse_stopped = 0;
    report->mergepoint_count = mergepoint_count;
    report->unit_count = entry_count_len - 1;
    report->joins = NULL;
    for (entry_count_index = 0; entry_count_index < entry_count_len;
         entry_count_index++) {
        if (entry_count[entry_count_index] > 1) {
            report->mergepoints[mergepoint_index].bytecode_position =
                entry_count_index * LE_UNIT;
            report->mergepoints[mergepoint_index].entries.fatlist =
                PyList_New(0);
            if (report->mergepoints[mergepoint_index].entries.fatlist ==
                NULL) {
                report->mergepoint_count = mergepoint_index;
                report_free(report);
                return NULL;
            }
            report->mergepoints[mergepoint_index].blocks = 0;
            report->mergepoints[mergepoint_index].entries.varies = NULL;
            mergepoint_index++;
        }
    }
    report->mergepoints[mergepoint_count].bytecode_position = -1;
    report->mergepoints[mergepoint_count].entries.fatlist = NULL;
    report->mergepoints[mergepoint_count].blocks = 0;
    report->mergepoints[mergepoint_count].entries.varies = NULL;
    if (!_find_dead_locals(co, report)) {
        /* out of memory: keep every local (the lists may hold some) */
        PyErr_Clear();
        for (mergepoint_index = 0; mergepoint_index < mergepoint_count;
             mergepoint_index++) {
            PyObject *l =
                report->mergepoints[mergepoint_index].entries.fatlist;
            PyList_SetSlice(l, 0, PyList_GET_SIZE(l), NULL);
        }
    }
    return report;
}

/* builtins that read the calling frame's locals, which compiled code keeps
   in registers and on the machine stack; the list is
   upstream's NoControlFlowIfBuiltin */
static const char *const frame_locals_builtins[] = {"locals", "vars", "dir",
                                                    "eval",   "exec", NULL};

static int
loads_frame_locals_builtin(PyCodeObject *co, int namei)
{
    int i;
    PyObject *name = PyTuple_GET_ITEM(co->co_names, namei);
    Py_ssize_t len = PyUnicode_GET_LENGTH(name);
    if (len < 3 || len > 6)
        return 0; /* none is shorter or longer */
    for (i = 0; frame_locals_builtins[i] != NULL; i++)
        if (!PyUnicode_CompareWithASCIIString(name, frame_locals_builtins[i]))
            return 1;
    return 0;
}

/* the CALL_INTRINSIC_1 arguments pycompiler.c compiles, besides those
   bytecode.h:le_instr folds to the opcodes they replaced: a generator's
   PEP 479 handler, and the REPL's print */
static int
compiled_intrinsic(int arg)
{
#if LE_HAS_OPCODE_CALL_INTRINSIC_1
    return arg == INTRINSIC_PRINT || arg == INTRINSIC_STOPITERATION_ERROR;
#else
    return 0;
#endif
}

static PyObject *
_build_merge_points(PyCodeObject *co, int module)
{
    le_code_t c;
    int pos, opcode = -1, oparg;
    co_report_t *report;
    int reads_class, bad = 0;

    /* XXX tony: all these not supported/buggy currently */
    if (module) {
        goto not_ok;
    }
#if LE_HAS_OPCODE_TO_BOOL
    /* 3.13's bytecode is not compiled yet */
    goto not_ok;
#endif
    if (le_code_prepare(co) < 0)
        return NULL;
    /* a generator compiles only into its consumer
       (psyfunc.c:psyco_call_genexpr): the eval hook never runs generator
       code, and psyco_call_pyfunc makes a pending generator of it */
    if (co->co_flags & (CO_COROUTINE | CO_GENERATOR | CO_ASYNC_GENERATOR) &&
        !psyco_is_fusable_generator(co)) {
        goto not_ok;
    }
    /* zero-argument super() reads the __class__ cell and the first
       argument from the real frame (typeobject.c:super_init), which a
       compiled callee hasn't: pbltinmodule.c:psuper_noargs passes both
       from the compiled frame instead. Code that rebinds its first
       argument stays interpreted, in case a super() call that meta doesn't
       see reaches a real frame, whose locals compiled code doesn't write */
    reads_class = psyco_class_cell(co) >= 0;

    c = le_code(co);
    for (pos = 0; pos < c.size;) {
        le_instr_t in = le_instr(c, pos);
        int prev = opcode;
        opcode = in.op;
        oparg = in.arg;
        pos = in.next;
        if (!(le_op_flags(opcode) & LE_OPF_COMPILED))
            goto not_ok;
        /* pycompiler.c:call_kwnames finds a CALL's KW_NAMES before this */
        if (LE_HAS_OPCODE_PRECALL && opcode == CALL && prev != PRECALL)
            goto not_ok;
        if (opcode == CALL_INTRINSIC_1 && !compiled_intrinsic(oparg))
            goto not_ok;
        if (opcode == LOAD_GLOBAL &&
            loads_frame_locals_builtin(co, LE_GLOBAL_NAME(oparg)))
            goto not_ok;
        if (reads_class && oparg == 0 &&
            (opcode == STORE_FAST || opcode == DELETE_FAST))
            goto not_ok;
        if (reads_class &&
            (opcode == STORE_FAST_STORE_FAST
                 ? (oparg >> 4) == 0 || (oparg & 15) == 0
                 : opcode == STORE_FAST_LOAD_FAST && (oparg >> 4) == 0))
            goto not_ok;
    }

    report = _find_merge_points(co, &bad);
    if (bad)
        goto not_ok;
    if (report == NULL)
        return NULL;
    {
        PyObject *capsule = PyCapsule_New(report, NULL, report_destructor);
        if (capsule == NULL)
            report_free(report);
        return capsule;
    }
not_ok:

    /*printf("Unsupported bytecode %d for %s\n", opcode,
           PyUnicode_AsUTF8(co->co_name));*/
    Py_RETURN_NONE;
}

/* whether an exception raised at 'yield' (the position of the code's one
   YIELD_VALUE) has a handler in the code, so that closing the generator
   there would run code: a try or with around it. 3.12's implicit PEP 479
   handler only turns StopIteration into RuntimeError. */
static int
yield_has_handler(PyCodeObject *co, le_code_t c, int yield)
{
#if LE_HAS_EXCEPTION_TABLE
    le_handler_t h;
    (void)c;
    if (!le_handler_find(co, yield, &h))
        return 0;
#if LE_HAS_OPCODE_CALL_INTRINSIC_1
    {
        le_instr_t in = le_instr(c, h.handler);
        if (in.op == CALL_INTRINSIC_1 &&
            in.arg == INTRINSIC_STOPITERATION_ERROR)
            return 0;
    }
#endif
    return 1;
#else
    int pos;
    (void)co;
    for (pos = 0; pos < c.size;) {
        le_instr_t in = le_instr(c, pos);
        int at = pos;
        pos = in.next;
        switch (in.op) {
            case SETUP_EXCEPT:
            case SETUP_FINALLY:
            case SETUP_WITH:
            case SETUP_ASYNC_WITH:
                if (at < yield && yield < in.target)
                    return 1;
        }
    }
    return 0;
#endif
}

/* the code of a generator a builtin consumer can run fused
   (psyfunc.c:psyco_call_genexpr), as a generator expression's: a plain
   generator with one yield (3.7 also allows a yield expression inside a
   generator expression), no yield from, and no handler around its yield,
   so that a consumer stopping early can drop it with no code run */
DEFINEFN int
psyco_is_fusable_generator(PyCodeObject *co)
{
    le_code_t c;
    int pos, yields = 0, yield = -1;
    if ((co->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR |
                         CO_ITERABLE_COROUTINE)) != CO_GENERATOR)
        return 0;
    if (le_code_prepare(co) < 0) {
        PyErr_Clear(); /* out of memory: made as a plain generator */
        return 0;
    }
    c = le_code(co);
    for (pos = 0; pos < c.size;) {
        le_instr_t in = le_instr(c, pos);
        int at = pos;
        pos = in.next;
        if (in.op == YIELD_VALUE) {
            yields++;
            yield = at;
        }
        else if (in.op == YIELD_FROM || in.op == GET_YIELD_FROM_ITER)
            return 0;
    }
    return yields == 1 && !yield_has_handler(co, c, yield);
}

DEFINEFN PyObject *
psyco_get_merge_points(PyCodeObject *co, int module)
{
    /* borrowed: the record keeps it */
    le_code_extra_t *x = le_code_extra(co);
    if (x == NULL) {
        PyErr_Clear(); /* out of memory: just don't compile */
        return Py_None;
    }
    if (x->merge_points != NULL)
        return x->merge_points;
    return psyco_analyse_code(co, x, module);
}

DEFINEFN __attribute__((noinline)) PyObject *
psyco_analyse_code(PyCodeObject *co, le_code_extra_t *x, int module)
{
    if (x->merge_points == NULL) {
        PyObject *mps;
        LE_COMPILE_BEGIN(LE_ENTER_ANALYSE);
        mps = _build_merge_points(co, module);
        LE_COMPILE_END(LE_ENTER_ANALYSE);
        if (mps == NULL) {
            /* out of memory: don't compile this time, and
               leave nothing cached so that a later call tries again */
            PyErr_Clear();
            return Py_None;
        }
        x->merge_points = mps;
    }
    return x->merge_points;
}

DEFINEFN mergepoint_t *
psyco_next_merge_point(PyObject *mergepoints, int position)
{
    mergepoint_t *mp = psyco_first_merge_point(mergepoints);
    /* the array ends with a sentinel whose bytecode_position is -1; stop
       there rather than walking past it */
    while (mp->bytecode_position >= 0 && mp->bytecode_position < position) {
        mp++;
    }
    return mp;
}

DEFINEFN mergepoint_t *
psyco_first_merge_point(PyObject *mergepoints)
{
    return &(((co_report_t *)PyCapsule_GetPointer(mergepoints, NULL))
                 ->mergepoints[0]);
}

DEFINEFN mergepoint_t *
psyco_exact_merge_point(PyObject *mergepoints, int position)
{
    mergepoint_t *mp = psyco_next_merge_point(mergepoints, position);
    if (mp->bytecode_position == position) {
        return mp;
    }
    return NULL;
}

/* The blocks started after a split site at 'position' (pycompiler.c, the
   split_after_global branch), made on first use; NULL if out of memory. */
DEFINEFN global_entries_t *
psyco_join_point(PyObject *mergepoints, int position)
{
    co_report_t *report = PyCapsule_GetPointer(mergepoints, NULL);
    global_entries_t *ge;
    psyco_assert(position >= 0 && position / 2 < report->unit_count);
    if (report->joins == NULL) {
        report->joins =
            PyMem_Calloc(report->unit_count, sizeof(global_entries_t));
        if (report->joins == NULL)
            return NULL;
    }
    ge = &report->joins[position / 2];
    if (ge->fatlist == NULL) {
        ge->fatlist = PyList_New(0);
        if (ge->fatlist == NULL) {
            PyErr_Clear();
            return NULL;
        }
    }
    return ge;
}

/* whether 'ge' is one of this report's lists (codemanager.c) */
DEFINEFN bool
psyco_ge_in_report(PyObject *mergepoints, global_entries_t *ge)
{
    co_report_t *report;
    if (mergepoints == NULL || !PyCapsule_CheckExact(mergepoints))
        return false;
    report = PyCapsule_GetPointer(mergepoints, NULL);
    if (ge >= &report->mergepoints[0].entries &&
        ge <= &report->mergepoints[report->mergepoint_count].entries)
        return true;
    return report->joins != NULL && ge >= report->joins &&
           ge < report->joins + report->unit_count;
}

DEFINEFN int
psyco_mp_flags(PyObject *mergepoints)
{
    co_report_t *report = PyCapsule_GetPointer(mergepoints, NULL);
    return report->flags;
}

DEFINEFN int *
psyco_fuse_stopped(PyObject *mergepoints)
{
    co_report_t *report = PyCapsule_GetPointer(mergepoints, NULL);
    return &report->fuse_stopped;
}

DEFINEFN int
psyco_breaker_check_at(PyObject *mergepoints, int position)
{
    co_report_t *report = PyCapsule_GetPointer(mergepoints, NULL);
    return report->breaker_checks[position / 2];
}
