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
 * expected; ni._unregistered_metas() then lists it.
 */

#include "../initialize.h"
#include "../Python/pyver.h"
#if HAVE_FP_FN_CALLS    /* disable float optimizations if functions with
                           float/double arguments are not implemented
                           in the back-end */

#include <math.h>
#include <errno.h>
#include "../Objects/pfloatobject.h"

static PyObject *cimpl_floor_to_int(double x) { return PyLong_FromDouble(floor(x)); }
static PyObject *cimpl_ceil_to_int(double x) { return PyLong_FromDouble(ceil(x)); }

/* ceil, floor: a float becomes an int; anything else (an int, or a type
   with __floor__/__ceil__) goes to the real function */
# define PMATH_TO_INT(func)\
static PyCFunction fallback_##func;\
static vinfo_t* pmath_##func(PsycoObject* po, vinfo_t* vself, vinfo_t* v) {\
    vinfo_t *a;\
    PyTypeObject *t = Psyco_NeedType(po, v);\
    if (t == NULL)\
        return NULL;\
    if (t != &PyFloat_Type)\
        return psyco_generic_call(po, fallback_##func, CfCommonNewRefPyObject,\
                                  "lv", NULL, v);\
    a = PsycoFloat_AS_DOUBLE(po, v); /* borrowed */\
    if (a == NULL)\
        return NULL;\
    return psyco_generic_call(po, cimpl_##func##_to_int, CfCommonNewRefPyObject,\
                              "d", a);\
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
        PyErr_SetString(PyExc_ValueError, "math domain error");
    else if (errno == ERANGE) {
        if (fabs(x) < 1.5)
            result = 0;
        else
            PyErr_SetString(PyExc_OverflowError, "math range error");
    }
    else
        PyErr_SetFromErrno(PyExc_ValueError);
    return result;
}

/* A one-argument function on doubles: math_1's code in a helper, and the
   meta that calls it. An infinite result from a finite argument raises
   inf_exc with inf_msg: OverflowError for the functions that can overflow
   (cosh, exp, sinh), ValueError for the others. cfunc_math_<func> is the C
   function, set by the registration. */
#define MATH_1(func, inf_exc, inf_msg)                                      \
static PyCFunction cfunc_math_##func;                                       \
                                                                            \
static int                                                                  \
cimpl_math_##func(double x, double *_r)                                     \
{                                                                           \
    double r;                                                               \
    errno = 0;                                                              \
    r = func(x);                                                            \
    if (isnan(r) && !isnan(x)) {                                            \
        PyErr_SetString(PyExc_ValueError, "math domain error");             \
        return -1;                                                          \
    }                                                                       \
    if (isinf(r) && isfinite(x)) {                                          \
        PyErr_SetString(inf_exc, inf_msg);                                  \
        return -1;                                                          \
    }                                                                       \
    if (isfinite(r) && errno && cimpl_is_error(r))                          \
        return -1;                                                          \
    *_r = r;                                                                \
    return 0;                                                               \
}                                                                           \
                                                                            \
static vinfo_t *                                                            \
math_##func(PsycoObject *po, vinfo_t *self, vinfo_t *args)                  \
{                                                                           \
    vinfo_array_t *result;                                                  \
    vinfo_t *x;                                                             \
    vinfo_t *r;                                                             \
    x = PsycoFloat_AsDouble(po, args);                                      \
    if (x == NULL) {                                                        \
        return NULL;                                                        \
    }                                                                       \
    result = array_new(1);                                                  \
    if (psyco_generic_call(po, cimpl_math_##func,                           \
                           CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da", \
                           x, result) == NULL) {                            \
        array_release(result);                                              \
        vinfo_decref(x, po);                                                \
        return NULL;                                                        \
    }                                                                       \
    r = result->items[0];                                                   \
    array_release(result);                                                  \
    r = PsycoFloat_FROM_DOUBLE(r);                                          \
    vinfo_decref(x, po);                                                    \
    return r;                                                               \
}

MATH_1(acos, PyExc_ValueError, "math domain error")
MATH_1(asin, PyExc_ValueError, "math domain error")
MATH_1(atan, PyExc_ValueError, "math domain error")
MATH_1(cos, PyExc_ValueError, "math domain error")
MATH_1(cosh, PyExc_OverflowError, "math range error")
MATH_1(exp, PyExc_OverflowError, "math range error")
MATH_1(fabs, PyExc_ValueError, "math domain error")
MATH_1(sin, PyExc_ValueError, "math domain error")
MATH_1(sinh, PyExc_OverflowError, "math range error")
MATH_1(sqrt, PyExc_ValueError, "math domain error")
MATH_1(tan, PyExc_ValueError, "math domain error")
MATH_1(tanh, PyExc_ValueError, "math domain error")

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
void psyco_initmath(void)
{
    PyObject* md = Psyco_DefineMetaModule("math");

    fallback_ceil = Psyco_DefineModuleFn(md, "ceil", METH_O, pmath_ceil);
    fallback_floor= Psyco_DefineModuleFn(md, "floor",METH_O, pmath_floor);
    gen_mathmodule_init(md);

    Py_XDECREF(md);
}

#else /* !HAVE_FP_FN_CALLS */
INITIALIZATIONFN
void psyco_initmath(void)
{
}
#endif /* !HAVE_FP_FN_CALLS */
