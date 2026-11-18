/* A thread counts Python calls down in py_recursion_remaining from
   py_recursion_limit, C calls apart (c_recursion_remaining, which
   Py_EnterRecursiveCall counts), with recursion_headroom for the calls
   that handle a RecursionError (D77). */
#error "Ni: the recursion count as py_recursion_remaining counting down is not written yet (ni/mechanisms/recursion/py_remaining_down.h; D77)"
