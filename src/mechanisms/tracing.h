#ifndef LE_MECHANISMS_TRACING_H
#define LE_MECHANISMS_TRACING_H
#if LE_HAS_MONITORING
/* Tracing is on while sys.monitoring has any event on in the interpreter
   (pyruntime.c:le_monitoring_on): sys.settrace and sys.setprofile are
   tools of its, so other tools are tracing too, and compiled code, which
   gives them no events, does not run. le_tracing_now tests it at run time
   (the eval hook); le_tracing_test emits a call of the same test in
   compiled code, on the thread state 'vts' (psyfunc.c:tracing_on).
   Included after vcompiler.h and codegen.h. */
extern int
le_monitoring_on(PyThreadState *ts);
static inline int
le_tracing_now(PyThreadState *ts)
{
    return le_monitoring_on(ts);
}

static inline int
cimpl_tracing_now(PyThreadState *ts)
{
    return le_tracing_now(ts);
}

static inline condition_code_t
le_tracing_test(PsycoObject *po, vinfo_t *vts)
{
    condition_code_t cc;
    vinfo_t *vflag =
        psyco_generic_call(po, cimpl_tracing_now,
                           CfReturnTypeInt | CfNoBlock | CfNoPython, "v", vts);
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
#endif
#endif /* LE_MECHANISMS_TRACING_H */
