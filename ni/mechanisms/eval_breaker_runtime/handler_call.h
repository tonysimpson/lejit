/* eval_breaker in the thread state; ceval's handler _Py_HandlePending is
   exported, so Ni would call it rather than copy it. */
#error "Ni: the eval_breaker handler by a call of _Py_HandlePending is not written yet (ni/mechanisms/eval_breaker_runtime/handler_call.h)"
