/* The only file built with Py_BUILD_CORE, for what the runtime state
   (_PyRuntime, in the internal headers) holds, which the extension headers
   don't describe. The two fields compiled code reads are generated
   (runtime_layouts_gen.h, policy_table.RUNTIME, D76). The handler below is
   a copy of the top of ceval's loop, a mechanism rather than a layout: its
   reads of the pending-calls and GIL state change with the loop. Not
   #included into ni.c under ALL_STATIC: setup.py always builds it on its
   own. */
#define Py_BUILD_CORE
#include <Python.h>
#include "internal/pystate.h"
#include "frameobject.h"
#include "opcode.h"
#include "runtime_layouts_gen.h"

/* where PyThreadState_GET() reads the current thread state: an
   _Py_atomic_address, which a plain load reads */
void *ni_tstate_current_address(void) { return &NI_RUNTIME_tstate_current; }

/* ceval's eval_breaker (KNOWN_BUGS C24): compiled code compares it with 0
   as a 32-bit int, at loop heads and after calls */
void *ni_eval_breaker_address(void) { return &NI_RUNTIME_eval_breaker; }

/* What ceval's main loop does when eval_breaker is set (3.7
   Python/ceval.c, the top of the for loop). PyEval_SaveThread and
   PyEval_RestoreThread are ceval's GIL drop and take sequence, with its
   exit when another thread is finalising. -1 with an exception set. */
int ni_eval_breaker_handler(void) {
  PyThreadState *tstate = PyThreadState_GET();
  if (_Py_atomic_load_relaxed(&_PyRuntime.ceval.pending.calls_to_do)) {
    if (Py_MakePendingCalls() < 0)
      return -1;
  }
  if (_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil_drop_request))
    PyEval_RestoreThread(PyEval_SaveThread());
  if (tstate->async_exc != NULL) {
    PyObject *exc = tstate->async_exc;
    tstate->async_exc = NULL;
    /* UNSIGNAL_ASYNC_EXC() */
    _PyRuntime.ceval.pending.async_exc = 0;
    _Py_atomic_store_relaxed(
        &_PyRuntime.ceval.eval_breaker,
        _Py_atomic_load_relaxed(&_PyRuntime.ceval.gil_drop_request) |
            _Py_atomic_load_relaxed(&_PyRuntime.ceval.pending.calls_to_do) |
            _PyRuntime.ceval.pending.async_exc);
    PyErr_SetNone(exc);
    Py_DECREF(exc);
    return -1;
  }
  return 0;
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:NiCode_Run). Compiled callers
   test after each call instead. -1 with the exception set and the frame in
   its traceback, as ceval's error path would. */
int ni_eval_breaker_at_entry(PyFrameObject *f) {
  int op;
  if (!_Py_atomic_load_relaxed(&NI_RUNTIME_eval_breaker))
    return 0;
  op = _Py_OPCODE(*(_Py_CODEUNIT *)PyBytes_AS_STRING(f->f_code->co_code));
  if (op == SETUP_FINALLY || op == SETUP_WITH || op == BEFORE_ASYNC_WITH ||
      op == YIELD_FROM)
    return 0;
  if (ni_eval_breaker_handler() < 0) {
    PyTraceBack_Here(f);
    return -1;
  }
  return 0;
}
