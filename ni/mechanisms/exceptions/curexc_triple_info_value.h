/* The exception being raised is the curexc triple; the one being handled
   is an exception object alone (_PyErr_StackItem.exc_value), its type and
   traceback read from it (D77). */
#error "Ni: the handled exception as exc_info->exc_value alone is not written yet (ni/mechanisms/exceptions/curexc_triple_info_value.h; D77)"
