/*
 * Virtual bound methods ([iMETHOD_IM_FUNC] constant, [iMETHOD_IM_SELF]). The
 * tp_call meta prepends self as a virtual tuple and re-dispatches the call.
 * The branch for a NULL im_self can't run: Python 3 has no unbound methods.
 */
#include "pclassobject.h"
#include "ptupleobject.h"
#include "../initialize.h"

DEFINEVAR source_virtual_t psyco_computed_method;

/* PsycoMethod_New, compute_method and direct_compute_method: a virtual
   PyMethod_New */
/* PyMethod_New(func, self) on raw values */
static PyObject *
direct_compute_method(vinfo_t *v, char *data)
{
    PyObject *o = NULL;
    PyObject *func = direct_xobj_vinfo(v->array->items[iMETHOD_IM_FUNC], data);
    PyObject *self = direct_xobj_vinfo(v->array->items[iMETHOD_IM_SELF], data);
    if (func != NULL && self != NULL)
        o = PyMethod_New(func, self);
    Py_XDECREF(self);
    Py_XDECREF(func);
    return o;
}

/* forced: a constant if every field is one and CPython would share the
   object (rule allocator_pure), else PyMethod_New at run time */
static bool
compute_method(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *newobj = NULL;
    if (is_compiletime(v->array->items[iMETHOD_IM_FUNC]->source) &&
        is_compiletime(v->array->items[iMETHOD_IM_SELF]->source)) {
        PyObject *o = direct_compute_method(v, NULL);
        if (o == NULL)
            OUT_OF_MEMORY();
        newobj = psyco_shared_constant(o);
    }
    if (newobj == NULL) {
        newobj = psyco_generic_call(
            po, PyMethod_New, CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
            "vv", v->array->items[iMETHOD_IM_FUNC],
            v->array->items[iMETHOD_IM_SELF]);
        if (newobj == NULL)
            return false;
    }
    vinfo_move(po, v, newobj);
    return true;
}

/* a virtual method: PyMethod_New's arguments, each a new reference */
DEFINEFN
vinfo_t *
PsycoMethod_New(vinfo_t *func, vinfo_t *self)
{
    vinfo_t *v = vinfo_new(VirtualTime_New(&psyco_computed_method));
    v->array = array_new(METHOD_TOTAL);
    v->array->items[iOB_TYPE] =
        vinfo_new(CompileTime_New((long)&PyMethod_Type));
    extra_assert(func != NULL);
    vinfo_incref(func);
    v->array->items[iMETHOD_IM_FUNC] = func;
    extra_assert(self != NULL);
    vinfo_incref(self);
    v->array->items[iMETHOD_IM_SELF] = self;
    return v;
}

/* the force and direct functions, with their weights */
static void
gen_virtual_method_init(void)
{
    INIT_SVIRTUAL(psyco_computed_method, compute_method, direct_compute_method,
                  (1 << iMETHOD_IM_FUNC) | (1 << iMETHOD_IM_SELF), 0, 0);
}

/***************************************************************/
/*** instance method objects meta-implementation             ***/

DEFINEFN
vinfo_t *
pinstancemethod_call(PsycoObject *po, vinfo_t *methobj, vinfo_t *arg,
                     vinfo_t *kw)
{
    vinfo_t *im_func;
    vinfo_t *im_self;
    vinfo_t *result;
    condition_code_t cc;

    /* get the 'im_self' field from the Python object 'methobj' */
    im_self = psyco_get_const(po, methobj, METHOD_im_self);
    if (im_self == NULL)
        return NULL;

    cc = object_non_null(po, im_self);
    if (cc == CC_ERROR) /* error or more likely promotion */
        return NULL;

    if (!runtime_condition_t(po, cc)) {
        /* Unbound methods, XXX implement me */
        goto fallback;
    }
    else {
        int i, argcount;
        vinfo_t *newarg;
        if (PycException_Occurred(po))
            return NULL;

        argcount = PsycoTuple_Load(arg);
        if (argcount < 0)
            goto fallback;

        newarg = PsycoTuple_New(argcount + 1, NULL);
        vinfo_incref(im_self);
        PsycoTuple_GET_ITEM(newarg, 0) = im_self;
        for (i = 0; i < argcount; i++) {
            vinfo_t *v = PsycoTuple_GET_ITEM(arg, i);
            vinfo_incref(v);
            PsycoTuple_GET_ITEM(newarg, i + 1) = v;
        }
        arg = newarg;
    }

    im_func = psyco_get_const(po, methobj, METHOD_im_func);
    if (im_func == NULL)
        result = NULL;
    else
        result = PsycoObject_Call(po, im_func, arg, kw);
    vinfo_decref(arg, po);
    return result;

fallback:
    return psyco_residual_call(po, PyMethod_Type.tp_call, methobj, arg, kw);
}

INITIALIZATIONFN
void
psy_classobject_init(void)
{
    Psyco_DefineCallMeta(&PyMethod_Type, pinstancemethod_call);

    gen_virtual_method_init();
}
