/*
 * Metas for member and method descriptor __get__, reached from
 * pobject.c:generic_getattr through Psyco_META3 with the argument format
 * "lvl": (po, PyObject *descr, vinfo_t *obj, PyTypeObject *type). They skip
 * descrobject.c's descr_check; generic_getattr uses them only when obj's
 * type is a subtype of the descriptor's d_type (assumed by member_get,
 * below). method_get stays here: its result is LeJIT's virtual
 * PyCFunction, which has no TYPES constructor, or for a method taking its
 * defining class a pending bound method.
 */
#include "pdescrobject.h"
#include "pstructmember.h"
#include "pmethodobject.h"
#include "pabstract.h"
#include "pdictobject.h"
#include "ptupleobject.h"
#include "../initialize.h"

static vinfo_t *
member_get(PsycoObject *po, PyMemberDescrObject *descr, vinfo_t *obj,
           PyObject *type)
{
#if LE_HAS_AUDIT_HOOKS
    if (descr->d_member->flags & READ_RESTRICTED) {
        if (psyco_generic_call(
                po, PySys_Audit,
                CfNoReturnValue | CfReturnTypeInt | CfPyErrIfNeg, "llvl",
                (long)"object.__getattr__", (long)"Os", obj,
                (long)descr->d_member->name) == NULL) {
            return NULL;
        }
    }
#endif
    return PsycoMember_GetOne(po, obj, descr->d_member);
}

static void
gen_descrobject_init(void)
{
    Psyco_DefineMeta(PyMemberDescr_Type.tp_descr_get, member_get);
}

#ifdef METH_METHOD
/* A method that takes its defining class (METH_METHOD) bound to obj, not
   made yet: [iOB_TYPE] PyCMethod_Type, the descriptor, then obj. A call
   is the descriptor's vectorcall with obj first, as after ceval's method
   load (PsycoCMethod_Call); anything else makes the bound method that
   method_get makes. */
static source_virtual_t psyco_pending_cmethod;
#define iPENDING_DESCR (iOB_TYPE + 1)
#define iPENDING_SELF (iOB_TYPE + 2)

static PyObject *
cimpl_method_get(PyObject *descr, PyObject *obj)
{
    return PyMethodDescr_Type.tp_descr_get(descr, obj,
                                           (PyObject *)Py_TYPE(obj));
}

static bool
compute_pending_cmethod(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *m = psyco_generic_call(
        po, cimpl_method_get, CfCommonNewRefPyObject, "vv",
        v->array->items[iPENDING_DESCR], v->array->items[iPENDING_SELF]);
    if (m == NULL)
        return false;
    vinfo_array_shrink(po, v, iOB_TYPE + 1);
    vinfo_move(po, v, m);
    return true;
}

/* PyCMethod_Type's tp_call */
static vinfo_t *
PsycoCMethod_Call(PsycoObject *po, vinfo_t *func, vinfo_t *args, vinfo_t *kw)
{
    vinfo_t **kwvalues = NULL;
    PyObject *kwnames = NULL;
    vinfo_t *r;
    int n = PsycoTuple_Load(args);
    if (!is_virtualtime(func->source) ||
        VirtualTime_Get(func->source) != &psyco_pending_cmethod || n < 0)
        goto generic;
    if (kw != NULL && !psyco_known_NULL(kw)) {
        kwnames = PsycoDict_PendingNames(kw, &kwvalues);
        if (kwnames == NULL)
            goto generic;
    }
    r = psyco_vectorcall(po, func->array->items[iPENDING_DESCR],
                         func->array->items[iPENDING_SELF],
                         &PsycoTuple_GET_ITEM(args, 0), n, kwnames, kwvalues);
    if (r != NULL || PycException_Occurred(po))
        return r;
generic:
    return psyco_residual_call(po, PyObject_Call, func, args, kw);
}
#endif

static vinfo_t *
pmethod_get(PsycoObject *po, PyMethodDescrObject *descr, vinfo_t *obj,
            PyTypeObject *type)
{
    /* a meta-implementation for method_get() of descrobject.c.
       Same remarks as at the top of the file. */
#ifdef METH_METHOD
    /* a method that takes its defining class is a PyCMethod, which the
       virtual PyCFunction does not hold */
    if (descr->d_method->ml_flags & METH_METHOD) {
        vinfo_t *v = vinfo_new(VirtualTime_New(&psyco_pending_cmethod));
        v->array = array_new(iPENDING_SELF + 1);
        v->array->items[iOB_TYPE] =
            vinfo_new(CompileTime_New((long)&PyCMethod_Type));
        /* the compiled code holds the descriptor (generic_getattr) */
        v->array->items[iPENDING_DESCR] =
            vinfo_new(CompileTime_New((long)descr));
        vinfo_incref(obj);
        v->array->items[iPENDING_SELF] = obj;
        return v;
    }
#endif
    return PsycoCFunction_New(po, descr->d_method, obj);
}

INITIALIZATIONFN
void
psy_descrobject_init(void)
{
    gen_descrobject_init();
    Psyco_DefineMeta(PyMethodDescr_Type.tp_descr_get, pmethod_get);
#ifdef METH_METHOD
    /* a bound method holds the pending one's descriptor and obj */
    INIT_SVIRTUAL_NOCALL(psyco_pending_cmethod, compute_pending_cmethod, 1);
    Psyco_DefineCallMeta(&PyCMethod_Type, PsycoCMethod_Call);
#endif
}
