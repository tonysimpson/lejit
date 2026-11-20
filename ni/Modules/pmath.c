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

/* the C functions, for the fallbacks: set by the registration */
static PyCFunction cfunc_math_acos;
static PyCFunction cfunc_math_asin;
static PyCFunction cfunc_math_atan;
static PyCFunction cfunc_math_cos;
static PyCFunction cfunc_math_cosh;
static PyCFunction cfunc_math_exp;
static PyCFunction cfunc_math_fabs;
static PyCFunction cfunc_math_sin;
static PyCFunction cfunc_math_sinh;
static PyCFunction cfunc_math_sqrt;
static PyCFunction cfunc_math_tan;
static PyCFunction cfunc_math_tanh;

/* run-time copies: where a heap type's slot changed, or where the meta has no compile-time form, CPython's algorithm resumes in these */
static int
cimpl_is_error(double x)
{
    int result = 1;
    assert(errno);
    if (errno == EDOM)
        PyErr_SetString(PyExc_ValueError, "math domain error");
    else if (errno == ERANGE) {
#if NI_HAS_MATH_UNDERFLOW_BELOW_1_5
        if (fabs(x) < 1.5)
#else
        if (fabs(x) < 1.0)
#endif
            result = 0;
        else
            PyErr_SetString(PyExc_OverflowError, "math range error");
    }
    else
        PyErr_SetFromErrno(PyExc_ValueError);
    return result;
}

/* Modules/mathmodule.c:math_acos: scalar code */
static int
cimpl_math_acos(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*acos)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (0)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_asin: scalar code */
static int
cimpl_math_asin(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*asin)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (0)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_atan: scalar code */
static int
cimpl_math_atan(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*atan)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (0)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_cos: scalar code */
static int
cimpl_math_cos(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*cos)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (0)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_cosh: scalar code */
static int
cimpl_math_cosh(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*cosh)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (1)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_exp: scalar code */
static int
cimpl_math_exp(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*exp)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (1)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_fabs: scalar code */
static int
cimpl_math_fabs(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*fabs)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (0)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_sin: scalar code */
static int
cimpl_math_sin(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*sin)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (0)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_sinh: scalar code */
static int
cimpl_math_sinh(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*sinh)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (1)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_sqrt: scalar code */
static int
cimpl_math_sqrt(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*sqrt)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (0)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_tan: scalar code */
static int
cimpl_math_tan(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*tan)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (0)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

/* Modules/mathmodule.c:math_tanh: scalar code */
static int
cimpl_math_tanh(double math_1_to_whatever_x, double *_r)
{
    double math_1_to_whatever_r;
    *__errno_location() = 0;
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    math_1_to_whatever_r = (*tanh)(math_1_to_whatever_x);
#if NI_HAS_PYFPE_PROTECT
    ;
#endif
    if (__builtin_isnan(math_1_to_whatever_r) &&
        (!__builtin_isnan(math_1_to_whatever_x))) {
        PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if (__builtin_isinf_sign(math_1_to_whatever_r) &&
        __builtin_isfinite(math_1_to_whatever_x)) {
        if (0)
            PyErr_SetString(PyExc_OverflowError, "math range error");
        else
            PyErr_SetString(PyExc_ValueError, "math domain error");
        return -1;
    }
    if ((__builtin_isfinite(math_1_to_whatever_r) && (*__errno_location())) &&
        cimpl_is_error(math_1_to_whatever_r))
        return -1;
    {
        *_r = math_1_to_whatever_r;
        return 0;
    }
}

static vinfo_t *
math_acos(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_asin(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_atan(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_cos(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_cosh(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_exp(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_fabs(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_sin(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_sinh(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_sqrt(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_tan(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
math_tanh(PsycoObject *po, vinfo_t *, vinfo_t *);

static vinfo_t *
math_acos(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_acos,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_asin(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_asin,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_atan(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_atan,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_cos(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_cos,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_cosh(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_cosh,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_exp(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_exp,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_fabs(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_fabs,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_sin(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_sin,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_sinh(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_sinh,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_sqrt(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_sqrt,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_tan(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_tan,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

static vinfo_t *
math_tanh(PsycoObject *po, vinfo_t *self, vinfo_t *args)
{
    vinfo_t *_t1;
    vinfo_array_t *_t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *math_1_to_whatever_x;
    _t1 = PsycoFloat_AsDouble(po, args);
    math_1_to_whatever_x = _t1;
    if (math_1_to_whatever_x == NULL) {
        return NULL;
    }
    _t2 = array_new(1);
    _t3 = psyco_generic_call(po, cimpl_math_tanh,
                             CfCommonIntZeroOk | CfNoPython | CfNoBlock, "da",
                             math_1_to_whatever_x, _t2);
    if (_t3 == NULL) {
        array_release(_t2);
        vinfo_decref(math_1_to_whatever_x, po);
        return NULL;
    }
    _t4 = _t2->items[0];
    array_release(_t2);
    _t5 = PsycoFloat_FROM_DOUBLE(_t4);
    _t6 = _t5;
    vinfo_decref(math_1_to_whatever_x, po);
    return _t6;
}

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
