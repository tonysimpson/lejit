#ifndef LE_MECHANISMS_RECURSION_H
#define LE_MECHANISMS_RECURSION_H
#if LE_HAS_PY_RECURSION_REMAINING
/* A thread counts down the Python calls it may still make, in
   py_recursion_remaining, apart from the C calls (c_recursion_remaining,
   or where it has none, Py_EnterRecursiveCall's test of the stack
   pointer);
   below 0 its check, the hidden _Py_CheckRecursiveCallPy, raises
   RecursionError, unless the thread is already raising one (its
   recursion_headroom). sys.setrecursionlimit sets the count for each
   thread. Included by psyfunc.c, eval_hook.c, frames.c and pycompiler.c
   after codegen.h and dispatcher.h.

   At run time: le_recursion_enter and le_recursion_leave count a frame
   entered from C on both counts, as ceval's entry does;
   le_recursion_suspend and le_recursion_resume give a C helper one more
   Python call that can't raise RecursionError.

   In compiled code: _Py_EnterRecursivePy and _Py_LeaveRecursiveCallPy
   (Python/ceval.c) around a direct compiled call, on the thread state
   'vts'. The count is changed in place; only a count gone below 0 calls
   the check (cimpl_check_recursive_call, a copy that also takes the
   count back when it raises, as ceval does then), in a
   runtime_condition_f branch compiled when first taken. */
#elif LE_HAS_RECURSION_REMAINING
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
#if LE_HAS_PY_RECURSION_REMAINING
static int
cimpl_check_recursive_call(void);
/* ceval's entry takes two from the C count where there is one (its
   PY_EVAL_C_STACK_UNITS), the second by Py_EnterRecursiveCall's test, and
   one from the Python count, as _Py_EnterRecursivePy */
#if LE_HAS_C_RECURSION_REMAINING
#define LE_C_RECURSION_TAKE(ts) ((ts)->c_recursion_remaining--)
#define LE_C_RECURSION_GIVE(ts) ((ts)->c_recursion_remaining++)
#else
#define LE_C_RECURSION_TAKE(ts) ((void)(ts))
#define LE_C_RECURSION_GIVE(ts) ((void)(ts))
#endif
/* Py_EnterRecursiveCall's test and Py_LeaveRecursiveCall's count, in
   place where they would neither raise nor call the check: while the C
   count is above 0, or the stack pointer is above the thread's soft limit
   (below it CPython's own test decides) */
#if LE_HAS_C_RECURSION_REMAINING
#define LE_C_RECURSION_ENTER(ts)                                          \
    ((ts)->c_recursion_remaining > 0 ? ((ts)->c_recursion_remaining--, 0) \
                                     : Py_EnterRecursiveCall(""))
#define LE_C_RECURSION_LEAVE(ts) ((ts)->c_recursion_remaining++)
#elif LE_HAS_C_STACK_SOFT_LIMIT
/* pyruntime.c: the offset of _PyThreadStateImpl's c_stack_soft_limit */
extern const long le_c_stack_soft_limit_at;
#define LE_C_RECURSION_ENTER(ts)                                    \
    ((uintptr_t)__builtin_frame_address(0) <                        \
         *(uintptr_t *)((char *)(ts) + le_c_stack_soft_limit_at) && \
     Py_EnterRecursiveCall(""))
#define LE_C_RECURSION_LEAVE(ts) ((void)(ts))
#else
#define LE_C_RECURSION_ENTER(ts) Py_EnterRecursiveCall("")
#define LE_C_RECURSION_LEAVE(ts) Py_LeaveRecursiveCall()
#endif
static inline int
le_recursion_enter(PyThreadState *ts)
{
    LE_C_RECURSION_TAKE(ts);
    if (LE_C_RECURSION_ENTER(ts)) {
        LE_C_RECURSION_GIVE(ts);
        return -1;
    }
    if (--ts->py_recursion_remaining < 0 && cimpl_check_recursive_call()) {
        LE_C_RECURSION_LEAVE(ts);
        LE_C_RECURSION_GIVE(ts);
        return -1;
    }
    return 0;
}
static inline void
le_recursion_leave(PyThreadState *ts)
{
    ts->py_recursion_remaining++;
    LE_C_RECURSION_LEAVE(ts);
    LE_C_RECURSION_GIVE(ts);
}
#elif LE_HAS_INTERP_RECURSION_LIMIT || LE_HAS_RECURSION_REMAINING
/* Py_EnterRecursiveCall and Py_LeaveRecursiveCall in place, as compiled
   code counts (enter_recursive_call): only an overflow calls C */
#if !LE_HAS_RECURSION_REMAINING
/* le_recursion_limit_address, kept by eval_hook.c:le_entry_install */
#define LE_KEEPS_RECURSION_LIMIT 1
extern int *le_recursion_limit;
#endif
static int
cimpl_check_recursive_call(void);
static inline int
le_recursion_enter(PyThreadState *ts)
{
#if LE_HAS_RECURSION_REMAINING
    return --ts->recursion_remaining < 0 && cimpl_check_recursive_call();
#else
    return ++ts->recursion_depth > *le_recursion_limit &&
           cimpl_check_recursive_call();
#endif
}
static inline void
le_recursion_leave(PyThreadState *ts)
{
#if LE_HAS_RECURSION_REMAINING
    ts->recursion_remaining++;
#else
    ts->recursion_depth--;
#endif
}
#else
static inline int
le_recursion_enter(PyThreadState *ts)
{
    (void)ts;
    return Py_EnterRecursiveCall("");
}
static inline void
le_recursion_leave(PyThreadState *ts)
{
    (void)ts;
    Py_LeaveRecursiveCall();
}
#endif
#ifndef LE_KEEPS_RECURSION_LIMIT
#define LE_KEEPS_RECURSION_LIMIT 0
#endif
/* the count compiled code changes, and by how much for one call */
#if LE_HAS_PY_RECURSION_REMAINING
#define LE_RECURSION_COUNT py_recursion_remaining
#define LE_RECURSION_STEP (-1)
#elif LE_HAS_RECURSION_REMAINING
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

#if LE_HAS_PY_RECURSION_REMAINING
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyThreadState *)0)->py_recursion_remaining),
                   int),
               "LeJIT: py_recursion_remaining is counted as an int");
#elif LE_HAS_RECURSION_REMAINING
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
#if LE_HAS_PY_RECURSION_REMAINING
/* _Py_CheckRecursiveCallPy (3.12 Python/ceval.c), on the count already
   taken, which it gives back when it raises */
static int
cimpl_check_recursive_call(void)
{
    PyThreadState *ts = PyThreadState_GET();
    if (ts->recursion_headroom) {
        if (ts->py_recursion_remaining < -50)
            Py_FatalError("Cannot recover from Python stack overflow.");
    }
    else if (ts->py_recursion_remaining <= 0) {
        ts->recursion_headroom++;
        PyErr_SetString(PyExc_RecursionError, LE_MSG_RECURSION_DEPTH);
        ts->recursion_headroom--;
        ts->py_recursion_remaining++;
        return -1;
    }
    return 0;
}
#elif LE_HAS_INTERP_RECURSION_LIMIT || LE_HAS_RECURSION_REMAINING
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

/* adds 'delta' to the count 'df' of thread state 'vts': the new count */
static inline vinfo_t *
add_to_count(PsycoObject *po, vinfo_t *vts, defield_t df, long delta)
{
    vinfo_t *vdepth, *vnew;
    vdepth = psyco_get_field(po, vts, df);
    if (vdepth == NULL)
        return NULL;
    vnew = integer_add_i(po, vdepth, delta, true);
    vinfo_decref(vdepth, po);
    if (vnew == NULL)
        return NULL;
    if (psyco_put_field(po, vts, df, vnew))
        return vnew;
    vinfo_decref(vnew, po);
    return NULL;
}

/* adds 'delta' to the Python count of thread state 'vts': the new count */
static inline vinfo_t *
add_to_depth(PsycoObject *po, long delta, vinfo_t *vts)
{
    return add_to_count(po, vts, TSTATE_depth, delta);
}

/* the check of a count taken for two levels at once
   (enter_recursive_levels): the second is given back too when it raises */
static int
cimpl_check_two_levels(void)
{
#if LE_HAS_INTERP_RECURSION_LIMIT || LE_HAS_RECURSION_REMAINING || \
    LE_HAS_PY_RECURSION_REMAINING
    if (cimpl_check_recursive_call() == 0)
#else
    if (_Py_CheckRecursiveCall("") == 0)
#endif
        return 0;
    le_recursion_suspend();
    return -1;
}

/* 'levels' (1 or 2) calls entered at once, the limit tested on the
   innermost, whose count's rare path calls 'check' (for two levels, one
   that gives the second back when it raises) */
static inline bool
enter_recursive_levels_checked_by(PsycoObject *po, vinfo_t *vts, int levels,
                                  void *check)
{
    condition_code_t cc = CC_ERROR;
    vinfo_t *vdepth = add_to_depth(po, levels * LE_RECURSION_STEP, vts);
    if (vdepth == NULL)
        return false;
#if LE_HAS_RECURSION_REMAINING || LE_HAS_PY_RECURSION_REMAINING
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
    if (!runtime_condition_f(po, cc))
        return true;
#if LE_HAS_INTERP_RECURSION_LIMIT || LE_HAS_RECURSION_REMAINING || \
    LE_HAS_PY_RECURSION_REMAINING
    return psyco_generic_call(po, check, CfCommonIntZeroOk, "") != NULL;
#else
    if (levels == 2)
        return psyco_generic_call(po, check, CfCommonIntZeroOk, "") != NULL;
    return psyco_generic_call(po, check, CfCommonIntZeroOk, "l", (long)"") !=
           NULL;
#endif
}

/* false with an exception set in 'po' */
static inline bool
enter_recursive_levels(PsycoObject *po, vinfo_t *vts, int levels)
{
    if (levels == 2)
        return enter_recursive_levels_checked_by(po, vts, 2,
                                                 cimpl_check_two_levels);
#if LE_HAS_INTERP_RECURSION_LIMIT || LE_HAS_RECURSION_REMAINING || \
    LE_HAS_PY_RECURSION_REMAINING
    return enter_recursive_levels_checked_by(po, vts, 1,
                                             cimpl_check_recursive_call);
#else
    return enter_recursive_levels_checked_by(po, vts, 1,
                                             _Py_CheckRecursiveCall);
#endif
}

/* false with an exception set in 'po' */
static inline bool
enter_recursive_call(PsycoObject *po, vinfo_t *vts)
{
    return enter_recursive_levels(po, vts, 1);
}

#if LE_HAS_RECURSION_REMAINING || LE_HAS_PY_RECURSION_REMAINING
/* enter_recursive_call where an overflow is not raised here but left to
   another path: 1 if counted, 0 if it would overflow (counted back, the
   count's rare path taken), -1 with an exception set in 'po' */
static inline int
enter_recursive_call_or_not(PsycoObject *po, vinfo_t *vts)
{
    condition_code_t cc;
    vinfo_t *vdepth = add_to_depth(po, LE_RECURSION_STEP, vts);
    if (vdepth == NULL)
        return -1;
    cc = integer_cmp_i(po, vdepth, 0, Py_LT);
    vinfo_decref(vdepth, po);
    if (cc == CC_ERROR)
        return -1;
    if (runtime_condition_f(po, cc)) {
        vdepth = add_to_depth(po, -LE_RECURSION_STEP, vts);
        if (vdepth == NULL)
            return -1;
        vinfo_decref(vdepth, po);
        return 0;
    }
    return 1;
}
#endif

#if LE_HAS_C_RECURSION_REMAINING || LE_HAS_C_STACK_SOFT_LIMIT
/* Where compiled code runs a frame that CPython runs from C (a generator
   consumed by sum() or stepped by next()), it also takes ceval's entry's
   C check, which a direct call (Python to Python, no C stack in CPython)
   doesn't. Inline: the C count less the units ceval's entry takes, below
   0, or the stack pointer at or below the thread's soft limit, tests
   whether the check could fail; only then is it made in C, by
   le_recursion_enter's own C part. */
#define LE_C_RECURSION_CHECKED 1
#if LE_HAS_C_RECURSION_REMAINING
#define LE_C_RECURSION_UNITS 2
#define TSTATE_c_count \
    FMUT(DEF_FIELD(PyThreadState, int, c_recursion_remaining, NO_PREV_FIELD))
#endif

/* the C part of le_recursion_enter, after compiled code took the units */
static int
cimpl_c_recursion_enter(void)
{
    PyThreadState *ts = PyThreadState_GET();
#if LE_HAS_C_RECURSION_REMAINING
    ts->c_recursion_remaining += LE_C_RECURSION_UNITS;
#endif
    LE_C_RECURSION_TAKE(ts);
    if (LE_C_RECURSION_ENTER(ts)) {
        LE_C_RECURSION_GIVE(ts);
        return -1;
    }
    return 0;
}

/* cimpl_check_recursive_call after the C part, which it gives back when it
   raises, as le_recursion_enter */
static int
cimpl_check_recursive_call_after_c(void)
{
    PyThreadState *ts;
    if (!cimpl_check_recursive_call())
        return 0;
    ts = PyThreadState_GET();
    LE_C_RECURSION_LEAVE(ts);
    LE_C_RECURSION_GIVE(ts);
    return -1;
}

/* takes the C units of thread state 'vts' (inline); the condition under
   which the C check could fail */
static inline condition_code_t
c_recursion_low(PsycoObject *po, vinfo_t *vts)
{
    condition_code_t cc;
#if LE_HAS_C_RECURSION_REMAINING
    vinfo_t *vcount =
        add_to_count(po, vts, TSTATE_c_count, -LE_C_RECURSION_UNITS);
    if (vcount == NULL)
        return CC_ERROR;
    cc = integer_cmp_i(po, vcount, 0, Py_LT);
    vinfo_decref(vcount, po);
#else
    vinfo_t *vlimit, *vsp;
    vlimit = psyco_get_field_offset(po, vts, FMUT(DEF_ARRAY(long, 0)),
                                    le_c_stack_soft_limit_at);
    if (vlimit == NULL)
        return CC_ERROR;
    vsp = psyco_stack_pointer(po);
    if (vsp == NULL) {
        vinfo_decref(vlimit, po);
        return CC_ERROR;
    }
    cc = integer_cmp(po, vsp, vlimit, Py_LE | COMPARE_UNSIGNED);
    vinfo_decref(vsp, po);
    vinfo_decref(vlimit, po);
#endif
    return cc;
}

/* gives back the C units of thread state 'vts' */
static inline bool
c_recursion_give(PsycoObject *po, vinfo_t *vts)
{
#if LE_HAS_C_RECURSION_REMAINING
    vinfo_t *vcount =
        add_to_count(po, vts, TSTATE_c_count, LE_C_RECURSION_UNITS);
    if (vcount == NULL)
        return false;
    vinfo_decref(vcount, po);
#else
    (void)po;
    (void)vts;
#endif
    return true;
}
#else
#define LE_C_RECURSION_CHECKED 0
#endif

#if LE_C_RECURSION_CHECKED
/* cimpl_check_recursive_call_after_c for two levels at once: the second is
   given back too when it raises */
static int
cimpl_check_two_levels_after_c(void)
{
    if (cimpl_check_recursive_call_after_c() == 0)
        return 0;
    le_recursion_suspend();
    return -1;
}
#endif

/* enter_recursive_levels where CPython enters the frame from C */
static inline bool
enter_c_recursive_levels(PsycoObject *po, vinfo_t *vts, int levels)
{
#if LE_C_RECURSION_CHECKED
    condition_code_t cc = c_recursion_low(po, vts);
    if (cc == CC_ERROR)
        return false;
    if (runtime_condition_f(po, cc) &&
        psyco_generic_call(po, cimpl_c_recursion_enter, CfCommonIntZeroOk,
                           "") == NULL)
        return false;
    return enter_recursive_levels_checked_by(
        po, vts, levels,
        levels == 2 ? (void *)cimpl_check_two_levels_after_c
                    : (void *)cimpl_check_recursive_call_after_c);
#else
    return enter_recursive_levels(po, vts, levels);
#endif
}

#if LE_HAS_RECURSION_REMAINING || LE_HAS_PY_RECURSION_REMAINING
/* enter_recursive_call_or_not where CPython enters the frame from C */
static inline int
enter_c_recursive_call_or_not(PsycoObject *po, vinfo_t *vts)
{
#if LE_C_RECURSION_CHECKED
    int r;
    condition_code_t cc = c_recursion_low(po, vts);
    if (cc == CC_ERROR)
        return -1;
    if (runtime_condition_f(po, cc))
        return c_recursion_give(po, vts) ? 0 : -1;
    r = enter_recursive_call_or_not(po, vts);
    if (r == 0 && !c_recursion_give(po, vts))
        return -1;
    return r;
#else
    return enter_recursive_call_or_not(po, vts);
#endif
}
#endif

/* consumes 'vts'; false with an exception set in 'po' */
static inline bool
leave_recursive_call(PsycoObject *po, vinfo_t *vts)
{
#if LE_HAS_INTERP_RECURSION_LIMIT || LE_HAS_RECURSION_REMAINING || \
    LE_HAS_PY_RECURSION_REMAINING
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

/* leave_recursive_call where CPython entered the frame from C; consumes
   'vts' */
static inline bool
leave_c_recursive_call(PsycoObject *po, vinfo_t *vts)
{
#if LE_C_RECURSION_CHECKED
    if (!c_recursion_give(po, vts)) {
        vinfo_decref(vts, po);
        return false;
    }
#endif
    return leave_recursive_call(po, vts);
}
#endif /* LE_MECHANISMS_RECURSION_H */
