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

/* where a jump instruction goes */
static inline int
le_jump_target(le_instr_t in)
{
    if (LE_IS_JABS(in.op))
        return LE_JUMP_BYTES(in.arg);
    return in.next + (LE_IS_JBACK(in.op) ? -1 : 1) * LE_JUMP_BYTES(in.arg);
}

/* the index in co_names of the name LOAD_GLOBAL with argument arg loads,
   and whether it pushes a NULL under the global */
#if LE_HAS_LOAD_GLOBAL_NULL_BIT
#define LE_GLOBAL_NAME(arg) ((arg) >> 1)
#define LE_GLOBAL_PUSHES_NULL(arg) ((arg) & 1)
#else
#define LE_GLOBAL_NAME(arg) (arg)
#define LE_GLOBAL_PUSHES_NULL(arg) 0
#endif

#endif /* _BYTECODE_H */
