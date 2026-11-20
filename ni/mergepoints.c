/*
 * Ni's replacement for Psyco's merge-point analysis. It has three jobs:
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
 *     SETUP and its handler), BREAK_LOOP every loop exit, END_FINALLY those
 *     and every CONTINUE_LOOP target.
 */
#include "mergepoints.h"
#include <Python.h>
#include "Python/pycinternal.h"
#include "vcompiler.h"
#include "codemanager.h"

typedef struct {
    int flags;
    Py_ssize_t mergepoint_count;
    Py_ssize_t unit_count;      /* code units in co_code */
    global_entries_t *joins;    /* per code unit, or NULL: psyco_join_point */
    /* one per code unit: compiled code tests eval_breaker before that
       instruction. Allocated after the merge points. */
    unsigned char *breaker_checks;
    /*
    NULL terminated.
    on allocation we extend this array to count(mergepoints) + 1.
    */
    mergepoint_t mergepoints[1];
} co_report_t;

/* when the code object dies (ni.c:code_extra_free) */
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

#define NEXTOPARG()                      \
    do {                                 \
        _Py_CODEUNIT word = *next_instr; \
        opcode = _Py_OPCODE(word);       \
        oparg = _Py_OPARG(word);         \
        next_instr++;                    \
    } while (0)

#define _ENTER_INC(loc)             \
    do {                            \
        if (entry_count[loc] < 8) { \
            entry_count[loc]++;     \
        }                           \
    } while (0)

#define ENTER_NEXT()                               \
    do {                                           \
        _ENTER_INC(next_instr - first_instr);      \
    } while (0)

/* a jump target, in code units, checked to be in the code (where it starts
   an instruction is checked once all are known: JUMP_TARGET_CHECK) */
#define JUMP_TARGET(target)                                                  \
    ({                                                                       \
        Py_ssize_t _j = (target);                                            \
        if (_j < 0 || _j >= entry_count_len - 1) {                           \
            bad_jump = 1;                                                    \
            _j = 0;                                                          \
        }                                                                    \
        is_target[_j] = 1;                                                   \
        _j;                                                                  \
    })

/* target: in code units. A backward jump is to a loop head: always a merge
   point, or compilation would unroll the loop */
#define ENTER_JUMP_TO(target)                       \
    do {                                            \
        Py_ssize_t _t = JUMP_TARGET(target);        \
        if (_t <= entry_count_index) {              \
            entry_count[_t] = 8;                    \
            BREAKER_CHECK(_t);                      \
        } else                                      \
            _ENTER_INC(_t);                         \
    } while (0)

/* the code unit just after the current instruction (relative jumps) */
#define NEXT_UNIT() (next_instr - first_instr)
/* a jump's argument in code units: NI_JUMP_BYTES, which pycompiler.c's
   positions use, over the unit */
#define JUMP_UNITS(a) (NI_JUMP_BYTES(a) / (int)sizeof(_Py_CODEUNIT))

#define MAX_MP_DISTANCE 170

/* inlining (upstream's rule): one merge point (the start), no block, no
   LOAD_GLOBAL, and at most this many instructions that are not "light"
   (produce little or no code) */
#define INLINE_MAXIMUM_WEIGHT 6
#define IS_LIGHT_INSTR(op)                                                   \
    ((op) == NOP || (op) == POP_TOP || (op) == ROT_TWO ||                    \
     (op) == ROT_THREE || (op) == DUP_TOP || (op) == DUP_TOP_TWO ||          \
     (op) == LOAD_CONST || (op) == LOAD_FAST || (op) == STORE_FAST ||        \
     (op) == DELETE_FAST || (op) == UNPACK_SEQUENCE ||                       \
     (op) == BUILD_TUPLE || (op) == BUILD_LIST || (op) == BUILD_MAP ||       \
     (op) == RETURN_VALUE || (op) == JUMP_FORWARD ||                         \
     (op) == JUMP_ABSOLUTE || (op) == POP_JUMP_IF_FALSE ||                   \
     (op) == POP_JUMP_IF_TRUE || (op) == JUMP_IF_FALSE_OR_POP ||             \
     (op) == JUMP_IF_TRUE_OR_POP || (op) == EXTENDED_ARG)

/* ceval tests eval_breaker before every instruction it reaches through
   DISPATCH(): after most opcodes, and at a frame's start. Compiled code
   tests it at the target of every backward jump, after calls (below) and
   NiCode_Run at the start; before one of BREAKER_SKIPS, the test is before
   the next instruction instead. */
#define BREAKER_CHECK(unit)                                                  \
    do {                                                                     \
        Py_ssize_t _u = (unit);                                              \
        if (BREAKER_SKIPS(_Py_OPCODE(first_instr[_u])))                      \
            _u++;                                                            \
        breaker_check[_u] = 1;                                               \
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
    for (fv = 0; fv < PyTuple_GET_SIZE(co->co_freevars); fv++)
        if (PyUnicode_CompareWithASCIIString(
                PyTuple_GET_ITEM(co->co_freevars, fv), "__class__") == 0)
            return (int)fv;
    return -1;
}

/* one backward pass of _find_dead_locals's dataflow: whether a set grew */
static inline __attribute__((always_inline)) int
_live_pass(Py_ssize_t n, const int *op, const int *arg, const int *nxt,
           const int *setup_at, const int *handler_at, int nsetups,
           const int *break_to, int nbreak, const int *exit_to, int nexit,
           uint64_t *live, uint64_t *tmp, int nlocals, int words)
{
    Py_ssize_t i, k;
    int changed = 0;
    for (i = n - 1; i >= 0; i--) {
        int o = op[i], a = arg[i];
        uint64_t *in = live + i * words;
        if (o < 0)
            continue;
        memset(tmp, 0, words * sizeof(uint64_t));
        for (k = 0; k < nsetups; k++)
            if (setup_at[k] <= i && i < handler_at[k])
                _live_union(tmp, live + handler_at[k] * words, words);
        switch (o) {
        case RETURN_VALUE:
        case RAISE_VARARGS:
            break;
        case JUMP_ABSOLUTE:
        case CONTINUE_LOOP:
            _live_union(tmp, live + JUMP_UNITS(a) * words, words);
            break;
        case JUMP_FORWARD:
        case CALL_FINALLY: /* its return, by END_FINALLY, is an exit_to */
            _live_union(tmp, live + (nxt[i] + JUMP_UNITS(a)) * words, words);
            break;
        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
        case JUMP_IF_FALSE_OR_POP:
        case JUMP_IF_TRUE_OR_POP:
            _live_union(tmp, live + JUMP_UNITS(a) * words, words);
            _live_union(tmp, live + nxt[i] * words, words);
            break;
        case FOR_ITER:
        case SETUP_LOOP:
        case SETUP_EXCEPT:
        case SETUP_FINALLY:
        case SETUP_WITH:
            _live_union(tmp, live + (nxt[i] + JUMP_UNITS(a)) * words, words);
            _live_union(tmp, live + nxt[i] * words, words);
            break;
        case BREAK_LOOP:
            for (k = 0; k < nbreak; k++)
                _live_union(tmp, live + break_to[k] * words, words);
            break;
        case END_FINALLY:
            for (k = 0; k < nexit; k++)
                _live_union(tmp, live + exit_to[k] * words, words);
            _live_union(tmp, live + nxt[i] * words, words);
            break;
        default:
            /* any other jump the source has may go either way */
            if (NI_IS_JABS(o))
                _live_union(tmp, live + JUMP_UNITS(a) * words, words);
            else if (NI_IS_JREL(o))
                _live_union(tmp, live + (nxt[i] + JUMP_UNITS(a)) * words, words);
            _live_union(tmp, live + nxt[i] * words, words);
        }
        /* in = uses | (out - defs) */
        if ((o == STORE_FAST || o == DELETE_FAST) && a < nlocals)
            tmp[a / 64] &= ~((uint64_t)1 << (a % 64));
        if ((o == LOAD_FAST || o == DELETE_FAST) && a < nlocals)
            tmp[a / 64] |= (uint64_t)1 << (a % 64);
        changed |= _live_union(in, tmp, words);
    }
    return changed;
}

/* write the dead locals at each merge point into its fatlist (before any
   code buffer is added). Returns 0 on memory error, with nothing set.
   The first argument of code with a __class__ cell is never dead: a
   zero-argument super() reads it (psuper_noargs). */
static int
_find_dead_locals(PyCodeObject *co, co_report_t *report)
{
    const _Py_CODEUNIT *first = (_Py_CODEUNIT *)PyBytes_AS_STRING(co->co_code);
    Py_ssize_t n = PyBytes_Size(co->co_code) / sizeof(_Py_CODEUNIT);
    int nlocals = co->co_nlocals, words = LIVE_WORDS(co->co_nlocals);
    Py_ssize_t i, m;
    int *op, *arg, *nxt;
    int *setup_at, *handler_at, nsetups = 0; /* try bodies: [setup_at, handler_at) */
    int *break_to, *exit_to, nbreak = 0, nexit = 0;
    uint64_t *live, *tmp;
    unsigned char *jump_targets; /* 1: loop exit, 2: continue target */
    int ok = 0;
    int keep_first = co->co_argcount > 0 && psyco_class_cell(co) >= 0;

    if (nlocals == 0)
        return 1;
    /* one block: the sets first, for their alignment */
    live = PyMem_Calloc(1, ((n + 2) * words) * sizeof(uint64_t) +
                               7 * (n + 1) * sizeof(int) + (n + 1));
    if (live == NULL)
        goto done;
    tmp = live + (n + 1) * words;
    op = (int *)(tmp + words);
    arg = op + (n + 1);
    nxt = arg + (n + 1);
    setup_at = nxt + (n + 1);
    handler_at = setup_at + (n + 1);
    break_to = handler_at + (n + 1);
    exit_to = break_to + (n + 1);
    jump_targets = (unsigned char *)(exit_to + (n + 1));

    /* decode: op[i] = -1 except at instruction starts */
    for (i = 0; i < n; i++)
        op[i] = -1;
    op[n] = -1;
    for (i = 0; i < n;) {
        Py_ssize_t start = i;
        int o = _Py_OPCODE(first[i]), a = _Py_OPARG(first[i]);
        i++;
        while (o == EXTENDED_ARG && i < n) {
            a = (a << 8) | _Py_OPARG(first[i]);
            o = _Py_OPCODE(first[i]);
            i++;
        }
        op[start] = o;
        arg[start] = a;
        nxt[start] = (int)i;
        switch (o) {
        case SETUP_LOOP:
            jump_targets[i + JUMP_UNITS(a)] |= 1;
            break;
        case CONTINUE_LOOP:
            jump_targets[JUMP_UNITS(a)] |= 2;
            break;
        case CALL_FINALLY: /* END_FINALLY returns after it */
            jump_targets[i] |= 2;
            break;
        case SETUP_EXCEPT:
        case SETUP_FINALLY:
        case SETUP_WITH:
            setup_at[nsetups] = (int)i;
            handler_at[nsetups++] = (int)(i + JUMP_UNITS(a));
            break;
        }
    }

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
        while (_live_pass(n, op, arg, nxt, setup_at, handler_at, nsetups,
                          break_to, nbreak, exit_to, nexit, live, tmp, nlocals,
                          1))
            ;
    else
        while (_live_pass(n, op, arg, nxt, setup_at, handler_at, nsetups,
                          break_to, nbreak, exit_to, nexit, live, tmp, nlocals,
                          words))
            ;

    for (m = 0; m < report->mergepoint_count; m++) {
        mergepoint_t *mp = &report->mergepoints[m];
        uint64_t *in = live + (mp->bytecode_position / 2) * words;
        int v;
        for (v = keep_first; v < nlocals; v++)
            if (!LIVE_TEST(in, v)) {
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

/* NULL with *bad set: a jump to no instruction's start, which Ni doesn't
   compile (a wrong unit would be a jump to the wrong place) */
static co_report_t *
_find_merge_points(PyCodeObject *co, int *bad)
{
    const _Py_CODEUNIT *next_instr;
    int opcode; /* Current opcode */
    int oparg;  /* Current opcode argument, if any */
    const _Py_CODEUNIT *first_instr;
    const _Py_CODEUNIT *instr_end;
    Py_ssize_t mergepoint_count = 0;
    Py_ssize_t mergepoint_index = 0;
    co_report_t *report;
    int flags = 0;
    int weight = 0, loads_global = 0;
    Py_ssize_t entry_count_index = -1;
    Py_ssize_t entry_count_len =
        (PyBytes_Size(co->co_code) / (Py_ssize_t)sizeof(_Py_CODEUNIT)) + 1;
    unsigned char entry_count[entry_count_len];
    unsigned char is_start[entry_count_len];
    unsigned char breaker_check[entry_count_len];
    unsigned char is_target[entry_count_len];
    int bad_jump = 0;
    memset(entry_count, 0, entry_count_len);
    memset(is_start, 0, entry_count_len);
    memset(breaker_check, 0, entry_count_len);
    memset(is_target, 0, entry_count_len);

    first_instr = (_Py_CODEUNIT *)PyBytes_AS_STRING(co->co_code);
    next_instr = first_instr;
    instr_end =
        first_instr + (PyBytes_Size(co->co_code) / sizeof(_Py_CODEUNIT));

    entry_count[0] = 2; /* start of code is always a merge point */
    for (; next_instr < instr_end;) {
        /* an instruction with EXTENDED_ARG prefixes is one unit starting at
           the first prefix: that is where jumps land, and pycompiler.c
           handles it in one step */
        entry_count_index = next_instr - first_instr;
        NEXTOPARG();
        while (opcode == EXTENDED_ARG && next_instr < instr_end) {
            int oldoparg = oparg;
            NEXTOPARG();
            oparg |= oldoparg << 8;
        }
        is_start[entry_count_index] = 1;
        if (!IS_LIGHT_INSTR(opcode))
            weight++;
        if (opcode == LOAD_GLOBAL)
            loads_global = 1;
        /* after a call pycompiler.c tests at the end of the call itself,
           if it emitted a call that may block, except before an
           instruction ceval skips: then always, after that one */
        if ((opcode == CALL_FUNCTION || opcode == CALL_FUNCTION_KW ||
             opcode == CALL_FUNCTION_EX || opcode == CALL_METHOD) &&
            next_instr < instr_end &&
            BREAKER_SKIPS(_Py_OPCODE(*next_instr)))
            BREAKER_CHECK(next_instr - first_instr);
        switch (opcode) {
            /* no successor in this code */
            case RETURN_VALUE:
            case RAISE_VARARGS:
            case BREAK_LOOP: /* to the SETUP_LOOP target, counted there */
                break;
            /* unconditional jumps */
            case JUMP_ABSOLUTE:
            case CONTINUE_LOOP:
                ENTER_JUMP_TO(JUMP_UNITS(oparg));
                break;
            case JUMP_FORWARD:
                ENTER_JUMP_TO(NEXT_UNIT() + JUMP_UNITS(oparg));
                break;
            /* the finally block, which END_FINALLY leaves for the next
               instruction */
            case CALL_FINALLY:
                ENTER_JUMP_TO(NEXT_UNIT() + JUMP_UNITS(oparg));
                ENTER_NEXT();
                break;
            /* two successors */
            case POP_JUMP_IF_FALSE:
            case POP_JUMP_IF_TRUE:
            case JUMP_IF_FALSE_OR_POP:
            case JUMP_IF_TRUE_OR_POP:
                ENTER_JUMP_TO(JUMP_UNITS(oparg));
                ENTER_NEXT();
                break;
            case FOR_ITER:
            case SETUP_LOOP: /* the loop's end, also reached by break */
                ENTER_JUMP_TO(NEXT_UNIT() + JUMP_UNITS(oparg));
                ENTER_NEXT();
                break;
            /* a handler can be entered from anywhere in the block */
            case SETUP_EXCEPT:
                flags |= MP_FLAGS_HAS_EXCEPT;
                MP_HERE();
                entry_count[JUMP_TARGET(NEXT_UNIT() + JUMP_UNITS(oparg))] = 8;
                ENTER_NEXT();
                break;
            case SETUP_FINALLY:
            case SETUP_WITH:
                flags |= MP_FLAGS_HAS_FINALLY;
                MP_HERE();
                entry_count[JUMP_TARGET(NEXT_UNIT() + JUMP_UNITS(oparg))] = 8;
                ENTER_NEXT();
                break;
            /* any other jump the source has may go either way */
            default:
                if (NI_IS_JABS(opcode))
                    ENTER_JUMP_TO(JUMP_UNITS(oparg));
                else if (NI_IS_JREL(opcode))
                    ENTER_JUMP_TO(NEXT_UNIT() + JUMP_UNITS(oparg));
                ENTER_NEXT();
        }
    }

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
    report->mergepoint_count = mergepoint_count;
    report->unit_count = entry_count_len - 1;
    report->joins = NULL;
    for (entry_count_index = 0; entry_count_index < entry_count_len;
         entry_count_index++) {
        if (entry_count[entry_count_index] > 1) {
            report->mergepoints[mergepoint_index].bytecode_position =
                entry_count_index * 2;
            report->mergepoints[mergepoint_index].entries.fatlist =
                PyList_New(0);
            if (report->mergepoints[mergepoint_index].entries.fatlist == NULL) {
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
            PyObject *l = report->mergepoints[mergepoint_index].entries.fatlist;
            PyList_SetSlice(l, 0, PyList_GET_SIZE(l), NULL);
        }
    }
    return report;
}

/* builtins that read the calling frame's locals, which compiled code keeps
   in registers and on the machine stack; the list is
   upstream's NoControlFlowIfBuiltin */
static const char *const frame_locals_builtins[] = {
    "locals", "vars", "dir", "eval", "exec", NULL};

static int
loads_frame_locals_builtin(PyCodeObject *co, int namei)
{
    int i;
    PyObject *name = PyTuple_GET_ITEM(co->co_names, namei);
    Py_ssize_t len = PyUnicode_GET_LENGTH(name);
    if (len < 3 || len > 6)
        return 0; /* none is shorter or longer */
    for (i = 0; frame_locals_builtins[i] != NULL; i++)
        if (_PyUnicode_EqualToASCIIString(name, frame_locals_builtins[i]))
            return 1;
    return 0;
}

static PyObject *
_build_merge_points(PyCodeObject *co, int module)
{
    const _Py_CODEUNIT *next_instr;
    int opcode; /* Current opcode */
    int oparg;  /* Current opcode argument, if any */
    const _Py_CODEUNIT *first_instr;
    const _Py_CODEUNIT *instr_end;
    co_report_t *report;
    int extended_arg = 0;
    int reads_class, bad = 0;

    /* XXX tony: all these not supported/buggy currently */
    if (module) {
        goto not_ok;
    }
    /* a generator expression compiles only into its consumer
       (psyfunc.c:psyco_call_genexpr): the eval hook never runs generator
       code, and psyco_call_pyfunc makes a pending generator of it */
    if (co->co_flags & (CO_COROUTINE | CO_GENERATOR | CO_ASYNC_GENERATOR) &&
        !psyco_is_genexpr(co)) {
        goto not_ok;
    }
    /* zero-argument super() reads the __class__ cell and the first
       argument from the real frame (typeobject.c:super_init), which a
       compiled callee hasn't: pbltinmodule.c:psuper_noargs passes both
       from the compiled frame instead. Code that rebinds its first
       argument stays interpreted, in case a super() call that meta doesn't
       see reaches a real frame, whose locals compiled code doesn't write */
    reads_class = psyco_class_cell(co) >= 0;

    first_instr = (_Py_CODEUNIT *)PyBytes_AS_STRING(co->co_code);
    next_instr = first_instr;
    instr_end =
        first_instr + (PyBytes_Size(co->co_code) / sizeof(_Py_CODEUNIT));

    for (; next_instr < instr_end;) {
        NEXTOPARG();
        oparg |= extended_arg;
        extended_arg = opcode == EXTENDED_ARG ? oparg << 8 : 0;
        if (opcode == LOAD_GLOBAL && loads_frame_locals_builtin(co, oparg))
            goto not_ok;
        if (reads_class && oparg == 0 &&
            (opcode == STORE_FAST || opcode == DELETE_FAST))
            goto not_ok;

        switch (opcode) {
            /* every case pycompiler.c has: those translated from ceval.c */
            case NOP:
            case LOAD_FAST:
            case LOAD_CONST:
            case STORE_FAST:
            case POP_TOP:
            case ROT_TWO:
            case ROT_THREE:
#if NI_HAS_OPCODE_ROT_FOUR
            case ROT_FOUR:
#endif
            case DUP_TOP:
            case DUP_TOP_TWO:
            case UNARY_POSITIVE:
            case UNARY_NEGATIVE:
            case UNARY_NOT:
            case UNARY_INVERT:
            case BINARY_POWER:
            case BINARY_MULTIPLY:
            case BINARY_MATRIX_MULTIPLY:
            case BINARY_TRUE_DIVIDE:
            case BINARY_FLOOR_DIVIDE:
            case BINARY_MODULO:
            case BINARY_ADD:
            case BINARY_SUBTRACT:
            case BINARY_SUBSCR:
            case BINARY_LSHIFT:
            case BINARY_RSHIFT:
            case BINARY_AND:
            case BINARY_XOR:
            case BINARY_OR:
            case LIST_APPEND:
            case SET_ADD:
            case INPLACE_POWER:
            case INPLACE_MULTIPLY:
            case INPLACE_MATRIX_MULTIPLY:
            case INPLACE_TRUE_DIVIDE:
            case INPLACE_FLOOR_DIVIDE:
            case INPLACE_MODULO:
            case INPLACE_ADD:
            case INPLACE_SUBTRACT:
            case INPLACE_LSHIFT:
            case INPLACE_RSHIFT:
            case INPLACE_AND:
            case INPLACE_XOR:
            case INPLACE_OR:
            case STORE_SUBSCR:
            case DELETE_SUBSCR:
            case PRINT_EXPR:
            case GET_AITER:
#if NI_HAS_CORO_GET_AWAITABLE_ITER_LINKABLE
            case GET_ANEXT:
#endif
#if NI_HAS_OPCODE_RERAISE
            case RERAISE:
#endif
#if NI_HAS_OPCODE_LOAD_ASSERTION_ERROR
            case LOAD_ASSERTION_ERROR:
#endif
            case LOAD_BUILD_CLASS:
            case STORE_ATTR:
            case DELETE_ATTR:
            case STORE_GLOBAL:
            case DELETE_GLOBAL:
            case DELETE_FAST:
            case DELETE_DEREF:
            case LOAD_CLOSURE:
            case LOAD_DEREF:
            case STORE_DEREF:
            case BUILD_STRING:
            case BUILD_TUPLE:
            case BUILD_LIST:
#if NI_HAS_OPCODE_BUILD_TUPLE_UNPACK_WITH_CALL
            case BUILD_TUPLE_UNPACK_WITH_CALL:
#endif
#if NI_HAS_OPCODE_BUILD_TUPLE_UNPACK
            case BUILD_TUPLE_UNPACK:
#endif
#if NI_HAS_OPCODE_BUILD_LIST_UNPACK
            case BUILD_LIST_UNPACK:
#endif
#if NI_HAS_OPCODE_LIST_TO_TUPLE
            case LIST_TO_TUPLE:
#endif
#if NI_HAS_OPCODE_LIST_EXTEND
            case LIST_EXTEND:
#endif
#if NI_HAS_OPCODE_SET_UPDATE
            case SET_UPDATE:
#endif
            case BUILD_SET:
#if NI_HAS_OPCODE_BUILD_SET_UNPACK
            case BUILD_SET_UNPACK:
#endif
            case BUILD_MAP:
#if NI_HAS_OPCODE_BUILD_MAP_UNPACK
            case BUILD_MAP_UNPACK:
#endif
#if NI_HAS_OPCODE_BUILD_MAP_UNPACK_WITH_CALL
            case BUILD_MAP_UNPACK_WITH_CALL:
#endif
#if NI_HAS_OPCODE_DICT_UPDATE
            case DICT_UPDATE:
#endif
#if NI_HAS_OPCODE_DICT_MERGE
            case DICT_MERGE:
#endif
            case MAP_ADD:
            case LOAD_ATTR:
            case COMPARE_OP:
#if NI_HAS_OPCODE_IS_OP
            case IS_OP:
#endif
#if NI_HAS_OPCODE_CONTAINS_OP
            case CONTAINS_OP:
#endif
#if NI_HAS_OPCODE_JUMP_IF_NOT_EXC_MATCH
            case JUMP_IF_NOT_EXC_MATCH:
#endif
            case IMPORT_FROM:
            case JUMP_FORWARD:
            case POP_JUMP_IF_FALSE:
            case POP_JUMP_IF_TRUE:
            case JUMP_IF_FALSE_OR_POP:
            case JUMP_IF_TRUE_OR_POP:
            case JUMP_ABSOLUTE:
#if NI_HAS_OPCODE_GET_LEN
            case GET_LEN:
#endif
#if NI_HAS_OPCODE_MATCH_CLASS
            case MATCH_CLASS:
#endif
#if NI_HAS_OPCODE_MATCH_MAPPING
            case MATCH_MAPPING:
#endif
#if NI_HAS_OPCODE_MATCH_SEQUENCE
            case MATCH_SEQUENCE:
#endif
#if NI_HAS_OPCODE_MATCH_KEYS
            case MATCH_KEYS:
#endif
#if NI_HAS_OPCODE_COPY_DICT_WITHOUT_KEYS
            case COPY_DICT_WITHOUT_KEYS:
#endif
            case GET_ITER:
            case GET_YIELD_FROM_ITER:
            case BEFORE_ASYNC_WITH:
#if NI_HAS_OPCODE_WITH_EXCEPT_START
            case WITH_EXCEPT_START:
#endif
            case LOAD_METHOD:
            case BUILD_SLICE:
            case FORMAT_VALUE:
#if NI_HAS_OPCODE_ROT_N
            case ROT_N:
#endif
            /* and the others */
            case RAISE_VARARGS:
            case RETURN_VALUE:
            case YIELD_VALUE:
            case POP_EXCEPT:
            case POP_BLOCK:
            case END_FINALLY:
            case STORE_NAME:
            case DELETE_NAME:
            case UNPACK_SEQUENCE:
            case UNPACK_EX:
            case LOAD_NAME:
            case LOAD_GLOBAL:
            case BUILD_CONST_KEY_MAP:
            case IMPORT_NAME:
            case IMPORT_STAR:
            case FOR_ITER:
            case BREAK_LOOP:
            case CONTINUE_LOOP:
            case SETUP_LOOP:
            case SETUP_EXCEPT:
            case SETUP_FINALLY:
            case SETUP_WITH:
            case WITH_CLEANUP_START:
            case WITH_CLEANUP_FINISH:
            case CALL_METHOD:
            case CALL_FUNCTION:
            case CALL_FUNCTION_KW:
            case CALL_FUNCTION_EX:
            case MAKE_FUNCTION:
            case EXTENDED_ARG:
            case BEGIN_FINALLY:
            case CALL_FINALLY:
            case POP_FINALLY:
            case GEN_START:
                continue;
            default:
                //printf("Not ok %s %d\n", PyUnicode_AsUTF8(co->co_name), opcode);
                goto not_ok;
        }
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

/* the code of a generator expression: one argument (the iterator), no
   cells, one yield (3.7 still allows a yield expression inside one) */
DEFINEFN int
psyco_is_genexpr(PyCodeObject *co)
{
    const _Py_CODEUNIT *p, *end;
    int yields = 0;
    if ((co->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR |
                         CO_ITERABLE_COROUTINE)) != CO_GENERATOR ||
        co->co_argcount != 1 || co->co_kwonlyargcount != 0 ||
        (co->co_flags & (CO_VARARGS | CO_VARKEYWORDS)) ||
        PyTuple_GET_SIZE(co->co_cellvars) != 0 ||
        PyUnicode_CompareWithASCIIString(co->co_name, "<genexpr>") != 0)
        return 0;
    p = (const _Py_CODEUNIT *)PyBytes_AS_STRING(co->co_code);
    end = p + PyBytes_GET_SIZE(co->co_code) / sizeof(_Py_CODEUNIT);
    for (; p < end; p++) {
        int op = _Py_OPCODE(*p);
        if (op == YIELD_VALUE)
            yields++;
        else if (op == YIELD_FROM || op == GET_YIELD_FROM_ITER)
            return 0;
    }
    return yields == 1;
}

DEFINEFN PyObject *
psyco_get_merge_points(PyCodeObject *co, int module)
{
    /* borrowed: the record keeps it */
    ni_code_extra_t *x = ni_code_extra(co);
    if (x == NULL) {
        PyErr_Clear(); /* out of memory: just don't compile */
        return Py_None;
    }
    if (x->merge_points != NULL)
        return x->merge_points;
    return psyco_analyse_code(co, x, module);
}

DEFINEFN __attribute__((noinline)) PyObject *
psyco_analyse_code(PyCodeObject *co, ni_code_extra_t *x, int module)
{
    if (x->merge_points == NULL) {
        PyObject *mps;
        NI_COMPILE_BEGIN(NI_ENTER_ANALYSE);
        mps = _build_merge_points(co, module);
        NI_COMPILE_END(NI_ENTER_ANALYSE);
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
        report->joins = PyMem_Calloc(report->unit_count,
                                     sizeof(global_entries_t));
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

DEFINEFN int
psyco_breaker_check_at(PyObject *mergepoints, int position)
{
    co_report_t *report = PyCapsule_GetPointer(mergepoints, NULL);
    return report->breaker_checks[position / 2];
}
