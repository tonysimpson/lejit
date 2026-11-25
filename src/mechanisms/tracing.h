#ifndef LE_MECHANISMS_TRACING_H
#define LE_MECHANISMS_TRACING_H
/* the thread's interpreter, whose eval_frame le_direct_call_refused reads */
#define TSTATE_interp \
    FMUT(DEF_FIELD(PyThreadState, PyInterpreterState *, interp, NO_PREV_FIELD))
#if LE_HAS_MONITORING
/* Tracing is on for a code object while sys.monitoring has any event on
   in the interpreter (pyruntime.c:le_monitoring_on), or on that code
   object alone (le_code_monitored, set_local_events): sys.settrace and
   sys.setprofile are tools of its, so other tools are tracing too, and
   compiled code, which gives them no events, does not run. le_tracing_now
   makes the interpreter's test in place at run time (the eval hook).
   Compiled code tests it as le_tracing_flag: the main interpreter's
   monitoring version against the last one seen with no tool set
   (pyruntime.c:le_quiet_version), which is -1 while a frame's locals are
   exposed; only where that fails does it make the exact test
   (psyfunc.c:tracing_on). Where it passes, le_code_tools_on tests the code's
   own tools. Included after vcompiler.h and codegen.h. */
extern int
le_monitoring_on(PyThreadState *ts);
extern int
le_code_monitored(PyCodeObject *co);
extern const long le_monitors_tools_at, le_monitors_tools_size;
static inline int
le_tracing_now(PyThreadState *ts, PyCodeObject *co)
{
    const char *t = (const char *)ts->interp + le_monitors_tools_at;
    uint64_t a, b;
    memcpy(&a, t, 8);
    memcpy(&b, t + le_monitors_tools_size - 8, 8);
    return (a | b) != 0 ||
           (co->_co_monitoring != NULL && le_code_monitored(co));
}
extern uint64_t le_quiet_version;
extern const void *
le_monitoring_version_address(void);
extern int
le_tracing_recheck(PyCodeObject *co);

/* nonzero where the tools may have changed since they were last seen
   empty, or a frame's locals are exposed ('vts', 'vinterp' and 'co'
   unused) */
static inline vinfo_t *
le_tracing_flag(PsycoObject *po, vinfo_t *vts, vinfo_t *vinterp,
                PyCodeObject *co)
{
    vinfo_t *vk, *vv, *vq, *vany;
    (void)vts;
    (void)vinterp;
    (void)co;
    vk = vinfo_new(CompileTime_New((long)le_monitoring_version_address()));
    vv = psyco_get_field_offset(po, vk, FMUT(DEF_ARRAY(long, 0)), 0);
    vinfo_decref(vk, po);
    if (vv == NULL)
        return NULL;
    vk = vinfo_new(CompileTime_New((long)&le_quiet_version));
    vq = psyco_get_field_offset(po, vk, FMUT(DEF_ARRAY(long, 0)), 0);
    vinfo_decref(vk, po);
    if (vq == NULL) {
        vinfo_decref(vv, po);
        return NULL;
    }
    vany = integer_xor(po, vv, vq);
    vinfo_decref(vv, po);
    vinfo_decref(vq, po);
    return vany;
}

/* Whether code 'co' has a tool of its own on (le_code_monitored), for
   compiled code once le_tracing_flag has passed: 1, 0, or -1 on an error.
   It tests the code's monitoring data first, which CPython makes the first
   time a tool instruments the code and keeps for the code's life. Only on
   the rare path where that exists, compiled when first taken, are its
   local tools read, from where the data now is, so code that a tool
   watched once goes on without a C call once none watches it. */
static inline int
le_code_tools_on(PsycoObject *po, PyCodeObject *co)
{
    const _PyCoMonitoringData *m;
    const char *tools;
    vinfo_t *vk, *va, *vb, *vany;
    int on;
    /* the compiled code holds its own code; a callee's is held for it */
    if (co != po->pr.co && !is_respawning(po))
        psyco_owner_pin((PyObject *)co);
    vk = vinfo_new(CompileTime_New((long)&co->_co_monitoring));
    va = psyco_get_field_offset(po, vk, FMUT(DEF_ARRAY(long, 0)), 0);
    vinfo_decref(vk, po);
    on = runtime_NON_NULL_f(po, va);
    if (on <= 0)
        return on;
    m = co->_co_monitoring;
    if (m == NULL)
        return 1; /* not reached: the data, once made, is kept */
    tools = (const char *)m->local_monitors.tools;
    vk = vinfo_new(CompileTime_New((long)tools));
    va = psyco_get_field_offset(po, vk, FMUT(DEF_ARRAY(long, 0)), 0);
    vb = psyco_get_field_offset(po, vk, FMUT(DEF_ARRAY(long, 0)),
                                sizeof(m->local_monitors.tools) - 8);
    vinfo_decref(vk, po);
    if (va == NULL || vb == NULL) {
        vinfo_xdecref(va, po);
        vinfo_xdecref(vb, po);
        return -1;
    }
    vany = integer_or(po, va, vb);
    vinfo_decref(va, po);
    vinfo_decref(vb, po);
    return runtime_NON_NULL_f(po, vany);
}

static inline condition_code_t
le_tracing_test(PsycoObject *po, vinfo_t *vts, PyCodeObject *co)
{
    condition_code_t cc;
    vinfo_t *vflag = le_tracing_flag(po, vts, NULL, co);
    if (vflag == NULL)
        return CC_ERROR;
    cc = integer_non_null(po, vflag);
    vinfo_decref(vflag, po);
    return cc;
}
#elif LE_HAS_CFRAME
/* Tracing is on in a thread while the use_tracing of its current CFrame
   (tstate->cframe) is set: by sys.settrace, sys.setprofile and their C
   forms, which set it in every CFrame of the thread. ceval gives each frame
   a CFrame of its own, copied from its caller's and copied back when it
   ends, so tstate->cframe is read again at each test. The flag's width is
   its field's: an int, or a byte that is 0 or 255. le_tracing_now tests it
   at run time (the eval hook); le_tracing_test emits the same test in
   compiled code on the thread state 'vts': a load of the CFrame and a
   compare of an int flag in memory (a byte's is loaded first)
   (psyfunc.c:tracing_on). Included after vcompiler.h and codegen.h. */
static inline int
le_tracing_now(PyThreadState *ts, PyCodeObject *co)
{
    (void)co;
    return ts->cframe->use_tracing != 0;
}

typedef __typeof__(*((PyThreadState *)0)->cframe) le_cframe_t;
#define TSTATE_cframe \
    FMUT(DEF_FIELD(PyThreadState, le_cframe_t *, cframe, NO_PREV_FIELD))
#define CFRAME_use_tracing                                                   \
    FMUT(DEF_FIELD(le_cframe_t, __typeof__(((le_cframe_t *)0)->use_tracing), \
                   use_tracing, NO_PREV_FIELD))

/* the flag itself, for le_direct_call_refused ('vinterp' and 'co' unused
   here) */
static inline vinfo_t *
le_tracing_flag(PsycoObject *po, vinfo_t *vts, vinfo_t *vinterp,
                PyCodeObject *co)
{
    vinfo_t *vflag, *vcframe = psyco_get_field(po, vts, TSTATE_cframe);
    (void)vinterp;
    (void)co;
    if (vcframe == NULL)
        return NULL;
    vflag = psyco_get_field(po, vcframe, CFRAME_use_tracing);
    vinfo_decref(vcframe, po);
    return vflag;
}

static inline condition_code_t
le_tracing_test(PsycoObject *po, vinfo_t *vts, PyCodeObject *co)
{
    condition_code_t cc;
    vinfo_t *vflag, *vcframe = psyco_get_field(po, vts, TSTATE_cframe);
    (void)co;
    if (vcframe == NULL)
        return CC_ERROR;
    if (sizeof(((le_cframe_t *)0)->use_tracing) == 4) {
        cc = psyco_int32_nonzero_in(po, vcframe,
                                    offsetof(le_cframe_t, use_tracing));
        vinfo_decref(vcframe, po);
        return cc;
    }
    vflag = psyco_get_field(po, vcframe, CFRAME_use_tracing);
    vinfo_decref(vcframe, po);
    if (vflag == NULL)
        return CC_ERROR;
    cc = integer_non_null(po, vflag);
    vinfo_decref(vflag, po);
    return cc;
}
#else
/* Tracing is on in a thread while its PyThreadState.use_tracing is set: by
   sys.settrace, sys.setprofile and their C forms PyEval_SetTrace and
   PyEval_SetProfile. le_tracing_now makes the same test in place at run time
   (the eval hook); le_tracing_test emits the same test in compiled code, on
   the thread state 'vts' (psyfunc.c:tracing_on). Included after vcompiler.h
   and codegen.h. */
static inline int
le_tracing_now(PyThreadState *ts, PyCodeObject *co)
{
    (void)co;
    return ts->use_tracing;
}

_Static_assert(sizeof(((PyThreadState *)0)->use_tracing) == 4,
               "LeJIT: PyThreadState.use_tracing is tested as a 32-bit int");
static inline condition_code_t
le_tracing_test(PsycoObject *po, vinfo_t *vts, PyCodeObject *co)
{
    (void)co;
    return psyco_int32_nonzero_in(po, vts,
                                  offsetof(PyThreadState, use_tracing));
}

/* the flag itself, for le_direct_call_refused ('vinterp' and 'co' unused
   here) */
static inline vinfo_t *
le_tracing_flag(PsycoObject *po, vinfo_t *vts, vinfo_t *vinterp,
                PyCodeObject *co)
{
    (void)vinterp;
    (void)co;
    return psyco_get_field(
        po, vts,
        FMUT(DEF_FIELD(PyThreadState, int, use_tracing, NO_PREV_FIELD)));
}
#endif

/* A compiled call or inlined frame skips the eval hook, so it is made only
   while the installed hook is LeJIT's (eval_hook.c:le_eval_hook): under
   another (torch.compile's, a debugger's), or after lejit.stop, the call
   is CPython's, so that hook sees the frame. le_direct_call_refused emits
   that test, a compare of the interpreter's eval_frame with LeJIT's hook,
   then the tracing one for the callee's code 'co', on the thread state
   'vts', each a branch to the real call; true where the real call is
   needed, as on an error. */
extern void *
le_eval_hook(void);
extern const long le_interp_eval_frame_at;

static inline bool
le_direct_call_refused(PsycoObject *po, vinfo_t *vts, PyCodeObject *co)
{
    condition_code_t cc;
    vinfo_t *vhook, *vmine, *vflag,
        *vinterp = psyco_get_field(po, vts, TSTATE_interp);
    if (vinterp == NULL)
        return true;
    vhook = psyco_get_field_offset(po, vinterp, FMUT(DEF_ARRAY(long, 0)),
                                   le_interp_eval_frame_at);
    if (vhook == NULL) {
        vinfo_decref(vinterp, po);
        return true;
    }
    vmine = vinfo_new(CompileTime_New((long)le_eval_hook()));
    cc = integer_cmp(po, vhook, vmine, Py_NE);
    vinfo_decref(vmine, po);
    vinfo_decref(vhook, po);
    if (cc == CC_ERROR || runtime_condition_f(po, cc)) {
        vinfo_decref(vinterp, po);
        return true;
    }
    vflag = le_tracing_flag(po, vts, vinterp, co);
    vinfo_decref(vinterp, po);
    if (vflag == NULL)
        return true;
    cc = integer_non_null(po, vflag);
    vinfo_decref(vflag, po);
    if (cc == CC_ERROR)
        return true;
#if LE_HAS_MONITORING
    if (!runtime_condition_f(po, cc))
        return le_code_tools_on(po, co) != 0;
#else
    if (!runtime_condition_f(po, cc))
        return false;
#endif
#if LE_HAS_MONITORING
    /* the exact test, which also makes the version's pass again once no
       tool is set */
    vflag = psyco_generic_call(po, le_tracing_recheck,
                               CfReturnTypeInt | CfNoBlock | CfNoPython, "l",
                               (long)co);
    if (vflag == NULL)
        return true;
    cc = integer_non_null(po, vflag);
    vinfo_decref(vflag, po);
    return cc == CC_ERROR || runtime_condition_f(po, cc);
#else
    return true;
#endif
}
#endif /* LE_MECHANISMS_TRACING_H */
