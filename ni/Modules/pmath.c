/*
 * The math module's metas. The one-argument functions on doubles (acos,
 * sqrt, exp...) are generated from Modules/mathmodule.c (pmathmodule_gen.h):
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

#include "pmathmodule_gen.h"

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
