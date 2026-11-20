/* The run-time copies of CPython's code that read the interpreter's
   internal state (a free list or an empty singleton through
   _PyInterpreterState_GET, an internal header's inline function), which
   only a Py_BUILD_CORE unit can compile, with the internal headers their
   files include; the families' headers declare the ones their metas
   call. Built on its own, as pyruntime.c. */
#define Py_BUILD_CORE
#include <Python.h>
#include "ni_features_gen.h"
#if NI_HAS_TUPLE_GET_EMPTY
#include "internal/pycore_abstract.h"
#endif
#if NI_HAS_TUPLE_GET_EMPTY
#include "internal/pycore_gc.h"
#endif
#if NI_HAS_TUPLE_GET_EMPTY
#include "internal/pycore_initconfig.h"
#endif
#if NI_HAS_TUPLE_GET_EMPTY
#include "internal/pycore_object.h"
#endif
#if NI_HAS_TUPLE_GET_EMPTY
struct _Py_tuple_state *
cimpl_get_tuple_state(void)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return &interp->tuple;
}
#endif

#if NI_HAS_TUPLE_GET_EMPTY
PyObject *
cimpl_tuple_get_empty(void)
{
    struct _Py_tuple_state *state = cimpl_get_tuple_state();
    PyTupleObject *op = state->free_list[0];
    assert(op != NULL);
    Py_INCREF(op);
    return (PyObject *)op;
}
#endif
