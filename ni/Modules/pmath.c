/*
 * The PMATH_FUNC1/2 macros generate a meta for each math function: unpack the
 * double arguments, call libm with CfPure (so constants fold), and return a
 * virtual float. In effect it is a tiny hand-written meta-compiler.
 * Limitations:
 *  - no errno or domain checks (math.sqrt(-1) gives nan, not ValueError);
 *  - ceil/floor return floats (Python 3 returns int);
 *  - a meta is silently not registered when the function's ml_flags differ
 *    from those expected (e.g. METH_FASTCALL).
 */
/* 
 * Psyco version of python math module, mathmodule.c 
 *
 * TODO: 
 * - raise python exceptions on math errors (domain error etc)
 * - implement:
 *   - frexp()
 *   - ldexp()
 *   - log()
 *   - log10()
 *   - modf()
 *   - degrees()
 *   - radians()
 */

#include "../initialize.h"
#include "../Python/pyver.h"
#if HAVE_FP_FN_CALLS    /* disable float optimizations if functions with
                           float/double arguments are not implemented
                           in the back-end */

#ifndef _MSC_VER
#ifndef __STDC__
extern double fmod (double, double);
extern double frexp (double, int *);
extern double ldexp (double, int);
extern double modf (double, double *);
#endif /* __STDC__ */
#endif /* _MSC_VER */


#include <math.h>
/* 
 * This is almost but not quite the same as the 
 * version in pfloatobject.c. The error handling 
 * on invalid args is different
 */
#define PMATH_CONVERT_TO_DOUBLE(vobj, v)			\
    switch (psyco_convert_to_double(po, vobj, &v)) {	\
    case true:							\
        break;   /* fine */					\
    case false:							\
        goto error;  /* error or promotion */			\
    default:							\
        goto fallback;  /* e.g. not a float object */		\
    }

#define PMATH_RELEASE_DOUBLE(v) \
    vinfo_decref(v, po);

# define PMATH_FUNC1(funcname, func)\
static PyCFunction fallback_##func;\
static vinfo_t* pmath_##func(PsycoObject* po, vinfo_t* vself, vinfo_t* v) {\
    vinfo_t *a, *x;\
    PMATH_CONVERT_TO_DOUBLE(v, a);\
    x = psyco_generic_call(po, func, CfPure|CfReturnTypeDouble, "d", a);\
    PMATH_RELEASE_DOUBLE(a);\
    return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;\
fallback:\
    return psyco_generic_call(po, fallback_##func, CfCommonNewRefPyObject,"lv" , NULL, v);\
error:\
    return NULL;\
}

#define PMATH_FUNC2(funcname, func ) \
static PyCFunction fallback_##func; \
static vinfo_t* pmath_##func(PsycoObject* po, vinfo_t* vself, vinfo_t* varg) { \
    vinfo_t *a=NULL, *b, *x; \
    vinfo_t *v1, *v2; \
    int tuplesize = PsycoTuple_Load(varg); \
    if (tuplesize != 2) /* wrong or unknown number of arguments */ \
        goto fallback; \
    v1 = PsycoTuple_GET_ITEM(varg, 0); \
    v2 = PsycoTuple_GET_ITEM(varg, 1); \
    PMATH_CONVERT_TO_DOUBLE(v1, a); \
    PMATH_CONVERT_TO_DOUBLE(v2, b); \
    x = psyco_generic_call(po, func, CfPure|CfReturnTypeDouble, \
                            "dd", a, b); \
    PMATH_RELEASE_DOUBLE(a); \
    PMATH_RELEASE_DOUBLE(b); \
    return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;\
fallback:                                                   \
    return psyco_generic_call(po, fallback_##func,          \
                                CfCommonNewRefPyObject,    \
                                "lv", NULL, varg);            \
error:                                                      \
    if (a != NULL) {                                       \
        PMATH_RELEASE_DOUBLE(a);                            \
    }                                                       \
    return NULL;                                            \
}

/* the functions pmath_sin() etc */
PMATH_FUNC1("acos", acos)
PMATH_FUNC1("asin", asin)
PMATH_FUNC1("atan", atan)
PMATH_FUNC2("atan2", atan2)
PMATH_FUNC1("ceil", ceil)
PMATH_FUNC1("cos", cos)
PMATH_FUNC1("cosh", cosh)
PMATH_FUNC1("exp", exp)
PMATH_FUNC1("fabs", fabs)
PMATH_FUNC1("floor", floor)
PMATH_FUNC2("fmod", fmod)
PMATH_FUNC2("hypot", hypot)
/*PMATH_FUNC2("power", power)*/
PMATH_FUNC2("pow", pow)
PMATH_FUNC1("sin", sin)
PMATH_FUNC1("sinh", sinh)
PMATH_FUNC1("sqrt", sqrt)
PMATH_FUNC1("tan", tan)
PMATH_FUNC1("tanh", tanh)

INITIALIZATIONFN
void psyco_initmath(void)
{
    PyObject* md = Psyco_DefineMetaModule("math");

#if MATHMODULE_USES_METH_O    /* Python >= 2.6 */
#  define METH1 METH_O
#else
#  define METH1 METH_VARARGS
#endif

    fallback_acos = Psyco_DefineModuleFn(md, "acos", METH1,        pmath_acos);
    fallback_asin = Psyco_DefineModuleFn(md, "asin", METH1,        pmath_asin);
    fallback_atan = Psyco_DefineModuleFn(md, "atan", METH1,        pmath_atan);
    fallback_atan2= Psyco_DefineModuleFn(md, "atan2",METH_VARARGS, pmath_atan2);
    fallback_ceil = Psyco_DefineModuleFn(md, "ceil", METH1,        pmath_ceil);
    fallback_cos  = Psyco_DefineModuleFn(md, "cos",  METH1,        pmath_cos);
    fallback_cosh = Psyco_DefineModuleFn(md, "cosh", METH1,        pmath_cosh);
    fallback_exp  = Psyco_DefineModuleFn(md, "exp",  METH1,        pmath_exp);
    fallback_fabs = Psyco_DefineModuleFn(md, "fabs", METH1,        pmath_fabs);
    fallback_floor= Psyco_DefineModuleFn(md, "floor",METH1,        pmath_floor);
    fallback_fmod = Psyco_DefineModuleFn(md, "fmod", METH_VARARGS, pmath_fmod);
    fallback_hypot= Psyco_DefineModuleFn(md, "hypot",METH_VARARGS, pmath_hypot);
    /*Psyco_DefineModuleFn(md, "power", METH_VARARGS, pmath_power);*/
    fallback_pow  = Psyco_DefineModuleFn(md, "pow",  METH_VARARGS, pmath_pow);
    fallback_sin  = Psyco_DefineModuleFn(md, "sin",  METH1,        pmath_sin);
    fallback_sinh = Psyco_DefineModuleFn(md, "sinh", METH1,        pmath_sinh);
    fallback_sqrt = Psyco_DefineModuleFn(md, "sqrt", METH1,        pmath_sqrt);
    fallback_tan  = Psyco_DefineModuleFn(md, "tan",  METH1,        pmath_tan);
    fallback_tanh = Psyco_DefineModuleFn(md, "tanh", METH1,        pmath_tanh);

#undef METH1

    Py_XDECREF(md);
}

#else /* !HAVE_FP_FN_CALLS */
INITIALIZATIONFN
void psyco_initmath(void)
{
}
#endif /* !HAVE_FP_FN_CALLS */
