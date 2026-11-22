#ifndef LE_MECHANISMS_RECURSION_H
#define LE_MECHANISMS_RECURSION_H
#if LE_HAS_RECURSION_REMAINING
/* A thread counts down the Python calls it may still make, in
   recursion_remaining; at 0 it calls _Py_CheckRecursiveCall, which
   compares its depth with the interpreter's recursion_limit (that
   sys.setrecursionlimit changes) and raises RecursionError, or gives the
   thread more calls. Included by psyfunc.c, eval_hook.c, frames.c and
   pycompiler.c after codegen.h and dispatcher.h.

   At run time: le_recursion_enter and le_recursion_leave count a frame
   entered from C (eval_hook.c:LeCode_Run), as ceval counts every frame;
   le_recursion_suspend and le_recursion_resume give a C helper one more
   call that can't raise RecursionError.

   In compiled code: _Py_EnterRecursiveCallTstate and
   _Py_LeaveRecursiveCallTstate (Include/internal/pycore_ceval.h) around a
   direct compiled call, on the thread state 'vts'. The count is changed in
   place; only a count gone below 0 calls _Py_CheckRecursiveCall, in a
   runtime_condition_f branch compiled when first taken. */
#elif LE_HAS_INTERP_RECURSION_LIMIT
/* A thread counts the Python calls it is in up, in recursion_depth, and
   compares the count with the interpreter's recursion_limit (internal:
   its address is Python/pyruntime.c's), which sys.setrecursionlimit
   changes. The overflow flag (overflowed, or
   recursion_headroom) is set only while RecursionError is raised, and only
   _Py_CheckRecursiveCall reads it, so leaving a call only counts it. Included
   by psyfunc.c, eval_hook.c, frames.c and pycompiler.c after codegen.h
   and dispatcher.h.

   At run time: le_recursion_enter and le_recursion_leave count a frame
   entered from C (eval_hook.c:LeCode_Run), as ceval counts every frame;
   le_recursion_suspend and le_recursion_resume give a C helper one more
   call that can't raise RecursionError.

   In compiled code: _Py_EnterRecursiveCall and _Py_LeaveRecursiveCall
   (Include/internal/pycore_ceval.h) around a direct compiled call, on the
   thread state 'vts'. The depth is counted in place; only an overflow
   calls _Py_CheckRecursiveCall, which knows the overflow flag and raises
   RecursionError, in a runtime_condition_f branch
   compiled when first taken. */
#else
/* A thread counts the Python calls it is in up, in recursion_depth, and
   compares the count with the exported _Py_CheckRecursionLimit, which
   sys.setrecursionlimit changes; overflowed, set by RecursionError, allows
   50 more calls until the depth is below the low-water mark. Included by
   psyfunc.c, eval_hook.c, frames.c and pycompiler.c after codegen.h and
   dispatcher.h.

   At run time: le_recursion_enter and le_recursion_leave count a frame
   entered from C (eval_hook.c:LeCode_Run), as ceval counts every frame;
   le_recursion_suspend and le_recursion_resume give a C helper one more
   call that can't raise RecursionError.

   In compiled code: Py_EnterRecursiveCall and Py_LeaveRecursiveCall
   (Include/ceval.h) around a direct compiled call, on the thread state
   'vts'. The depth is counted in place; only an overflow calls
   _Py_CheckRecursiveCall, which knows recursion_critical and overflowed
   and raises RecursionError, and a leave calls C only while overflowed is
   set. Both rare paths are runtime_condition_f branches, compiled when
   first taken. */
#endif
static inline int
le_recursion_enter(void)
{
    return Py_EnterRecursiveCall("");
}
static inline void
le_recursion_leave(void)
{
    Py_LeaveRecursiveCall();
}
#if LE_HAS_RECURSION_REMAINING
/* the count compiled code changes, and by how much for one call */
#define LE_RECURSION_COUNT recursion_remaining
#define LE_RECURSION_STEP (-1)
#else
#define LE_RECURSION_COUNT recursion_depth
#define LE_RECURSION_STEP 1
#endif
static inline void
le_recursion_suspend(void)
{
    PyThreadState_GET()->LE_RECURSION_COUNT -= LE_RECURSION_STEP;
}
static inline void
le_recursion_resume(void)
{
    PyThreadState_GET()->LE_RECURSION_COUNT += LE_RECURSION_STEP;
}

#if LE_HAS_RECURSION_REMAINING
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyThreadState *)0)->recursion_remaining), int),
               "LeJIT: recursion_remaining is counted as an int");
#elif LE_HAS_INTERP_RECURSION_LIMIT
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyThreadState *)0)->recursion_depth), int),
               "LeJIT: recursion_depth is counted as an int");
#else
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyThreadState *)0)->recursion_depth), int) &&
                   __builtin_types_compatible_p(
                       __typeof__(((PyThreadState *)0)->overflowed), char),
               "LeJIT: recursion_depth is counted as an int and overflowed "
               "tested as a char");
#endif
typedef struct {
    int value;
} int_slot_t;
#define TSTATE_depth \
    FMUT(DEF_FIELD(PyThreadState, int, LE_RECURSION_COUNT, NO_PREV_FIELD))
#define INT_value FMUT(DEF_FIELD(int_slot_t, int, value, NO_PREV_FIELD))
#if LE_HAS_INTERP_RECURSION_LIMIT || LE_HAS_RECURSION_REMAINING
/* Python/pyruntime.c */
extern int *
le_recursion_limit_address(void);
/* exported, declared in an internal header */
PyAPI_FUNC(int)
    _Py_CheckRecursiveCall(PyThreadState *tstate, const char *where);

static int
cimpl_check_recursive_call(void)
{
    return _Py_CheckRecursiveCall(PyThreadState_GET(), "");
}
#else
#define TSTATE_overflowed \
    FMUT(DEF_FIELD(PyThreadState, char, overflowed, NO_PREV_FIELD))

/* Py_LeaveRecursiveCall after the decrement */
static inline void
cimpl_leave_overflowed(void)
{
    PyThreadState *ts = PyThreadState_GET();
    if (ts->recursion_depth <
        _Py_RecursionLimitLowerWaterMark(_Py_CheckRecursionLimit))
        ts->overflowed = 0;
}
#endif

/* adds 'delta' to the count of thread state 'vts': the new count */
static inline vinfo_t *
add_to_depth(PsycoObject *po, long delta, vinfo_t *vts)
{
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
static inline bool
enter_recursive_call(PsycoObject *po, vinfo_t *vts)
{
    condition_code_t cc = CC_ERROR;
    vinfo_t *vdepth = add_to_depth(po, LE_RECURSION_STEP, vts);
    if (vdepth == NULL)
        return false;
#if LE_HAS_RECURSION_REMAINING
    cc = integer_cmp_i(po, vdepth, 0, Py_LT);
#else
    {
#if LE_HAS_INTERP_RECURSION_LIMIT
        vinfo_t *vslot =
            vinfo_new(CompileTime_New((long)le_recursion_limit_address()));
#else
        vinfo_t *vslot =
            vinfo_new(CompileTime_New((long)&_Py_CheckRecursionLimit));
#endif
        vinfo_t *vlimit = psyco_get_field(po, vslot, INT_value);
        vinfo_decref(vslot, po);
        if (vlimit != NULL) {
            cc = integer_cmp(po, vdepth, vlimit, Py_GT);
            vinfo_decref(vlimit, po);
        }
    }
#endif
    vinfo_decref(vdepth, po);
    if (cc == CC_ERROR)
        return false;
    if (runtime_condition_f(po, cc))
#if LE_HAS_INTERP_RECURSION_LIMIT || LE_HAS_RECURSION_REMAINING
        return psyco_generic_call(po, cimpl_check_recursive_call,
                                  CfCommonIntZeroOk, "") != NULL;
#else
        return psyco_generic_call(po, _Py_CheckRecursiveCall,
                                  CfCommonIntZeroOk, "l", (long)"") != NULL;
#endif
    return true;
}

/* consumes 'vts'; false with an exception set in 'po' */
static inline bool
leave_recursive_call(PsycoObject *po, vinfo_t *vts)
{
#if LE_HAS_INTERP_RECURSION_LIMIT || LE_HAS_RECURSION_REMAINING
    vinfo_t *vdepth = add_to_depth(po, -LE_RECURSION_STEP, vts);
    vinfo_decref(vts, po);
    if (vdepth == NULL)
        return false;
    vinfo_decref(vdepth, po);
#else
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
#endif
    return true;
}
#endif /* LE_MECHANISMS_RECURSION_H */
