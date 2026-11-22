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

static inline le_code_t
le_code(PyCodeObject *co)
{
    le_code_t c;
#if LE_HAS_CODE_GET_CODE
    /* a new reference to bytes the code object keeps until it dies */
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

/* one instruction: its EXTENDED_ARG prefixes folded into arg, next the
   position after it and its caches */
typedef struct {
    int op, arg, next;
} le_instr_t;

/* the instruction that starts at pos, at its first prefix if it has any */
static inline le_instr_t
le_instr(le_code_t c, int pos)
{
    le_instr_t in;
    in.op = le_op(c, pos);
    in.arg = le_arg(c, pos);
    while (in.op == EXTENDED_ARG && pos + LE_UNIT < c.size) {
        pos += LE_UNIT;
        in.op = le_op(c, pos);
        in.arg = in.arg << 8 | le_arg(c, pos);
    }
    in.next = le_after(c, pos);
    return in;
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

/* where a jump instruction goes; FOR_ITER's, when its iterator ends */
static inline int
le_jump_target(le_code_t c, le_instr_t in)
{
    int target;
    if (LE_IS_JABS(in.op))
        return LE_JUMP_BYTES(in.arg);
    target = in.next + (LE_IS_JBACK(in.op) ? -1 : 1) * LE_JUMP_BYTES(in.arg);
    return in.op == FOR_ITER ? le_for_iter_exit(c, target) : target;
}

/* the index in co_names of the name LOAD_ATTR with argument arg reads, and
   whether it pushes the method form: a NULL or the method under the
   attribute, as LOAD_METHOD does */
#if LE_HAS_LOAD_ATTR_METHOD_BIT
#define LE_ATTR_NAME(arg) ((arg) >> 1)
#define LE_ATTR_METHOD(arg) ((arg) & 1)
#else
#define LE_ATTR_NAME(arg) (arg)
#define LE_ATTR_METHOD(arg) 0
#endif

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
