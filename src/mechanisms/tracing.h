#ifndef LE_MECHANISMS_TRACING_H
#define LE_MECHANISMS_TRACING_H
/* the thread's interpreter, whose eval_frame le_direct_call_refused reads */
#define TSTATE_interp \
    FMUT(DEF_FIELD(PyThreadState, PyInterpreterState *, interp, NO_PREV_FIELD))
#if LE_HAS_MONITORING
/* Tracing is on while sys.monitoring has any event on in the interpreter
   (pyruntime.c:le_monitoring_on): sys.settrace and sys.setprofile are
   tools of its, so other tools are tracing too, and compiled code, which
   gives them no events, does not run. le_tracing_now tests it at run time
   (the eval hook); le_tracing_test emits the same test in compiled code,
   on the thread state 'vts' (psyfunc.c:tracing_on), on le_tracing_flag:
   the interpreter's table of tools per event, a byte each, read as two
   words that overlap, OR-ed. Included after vcompiler.h and codegen.h. */
/* where the interpreter's table of tools is, and its size (8 to 16) */
extern const long le_monitors_tools_at, le_monitors_tools_size;
static inline int
le_tracing_now(PyThreadState *ts)
{
    const char *t = (const char *)ts->interp + le_monitors_tools_at;
    uint64_t a, b;
    memcpy(&a, t, 8);
    memcpy(&b, t + le_monitors_tools_size - 8, 8);
    return (a | b) != 0;
}

#ifdef PyFrameLocalsProxy_Check
/* frames.c:psyco_frames_exposed */
extern int psyco_frames_exposed;
#endif

/* compiled code's test, which is also on while a frame's locals are
   exposed, which a resume test then sees (pycompiler.c:resume_if_tracing) */
static inline vinfo_t *
le_tracing_flag(PsycoObject *po, vinfo_t *vts, vinfo_t *vinterp)
{
    vinfo_t *va, *vb, *vany;
    va = psyco_get_field_offset(po, vinterp, FMUT(DEF_ARRAY(long, 0)),
                                le_monitors_tools_at);
    vb = va == NULL ? NULL
                    : psyco_get_field_offset(
                          po, vinterp, FMUT(DEF_ARRAY(long, 0)),
                          le_monitors_tools_at + le_monitors_tools_size - 8);
    if (vb == NULL) {
        vinfo_xdecref(va, po);
        return NULL;
    }
    vany = integer_or(po, va, vb);
    vinfo_decref(va, po);
    vinfo_decref(vb, po);
    if (vany == NULL)
        return NULL;
#ifdef PyFrameLocalsProxy_Check
    {
        vinfo_t *vk = vinfo_new(CompileTime_New((long)&psyco_frames_exposed));
        vinfo_t *vx =
            psyco_get_field_offset(po, vk, FMUT(DEF_ARRAY(int, 0)), 0);
        vinfo_decref(vk, po);
        if (vx == NULL) {
            vinfo_decref(vany, po);
            return NULL;
        }
        vk = integer_or(po, vany, vx);
        vinfo_decref(vany, po);
        vinfo_decref(vx, po);
        vany = vk;
    }
#endif
    return vany;
}

static inline condition_code_t
le_tracing_test(PsycoObject *po, vinfo_t *vts)
{
    condition_code_t cc;
    vinfo_t *vflag, *vinterp = psyco_get_field(po, vts, TSTATE_interp);
    if (vinterp == NULL)
        return CC_ERROR;
    vflag = le_tracing_flag(po, vts, vinterp);
    vinfo_decref(vinterp, po);
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
le_tracing_now(PyThreadState *ts)
{
    return ts->cframe->use_tracing != 0;
}

typedef __typeof__(*((PyThreadState *)0)->cframe) le_cframe_t;
#define TSTATE_cframe \
    FMUT(DEF_FIELD(PyThreadState, le_cframe_t *, cframe, NO_PREV_FIELD))
#define CFRAME_use_tracing                                                   \
    FMUT(DEF_FIELD(le_cframe_t, __typeof__(((le_cframe_t *)0)->use_tracing), \
                   use_tracing, NO_PREV_FIELD))

/* the flag itself, for le_direct_call_refused ('vinterp' unused here) */
static inline vinfo_t *
le_tracing_flag(PsycoObject *po, vinfo_t *vts, vinfo_t *vinterp)
{
    vinfo_t *vflag, *vcframe = psyco_get_field(po, vts, TSTATE_cframe);
    (void)vinterp;
    if (vcframe == NULL)
        return NULL;
    vflag = psyco_get_field(po, vcframe, CFRAME_use_tracing);
    vinfo_decref(vcframe, po);
    return vflag;
}

static inline condition_code_t
le_tracing_test(PsycoObject *po, vinfo_t *vts)
{
    condition_code_t cc;
    vinfo_t *vflag, *vcframe = psyco_get_field(po, vts, TSTATE_cframe);
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
   PyEval_SetProfile. le_tracing_now tests it at run time (the
   eval hook); le_tracing_test emits the same test in compiled code, on the
   thread state 'vts' (psyfunc.c:tracing_on). Included after vcompiler.h and
   codegen.h. */
static inline int
le_tracing_now(PyThreadState *ts)
{
    return ts->use_tracing;
}

_Static_assert(sizeof(((PyThreadState *)0)->use_tracing) == 4,
               "LeJIT: PyThreadState.use_tracing is tested as a 32-bit int");
static inline condition_code_t
le_tracing_test(PsycoObject *po, vinfo_t *vts)
{
    return psyco_int32_nonzero_in(po, vts,
                                  offsetof(PyThreadState, use_tracing));
}

/* the flag itself, for le_direct_call_refused ('vinterp' unused here) */
static inline vinfo_t *
le_tracing_flag(PsycoObject *po, vinfo_t *vts, vinfo_t *vinterp)
{
    (void)vinterp;
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
   then the tracing one, on the thread state 'vts', each a branch to the
   real call; true where the real call is needed, as on an error. */
extern void *
le_eval_hook(void);
extern const long le_interp_eval_frame_at;

static inline bool
le_direct_call_refused(PsycoObject *po, vinfo_t *vts)
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
    vflag = le_tracing_flag(po, vts, vinterp);
    vinfo_decref(vinterp, po);
    if (vflag == NULL)
        return true;
    cc = integer_non_null(po, vflag);
    vinfo_decref(vflag, po);
    return cc == CC_ERROR || runtime_condition_f(po, cc);
}
#endif /* LE_MECHANISMS_TRACING_H */
