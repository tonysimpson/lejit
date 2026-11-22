#ifndef LE_MECHANISMS_EXC_H
#define LE_MECHANISMS_EXC_H
#include <Python.h>
/* the LE_HAS_ features of the CPython built for */
#include "le_features_gen.h"
#include <stddef.h>

/* The exception being raised is the thread state's curexc_type,
   curexc_value and curexc_traceback; the one being handled
   (sys.exc_info()) is the type, value and traceback of the top entry of
   tstate->exc_info, a stack linked by previous_item. LeJIT reaches them
   only through this.

   Compiled code's PyErr_Occurred() tests one pointer at
   LE_EXC_OCCURRED_OFFSET in the thread state (LE_TSTATE_exc_occurred as a
   field). The C helpers' get functions give borrowed references, NULL
   for none; the put functions take the references given and don't
   release the old ones, which the caller has taken with a get. */
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyThreadState *)0)->curexc_type), PyObject *),
               "LeJIT: PyErr_Occurred() is tested on curexc_type, a pointer");
#define LE_EXC_OCCURRED_OFFSET offsetof(PyThreadState, curexc_type)
#define LE_TSTATE_exc_occurred \
    FMUT(DEF_FIELD(PyThreadState, PyObject *, curexc_type, NO_PREV_FIELD))

static inline void
le_exc_get_current(PyThreadState *ts, PyObject **type, PyObject **value,
                   PyObject **tb)
{
    *type = ts->curexc_type;
    *value = ts->curexc_value;
    *tb = ts->curexc_traceback;
}
static inline void
le_exc_put_current(PyThreadState *ts, PyObject *type, PyObject *value,
                   PyObject *tb)
{
    ts->curexc_type = type;
    ts->curexc_value = value;
    ts->curexc_traceback = tb;
}
static inline PyObject *
le_exc_traceback(PyThreadState *ts)
{
    return ts->curexc_traceback;
}
static inline void
le_exc_put_traceback(PyThreadState *ts, PyObject *tb)
{
    ts->curexc_traceback = tb;
}

#if LE_HAS_EXC_INFO_VALUE_ONLY
/* An entry of the handled-exception stack holds only the exception's value
   (None or NULL for none); its type and traceback are the value's. A get
   gives the entry's value and new references to that type and traceback,
   so that the caller, which releases the old ones it took with a get, holds
   one of each as with three fields; a put keeps the value and releases the
   type and traceback given. */
static inline void
le_exc_info_parts(PyObject *value, PyObject **type, PyObject **tb)
{
    if (value == NULL || value == Py_None) {
        *type = *tb = value;
    }
    else {
        *type = (PyObject *)Py_TYPE(value);
        *tb = ((PyBaseExceptionObject *)value)->traceback;
    }
}
static inline void
le_exc_info_get(PyThreadState *ts, PyObject **type, PyObject **value,
                PyObject **tb)
{
    *value = ts->exc_info->exc_value;
    le_exc_info_parts(*value, type, tb);
    Py_XINCREF(*type);
    Py_XINCREF(*tb);
}
static inline void
le_exc_info_put(PyThreadState *ts, PyObject *type, PyObject *value,
                PyObject *tb)
{
    ts->exc_info->exc_value = value;
    Py_XDECREF(type);
    Py_XDECREF(tb);
}
/* the exception sys.exc_info() gives: the topmost entry with one, else the
   bottom entry; borrowed */
static inline void
le_exc_info_topmost(PyThreadState *ts, PyObject **type, PyObject **value,
                    PyObject **tb)
{
    _PyErr_StackItem *exc_info = ts->exc_info;
    while ((exc_info->exc_value == NULL || exc_info->exc_value == Py_None) &&
           exc_info->previous_item != NULL)
        exc_info = exc_info->previous_item;
    *value = exc_info->exc_value;
    le_exc_info_parts(*value, type, tb);
}
#else
/* the entry on top of the handled-exception stack */
static inline void
le_exc_info_get(PyThreadState *ts, PyObject **type, PyObject **value,
                PyObject **tb)
{
    _PyErr_StackItem *exc_info = ts->exc_info;
    *type = exc_info->exc_type;
    *value = exc_info->exc_value;
    *tb = exc_info->exc_traceback;
}
static inline void
le_exc_info_put(PyThreadState *ts, PyObject *type, PyObject *value,
                PyObject *tb)
{
    _PyErr_StackItem *exc_info = ts->exc_info;
    exc_info->exc_type = type;
    exc_info->exc_value = value;
    exc_info->exc_traceback = tb;
}
/* the exception sys.exc_info() gives: the topmost entry with one, else the
   bottom entry (_PyErr_GetTopmostException, not exported in 3.7) */
static inline void
le_exc_info_topmost(PyThreadState *ts, PyObject **type, PyObject **value,
                    PyObject **tb)
{
    _PyErr_StackItem *exc_info = ts->exc_info;
    while ((exc_info->exc_type == NULL || exc_info->exc_type == Py_None) &&
           exc_info->previous_item != NULL)
        exc_info = exc_info->previous_item;
    *type = exc_info->exc_type;
    *value = exc_info->exc_value;
    *tb = exc_info->exc_traceback;
}
#endif
#endif /* LE_MECHANISMS_EXC_H */
