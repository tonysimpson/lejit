/*
 * Virtual bound methods ([iMETHOD_IM_FUNC] constant, [iMETHOD_IM_SELF]). The
 * tp_call meta prepends self as a virtual tuple and re-dispatches the call.
 * The branch for a NULL im_self can't run: Python 3 has no unbound methods.
 */
#include "pclassobject.h"
#include "ptupleobject.h"

DEFINEVAR source_virtual_t psyco_computed_method;

/* PsycoMethod_New, compute_method and direct_compute_method, generated
   from PyMethod_New and policy_table.TYPES */
#include "pclassobject_virtual_gen.h"

/***************************************************************/
/*** instance method objects meta-implementation             ***/

DEFINEFN
vinfo_t *pinstancemethod_call(PsycoObject *po, vinfo_t *methobj, vinfo_t *arg,
                              vinfo_t *kw) {
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
  } else {
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
  return psyco_generic_call(po, PyMethod_Type.tp_call, CfCommonNewRefPyObject,
                            "vvv", methobj, arg, kw);
}

INITIALIZATIONFN
void psy_classobject_init(void) {
  Psyco_DefineMeta(PyMethod_Type.tp_call, pinstancemethod_call);

  gen_virtual_method_init();
}
