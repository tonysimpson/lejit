/*
 * Metas for member and method descriptor __get__, reached from
 * pobject.c:generic_getattr through Psyco_META3 with the argument format
 * "lvl": (po, PyObject *descr, vinfo_t *obj, PyTypeObject *type). They skip
 * descrobject.c's descr_check; generic_getattr uses them only when obj's
 * type is a subtype of the descriptor's d_type.
 */
#include "pdescrobject.h"
#include "pstructmember.h"
#include "pmethodobject.h"


static vinfo_t* pmember_get(PsycoObject* po, PyMemberDescrObject* descr,
			    vinfo_t* obj, PyTypeObject *type)
{
	/* a meta-implementation for member_get() of descrobject.c.
	   Note that not all parameters are 'vinfo_t*'; only 'obj'
	   is. This is because PsycoObject_GenericGetAttr() gives
	   immediate values for the other two arguments. */

	/* XXX We assume that 'obj' is a valid instance of 'type'. */
	return PsycoMember_GetOne(po, obj, descr->d_member);
}

static vinfo_t* pmethod_get(PsycoObject* po, PyMethodDescrObject* descr,
			    vinfo_t* obj, PyTypeObject *type)
{
	/* a meta-implementation for method_get() of descrobject.c.
	   Same remarks as for pmember_get(). */
	return PsycoCFunction_New(po, descr->d_method, obj);
}


INITIALIZATIONFN
void psy_descrobject_init(void)
{
	Psyco_DefineMeta(PyMemberDescr_Type.tp_descr_get,
			 pmember_get);
	Psyco_DefineMeta(PyMethodDescr_Type.tp_descr_get,
			 pmethod_get);
}
