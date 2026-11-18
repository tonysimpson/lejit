/* eval_breaker in each thread state, a uintptr_t whose low bits are
   events and whose high bits are the instrumentation version: compiled
   code would load the thread state and test the event bits. */
#error "Ni: the eval_breaker test on the thread state's word is not written yet (ni/mechanisms/eval_breaker/tstate_word.h)"
