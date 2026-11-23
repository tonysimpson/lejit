/*
 * The math module's metas. The one-argument functions on doubles (acos,
 * sqrt, exp...) are translated from Modules/mathmodule.c (below):
 * PyFloat_AsDouble's meta for the argument, so an int or a type with
 * __float__ converts as CPython does, then math_1's double
 * code copied into a helper per function (the libm call, errno and the
 * ValueError/OverflowError tests).
 * Hand-written here: ceil and floor, which take exact floats only and
 * return an int, as 3.7 does for a float (it has no __floor__ there);
 * other types go to the real function. Functions with no meta (atan2,
 * fmod, hypot, pow, log...) are called.
 * A meta is not registered when the function's ml_flags differ from those
 * expected; lejit._core._unregistered_metas() then lists it.
 */

#include "../initialize.h"
#include "../Python/pyver.h"
#if HAVE_FP_FN_CALLS /* disable float optimizations if functions with \
                        float/double arguments are not implemented    \
                        in the back-end */

#include <math.h>
#include <errno.h>
#include "../Objects/pfloatobject.h"

static PyObject *
cimpl_floor_to_int(double x)
{
    return PyLong_FromDouble(floor(x));
}
static PyObject *
cimpl_ceil_to_int(double x)
{
    return PyLong_FromDouble(ceil(x));
}

/* ceil, floor: a float becomes an int; anything else (an int, or a type
   with __floor__/__ceil__) goes to the real function */
#define PMATH_TO_INT(func)                                                    \
    static PyCFunction fallback_##func;                                       \
    static vinfo_t *pmath_##func(PsycoObject *po, vinfo_t *vself, vinfo_t *v) \
    {                                                                         \
        vinfo_t *a;                                                           \
        PyTypeObject *t = Psyco_NeedType(po, v);                              \
        if (t == NULL)                                                        \
            return NULL;                                                      \
        /* the real function may read the module's state, from its self */    \
        if (t != &PyFloat_Type)                                               \
            return psyco_generic_call(                                        \
                po, fallback_##func, CfCommonNewRefPyObject, "vv", vself, v); \
        a = PsycoFloat_AS_DOUBLE(po, v); /* borrowed */                       \
        if (a == NULL)                                                        \
            return NULL;                                                      \
        return psyco_generic_call(po, cimpl_##func##_to_int,                  \
                                  CfCommonNewRefPyObject, "d", a);            \
    }

PMATH_TO_INT(ceil)
PMATH_TO_INT(floor)

/* math_1's error tests when errno is set and the result is finite: EDOM is
   a ValueError, ERANGE an OverflowError unless the result is small enough
   to be an underflow, which is not an error */
static int
cimpl_is_error(double x)
{
    int result = 1;
    assert(errno);
    if (errno == EDOM)
        PyErr_SetString(PyExc_ValueError, LE_MSG_MATH_DOMAIN_ERROR);
    else if (errno == ERANGE) {
        if (fabs(x) < 1.5)
            result = 0;
        else
            PyErr_SetString(PyExc_OverflowError, LE_MSG_MATH_RANGE_ERROR);
    }
    else
        PyErr_SetFromErrno(PyExc_ValueError);
    return result;
}

/* math_1's domain error: a function whose message names its argument
   shows it as repr does */
static void
cimpl_domain_error(double x, const char *fmt)
{
    char *buf;
    if (fmt == NULL) {
        PyErr_SetString(PyExc_ValueError, LE_MSG_MATH_DOMAIN_ERROR);
        return;
    }
    buf = PyOS_double_to_string(x, 'r', 0, Py_DTSF_ADD_DOT_0, NULL);
    if (buf != NULL) {
        PyErr_Format(PyExc_ValueError, fmt, buf);
        PyMem_Free(buf);
    }
}

/* the functions whose domain error names the argument, where it does */
#ifdef LE_MSG_MATH_DOMAIN_ERROR_ACOS
#define DOMAIN_acos LE_MSG_MATH_DOMAIN_ERROR_ACOS
#else
#define DOMAIN_acos NULL
#endif
#ifdef LE_MSG_MATH_DOMAIN_ERROR_ASIN
#define DOMAIN_asin LE_MSG_MATH_DOMAIN_ERROR_ASIN
#else
#define DOMAIN_asin NULL
#endif
#ifdef LE_MSG_MATH_DOMAIN_ERROR_COS
#define DOMAIN_cos LE_MSG_MATH_DOMAIN_ERROR_COS
#else
#define DOMAIN_cos NULL
#endif
#ifdef LE_MSG_MATH_DOMAIN_ERROR_SIN
#define DOMAIN_sin LE_MSG_MATH_DOMAIN_ERROR_SIN
#else
#define DOMAIN_sin NULL
#endif
#ifdef LE_MSG_MATH_DOMAIN_ERROR_SQRT
#define DOMAIN_sqrt LE_MSG_MATH_DOMAIN_ERROR_SQRT
#else
#define DOMAIN_sqrt NULL
#endif
#ifdef LE_MSG_MATH_DOMAIN_ERROR_TAN
#define DOMAIN_tan LE_MSG_MATH_DOMAIN_ERROR_TAN
#else
#define DOMAIN_tan NULL
#endif
#define DOMAIN_atan NULL
#define DOMAIN_cosh NULL
#define DOMAIN_exp NULL
#define DOMAIN_fabs NULL
#define DOMAIN_sinh NULL
#define DOMAIN_tanh NULL

/* A one-argument function on doubles: math_1's code in a helper, and the
   meta that calls it. An infinite result from a finite argument is an
   OverflowError for the functions that can overflow (cosh, exp, sinh),
   and a domain error for the others. cfunc_math_<func> is the C
   function, set by the registration. */
#define MATH_1(func, can_overflow)                                         \
    static PyCFunction cfunc_math_##func;                                  \
                                                                           \
    static int cimpl_math_##func(double x, double *_r)                     \
    {                                                                      \
        double r;                                                          \
        errno = 0;                                                         \
        r = func(x);                                                       \
        if (isnan(r) && !isnan(x)) {                                       \
            cimpl_domain_error(x, DOMAIN_##func);                          \
            return -1;                                                     \
        }                                                                  \
        if (isinf(r) && isfinite(x)) {                                     \
            if (can_overflow)                                              \
                PyErr_SetString(PyExc_OverflowError,                       \
                                LE_MSG_MATH_RANGE_ERROR);                  \
            else                                                           \
                cimpl_domain_error(x, DOMAIN_##func);                      \
            return -1;                                                     \
        }                                                                  \
        if (isfinite(r) && errno && cimpl_is_error(r))                     \
            return -1;                                                     \
        *_r = r;                                                           \
        return 0;                                                          \
    }                                                                      \
                                                                           \
    static vinfo_t *math_##func(PsycoObject *po, vinfo_t *self,            \
                                vinfo_t *args)                             \
    {                                                                      \
        vinfo_array_t *result;                                             \
        vinfo_t *x;                                                        \
        vinfo_t *r;                                                        \
        x = PsycoFloat_AsDouble(po, args);                                 \
        if (x == NULL) {                                                   \
            return NULL;                                                   \
        }                                                                  \
        result = array_new(1);                                             \
        if (psyco_generic_call(po, cimpl_math_##func,                      \
                               CfCommonIntZeroOk | CfNoPython | CfNoBlock, \
                               "da", x, result) == NULL) {                 \
            array_release(result);                                         \
            vinfo_decref(x, po);                                           \
            return NULL;                                                   \
        }                                                                  \
        r = result->items[0];                                              \
        array_release(result);                                             \
        r = PsycoFloat_FROM_DOUBLE(r);                                     \
        vinfo_decref(x, po);                                               \
        return r;                                                          \
    }

MATH_1(acos, 0)
MATH_1(asin, 0)
MATH_1(atan, 0)
MATH_1(cos, 0)
MATH_1(cosh, 1)
MATH_1(exp, 1)
MATH_1(fabs, 0)
MATH_1(sin, 0)
MATH_1(sinh, 1)
MATH_1(sqrt, 0)
MATH_1(tan, 0)
MATH_1(tanh, 0)

static void
gen_mathmodule_init(PyObject *md)
{
    cfunc_math_acos = Psyco_DefineModuleFn(md, "acos", METH_O, &math_acos);
    cfunc_math_asin = Psyco_DefineModuleFn(md, "asin", METH_O, &math_asin);
    cfunc_math_atan = Psyco_DefineModuleFn(md, "atan", METH_O, &math_atan);
    cfunc_math_cos = Psyco_DefineModuleFn(md, "cos", METH_O, &math_cos);
    cfunc_math_cosh = Psyco_DefineModuleFn(md, "cosh", METH_O, &math_cosh);
    cfunc_math_exp = Psyco_DefineModuleFn(md, "exp", METH_O, &math_exp);
    cfunc_math_fabs = Psyco_DefineModuleFn(md, "fabs", METH_O, &math_fabs);
    cfunc_math_sin = Psyco_DefineModuleFn(md, "sin", METH_O, &math_sin);
    cfunc_math_sinh = Psyco_DefineModuleFn(md, "sinh", METH_O, &math_sinh);
    cfunc_math_sqrt = Psyco_DefineModuleFn(md, "sqrt", METH_O, &math_sqrt);
    cfunc_math_tan = Psyco_DefineModuleFn(md, "tan", METH_O, &math_tan);
    cfunc_math_tanh = Psyco_DefineModuleFn(md, "tanh", METH_O, &math_tanh);
}

INITIALIZATIONFN
void
psyco_initmath(void)
{
    PyObject *md = Psyco_DefineMetaModule("math");

    fallback_ceil = Psyco_DefineModuleFn(md, "ceil", METH_O, pmath_ceil);
    fallback_floor = Psyco_DefineModuleFn(md, "floor", METH_O, pmath_floor);
    gen_mathmodule_init(md);

    Py_XDECREF(md);
}

#else  /* !HAVE_FP_FN_CALLS */
INITIALIZATIONFN
void
psyco_initmath(void)
{
}
#endif /* !HAVE_FP_FN_CALLS */
