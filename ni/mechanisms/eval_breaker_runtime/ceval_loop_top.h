/* eval_breaker in _PyRuntime.ceval, and what ceval does when it is set:
   inline at the top of _PyEval_EvalFrameDefault's loop, a signal coming as
   a pending call. Included by Python/pyruntime.c (Py_BUILD_CORE)
   after internal/pystate.h, frameobject.h and opcode.h.
   ni_eval_breaker_handler is a copy of that loop top (3.7 Python/ceval.c):
   PyEval_SaveThread and PyEval_RestoreThread are its GIL drop and take
   sequence, with its exit when another thread is finalising. */
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(_PyRuntime.ceval.eval_breaker), _Py_atomic_int) &&
                   sizeof(_PyRuntime.ceval.eval_breaker) == 4,
               "Ni: _PyRuntime.ceval.eval_breaker is tested as a 32-bit int");

/* compiled code compares it with 0 as a 32-bit int, at loop heads and
   after calls */
void *ni_eval_breaker_address(void) { return &_PyRuntime.ceval.eval_breaker; }

/* -1 with an exception set */
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
   entered through the eval hook (eval_hook.c:NiCode_Run), skipping the
   instructions ceval's loop top skips. Compiled callers test after each
   call instead. -1 with the exception set and the frame in its traceback,
   as ceval's error path would. */
int ni_eval_breaker_at_entry(PyFrameObject *f) {
  int op;
  if (!_Py_atomic_load_relaxed(&_PyRuntime.ceval.eval_breaker))
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
