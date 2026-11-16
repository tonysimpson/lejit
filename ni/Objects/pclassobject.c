/*
 * Virtual bound methods ([iMETHOD_IM_FUNC] constant, [iMETHOD_IM_SELF]). The
 * tp_call meta prepends self as a virtual tuple and re-dispatches the call.
 * The unbound-method branch is a Python 2 leftover.
 */
#include "pclassobject.h"
#include "ptupleobject.h"

DEFINEFN
vinfo_t *PsycoMethod_New(PyObject *func, vinfo_t *self) {
  vinfo_t *result = vinfo_new(VirtualTime_New(&psyco_computed_method));

  result->array = array_new(METHOD_TOTAL);
  result->array->items[iOB_TYPE] =
      vinfo_new(CompileTime_New((long)(&PyMethod_Type)));

  Py_INCREF(func);
  result->array->items[iMETHOD_IM_FUNC] =
      vinfo_new(CompileTime_NewSk(sk_new((long)func, SkFlagPyObj)));

  vinfo_incref(self);
  result->array->items[iMETHOD_IM_SELF] = self;

  return result;
}

static bool compute_method(PsycoObject *po, vinfo_t *methobj) {
  vinfo_t *newobj;
  vinfo_t *im_func;
  vinfo_t *im_self;

  /* get the fields from the Python object 'methobj' */
  im_func = vinfo_getitem(methobj, iMETHOD_IM_FUNC);
  if (im_func == NULL)
    return false;
  im_self = vinfo_getitem(methobj, iMETHOD_IM_SELF);
  if (im_self == NULL)
    return false;

  /* call PyMethod_New() */
  newobj = psyco_generic_call(po, PyMethod_New, CfPure | CfCommonNewRefPyObject,
                              "vv", im_func, im_self);
  if (newobj == NULL)
    return false;

  /* move the resulting non-virtual Python object back into 'methobj' */
  vinfo_move(po, methobj, newobj);
  return true;
}

static PyObject *direct_compute_method(vinfo_t *methobj, char *data) {
  PyObject *im_func;
  PyObject *im_self;
  PyObject *result = NULL;

  im_func = direct_xobj_vinfo(vinfo_getitem(methobj, iMETHOD_IM_FUNC), data);
  im_self = direct_xobj_vinfo(vinfo_getitem(methobj, iMETHOD_IM_SELF), data);

  if (!PyErr_Occurred() && im_func != NULL)
    result = PyMethod_New(im_func, im_self);

  Py_XDECREF(im_self);
  Py_XDECREF(im_func);
  return result;
}

DEFINEVAR source_virtual_t psyco_computed_method;

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

  INIT_SVIRTUAL(psyco_computed_method, compute_method, direct_compute_method,
                (1 << iMETHOD_IM_FUNC) | (1 << iMETHOD_IM_SELF), 0, 0);
}
