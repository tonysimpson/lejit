/* The exception being raised is one normalised object,
   tstate->current_exception, its traceback on the object; the one being
   handled is exc_info->exc_value alone (D77). PyErr_Occurred() is still a
   NULL test of one pointer. */
#error "Ni: the exception state as current_exception is not written yet (ni/mechanisms/exceptions/current_exception.h; D77)"
