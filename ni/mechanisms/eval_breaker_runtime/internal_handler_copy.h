/* eval_breaker in the interpreter's ceval state; ceval handles it in
   _Py_HandlePending, which isn't exported, so Ni would copy it (D77). */
#error "Ni: the eval_breaker handler as a copy of _Py_HandlePending is not written yet (ni/mechanisms/eval_breaker_runtime/internal_handler_copy.h; D77)"
