/*
 * Metas for member and method descriptor __get__, reached from
 * pobject.c:generic_getattr through Psyco_META3 with the argument format
 * "lvl": (po, PyObject *descr, vinfo_t *obj, PyTypeObject *type). They skip
 * descrobject.c's descr_check; generic_getattr uses them only when obj's
 * type is a subtype of the descriptor's d_type (policy ('assume') for the
 * generated member_get). method_get stays here: its result is Ni's virtual
 * PyCFunction, which has no TYPES constructor.
 */
#include "pdescrobject.h"
#include "pstructmember.h"
#include "pmethodobject.h"


/* member_get (D89) */
#include "pdescrobject_gen.h"

static vinfo_t* pmethod_get(PsycoObject* po, PyMethodDescrObject* descr,
			    vinfo_t* obj, PyTypeObject *type)
{
	/* a meta-implementation for method_get() of descrobject.c.
	   Same remarks as at the top of the file. */
	return PsycoCFunction_New(po, descr->d_method, obj);
}


INITIALIZATIONFN
void psy_descrobject_init(void)
{
	gen_descrobject_init();
	Psyco_DefineMeta(PyMethodDescr_Type.tp_descr_get,
			 pmethod_get);
}
