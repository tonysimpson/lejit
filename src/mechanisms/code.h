#ifndef LE_MECHANISMS_CODE_H
#define LE_MECHANISMS_CODE_H
/* A code object's variables as LeJIT lays out a frame's: its locals
   (arguments first), then its cells, then its free variables. LeJIT reads
   them from CPython's code object only here. */
#include <Python.h>

static inline int
le_code_nlocals(PyCodeObject *co)
{
    return co->co_nlocals;
}

static inline int
le_code_ncells(PyCodeObject *co)
{
    return (int)PyTuple_GET_SIZE(co->co_cellvars);
}

static inline int
le_code_nfrees(PyCodeObject *co)
{
    return (int)PyTuple_GET_SIZE(co->co_freevars);
}

/* locals, cells and free variables */
static inline int
le_code_nlocalsplus(PyCodeObject *co)
{
    return le_code_nlocals(co) + le_code_ncells(co) + le_code_nfrees(co);
}

/* the argument cell i starts with, or -1 */
static inline int
le_code_cell_arg(PyCodeObject *co, int i)
{
    return co->co_cell2arg != NULL ? (int)co->co_cell2arg[i] : -1;
}

/* names, borrowed */
static inline PyObject *
le_code_local_name(PyCodeObject *co, int i)
{
    return PyTuple_GET_ITEM(co->co_varnames, i);
}

static inline PyObject *
le_code_cell_name(PyCodeObject *co, int i)
{
    return PyTuple_GET_ITEM(co->co_cellvars, i);
}

static inline PyObject *
le_code_free_name(PyCodeObject *co, int i)
{
    return PyTuple_GET_ITEM(co->co_freevars, i);
}
#endif /* LE_MECHANISMS_CODE_H */
