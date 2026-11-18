/* ceval's eval_breaker is a 32-bit int at an address fixed for the
   process (D45, D77): compiled code compares it with 0 in place. The
   address and the handler are the eval_breaker_runtime mechanism's
   (Python/pyruntime.c). Included by psyfunc.c after codegen.h. */
extern void *ni_eval_breaker_address(void);
extern int ni_eval_breaker_handler(void);

static inline condition_code_t ni_eval_breaker_test(PsycoObject *po) {
  return psyco_int32_nonzero_at(po, ni_eval_breaker_address());
}
