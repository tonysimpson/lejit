/* The running thread's state is the word _PyRuntime.gilstate.tstate_current
   (an _Py_atomic_address, which a plain load reads), at an address fixed
   for the process: what PyThreadState_GET() reads (D77). Included by
   Python/pyruntime.c (Py_BUILD_CORE); compiled code's load is the tstate
   mechanism's. */
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(_PyRuntime.gilstate.tstate_current),
                   _Py_atomic_address) &&
                   sizeof(_PyRuntime.gilstate.tstate_current) == 8,
               "Ni: _PyRuntime.gilstate.tstate_current is loaded as a pointer");

void *ni_tstate_current_address(void) {
  return &_PyRuntime.gilstate.tstate_current;
}
