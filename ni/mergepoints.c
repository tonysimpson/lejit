/*
 * Ni's replacement for Psyco's merge-point analysis. It has two jobs:
 *  1. It is the opcode whitelist. A code object containing any opcode not
 *     handled in the switch gets Py_None and is never compiled.
 *  2. It marks merge points: bytecode positions where the state is compared
 *     with already-compiled code, and where a new code buffer (and the
 *     snapshot a respawn replays from) starts. As upstream, they go only
 *     where control flow needs them (D10): the start, every position
 *     entered from two or more places, every backward-jump target (loop
 *     heads), every exception or finally handler, and at least every
 *     MAX_MP_DISTANCE instructions so that respawn replays stay short.
 *     Entries are counted per instruction from its successors; an
 *     instruction with EXTENDED_ARG prefixes counts as one, starting at
 *     the first prefix. Placement only affects how much code is compiled
 *     and shared: any instruction boundary may be a merge point, which is
 *     what the earlier stop-gap did everywhere.
 *  3. Liveness of locals (D15, upstream's FULL_CONTROL_FLOW): at each merge
 *     point, the locals never read again are listed first in its fatlist,
 *     and psyco_delete_unused_vars drops them before the state is compared,
 *     so states differing only in dead locals share code. Successors are
 *     those of 2, made conservative where 3.7 bytecode doesn't say: an
 *     instruction may reach the handler of every SETUP_EXCEPT/SETUP_FINALLY
 *     whose body contains it (the compiler lays the body out between the
 *     SETUP and its handler), BREAK_LOOP every loop exit, END_FINALLY those
 *     and every CONTINUE_LOOP target. Functions
 *     that read their frame's locals (locals(), eval()...) are never
 *     compiled (C15), so dropping dead locals is invisible.
 */
#include "mergepoints.h"
#include <Python.h>
#include "Python/pycinternal.h"
#include "vcompiler.h"
#include "codemanager.h"

typedef struct {
    int flags;
    Py_ssize_t mergepoint_count;
    /*
    NULL terminated.
    on allocation we extend this array to count(mergepoints) + 1.
    */
    mergepoint_t mergepoints[1];
} co_report_t;

/* when the code object dies (ni.c:code_extra_free): its fatlists hold its
   code buffers (C14) */
static void
report_destructor(PyObject *capsule)
{
    co_report_t *report = PyCapsule_GetPointer(capsule, NULL);
    Py_ssize_t i;
    for (i = 0; i < report->mergepoint_count; i++)
        Py_XDECREF(report->mergepoints[i].entries.fatlist);
    PyMem_Free(report);
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

/* target: a byte offset. A backward jump is to a loop head: always a merge
   point, or compilation would unroll the loop */
#define ENTER_JUMP_TO(target)                       \
    do {                                            \
        Py_ssize_t _t = (target) / 2;               \
        if (_t <= entry_count_index)                \
            entry_count[_t] = 8;                    \
        else                                        \
            _ENTER_INC(_t);                         \
    } while (0)

/* byte offset just after the current instruction (relative jumps) */
#define NEXT_OFFSET() ((next_instr - first_instr) * 2)

#define MAX_MP_DISTANCE 170

/* inlining (upstream's rule): one merge point (the start), no block, no
   LOAD_GLOBAL (upstream forced a merge point after it), and at most this
   many instructions that are not "light" (produce little or no code) */
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

#define MP_HERE()                           \
    do {                                    \
        entry_count[entry_count_index] = 8; \
    } while (0)

/* one bit per local, per instruction start */
#define LIVE_WORDS(nlocals) (((nlocals) + 63) / 64)
#define LIVE_TEST(set, i) (((set)[(i) / 64] >> ((i) % 64)) & 1)

static int
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

/* write the dead locals at each merge point into its fatlist (before any
   code buffer is added). Returns 0 on memory error, with nothing set. */
static int
_find_dead_locals(PyCodeObject *co, co_report_t *report)
{
    const _Py_CODEUNIT *first = (_Py_CODEUNIT *)PyBytes_AS_STRING(co->co_code);
    Py_ssize_t n = PyBytes_Size(co->co_code) / sizeof(_Py_CODEUNIT);
    int nlocals = co->co_nlocals, words = LIVE_WORDS(co->co_nlocals);
    Py_ssize_t i, k, m;
    int *op, *arg, *nxt;
    int *setup_at, *handler_at, nsetups = 0; /* try bodies: [setup_at, handler_at) */
    uint64_t *live, *tmp;
    unsigned char *jump_targets; /* 1: loop exit, 2: continue target */
    int changed, ok = 0;

    if (nlocals == 0)
        return 1;
    op = PyMem_Calloc(n + 1, sizeof(int));
    arg = PyMem_Calloc(n + 1, sizeof(int));
    nxt = PyMem_Calloc(n + 1, sizeof(int));
    jump_targets = PyMem_Calloc(n + 1, 1);
    live = PyMem_Calloc((n + 1) * words, sizeof(uint64_t));
    tmp = PyMem_Calloc(words, sizeof(uint64_t));
    setup_at = PyMem_Calloc(n + 1, sizeof(int));
    handler_at = PyMem_Calloc(n + 1, sizeof(int));
    if (!op || !arg || !nxt || !jump_targets || !live || !tmp || !setup_at ||
        !handler_at)
        goto done;

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
            jump_targets[i + a / 2] |= 1;
            break;
        case CONTINUE_LOOP:
            jump_targets[a / 2] |= 2;
            break;
        case SETUP_EXCEPT:
        case SETUP_FINALLY:
            setup_at[nsetups] = (int)i;
            handler_at[nsetups++] = (int)(i + a / 2);
            break;
        }
    }

    /* backward dataflow to a fixed point */
    do {
        changed = 0;
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
                _live_union(tmp, live + (a / 2) * words, words);
                break;
            case JUMP_FORWARD:
                _live_union(tmp, live + (nxt[i] + a / 2) * words, words);
                break;
            case POP_JUMP_IF_FALSE:
            case POP_JUMP_IF_TRUE:
            case JUMP_IF_FALSE_OR_POP:
            case JUMP_IF_TRUE_OR_POP:
                _live_union(tmp, live + (a / 2) * words, words);
                _live_union(tmp, live + nxt[i] * words, words);
                break;
            case FOR_ITER:
            case SETUP_LOOP:
            case SETUP_EXCEPT:
            case SETUP_FINALLY:
                _live_union(tmp, live + (nxt[i] + a / 2) * words, words);
                _live_union(tmp, live + nxt[i] * words, words);
                break;
            case BREAK_LOOP:
            case END_FINALLY:
                for (k = 0; k < n; k++)
                    if (op[k] >= 0 && (jump_targets[k] & (o == BREAK_LOOP ? 1 : 3)))
                        _live_union(tmp, live + k * words, words);
                if (o == END_FINALLY)
                    _live_union(tmp, live + nxt[i] * words, words);
                break;
            default:
                _live_union(tmp, live + nxt[i] * words, words);
            }
            /* in = uses | (out - defs) */
            if ((o == STORE_FAST || o == DELETE_FAST) && a < nlocals)
                tmp[a / 64] &= ~((uint64_t)1 << (a % 64));
            if ((o == LOAD_FAST || o == DELETE_FAST) && a < nlocals)
                tmp[a / 64] |= (uint64_t)1 << (a % 64);
            changed |= _live_union(in, tmp, words);
        }
    } while (changed);

    for (m = 0; m < report->mergepoint_count; m++) {
        mergepoint_t *mp = &report->mergepoints[m];
        uint64_t *in = live + (mp->bytecode_position / 2) * words;
        int v;
        for (v = 0; v < nlocals; v++)
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
    PyMem_Free(op);
    PyMem_Free(arg);
    PyMem_Free(nxt);
    PyMem_Free(jump_targets);
    PyMem_Free(live);
    PyMem_Free(tmp);
    PyMem_Free(setup_at);
    PyMem_Free(handler_at);
    return ok;
}

static co_report_t *
_find_merge_points(PyCodeObject *co)
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
        (PyBytes_Size(co->co_code) / sizeof(_Py_CODEUNIT)) + 1;
    unsigned char entry_count[entry_count_len];
    unsigned char is_start[entry_count_len];
    memset(entry_count, 0, entry_count_len);
    memset(is_start, 0, entry_count_len);

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
        switch (opcode) {
            /* no successor in this code */
            case RETURN_VALUE:
            case RAISE_VARARGS:
            case BREAK_LOOP: /* to the SETUP_LOOP target, counted there */
                break;
            /* unconditional jumps */
            case JUMP_ABSOLUTE:
            case CONTINUE_LOOP:
                ENTER_JUMP_TO(oparg);
                break;
            case JUMP_FORWARD:
                ENTER_JUMP_TO(NEXT_OFFSET() + oparg);
                break;
            /* two successors */
            case POP_JUMP_IF_FALSE:
            case POP_JUMP_IF_TRUE:
            case JUMP_IF_FALSE_OR_POP:
            case JUMP_IF_TRUE_OR_POP:
                ENTER_JUMP_TO(oparg);
                ENTER_NEXT();
                break;
            case FOR_ITER:
            case SETUP_LOOP: /* the loop's end, also reached by break */
                ENTER_JUMP_TO(NEXT_OFFSET() + oparg);
                ENTER_NEXT();
                break;
            /* a handler can be entered from anywhere in the block */
            case SETUP_EXCEPT:
                flags |= MP_FLAGS_HAS_EXCEPT;
                MP_HERE();
                entry_count[(NEXT_OFFSET() + oparg) / 2] = 8;
                ENTER_NEXT();
                break;
            case SETUP_FINALLY:
                flags |= MP_FLAGS_HAS_FINALLY;
                MP_HERE();
                entry_count[(NEXT_OFFSET() + oparg) / 2] = 8;
                ENTER_NEXT();
                break;
            default:
                ENTER_NEXT();
        }
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
        sizeof(co_report_t) + (sizeof(mergepoint_t) * mergepoint_count));
    /* the single merge point means no loop and no join: after the call
       enters the inlined body (pycompiler.c, EInline) the next merge point
       looked for is past the start, so inlined state is never snapshotted */
    if (mergepoint_count == 1 && flags == 0 && !loads_global &&
        weight <= INLINE_MAXIMUM_WEIGHT)
        flags |= MP_FLAGS_INLINABLE;
    report->flags = flags;
    report->mergepoint_count = mergepoint_count;
    for (entry_count_index = 0; entry_count_index < entry_count_len;
         entry_count_index++) {
        if (entry_count[entry_count_index] > 1) {
            report->mergepoints[mergepoint_index].bytecode_position =
                entry_count_index * 2;
            report->mergepoints[mergepoint_index].entries.fatlist =
                PyList_New(0);
            mergepoint_index++;
        }
    }
    report->mergepoints[mergepoint_count].bytecode_position = -1;
    report->mergepoints[mergepoint_count].entries.fatlist = NULL;
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
   in registers and on the machine stack (KNOWN_BUGS C15); upstream had the
   same list (NoControlFlowIfBuiltin) but only warned at run time */
static const char *const frame_locals_builtins[] = {
    "locals", "vars", "dir", "eval", "exec", NULL};

static int
loads_frame_locals_builtin(PyCodeObject *co, int namei)
{
    int i;
    PyObject *name = PyTuple_GET_ITEM(co->co_names, namei);
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
    Py_ssize_t fv;
    int reads_class;

    /* XXX tony: all these not supported/buggy currently */
    if (module) {
        goto not_ok;
    }
    /* a generator expression compiles only into its consumer
       (psyfunc.c:psyco_call_genexpr): the eval hook never runs generator
       code, and psyco_call_pyfunc makes a pending generator of it */
    if (co->co_flags & (CO_COROUTINE | CO_GENERATOR) && !psyco_is_genexpr(co)) {
        goto not_ok;
    }
    /* zero-argument super() reads the __class__ cell and the first
       argument from the real frame (typeobject.c:super_init). Closure code
       only runs compiled from its own frame (never inlined: psyfunc.c),
       where CPython put both; compiled code doesn't write locals back, so
       the frame's first argument is right unless the code rebinds it */
    reads_class = 0;
    for (fv = 0; fv < PyTuple_GET_SIZE(co->co_freevars); fv++)
        if (PyUnicode_CompareWithASCIIString(
                PyTuple_GET_ITEM(co->co_freevars, fv), "__class__") == 0)
            reads_class = 1;

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
            case POP_TOP:
            case ROT_TWO:
            case ROT_THREE:
            case DUP_TOP:
            case DUP_TOP_TWO:
            case UNARY_POSITIVE:
            case UNARY_NEGATIVE:
            case UNARY_NOT:
            case UNARY_INVERT:
            case BINARY_POWER:
            case LIST_APPEND:
            case INPLACE_POWER:
            case STORE_SUBSCR:
            case DELETE_SUBSCR:
            case BREAK_LOOP:
            case CONTINUE_LOOP:
            case RAISE_VARARGS:
            case RETURN_VALUE:
            case NOP:
            case POP_BLOCK:
            case END_FINALLY:
            case POP_EXCEPT:
            case UNPACK_SEQUENCE:
            case STORE_ATTR:
            case DELETE_ATTR:
            case STORE_GLOBAL:
            case DELETE_NAME:
            case DELETE_GLOBAL:
            case LOAD_CONST:
            /* XXX tony: add support for these; neede for class def and modules */
            /*
            case LOAD_NAME: 
            case STORE_NAME:
            */
            case LOAD_GLOBAL:
            case LOAD_FAST:
            case STORE_FAST:
            case DELETE_FAST:
            case BUILD_TUPLE:
            case BUILD_LIST:
            case BUILD_MAP:
            case BUILD_CONST_KEY_MAP:
            case MAP_ADD:
            case LOAD_ATTR:
            case LOAD_METHOD:
            case CALL_METHOD:
            case COMPARE_OP:
            case IMPORT_NAME:
            case IMPORT_STAR:
            case IMPORT_FROM:
            case JUMP_FORWARD:
            case JUMP_IF_TRUE_OR_POP:
            case JUMP_IF_FALSE_OR_POP:
            case POP_JUMP_IF_FALSE:
            case POP_JUMP_IF_TRUE:
            case JUMP_ABSOLUTE:
            case GET_ITER:
            case FOR_ITER:
            case SETUP_LOOP:
            case BINARY_MULTIPLY:
            case BINARY_MODULO:
            case BINARY_ADD:
            case BINARY_SUBTRACT:
            case BINARY_SUBSCR:
            case BINARY_LSHIFT:
            case BINARY_RSHIFT:
            case BINARY_AND:
            case BINARY_XOR:
            case BINARY_OR:
            case BINARY_TRUE_DIVIDE:
            case BINARY_FLOOR_DIVIDE:
            case INPLACE_TRUE_DIVIDE:
            case INPLACE_FLOOR_DIVIDE:
            case INPLACE_MULTIPLY:
            case INPLACE_MODULO:
            case INPLACE_ADD:
            case INPLACE_SUBTRACT:
            case INPLACE_LSHIFT:
            case INPLACE_RSHIFT:
            case INPLACE_AND:
            case INPLACE_XOR:
            case INPLACE_OR:
            case SETUP_EXCEPT:
            case SETUP_FINALLY:
            case CALL_FUNCTION:
            case CALL_FUNCTION_KW:
            case CALL_FUNCTION_EX:
            case MAKE_FUNCTION:
            case BUILD_TUPLE_UNPACK_WITH_CALL:
            case BUILD_TUPLE_UNPACK:
            case BUILD_LIST_UNPACK:
            case LOAD_CLOSURE:
            case LOAD_DEREF:
            case STORE_DEREF:
            case BUILD_SLICE:
            case BUILD_SET:
            case SET_ADD:
            case FORMAT_VALUE:
            case BUILD_STRING:
            case EXTENDED_ARG:
                continue;
            case YIELD_VALUE:
                if (co->co_flags & CO_GENERATOR)
                    continue;
                goto not_ok;
            default:
                //printf("Not ok %s %d\n", PyUnicode_AsUTF8(co->co_name), opcode);
                goto not_ok;
        }
    }

    report = _find_merge_points(co);

    return PyCapsule_New(report, NULL, report_destructor);
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
    if (x->merge_points == NULL) {
        NI_COMPILE_BEGIN(NI_ENTER_ANALYSE);
        x->merge_points = _build_merge_points(co, module);
        NI_COMPILE_END(NI_ENTER_ANALYSE);
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

DEFINEFN int
psyco_mp_flags(PyObject *mergepoints)
{
    co_report_t *report = PyCapsule_GetPointer(mergepoints, NULL);
    return report->flags;
}