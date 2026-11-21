/*
 * Metas for member and method descriptor __get__, reached from
 * pobject.c:generic_getattr through Psyco_META3 with the argument format
 * "lvl": (po, PyObject *descr, vinfo_t *obj, PyTypeObject *type). They skip
 * descrobject.c's descr_check; generic_getattr uses them only when obj's
 * type is a subtype of the descriptor's d_type (assumed by member_get,
 * below). method_get stays here: its result is LeJIT's virtual
 * PyCFunction, which has no TYPES constructor.
 */
#include "pdescrobject.h"
#include "pstructmember.h"
#include "pmethodobject.h"

static vinfo_t *
member_get(PsycoObject *po, PyMemberDescrObject *, vinfo_t *, PyObject *);

static vinfo_t *
member_get(PsycoObject *po, PyMemberDescrObject *descr, vinfo_t *obj,
           PyObject *type)
{
    vinfo_t *_t1;
#if LE_HAS_AUDIT_HOOKS
    if (((descr->d_member)->flags & 2)) {
        if (psyco_generic_call(
                po, PySys_Audit,
                CfNoReturnValue | CfReturnTypeInt | CfPyErrIfNeg, "llvl",
                (long)("object.__getattr__"), (long)("Os"), obj,
                (long)((descr->d_member)->name)) == NULL) {
            return NULL;
        }
    }
#endif
    _t1 = PsycoMember_GetOne(po, obj, descr->d_member);
    return _t1;
}

static void
gen_descrobject_init(void)
{
    Psyco_DefineMeta(PyMemberDescr_Type.tp_descr_get, member_get);
}

static vinfo_t *
pmethod_get(PsycoObject *po, PyMethodDescrObject *descr, vinfo_t *obj,
            PyTypeObject *type)
{
    /* a meta-implementation for method_get() of descrobject.c.
       Same remarks as at the top of the file. */
#ifdef METH_METHOD
    /* a method that takes its defining class is a PyCMethod, which the
       virtual PyCFunction does not hold */
    if (descr->d_method->ml_flags & METH_METHOD)
        return psyco_generic_call(po, PyMethodDescr_Type.tp_descr_get,
                                  CfCommonNewRefPyObject, "lvl", descr, obj,
                                  type);
#endif
    return PsycoCFunction_New(po, descr->d_method, obj);
}

INITIALIZATIONFN
void
psy_descrobject_init(void)
{
    gen_descrobject_init();
    Psyco_DefineMeta(PyMethodDescr_Type.tp_descr_get, pmethod_get);
}
