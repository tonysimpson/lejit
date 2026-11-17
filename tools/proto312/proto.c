/* Feasibility prototype: entering "compiled" code on 3.12 through
   PyFunction_SetVectorcall, with no interpreter frame. The "compiled body"
   of a 2-argument function is PyNumber_Add(a, b); anything else falls back
   to _PyFunction_Vectorcall. */
#define Py_BUILD_CORE 1
#include <Python.h>
#include "frameobject.h"
#include "opcode.h"
#include "internal/pycore_frame.h"

static long hits, fallbacks;

static PyObject *traceback_frame(PyFunctionObject *f) {
  /* as Ni's cimpl_rt_traceback: a frame made only for the traceback */
  PyThreadState *ts = PyThreadState_Get();
  PyCodeObject *co = (PyCodeObject *)f->func_code;
  PyFrameObject *fr = PyFrame_New(ts, co, f->func_globals, NULL);
  if (fr == NULL)
    return NULL;
  /* point the frame at the instruction that failed (here: the first
     BINARY_OP), so the traceback gets its line */
  /* the code object's own bytecode is rewritten by the specialiser
     (BINARY_OP -> BINARY_OP_ADD_INT); PyCode_GetCode gives it unspecialised,
     at the same offsets */
  PyObject *bytes = PyCode_GetCode(co);
  if (bytes == NULL) { Py_DECREF(fr); return NULL; }
  const unsigned char *b = (const unsigned char *)PyBytes_AS_STRING(bytes);
  Py_ssize_t n = PyBytes_GET_SIZE(bytes) / 2, i;
  for (i = 0; i < n; i++)
    if (b[2 * i] == BINARY_OP)
      break;
  Py_DECREF(bytes);
  fr->f_frame->prev_instr = _PyCode_CODE(co) + i;
  PyTraceBack_Here(fr);
  Py_DECREF(fr);
  return NULL;
}

static PyObject *compiled(PyObject *callable, PyObject *const *args,
                          size_t nargsf, PyObject *kwnames) {
  Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
  PyFunctionObject *f = (PyFunctionObject *)callable;
  if (nargs != 2 || kwnames != NULL) {
    fallbacks++;
    return _PyFunction_Vectorcall(callable, args, nargsf, kwnames);
  }
  hits++;
  PyObject *r = PyNumber_Add(args[0], args[1]);
  if (r == NULL)
    return traceback_frame(f);
  return r;
}

/* __code__, defaults etc. changed: forget the compiled code */
static int watcher(PyFunction_WatchEvent ev, PyFunctionObject *f,
                   PyObject *new_value) {
  if (ev != PyFunction_EVENT_CREATE && f->vectorcall == compiled)
    f->vectorcall = _PyFunction_Vectorcall;
  return 0;
}
static int watcher_id = -1;

static PyObject *jit(PyObject *self, PyObject *f) {
  if (watcher_id < 0 && (watcher_id = PyFunction_AddWatcher(watcher)) < 0)
    return NULL;
  if (!PyFunction_Check(f))
    return PyErr_Format(PyExc_TypeError, "not a function");
  PyFunction_SetVectorcall((PyFunctionObject *)f, compiled);
  Py_RETURN_NONE;
}

static PyObject *counts(PyObject *self, PyObject *unused) {
  PyObject *r = Py_BuildValue("ll", hits, fallbacks);
  hits = fallbacks = 0;
  return r;
}

static PyMethodDef methods[] = {
    {"jit", jit, METH_O, NULL}, {"counts", counts, METH_NOARGS, NULL}, {0}};
static struct PyModuleDef mod = {PyModuleDef_HEAD_INIT, "proto", NULL, -1,
                                 methods};
PyMODINIT_FUNC PyInit_proto(void) { return PyModule_Create(&mod); }
