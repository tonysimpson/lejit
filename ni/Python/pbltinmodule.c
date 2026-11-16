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
static PyCFunction cimpl_hash;
static PyCFunction cimpl_min;
static PyCFunction cimpl_max;
static PyCFunction cimpl_pow;

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

/* Python/bltinmodule.c:builtin_hash */
static vinfo_t *pbuiltin_hash(PsycoObject *po, vinfo_t *vself, vinfo_t *vobj) {
  vinfo_t *x = PsycoObject_Hash(po, vobj);
  return x != NULL ? PsycoLong_FROM_LONG(po, x) : NULL;
}

/* bltinmodule.c:min_max with two or more positional arguments: compare in
   order, keeping the first extreme item, as CPython does. Each comparison
   picks a path at run time (respawn), so a virtual result stays virtual.
   Every type is promoted before the first comparison: a promotion restarts
   the instruction, which would run a comparison already emitted again
   (a user __gt__ twice). One iterable argument, or key=/default=, goes to
   CPython: walking a sequence of run-time length can't be compiled (the
   NFS branch tried). */
static vinfo_t *pbuiltin_min_max(PsycoObject *po, vinfo_t *vargs,
                                 vinfo_t *vkw, int op, PyCFunction cimpl) {
  int i, n = PsycoTuple_Load(vargs);
  vinfo_t *best, *item, *cmp;
  if (n < 2 || !psyco_knowntobe(vkw, (long)NULL))
    return psyco_generic_call(po, cimpl, CfCommonNewRefPyObject, "lvv", NULL,
                              vargs, vkw);
  for (i = 0; i < n; i++)
    if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(vargs, i)) == NULL)
      return NULL;
  best = PsycoTuple_GET_ITEM(vargs, 0);
  for (i = 1; i < n; i++) {
    item = PsycoTuple_GET_ITEM(vargs, i);
    cmp = PsycoObject_RichCompareBool(po, item, best, op);
    if (cmp == NULL)
      return NULL;
    switch (runtime_NON_NULL_f(po, cmp)) { /* consumes cmp */
    case true:
      best = item;
      break;
    case false:
      break;
    default:
      return NULL;
    }
  }
  vinfo_incref(best);
  return best;
}

static vinfo_t *pbuiltin_min(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs,
                             vinfo_t *vkw) {
  return pbuiltin_min_max(po, vargs, vkw, Py_LT, cimpl_min);
}

static vinfo_t *pbuiltin_max(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs,
                             vinfo_t *vkw) {
  return pbuiltin_min_max(po, vargs, vkw, Py_GT, cimpl_max);
}

/* builtin_pow: PyNumber_Power(x, y, z=None) */
static vinfo_t *pbuiltin_pow(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs) {
  vinfo_t *x, *z;
  int n = PsycoTuple_Load(vargs);
  if (n == 2) {
    z = psyco_vi_None();
    x = PsycoNumber_Power(po, PsycoTuple_GET_ITEM(vargs, 0),
                          PsycoTuple_GET_ITEM(vargs, 1), z);
    vinfo_decref(z, po);
    return x;
  }
  if (n == 3)
    return PsycoNumber_Power(po, PsycoTuple_GET_ITEM(vargs, 0),
                             PsycoTuple_GET_ITEM(vargs, 1),
                             PsycoTuple_GET_ITEM(vargs, 2));
  return PsycoCFunction_CallFast(po, cimpl_pow, METH_FASTCALL, vself, vargs);
}

/***************************************************************/

INITIALIZATIONFN
void psyco_bltinmodule_init(void) {
  /* a meta is registered only if the builtin's ml_flags match (3.7's,
     from Python/bltinmodule.c and its clinic header); apply no longer
     exists. A METH_FASTCALL meta takes a tuple (pmethodobject.h). No meta
     for iter(): PsycoObject_GetIter gives lists a virtual seq-iterator,
     right for a for loop but the wrong type for iter() to hand out. */
  PyObject *md = Psyco_DefineMetaModule("builtins");

#define DEFMETA(name, flags)                                                   \
  cimpl_##name = Psyco_DefineModuleFn(md, #name, flags, &pbuiltin_##name)
  DEFMETA(id, METH_O);
  DEFMETA(len, METH_O);
  DEFMETA(abs, METH_O);
  DEFMETA(apply, METH_VARARGS);
  DEFMETA(divmod, METH_FASTCALL);
  DEFMETA(hash, METH_O);
  DEFMETA(min, METH_VARARGS | METH_KEYWORDS);
  DEFMETA(max, METH_VARARGS | METH_KEYWORDS);
  DEFMETA(pow, METH_FASTCALL);
#undef DEFMETA
  Py_XDECREF(md);
}
