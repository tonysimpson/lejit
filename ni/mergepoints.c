/*
 * Ni's replacement for Psyco's merge-point analysis. It has two jobs:
 *  1. It is the opcode whitelist. A code object containing any opcode not
 *     handled in the switch gets Py_None and is never compiled.
 *  2. It marks merge points: bytecode positions where the state is compared
 *     with already-compiled code. Upstream built a control-flow graph and
 *     placed them only where paths join. Here 'default: MP_HERE()' makes
 *     almost every instruction a merge point. That works, but it multiplies
 *     snapshots, and because MP_FLAGS_INLINABLE requires a single merge point
 *     it also disables inlining. This is a stop-gap, to be replaced by
 *     control-flow-based placement (doc/DECISIONS.md D10; history A2-A4).
 */
#include "mergepoints.h"
#include <Python.h>
#include "Python/pycinternal.h"
#include "vcompiler.h"

typedef struct {
    int flags;
    Py_ssize_t mergepoint_count;
    /*
    NULL terminated.
    on allocation we extend this array to count(mergepoints) + 1.
    */
    mergepoint_t mergepoints[1];
} co_report_t;

static void
report_destructor(PyObject *capsule)
{
    PyMem_Free(PyCapsule_GetPointer(capsule, NULL));
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

#define ENTER_JUMP_TO(target)   \
    do {                        \
        _ENTER_INC(target / 2); \
    } while (0)

#define MP_HERE()                           \
    do {                                    \
        entry_count[entry_count_index] = 8; \
    } while (0)

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
    Py_ssize_t entry_count_index = -1;
    Py_ssize_t entry_count_len =
        (PyBytes_Size(co->co_code) / sizeof(_Py_CODEUNIT)) + 1;
    unsigned char entry_count[entry_count_len];
    memset(entry_count, 0, entry_count_len);

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
        switch (opcode) {
            case RETURN_VALUE:
                break;
            case JUMP_ABSOLUTE:
                ENTER_JUMP_TO(oparg);
                break;
            case SETUP_EXCEPT:
                flags |= MP_FLAGS_HAS_EXCEPT;
                MP_HERE();
                ENTER_NEXT();
                break;
            case SETUP_FINALLY:
                flags |= MP_FLAGS_HAS_FINALLY;
                MP_HERE();
                ENTER_NEXT();
                break;
            case POP_JUMP_IF_FALSE:
                ENTER_JUMP_TO(oparg);
                ENTER_NEXT();
                break;
            default:
                MP_HERE(); /* XXX tony: we only need MPs where we might
                              respawn/reenter need to tie this with pycompiler
                              and make effecient. Just making everything a
                              mergepoints works but I think it causes lots of
                              unnecessary reentry points to be written.
                               */
                ENTER_NEXT();
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
    report->flags = flags;
    report->mergepoint_count = mergepoint_count;
    if (mergepoint_count == 1 && flags == 0) {
        report->flags = MP_FLAGS_INLINABLE;
    }
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
    return report;
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

    /* XXX tony: all these not supported/buggy currently */
    if (module) {
        goto not_ok;
    }
    if (co->co_flags & (CO_COROUTINE | CO_GENERATOR)) {
        goto not_ok;
    }
    if (co->co_kwonlyargcount) {
        goto not_ok;
    }
    /* closures aren't supported, and zero-argument super() reads the
       __class__ cell from a real frame */
    if (PyTuple_GET_SIZE(co->co_freevars) || PyTuple_GET_SIZE(co->co_cellvars)) {
        goto not_ok;
    }

    first_instr = (_Py_CODEUNIT *)PyBytes_AS_STRING(co->co_code);
    next_instr = first_instr;
    instr_end =
        first_instr + (PyBytes_Size(co->co_code) / sizeof(_Py_CODEUNIT));

    for (; next_instr < instr_end;) {
        NEXTOPARG();

        switch (opcode) {
            case POP_TOP:
            case ROT_TWO:
            case ROT_THREE:
            case DUP_TOP:
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
            case MAKE_FUNCTION:
            case BUILD_SLICE:
            case EXTENDED_ARG:
                continue;
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

DEFINEFN PyObject *
psyco_get_merge_points(PyCodeObject *co, int module)
{
    PyObject *capsule_or_none;
    capsule_or_none = PyDict_GetItem(co_to_mp, (PyObject *)co);
    if (capsule_or_none == NULL) {
        capsule_or_none = _build_merge_points(co, module);
        PyDict_SetItem(co_to_mp, (PyObject *)co, capsule_or_none);
    }
    return capsule_or_none;
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