/*
 * Metas for member and method descriptor __get__. They are currently
 * unreachable: nothing dispatches tp_descr_get through Psyco_META, because
 * attribute lookup (pobject.c:PsycoObject_GetAttr) is a plain residual call.
 * They do show the convention a translated _PyObject_GenericGetAttrWithDict
 * would use: (po, PyObject *descr, vinfo_t *obj, PyTypeObject *type), i.e. the
 * argument format "lvl".
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
