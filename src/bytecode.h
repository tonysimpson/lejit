/* Reading a code object's bytecode: CPython's instructions as its compiler
   wrote them, never the interpreter's specialised copy. Positions are in
   bytes, as everywhere in LeJIT. */

#ifndef _BYTECODE_H
#define _BYTECODE_H

#include <Python.h>
#include "Python/pycinternal.h"

#define LE_UNIT ((int)sizeof(_Py_CODEUNIT))

#ifndef LE_IS_JBACK
#define LE_IS_JBACK(o) 0
#endif
#ifndef LE_CACHE_ENTRIES
#define LE_CACHE_ENTRIES(o) 0
#endif

/* the code units of one code object */
typedef struct {
    const unsigned char *bytes;
    int size; /* in bytes */
} le_code_t;

/* Where CPython makes a code object's bytes when first asked for them
   (PyCode_GetCode), and keeps them until it dies: 0 once they are made,
   -1 with a MemoryError set. le_code is only used after this. */
static inline int
le_code_prepare(PyCodeObject *co)
{
#if LE_HAS_CODE_GET_CODE
    PyObject *b = PyCode_GetCode(co);
    if (b == NULL)
        return -1;
    Py_DECREF(b);
#endif
    return 0;
}

static inline le_code_t
le_code(PyCodeObject *co)
{
    le_code_t c;
#if LE_HAS_CODE_GET_CODE
    /* a new reference to the bytes le_code_prepare made */
    PyObject *b = PyCode_GetCode(co);
    if (b == NULL)
        Py_FatalError("PyCode_GetCode failed");
    Py_DECREF(b);
#else
    PyObject *b = co->co_code;
#endif
    c.bytes = (const unsigned char *)PyBytes_AS_STRING(b);
    c.size = (int)PyBytes_GET_SIZE(b);
    return c;
}

/* the opcode and argument byte of the one code unit at pos (an
   EXTENDED_ARG prefix is a unit of its own) */
static inline int
le_op(le_code_t c, int pos)
{
    return c.bytes[pos];
}

static inline int
le_arg(le_code_t c, int pos)
{
    return c.bytes[pos + 1];
}

/* the bytes of inline cache that follow an instruction */
#define LE_CACHE_BYTES(op) (LE_CACHE_ENTRIES(op) * LE_UNIT)

/* the position after the instruction whose last unit is at pos */
static inline int
le_after(le_code_t c, int pos)
{
    return pos + LE_UNIT + LE_CACHE_BYTES(le_op(c, pos));
}

/* where FOR_ITER's iterator ending goes: its target, past the END_FOR
   there that pops the iterator and its item where FOR_ITER pops the
   iterator itself */
static inline int
le_for_iter_exit(le_code_t c, int target)
{
    if (target < c.size && le_op(c, target) == END_FOR)
        return le_after(c, target);
    return target;
}

#if LE_HAS_OPCODE_BINARY_OP
/* the opcode that had BINARY_OP's operator, by its NB_ number, before
   (absent here, so its number is free) */
static inline int
le_binary_op_opcode(int nb)
{
    static const short ops[] = {
        [NB_ADD] = BINARY_ADD,
        [NB_AND] = BINARY_AND,
        [NB_FLOOR_DIVIDE] = BINARY_FLOOR_DIVIDE,
        [NB_LSHIFT] = BINARY_LSHIFT,
        [NB_MATRIX_MULTIPLY] = BINARY_MATRIX_MULTIPLY,
        [NB_MULTIPLY] = BINARY_MULTIPLY,
        [NB_REMAINDER] = BINARY_MODULO,
        [NB_OR] = BINARY_OR,
        [NB_POWER] = BINARY_POWER,
        [NB_RSHIFT] = BINARY_RSHIFT,
        [NB_SUBTRACT] = BINARY_SUBTRACT,
        [NB_TRUE_DIVIDE] = BINARY_TRUE_DIVIDE,
        [NB_XOR] = BINARY_XOR,
        [NB_INPLACE_ADD] = INPLACE_ADD,
        [NB_INPLACE_AND] = INPLACE_AND,
        [NB_INPLACE_FLOOR_DIVIDE] = INPLACE_FLOOR_DIVIDE,
        [NB_INPLACE_LSHIFT] = INPLACE_LSHIFT,
        [NB_INPLACE_MATRIX_MULTIPLY] = INPLACE_MATRIX_MULTIPLY,
        [NB_INPLACE_MULTIPLY] = INPLACE_MULTIPLY,
        [NB_INPLACE_REMAINDER] = INPLACE_MODULO,
        [NB_INPLACE_OR] = INPLACE_OR,
        [NB_INPLACE_POWER] = INPLACE_POWER,
        [NB_INPLACE_RSHIFT] = INPLACE_RSHIFT,
        [NB_INPLACE_SUBTRACT] = INPLACE_SUBTRACT,
        [NB_INPLACE_TRUE_DIVIDE] = INPLACE_TRUE_DIVIDE,
        [NB_INPLACE_XOR] = INPLACE_XOR};
    if (nb < 0 || nb >= (int)(sizeof(ops) / sizeof(ops[0])) || ops[nb] == 0)
        return -1;
    return ops[nb];
}
#endif

/* One instruction, as LeJIT's passes read it, the same on every version:
   'op' is a LeJIT opcode, CPython's name in its oldest spelling where
   versions spell one operation differently (BINARY_OP 0 is BINARY_ADD,
   POP_JUMP_BACKWARD_IF_TRUE is POP_JUMP_IF_TRUE, LOAD_ATTR with the method
   bit is LOAD_METHOD, CALL_INTRINSIC_1 with INTRINSIC_UNARY_POSITIVE is
   UNARY_POSITIVE); 'arg' with its EXTENDED_ARG prefixes folded in, and
   no longer holding what 'op' now says; 'raw' the opcode as CPython wrote
   it; 'target' a jump's target in bytes (FOR_ITER's when its iterator
   ends), else -1; 'next' the position after it and its caches. */
typedef struct {
    int op, arg, raw, target, next;
} le_instr_t;

/* the instruction that starts at pos, at its first prefix if it has any */
static inline le_instr_t
le_instr(le_code_t c, int pos)
{
    le_instr_t in;
    in.raw = le_op(c, pos);
    in.arg = le_arg(c, pos);
    while (in.raw == EXTENDED_ARG && pos + LE_UNIT < c.size) {
        pos += LE_UNIT;
        in.raw = le_op(c, pos);
        in.arg = in.arg << 8 | le_arg(c, pos);
    }
    in.next = le_after(c, pos);
    in.op = in.raw;
    in.target = -1;
    if (LE_IS_JABS(in.raw))
        in.target = LE_JUMP_BYTES(in.arg);
    else if (LE_IS_JREL(in.raw)) {
        in.target =
            in.next + (LE_IS_JBACK(in.raw) ? -1 : 1) * LE_JUMP_BYTES(in.arg);
        if (in.raw == FOR_ITER)
            in.target = le_for_iter_exit(c, in.target);
    }
    switch (in.raw) {
#if LE_HAS_OPCODE_BINARY_OP
        case BINARY_OP:
            in.op = le_binary_op_opcode(in.arg);
            if (in.op < 0)
                in.op = in.raw; /* an operator LeJIT doesn't know */
            else
                in.arg = 0;
            break;
#endif
        case POP_JUMP_FORWARD_IF_FALSE:
        case POP_JUMP_BACKWARD_IF_FALSE:
            in.op = POP_JUMP_IF_FALSE;
            break;
        case POP_JUMP_FORWARD_IF_TRUE:
        case POP_JUMP_BACKWARD_IF_TRUE:
            in.op = POP_JUMP_IF_TRUE;
            break;
        case POP_JUMP_FORWARD_IF_NONE:
        case POP_JUMP_BACKWARD_IF_NONE:
            in.op = POP_JUMP_IF_NONE;
            break;
        case POP_JUMP_FORWARD_IF_NOT_NONE:
        case POP_JUMP_BACKWARD_IF_NOT_NONE:
            in.op = POP_JUMP_IF_NOT_NONE;
            break;
#if LE_HAS_LOAD_ATTR_METHOD_BIT
        case LOAD_ATTR:
            if (in.arg & 1)
                in.op = LOAD_METHOD;
            in.arg >>= 1;
            break;
#endif
#if LE_HAS_OPCODE_CALL_INTRINSIC_1
        case CALL_INTRINSIC_1:
            if (in.arg == INTRINSIC_UNARY_POSITIVE)
                in.op = UNARY_POSITIVE;
            else if (in.arg == INTRINSIC_LIST_TO_TUPLE)
                in.op = LIST_TO_TUPLE;
            break;
#endif
    }
    return in;
}

/* the start of the instruction before the one at pos (at its first
   prefix). The caches PyCode_GetCode gives are zero, and no opcode is,
   so zero units are the caches of the instruction before them. */
static inline int
le_prev(le_code_t c, int pos)
{
    pos -= LE_UNIT;
    while (pos > 0 && le_op(c, pos) == 0 && le_arg(c, pos) == 0)
        pos -= LE_UNIT;
    while (pos >= LE_UNIT && le_op(c, pos - LE_UNIT) == EXTENDED_ARG)
        pos -= LE_UNIT;
    return pos;
}

/* What a LeJIT opcode does, for the passes over the bytecode: one bit
   each. */
/* pycompiler.c compiles it: code with any other runs in ceval */
#define LE_OPF_COMPILED 1
/* it makes little or no code (mergepoints.c's inlining weight) */
#define LE_OPF_LIGHT 2
/* a call: compiled code may test eval_breaker after it */
#define LE_OPF_CALL 4
/* ceval doesn't test eval_breaker before it, but before the next one */
#define LE_OPF_BREAKER_SKIPS 8
/* no successor in this code */
#define LE_OPF_STOPS 16
/* only to its target */
#define LE_OPF_JUMPS 32
/* to its target, or to the next instruction */
#define LE_OPF_BRANCHES 64

/* compiled only where handlers are in blocks, not an exception table */
#define LE_OPF_COMPILED_IN_BLOCKS \
    (LE_HAS_EXCEPTION_TABLE ? 0 : LE_OPF_COMPILED)

static inline int
le_op_flags(int op)
{
    switch (op) {
        /* locals, constants and the stack */
        case NOP:
        case EXTENDED_ARG:
        case RESUME:
        case LOAD_FAST:
        case LOAD_FAST_CHECK:
        case LOAD_CONST:
        case STORE_FAST:
        case DELETE_FAST:
        case POP_TOP:
        case ROT_TWO:
        case ROT_THREE:
        case DUP_TOP:
        case DUP_TOP_TWO:
        case COPY:
        case SWAP:
        case PUSH_NULL:
            return LE_OPF_COMPILED | LE_OPF_LIGHT;
        case LOAD_FAST_AND_CLEAR:
#if LE_HAS_OPCODE_ROT_FOUR
        case ROT_FOUR:
#endif
#if LE_HAS_OPCODE_ROT_N
        case ROT_N:
#endif
        /* attributes */
        case LOAD_ATTR:
        case STORE_ATTR:
        case DELETE_ATTR:
        case LOAD_SUPER_ATTR:
        /* operators */
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
        case BINARY_LSHIFT:
        case BINARY_RSHIFT:
        case BINARY_AND:
        case BINARY_XOR:
        case BINARY_OR:
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
        case COMPARE_OP:
#if LE_HAS_OPCODE_IS_OP
        case IS_OP:
#endif
#if LE_HAS_OPCODE_CONTAINS_OP
        case CONTAINS_OP:
#endif
            return LE_OPF_COMPILED;
        /* exceptions (except* stays interpreted where it has opcodes of its
           own) */
        case RAISE_VARARGS:
            return LE_OPF_COMPILED | LE_OPF_STOPS;
        case RERAISE:
            return LE_OPF_COMPILED |
                   (LE_HAS_EXCEPTION_TABLE ? LE_OPF_STOPS : 0);
        case POP_EXCEPT:
        case WITH_EXCEPT_START:
            return LE_OPF_COMPILED;
#if LE_HAS_EXCEPTION_TABLE
        case PUSH_EXC_INFO:
        case CHECK_EXC_MATCH:
        case BEFORE_WITH:
            return LE_OPF_COMPILED;
#endif
        /* subscripts, building and unpacking, imports, f-strings,
           iteration and pattern matching */
        case UNPACK_SEQUENCE:
        case BUILD_TUPLE:
        case BUILD_LIST:
        case BUILD_MAP:
            return LE_OPF_COMPILED | LE_OPF_LIGHT;
        case BINARY_SUBSCR:
        case STORE_SUBSCR:
        case DELETE_SUBSCR:
        case BINARY_SLICE:
        case STORE_SLICE:
        case BUILD_SET:
        case BUILD_CONST_KEY_MAP:
        case BUILD_STRING:
        case BUILD_SLICE:
        case LIST_APPEND:
        case SET_ADD:
        case MAP_ADD:
#if LE_HAS_OPCODE_BUILD_TUPLE_UNPACK_WITH_CALL
        case BUILD_TUPLE_UNPACK_WITH_CALL:
#endif
#if LE_HAS_OPCODE_BUILD_TUPLE_UNPACK
        case BUILD_TUPLE_UNPACK:
#endif
#if LE_HAS_OPCODE_BUILD_LIST_UNPACK
        case BUILD_LIST_UNPACK:
#endif
        case LIST_TO_TUPLE:
#if LE_HAS_OPCODE_LIST_EXTEND
        case LIST_EXTEND:
#endif
#if LE_HAS_OPCODE_SET_UPDATE
        case SET_UPDATE:
#endif
#if LE_HAS_OPCODE_BUILD_SET_UNPACK
        case BUILD_SET_UNPACK:
#endif
#if LE_HAS_OPCODE_BUILD_MAP_UNPACK
        case BUILD_MAP_UNPACK:
#endif
#if LE_HAS_OPCODE_BUILD_MAP_UNPACK_WITH_CALL
        case BUILD_MAP_UNPACK_WITH_CALL:
#endif
#if LE_HAS_OPCODE_DICT_UPDATE
        case DICT_UPDATE:
#endif
#if LE_HAS_OPCODE_DICT_MERGE
        case DICT_MERGE:
#endif
        case UNPACK_EX:
        case IMPORT_NAME:
        case IMPORT_FROM:
        case FORMAT_VALUE:
        case PRINT_EXPR:
        case CALL_INTRINSIC_1: /* mergepoints.c:compiled_intrinsic's */
        case LOAD_BUILD_CLASS:
#if LE_HAS_OPCODE_LOAD_ASSERTION_ERROR
        case LOAD_ASSERTION_ERROR:
#endif
        case GET_ITER:
        case END_FOR:
#if LE_HAS_OPCODE_GET_LEN
        case GET_LEN:
#endif
#if LE_HAS_OPCODE_MATCH_CLASS
        case MATCH_CLASS:
#endif
#if LE_HAS_OPCODE_MATCH_MAPPING
        case MATCH_MAPPING:
#endif
#if LE_HAS_OPCODE_MATCH_SEQUENCE
        case MATCH_SEQUENCE:
#endif
#if LE_HAS_OPCODE_MATCH_KEYS
        case MATCH_KEYS:
#endif
#if LE_HAS_OPCODE_COPY_DICT_WITHOUT_KEYS
        case COPY_DICT_WITHOUT_KEYS:
#endif
            return LE_OPF_COMPILED;
        case FOR_ITER:
            return LE_OPF_COMPILED | LE_OPF_BRANCHES;
        /* the truth a jump tests, as the jump found it itself before */
        case TO_BOOL:
            return LE_OPF_COMPILED | LE_OPF_LIGHT;
        /* jumps and return */
        case JUMP_FORWARD:
        case JUMP_ABSOLUTE:
        case JUMP_BACKWARD:
            return LE_OPF_COMPILED | LE_OPF_LIGHT | LE_OPF_JUMPS;
        case JUMP_BACKWARD_NO_INTERRUPT:
            return LE_OPF_COMPILED | LE_OPF_JUMPS;
        case POP_JUMP_IF_FALSE:
        case POP_JUMP_IF_TRUE:
        case JUMP_IF_FALSE_OR_POP:
        case JUMP_IF_TRUE_OR_POP:
            return LE_OPF_COMPILED | LE_OPF_LIGHT | LE_OPF_BRANCHES;
        case POP_JUMP_IF_NONE:
        case POP_JUMP_IF_NOT_NONE:
            return LE_OPF_COMPILED | LE_OPF_BRANCHES;
        case RETURN_VALUE:
        case RETURN_CONST:
            return LE_OPF_COMPILED | LE_OPF_LIGHT | LE_OPF_STOPS;
        /* globals */
        case LOAD_GLOBAL:
        case STORE_GLOBAL:
        case DELETE_GLOBAL:
            return LE_OPF_COMPILED;
        /* calls and functions */
        case CALL_FUNCTION:
        case CALL_FUNCTION_KW:
        case CALL_FUNCTION_EX:
        case CALL_METHOD:
        case CALL:
            return LE_OPF_COMPILED | LE_OPF_CALL;
        case PRECALL:
        case KW_NAMES:
            return LE_OPF_COMPILED | LE_OPF_LIGHT;
        case LOAD_METHOD:
        case MAKE_FUNCTION:
            return LE_OPF_COMPILED;
        /* cells */
        case MAKE_CELL:
        case COPY_FREE_VARS:
            return LE_OPF_COMPILED | LE_OPF_LIGHT;
        case LOAD_CLOSURE:
        case LOAD_DEREF:
        case STORE_DEREF:
        case DELETE_DEREF:
        /* generator expressions (psyco_is_genexpr) */
        case RETURN_GENERATOR:
        case YIELD_VALUE:
            return LE_OPF_COMPILED;
        /* exceptions, and the blocks that handle them, where handlers are
           in blocks */
        case SETUP_LOOP:
        case SETUP_EXCEPT:
            return LE_OPF_COMPILED_IN_BLOCKS | LE_OPF_BRANCHES;
        case SETUP_FINALLY:
        case SETUP_WITH:
            return LE_OPF_COMPILED_IN_BLOCKS | LE_OPF_BRANCHES |
                   LE_OPF_BREAKER_SKIPS;
        case CALL_FINALLY:
            return LE_OPF_COMPILED_IN_BLOCKS | LE_OPF_BRANCHES;
        case CONTINUE_LOOP:
            return LE_OPF_COMPILED_IN_BLOCKS | LE_OPF_JUMPS;
        case BREAK_LOOP:
            return LE_OPF_COMPILED_IN_BLOCKS | LE_OPF_STOPS;
#if LE_HAS_OPCODE_JUMP_IF_NOT_EXC_MATCH
        case JUMP_IF_NOT_EXC_MATCH:
            return LE_OPF_COMPILED_IN_BLOCKS | LE_OPF_BRANCHES;
#endif
        case POP_BLOCK:
        case END_FINALLY:
        case BEGIN_FINALLY:
        case POP_FINALLY:
        case WITH_CLEANUP_START:
        case WITH_CLEANUP_FINISH:
        /* generators */
        case GEN_START:
        /* in a plain function too: an async generator expression's
           outermost iterable */
        case GET_AITER:
            return LE_OPF_COMPILED_IN_BLOCKS;
        case BEFORE_ASYNC_WITH:
        case YIELD_FROM:
            return LE_OPF_BREAKER_SKIPS;
    }
    return 0;
}

/* the rich comparison COMPARE_OP with argument arg makes */
#if LE_HAS_COMPARE_OP_SHIFT_5
#define LE_COMPARE_OP(arg) ((arg) >> 5)
#elif LE_HAS_COMPARE_OP_SHIFT_4
#define LE_COMPARE_OP(arg) ((arg) >> 4)
#else
#define LE_COMPARE_OP(arg) (arg)
#endif

/* the index in co_names of the name LOAD_GLOBAL with argument arg loads,
   and whether it pushes a NULL under the global */
#if LE_HAS_LOAD_GLOBAL_NULL_BIT
#define LE_GLOBAL_NAME(arg) ((arg) >> 1)
#define LE_GLOBAL_PUSHES_NULL(arg) ((arg) & 1)
#else
#define LE_GLOBAL_NAME(arg) (arg)
#define LE_GLOBAL_PUSHES_NULL(arg) 0
#endif

/* the place CPython's frame gives the instruction that starts at pos (its
   lasti, a traceback's tb_lasti): past its EXTENDED_ARG prefixes, or the
   first of them */
static inline int
le_place(le_code_t c, int pos)
{
#if LE_HAS_LASTI_PAST_PREFIXES
    while (pos + LE_UNIT < c.size && le_op(c, pos) == EXTENDED_ARG)
        pos += LE_UNIT;
#endif
    return pos;
}

#if LE_HAS_EXCEPTION_TABLE
/* one entry of a code object's exception table: an exception raised by an
   instruction from start up to end goes to handler, with the value stack
   cut to depth items, then the failing instruction's place pushed if lasti
   is set, then the exception. Positions in bytes. */
typedef struct {
    int start, end, handler, depth, lasti;
} le_handler_t;

/* ceval.c:parse_varint */
static inline const unsigned char *
le_varint(const unsigned char *p, int *result)
{
    int val = p[0] & 63;
    while (p[0] & 64) {
        p++;
        val = (val << 6) | (p[0] & 63);
    }
    *result = val;
    return p + 1;
}

/* the entry at *at (0 for the first) into h, and *at moved past it; false
   after the last */
static inline int
le_handler_next(PyCodeObject *co, Py_ssize_t *at, le_handler_t *h)
{
    const unsigned char *t =
        (const unsigned char *)PyBytes_AS_STRING(co->co_exceptiontable);
    const unsigned char *p = t + *at;
    int size, depth_lasti;
    if (*at >= PyBytes_GET_SIZE(co->co_exceptiontable))
        return 0;
    p = le_varint(p, &h->start);
    p = le_varint(p, &size);
    p = le_varint(p, &h->handler);
    p = le_varint(p, &depth_lasti);
    h->end = (h->start + size) * LE_UNIT;
    h->start *= LE_UNIT;
    h->handler *= LE_UNIT;
    h->depth = depth_lasti >> 1;
    h->lasti = depth_lasti & 1;
    *at = p - t;
    return 1;
}

/* the entry for an exception raised by the instruction at pos, as
   ceval.c:get_exception_handler finds it; false if none */
static inline int
le_handler_find(PyCodeObject *co, int pos, le_handler_t *h)
{
    Py_ssize_t at = 0;
    while (le_handler_next(co, &at, h)) {
        if (h->start > pos)
            return 0;
        if (pos < h->end)
            return 1;
    }
    return 0;
}
#endif

#endif /* _BYTECODE_H */
