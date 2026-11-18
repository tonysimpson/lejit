/* The running thread's state is a C thread-local (_Py_tss_tstate):
   compiled code would read it through the thread pointer (fs) at an offset
   found at start-up, or call PyThreadState_Get (D77). */
#error "Ni: loading the current thread state from a thread-local is not written yet (ni/mechanisms/tstate/thread_local.h; D77)"
