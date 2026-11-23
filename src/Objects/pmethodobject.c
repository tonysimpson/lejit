/*
 * Virtual builtin-method objects ([iCFUNC_M_ML] = PyMethodDef*,
 * [iCFUNC_M_SELF]) and the PyCFunction tp_call meta. The meta promotes m_ml,
 * switches on ml_flags exactly as CPython does, and dispatches to a meta for
 * ml_meth if one is registered. Without a meta, a METH_FASTCALL call with
 * no keywords and up to FAST_MAX arguments calls the C function directly
 * through a cimpl_fastN helper that puts the arguments in a C array, so no
 * tuple is built. Like the METH_O and METH_VARARGS cases,
 * these direct calls skip PyCFunction_Call's Py_EnterRecursiveCall, but do
 * _Py_CheckFunctionResult's checks (check_result). Keywords, or more
 * arguments, take the residual PyCFunction_Call / tuple paths.
 */
#include "pmethodobject.h"
#include "ptupleobject.h"
/* le_tstate_load */
#include "../mechanisms/tstate.h"
#include "../Python/frames.h"

static bool
compute_cfunction(PsycoObject *po, vinfo_t *methobj)
{
    vinfo_t *newobj;
    vinfo_t *m_self;
    vinfo_t *m_ml;

    /* get the fields from the Python object 'methobj' */
    m_self = vinfo_getitem(methobj, iCFUNC_M_SELF);
    if (m_self == NULL)
        return false;
    m_ml = vinfo_getitem(methobj, iCFUNC_M_ML);
    if (m_ml == NULL)
        return false;

    /* call PyCFunction_New() */
#ifdef PyCFunction_New /* Python >= 2.3 introduced PyCFunction_NewEx() */
    newobj = psyco_generic_call(po, PyCFunction_NewEx,
                                CfPure | CfCommonNewRefPyObject, "vvl", m_ml,
                                m_self, NULL);
#else
    newobj = psyco_generic_call(po, PyCFunction_New,
                                CfPure | CfCommonNewRefPyObject, "vv", m_ml,
                                m_self);
#endif
    if (newobj == NULL)
        return false;

    /* move the resulting non-virtual Python object back into 'methobj' */
    vinfo_move(po, methobj, newobj);
    return true;
}

static PyObject *
direct_compute_cfunction(vinfo_t *methobj, char *data)
{
    PyObject *m_self;
    long m_ml;
    PyObject *result = NULL;

    m_self = direct_xobj_vinfo(vinfo_getitem(methobj, iCFUNC_M_SELF), data);
    m_ml = direct_read_vinfo(vinfo_getitem(methobj, iCFUNC_M_ML), data);

    if (!PyErr_Occurred())
        result = PyCFunction_New((PyMethodDef *)m_ml, m_self);

    Py_XDECREF(m_self);
    return result;
}

DEFINEVAR source_virtual_t psyco_computed_cfunction;

/***************************************************************/
/*** C method objects meta-implementation                    ***/

/* call.c:_Py_CheckFunctionResult, for a C function that compiled code
   calls itself: a NULL result must come with an
   exception, and any other result without one. 3.7 checks both in
   release builds. The test of the exception is inline (the exception
   word of the current thread state, as PyErr_Occurred reads it:
   mechanisms/tstate.h and Python/pycompiler.h); the SystemError is made by
   CPython's own function, in branches compiled only if a C function breaks the
   rule. A meta's result is not checked. */
#define CF_UNCHECKED (CfReturnTypePtr | CfNewRef)

#if LE_HAS_CHECK_RESULT_TSTATE
#define CHECK_FUNCTION_RESULT(f, r) \
    _Py_CheckFunctionResult(PyThreadState_GET(), f, r, NULL)
#else
#define CHECK_FUNCTION_RESULT(f, r) _Py_CheckFunctionResult(f, r, NULL)
#endif

static PyObject *
cimpl_null_result(PyObject *func)
{
    return CHECK_FUNCTION_RESULT(func, NULL);
}

/* the result has been released already: None stands for it */
static PyObject *
cimpl_result_with_error(PyObject *func)
{
    Py_INCREF(Py_None);
    return CHECK_FUNCTION_RESULT(func, Py_None);
}

/* consumes 'r', the result of a CF_UNCHECKED call */
static vinfo_t *
check_result(PsycoObject *po, vinfo_t *r, vinfo_t *vfunc)
{
    condition_code_t cc;
    vinfo_t *vts, *vexc;
    if (r == NULL)
        return NULL;
    cc = integer_non_null(po, r);
    if (cc == CC_ERROR)
        goto fail;
    if (!runtime_condition_t(po, cc)) {
        r->source = remove_rtref(r->source); /* NULL holds nothing */
        vinfo_decref(r, po);
        psyco_generic_call(po, cimpl_null_result,
                           CfNoReturnValue | CfPyErrAlways, "v", vfunc);
        return NULL;
    }
    vts = le_tstate_load(po);
    if (vts == NULL)
        goto fail;
    vexc = psyco_get_field(po, vts, LE_TSTATE_exc_occurred);
    vinfo_decref(vts, po);
    cc = integer_NON_NULL(po, vexc); /* consumes vexc */
    if (cc == CC_ERROR)
        goto fail;
    if (runtime_condition_f(po, cc)) {
        vinfo_decref(r, po);
        psyco_generic_call(po, cimpl_result_with_error,
                           CfNoReturnValue | CfPyErrAlways, "v", vfunc);
        return NULL;
    }
    return r;
fail:
    vinfo_decref(r, po);
    return NULL;
}

static vinfo_t *
call_fast(PsycoObject *po, PyCFunction meth, int flags, vinfo_t *vself,
          vinfo_t *vargs, vinfo_t *vfunc);

/* ml_meth through its meta, or called directly and checked */
#define CALL_METH(META, fmt, ...)                                          \
    (meta != NULL                                                          \
         ? META(po, ml->ml_meth, CfCommonNewRefPyObject, fmt, __VA_ARGS__) \
         : check_result(po,                                                \
                        psyco_generic_call(po, ml->ml_meth, CF_UNCHECKED,  \
                                           fmt, __VA_ARGS__),              \
                        func))

DEFINEFN
vinfo_t *
PsycoCFunction_Call(PsycoObject *po, vinfo_t *func, vinfo_t *args,
                    vinfo_t *kwargs)
{
    long mllong;
    vinfo_t *vml;
    /* made run-time at a capped merge point: m_ml isn't promoted either */
    if (is_megamorphic(func->source))
        goto use_proxy;
    vml = psyco_get_const(po, func, CFUNC_m_ml);
    if (vml == NULL)
        return NULL;

    /* promote to compile-time the function if we do not know which one
       it is yet */
    mllong = psyco_atcompiletime(po, vml);
    if (mllong == -1) {
        /* -1 is not a valid pointer */
        extra_assert(PycException_Occurred(po));
        return NULL;
    }
    else {
        PyMethodDef *ml = (PyMethodDef *)CompileTime_Get(vml->source)->value;
        int flags = ml->ml_flags & ~(METH_CLASS | METH_STATIC | METH_COEXIST);
        int tuplesize;

        /* a static method's self is NULL whatever m_self holds
           (PyCFunction_GET_SELF) */
        if (ml->ml_flags & METH_STATIC)
            goto use_proxy;

        vinfo_t *vself = psyco_get_const(po, func, CFUNC_m_self);
        if (vself == NULL)
            return NULL;

        vinfo_t *(*meta)(PsycoObject *, vinfo_t *, vinfo_t *);

        /* _warnings.warn of a module imported again since LeJIT replaced
           it walks the frames as the replacement does */
        if (ml->ml_meth == le_c_warn && le_c_warn != NULL)
            return psyco_generic_call(po, le_warn_with_frames,
                                      CfCommonNewRefPyObject, "vvv", func,
                                      args, kwargs);

        if (!psyco_knowntobe(kwargs, (long)NULL))
            goto use_proxy;

        meta = Psyco_Lookup(ml->ml_meth);
        switch (flags) {
            case METH_O:
                tuplesize = PsycoTuple_Load(args);
                if (tuplesize != 1)
                    goto use_proxy;
                return CALL_METH(Psyco_META2, "vv", vself,
                                 PsycoTuple_GET_ITEM(args, 0));
            case METH_VARARGS:
                return CALL_METH(Psyco_META2, "vv", vself, args);
            case METH_VARARGS | METH_KEYWORDS:
                return CALL_METH(Psyco_META3, "vvv", vself, args, kwargs);
            case METH_NOARGS:
                tuplesize = PsycoTuple_Load(args);
                if (tuplesize != 0)
                    goto use_proxy;
                return CALL_METH(Psyco_META2, "vl", vself, NULL);
            case METH_FASTCALL:
            case METH_FASTCALL | METH_KEYWORDS:
                if (meta == NULL)
                    return call_fast(po, ml->ml_meth, flags, vself, args,
                                     func);
                return meta(po, vself, args); /* no keywords here */
        }
    }

    /* default, slow version */
use_proxy:
    LE_STAT(psyco_stat_cfunc_proxy++);
    return psyco_residual_call(po, PyCFunction_Call, func, args, kwargs);
}

#if ALL_CHECKS
DEFINEVAR long psyco_stat_cfunc_fast = 0;
DEFINEVAR long psyco_stat_cfunc_proxy = 0;
#endif

/* call a METH_FASTCALL function with a tuple's items, no keywords */
static PyObject *
cimpl_call_fast(PyCFunction meth, PyObject *self, PyObject *args)
{
    return ((_PyCFunctionFast)(void (*)(void))meth)(
        self, &PyTuple_GET_ITEM(args, 0), PyTuple_GET_SIZE(args));
}

static PyObject *
cimpl_call_fast_kw(PyCFunction meth, PyObject *self, PyObject *args)
{
    return ((_PyCFunctionFastWithKeywords)(void (*)(void))meth)(
        self, &PyTuple_GET_ITEM(args, 0), PyTuple_GET_SIZE(args), NULL);
}

/* the same with n arguments passed one by one (psyco_generic_call takes
   at most 7: meth, self and FAST_MAX arguments) */
#define FAST_MAX 5
#define FAST_FN(n, params, ...)                                               \
    static PyObject *cimpl_fast##n(PyCFunction meth, PyObject *self params)   \
    {                                                                         \
        PyObject *a[] = {__VA_ARGS__};                                        \
        return ((_PyCFunctionFast)(void (*)(void))meth)(self, a, n);          \
    }                                                                         \
    static PyObject *cimpl_fastkw##n(PyCFunction meth, PyObject *self params) \
    {                                                                         \
        PyObject *a[] = {__VA_ARGS__};                                        \
        return ((_PyCFunctionFastWithKeywords)(void (*)(void))meth)(self, a,  \
                                                                    n, NULL); \
    }
#define P(x) , PyObject *x
FAST_FN(0, , NULL)
FAST_FN(1, P(a0), a0)
FAST_FN(2, P(a0) P(a1), a0, a1)
FAST_FN(3, P(a0) P(a1) P(a2), a0, a1, a2)
FAST_FN(4, P(a0) P(a1) P(a2) P(a3), a0, a1, a2, a3)
FAST_FN(5, P(a0) P(a1) P(a2) P(a3) P(a4), a0, a1, a2, a3, a4)
#undef P
#undef FAST_FN

static void *const cimpl_fast[2][FAST_MAX + 1] = {
    {cimpl_fast0, cimpl_fast1, cimpl_fast2, cimpl_fast3, cimpl_fast4,
     cimpl_fast5},
    {cimpl_fastkw0, cimpl_fastkw1, cimpl_fastkw2, cimpl_fastkw3, cimpl_fastkw4,
     cimpl_fastkw5}};

/* the result is checked when 'vfunc' is the function object, not for
   LeJIT's own C functions (vfunc NULL) */
static vinfo_t *
call_fast(PsycoObject *po, PyCFunction meth, int flags, vinfo_t *vself,
          vinfo_t *vargs, vinfo_t *vfunc)
{
    static const char *const fmt[FAST_MAX + 1] = {
        "lv", "lvv", "lvvv", "lvvvv", "lvvvvv", "lvvvvvv"};
    int kw = (flags & METH_KEYWORDS) != 0;
    int n = PsycoTuple_Load(vargs);
    vinfo_t *a[FAST_MAX] = {NULL};
    int cf = vfunc != NULL ? CF_UNCHECKED : CfCommonNewRefPyObject;
    vinfo_t *r;
    int i;
    if (psyco_cannot_block(meth)) /* meth's, not cimpl_fastN's */
        cf |= CfNoBlock;
    if (n < 0 || n > FAST_MAX) {
        void *fn = kw ? (void *)cimpl_call_fast_kw : (void *)cimpl_call_fast;
        r = psyco_generic_call(po, fn, cf, "lvv", (long)meth, vself, vargs);
    }
    else {
        for (i = 0; i < n; i++) a[i] = PsycoTuple_GET_ITEM(vargs, i);
        LE_STAT(psyco_stat_cfunc_fast++);
        /* fmt[n] stops before the unused (NULL) trailing arguments */
        r = psyco_generic_call(po, cimpl_fast[kw][n], cf, fmt[n], (long)meth,
                               vself, a[0], a[1], a[2], a[3], a[4]);
    }
    return vfunc != NULL ? check_result(po, r, vfunc) : r;
}

DEFINEFN
vinfo_t *
PsycoCFunction_CallFast(PsycoObject *po, PyCFunction meth, int flags,
                        vinfo_t *vself, vinfo_t *vargs)
{
    return call_fast(po, meth, flags, vself, vargs, NULL);
}

INITIALIZATIONFN
void
psy_methodobject_init(void)
{
    Psyco_DefineCallMeta(&PyCFunction_Type, PsycoCFunction_Call);
    INIT_SVIRTUAL(psyco_computed_cfunction, compute_cfunction,
                  direct_compute_cfunction, (1 << iCFUNC_M_SELF), 1, 1);
}
