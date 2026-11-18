/* A thread counts the Python calls it is in up, in recursion_depth, and
   compares the count with the exported _Py_CheckRecursionLimit, which
   sys.setrecursionlimit changes; overflowed, set by RecursionError, allows
   50 more calls until the depth is below the low-water mark. Included by
   psyfunc.c, eval_hook.c, frames.c and pycompiler.c after codegen.h and
   dispatcher.h.

   At run time: ni_recursion_enter and ni_recursion_leave count a frame
   entered from C (eval_hook.c:NiCode_Run), as ceval counts every frame;
   ni_recursion_suspend and ni_recursion_resume give a C helper one more
   call that can't raise RecursionError.

   In compiled code: Py_EnterRecursiveCall and Py_LeaveRecursiveCall
   (Include/ceval.h) around a direct compiled call, on the thread state
   'vts'. The depth is counted in place; only an overflow calls
   _Py_CheckRecursiveCall, which knows recursion_critical and overflowed
   and raises RecursionError, and a leave calls C only while overflowed is
   set. Both rare paths are runtime_condition_f branches, compiled when
   first taken. */
static inline int ni_recursion_enter(void) { return Py_EnterRecursiveCall(""); }
static inline void ni_recursion_leave(void) { Py_LeaveRecursiveCall(); }
static inline void ni_recursion_suspend(void) {
  PyThreadState_GET()->recursion_depth--;
}
static inline void ni_recursion_resume(void) {
  PyThreadState_GET()->recursion_depth++;
}

_Static_assert(
    __builtin_types_compatible_p(
        __typeof__(((PyThreadState *)0)->recursion_depth), int) &&
        __builtin_types_compatible_p(
            __typeof__(((PyThreadState *)0)->overflowed), char),
    "Ni: recursion_depth is counted as an int and overflowed tested as a char");
typedef struct {
  int value;
} int_slot_t;
#define TSTATE_depth                                                           \
  FMUT(DEF_FIELD(PyThreadState, int, recursion_depth, NO_PREV_FIELD))
#define TSTATE_overflowed                                                      \
  FMUT(DEF_FIELD(PyThreadState, char, overflowed, NO_PREV_FIELD))
#define INT_value FMUT(DEF_FIELD(int_slot_t, int, value, NO_PREV_FIELD))

/* Py_LeaveRecursiveCall after the decrement */
static inline void cimpl_leave_overflowed(void) {
  PyThreadState *ts = PyThreadState_GET();
  if (ts->recursion_depth <
      _Py_RecursionLimitLowerWaterMark(_Py_CheckRecursionLimit))
    ts->overflowed = 0;
}

/* adds 'delta' to the depth of thread state 'vts': the new depth */
static inline vinfo_t *add_to_depth(PsycoObject *po, long delta,
                                    vinfo_t *vts) {
  vinfo_t *vdepth, *vnew;
  vdepth = psyco_get_field(po, vts, TSTATE_depth);
  if (vdepth == NULL)
    return NULL;
  vnew = integer_add_i(po, vdepth, delta, true);
  vinfo_decref(vdepth, po);
  if (vnew == NULL)
    return NULL;
  if (psyco_put_field(po, vts, TSTATE_depth, vnew))
    return vnew;
  vinfo_decref(vnew, po);
  return NULL;
}

/* false with an exception set in 'po' */
static inline bool enter_recursive_call(PsycoObject *po, vinfo_t *vts) {
  vinfo_t *vlimit, *vslot;
  condition_code_t cc = CC_ERROR;
  vinfo_t *vdepth = add_to_depth(po, 1, vts);
  if (vdepth == NULL)
    return false;
  vslot = vinfo_new(CompileTime_New((long)&_Py_CheckRecursionLimit));
  vlimit = psyco_get_field(po, vslot, INT_value);
  vinfo_decref(vslot, po);
  if (vlimit != NULL) {
    cc = integer_cmp(po, vdepth, vlimit, Py_GT);
    vinfo_decref(vlimit, po);
  }
  vinfo_decref(vdepth, po);
  if (cc == CC_ERROR)
    return false;
  if (runtime_condition_f(po, cc))
    return psyco_generic_call(po, _Py_CheckRecursiveCall, CfCommonIntZeroOk,
                              "l", (long)"") != NULL;
  return true;
}

/* consumes 'vts'; false with an exception set in 'po' */
static inline bool leave_recursive_call(PsycoObject *po, vinfo_t *vts) {
  vinfo_t *vover, *vdepth;
  condition_code_t cc;
  vdepth = add_to_depth(po, -1, vts);
  if (vdepth == NULL) {
    vinfo_decref(vts, po);
    return false;
  }
  vinfo_decref(vdepth, po);
  vover = psyco_get_field(po, vts, TSTATE_overflowed);
  vinfo_decref(vts, po);
  cc = integer_NON_NULL(po, vover); /* consumes vover */
  if (cc == CC_ERROR)
    return false;
  if (runtime_condition_f(po, cc))
    return psyco_generic_call(po, cimpl_leave_overflowed, CfNoReturnValue,
                              "") != NULL;
  return true;
}
