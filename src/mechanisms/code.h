#ifndef LE_MECHANISMS_CODE_H
#define LE_MECHANISMS_CODE_H
/* A code object's variables as LeJIT lays out a frame's: its locals
   (arguments first), then its cells, then its free variables. LeJIT reads
   them from CPython's code object only here. */
#include <Python.h>
/* the LE_HAS_ features of the CPython built for */
#include "le_features_gen.h"

#if LE_HAS_CODE_LOCALSPLUS
/* CPython names them all in co_localsplusnames, each with a kind in
   co_localspluskinds: its locals first, then its cells that are not also
   arguments, then its free variables; an argument's cell is in the
   argument's place, with both kinds (Include/internal/pycore_code.h). */
#define Py_BUILD_CORE
#include "internal/pycore_code.h"
#undef Py_BUILD_CORE

static inline int
le_code_nlocals(PyCodeObject *co)
{
    return co->co_nlocals;
}

static inline int
le_code_ncells(PyCodeObject *co)
{
    return co->co_ncellvars;
}

static inline int
le_code_nfrees(PyCodeObject *co)
{
    return co->co_nfreevars;
}

/* the place in co_localsplusnames of the i-th variable of kind 'kind' */
static inline int
le_code_kind_index(PyCodeObject *co, int kind, int i)
{
    int k;
    for (k = 0; k < co->co_nlocalsplus; k++)
        if ((_PyLocals_GetKind(co->co_localspluskinds, k) & kind) && i-- == 0)
            return k;
    return -1;
}

/* the argument cell i starts with, or -1 */
static inline int
le_code_cell_arg(PyCodeObject *co, int i)
{
    int k = le_code_kind_index(co, CO_FAST_CELL, i);
    return _PyLocals_GetKind(co->co_localspluskinds, k) & CO_FAST_LOCAL ? k
                                                                        : -1;
}

/* names, borrowed */
static inline PyObject *
le_code_local_name(PyCodeObject *co, int i)
{
    return PyTuple_GET_ITEM(co->co_localsplusnames, i);
}

static inline PyObject *
le_code_cell_name(PyCodeObject *co, int i)
{
    return PyTuple_GET_ITEM(co->co_localsplusnames,
                            le_code_kind_index(co, CO_FAST_CELL, i));
}

static inline PyObject *
le_code_free_name(PyCodeObject *co, int i)
{
    return PyTuple_GET_ITEM(co->co_localsplusnames,
                            co->co_nlocalsplus - co->co_nfreevars + i);
}

/* the frame's slots: all of them, then where cell i and free variable i
   are, and the slot of a *_DEREF or LOAD_CLOSURE argument */
static inline int
le_code_nlocalsplus(PyCodeObject *co)
{
    return co->co_nlocalsplus;
}

static inline int
le_code_cell_slot(PyCodeObject *co, int i)
{
    return le_code_kind_index(co, CO_FAST_CELL, i);
}

static inline int
le_code_free_slot(PyCodeObject *co, int i)
{
    return co->co_nlocalsplus - co->co_nfreevars + i;
}

static inline int
le_code_deref_slot(PyCodeObject *co, int oparg)
{
    (void)co;
    return oparg;
}

/* local i's slot holds a cell (MAKE_CELL put it there) */
static inline int
le_code_local_is_cell(PyCodeObject *co, int i)
{
    return (_PyLocals_GetKind(co->co_localspluskinds, i) & CO_FAST_CELL) != 0;
}
#else
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

/* the frame's slots: locals, cells and free variables; then where cell i
   and free variable i are, and the slot of a *_DEREF or LOAD_CLOSURE
   argument (cells, then free variables) */
static inline int
le_code_nlocalsplus(PyCodeObject *co)
{
    return le_code_nlocals(co) + le_code_ncells(co) + le_code_nfrees(co);
}

static inline int
le_code_cell_slot(PyCodeObject *co, int i)
{
    return le_code_nlocals(co) + i;
}

static inline int
le_code_free_slot(PyCodeObject *co, int i)
{
    return le_code_nlocals(co) + le_code_ncells(co) + i;
}

static inline int
le_code_deref_slot(PyCodeObject *co, int oparg)
{
    return le_code_nlocals(co) + oparg;
}

/* local i's slot holds a cell: never, an argument's cell has its own */
static inline int
le_code_local_is_cell(PyCodeObject *co, int i)
{
    (void)co;
    (void)i;
    return 0;
}
#endif

/* the name of a *_DEREF argument's variable, borrowed */
static inline PyObject *
le_code_deref_name(PyCodeObject *co, int oparg)
{
    int slot = le_code_deref_slot(co, oparg), i;
    for (i = 0; i < le_code_nfrees(co); i++)
        if (le_code_free_slot(co, i) == slot)
            return le_code_free_name(co, i);
    for (i = 0; i < le_code_ncells(co); i++)
        if (le_code_cell_slot(co, i) == slot)
            return le_code_cell_name(co, i);
    return NULL;
}
#endif /* LE_MECHANISMS_CODE_H */
