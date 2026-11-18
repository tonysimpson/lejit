/* Tracing is on in a thread while its PyThreadState.use_tracing is set: by
   sys.settrace, sys.setprofile and their C forms PyEval_SetTrace and
   PyEval_SetProfile. ni_tracing_now tests it at run time (the
   eval hook); ni_tracing_test emits the same test in compiled code, on the
   thread state 'vts' (psyfunc.c:tracing_on). Included after vcompiler.h and
   codegen.h. */
static inline int ni_tracing_now(PyThreadState *ts) { return ts->use_tracing; }

_Static_assert(sizeof(((PyThreadState *)0)->use_tracing) == 4,
               "Ni: PyThreadState.use_tracing is tested as a 32-bit int");
static inline condition_code_t ni_tracing_test(PsycoObject *po, vinfo_t *vts) {
  return psyco_int32_nonzero_in(po, vts, offsetof(PyThreadState, use_tracing));
}
