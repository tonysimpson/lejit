#include "pbltinmodule.h"
#include "../Objects/plistobject.h"
#include "../Objects/plongobject.h"
#include "../Objects/pmethodobject.h"
#include "../Objects/pstringobject.h"
#include "../Objects/ptupleobject.h"

static PyCFunction cimpl_range;
static PyCFunction cimpl_id;
static PyCFunction cimpl_len;
static PyCFunction cimpl_abs;
static PyCFunction cimpl_apply;
static PyCFunction cimpl_divmod;

static vinfo_t *get_len_of_range(PsycoObject *po, vinfo_t *lo, vinfo_t *hi
                                 /*, vinfo_t* step == 1 currently*/) {
  /* translated from bltinmodule.c */
  condition_code_t cc = integer_cmp(po, lo, hi, Py_LT);
  if (cc == CC_ERROR)
    return NULL;
  if (runtime_condition_t(po, cc)) {
    vinfo_t *vresult = integer_sub(po, hi, lo, false);
    assert_nonneg(vresult);
    return vresult;
  } else
    return psyco_vi_Zero();
}

static vinfo_t *longobj_as_long(PsycoObject *po, vinfo_t *v) {
  if (Psyco_VerifyType(po, v, &PyLong_Type) == 1)
    return PsycoLong_AsLong(po, v);
  else
    return NULL;
}

static bool parse_range_args(PsycoObject *po, vinfo_t *vargs, vinfo_t **iistart,
                             vinfo_t **iilen) {
  vinfo_t *ilow;
  vinfo_t *ihigh;
  int tuplesize = PsycoTuple_Load(vargs); /* -1 if unknown */

  switch (tuplesize) {
  case 1:
    ihigh = longobj_as_long(po, PsycoTuple_GET_ITEM(vargs, 0));
    if (ihigh == NULL)
      return false;
    ilow = psyco_vi_Zero();
    vinfo_incref(ihigh);
    break;
    /*case 3:
            istep = intobj_as_long(po, PsycoTuple_GET_ITEM(vargs, 2));
            if (istep == NULL) return NULL;*/
    /* fall through */
  case 2:
    ilow = longobj_as_long(po, PsycoTuple_GET_ITEM(vargs, 0));
    if (ilow == NULL)
      return false;
    ihigh = longobj_as_long(po, PsycoTuple_GET_ITEM(vargs, 1));
    if (ihigh == NULL)
      return false;
    vinfo_incref(ilow);
    vinfo_incref(ihigh);
    break;
  default:
    return false;
  }
  *iilen = get_len_of_range(po, ilow, ihigh);
  vinfo_decref(ihigh, po);
  if (*iilen == NULL) {
    vinfo_decref(ilow, po);
    return false;
  }
  *iistart = ilow;
  return true;
}

static vinfo_t *pbuiltin_id(PsycoObject *po, vinfo_t *vself, vinfo_t *vobj) {
  return psyco_generic_call(po, PyLong_FromVoidPtr, CfCommonNewRefPyObject, "v",
                            vobj);
}

static vinfo_t *pbuiltin_len(PsycoObject *po, vinfo_t *vself, vinfo_t *vobj) {
  vinfo_t *result;
  result = PsycoObject_Size(po, vobj);
  if (result != NULL)
    result = PsycoLong_FROM_LONG(po, result);
  return result;
}

static vinfo_t *pbuiltin_abs(PsycoObject *po, vinfo_t *vself, vinfo_t *vobj) {
  return PsycoNumber_Absolute(po, vobj);
}

static vinfo_t *pbuiltin_apply(PsycoObject *po, vinfo_t *vself,
                               vinfo_t *vargs) {
  vinfo_t *alist = NULL;
  vinfo_t *kwdict = NULL;
  vinfo_t *retval;
  int tuplesize = PsycoTuple_Load(vargs); /* -1 if unknown */
  PyTypeObject *argt;
  vinfo_t *t = NULL;

  switch (tuplesize) {
  case 3:
    kwdict = PsycoTuple_GET_ITEM(vargs, 2);
    if (Psyco_VerifyType(po, kwdict, &PyDict_Type) != true) {
      /* 'kwdict' is not a dictionary */
      break;
    }
    /* fall through */
  case 2:
    alist = PsycoTuple_GET_ITEM(vargs, 1);
    argt = Psyco_NeedType(po, alist);
    if (argt == NULL)
      return NULL;
    if (!PyType_TypeCheck(argt, &PyTuple_Type)) {
      /* 'alist' is not a tuple */
      if (!PsycoSequence_Check(argt))
        break; /* give up */
      t = PsycoSequence_Tuple(po, alist);
      if (t == NULL)
        break; /* give up */
      alist = t;
    }
    /* fall through */
  case 1:
    retval = PsycoEval_CallObjectWithKeywords(po, PsycoTuple_GET_ITEM(vargs, 0),
                                              alist, kwdict);
    vinfo_xdecref(t, po);
    return retval;
  }

  if (PycException_Occurred(po))
    return NULL;
  return psyco_generic_call(po, cimpl_apply, CfCommonNewRefPyObject, "lv", NULL,
                            vargs);
}

static vinfo_t *pbuiltin_divmod(PsycoObject *po, vinfo_t *vself,
                                vinfo_t *vargs) {
  int tuplesize = PsycoTuple_Load(vargs); /* -1 if unknown */

  if (tuplesize == 2) {
    return PsycoNumber_Divmod(po, PsycoTuple_GET_ITEM(vargs, 0),
                              PsycoTuple_GET_ITEM(vargs, 1));
  }
  return PsycoCFunction_CallFast(po, cimpl_divmod, METH_FASTCALL, vself,
                                 vargs);
}

/***************************************************************/

INITIALIZATIONFN
void psyco_bltinmodule_init(void) {
  /* a meta is registered only if the builtin's ml_flags match (3.7's,
     from Python/bltinmodule.c and its clinic header); apply no longer
     exists. A METH_FASTCALL meta takes a tuple (pmethodobject.h). */
  PyObject *md = Psyco_DefineMetaModule("builtins");

#define DEFMETA(name, flags)                                                   \
  cimpl_##name = Psyco_DefineModuleFn(md, #name, flags, &pbuiltin_##name)
  DEFMETA(id, METH_O);
  DEFMETA(len, METH_O);
  DEFMETA(abs, METH_O);
  DEFMETA(apply, METH_VARARGS);
  DEFMETA(divmod, METH_FASTCALL);
#undef DEFMETA
  Py_XDECREF(md);
}
