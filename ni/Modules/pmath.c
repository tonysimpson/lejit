/*
 * The PMATH_FUNC1 macro generates a meta for each one-argument math
 * function: unpack the double argument, call cimpl_math_1 (3.7's math_1:
 * the libm call, then ValueError/OverflowError from the result, KNOWN_BUGS
 * F5) and return a virtual float. ceil and floor take exact floats only and
 * return an int, as 3.7 does for a float (it has no __floor__ there); other
 * types go to the real function. atan2, fmod, hypot and pow have special
 * cases in mathmodule.c and get no meta: the real function is called.
 * A meta is silently not registered when the function's ml_flags differ
 * from those expected (e.g. METH_FASTCALL).
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
#include <errno.h>
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

/* 3.7's math_1_to_whatever for a float result */
static double cimpl_math_1(double x, long func, long can_overflow) {
    double r;
    errno = 0;
    r = ((double (*)(double))func)(x);
    if (Py_IS_NAN(r) && !Py_IS_NAN(x)) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1.0;
    }
    if (Py_IS_INFINITY(r) && Py_IS_FINITE(x)) {
        if (can_overflow)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1.0;
    }
    if (Py_IS_FINITE(r) && errno) {
        /* is_error(): ERANGE with a result below 1 is an underflow, fine */
        if (errno == EDOM)
            PyErr_SetString(PyExc_ValueError, "math domain error");
        else if (errno == ERANGE) {
            if (fabs(r) < 1.0)
                return r;
            PyErr_SetString(PyExc_OverflowError, "math range error");
        }
        else
            PyErr_SetFromErrno(PyExc_ValueError);
        return -1.0;
    }
    return r;
}

# define PMATH_FUNC1(funcname, func, can_overflow)\
static PyCFunction fallback_##func;\
static vinfo_t* pmath_##func(PsycoObject* po, vinfo_t* vself, vinfo_t* v) {\
    vinfo_t *a, *x;\
    PMATH_CONVERT_TO_DOUBLE(v, a);\
    x = psyco_generic_call(po, cimpl_math_1, CfPyErrCheck|CfReturnTypeDouble,\
                           "dll", a, (long)&func, (long)(can_overflow));\
    PMATH_RELEASE_DOUBLE(a);\
    return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;\
fallback:\
    return psyco_generic_call(po, fallback_##func, CfCommonNewRefPyObject,"lv" , NULL, v);\
error:\
    return NULL;\
}

static PyObject *cimpl_floor_to_int(double x) { return PyLong_FromDouble(floor(x)); }
static PyObject *cimpl_ceil_to_int(double x) { return PyLong_FromDouble(ceil(x)); }

/* ceil, floor: a float becomes an int; anything else (an int, or a type
   with __floor__/__ceil__) goes to the real function */
# define PMATH_TO_INT(func)\
static PyCFunction fallback_##func;\
static vinfo_t* pmath_##func(PsycoObject* po, vinfo_t* vself, vinfo_t* v) {\
    vinfo_t *a, *x;\
    PyTypeObject *t = Psyco_NeedType(po, v);\
    if (t == NULL)\
        return NULL;\
    if (t != &PyFloat_Type)\
        goto fallback;\
    PMATH_CONVERT_TO_DOUBLE(v, a);\
    x = psyco_generic_call(po, cimpl_##func##_to_int, CfCommonNewRefPyObject,\
                           "d", a);\
    PMATH_RELEASE_DOUBLE(a);\
    return x;\
fallback:\
    return psyco_generic_call(po, fallback_##func, CfCommonNewRefPyObject,"lv" , NULL, v);\
error:\
    return NULL;\
}

/* the functions pmath_sin() etc; can_overflow as in mathmodule.c */
PMATH_FUNC1("acos", acos, 0)
PMATH_FUNC1("asin", asin, 0)
PMATH_FUNC1("atan", atan, 0)
PMATH_TO_INT(ceil)
PMATH_FUNC1("cos", cos, 0)
PMATH_FUNC1("cosh", cosh, 1)
PMATH_FUNC1("exp", exp, 1)
PMATH_FUNC1("fabs", fabs, 0)
PMATH_TO_INT(floor)
PMATH_FUNC1("sin", sin, 0)
PMATH_FUNC1("sinh", sinh, 1)
PMATH_FUNC1("sqrt", sqrt, 0)
PMATH_FUNC1("tan", tan, 0)
PMATH_FUNC1("tanh", tanh, 0)

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
    fallback_ceil = Psyco_DefineModuleFn(md, "ceil", METH1,        pmath_ceil);
    fallback_cos  = Psyco_DefineModuleFn(md, "cos",  METH1,        pmath_cos);
    fallback_cosh = Psyco_DefineModuleFn(md, "cosh", METH1,        pmath_cosh);
    fallback_exp  = Psyco_DefineModuleFn(md, "exp",  METH1,        pmath_exp);
    fallback_fabs = Psyco_DefineModuleFn(md, "fabs", METH1,        pmath_fabs);
    fallback_floor= Psyco_DefineModuleFn(md, "floor",METH1,        pmath_floor);
    /*Psyco_DefineModuleFn(md, "power", METH_VARARGS, pmath_power);*/
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
