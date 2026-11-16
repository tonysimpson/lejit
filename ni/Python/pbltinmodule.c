#include "pbltinmodule.h"
#include "../Objects/pboolobject.h"
#include "../Objects/pfloatobject.h"
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
static PyCFunction cimpl_isinstance;
static PyCFunction cimpl_round;
static ternaryfunc type_call;
static PyObject *object_class_descr; /* object.__class__ */
static PyObject *str___class__;

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

/* builtin_isinstance: Objects/abstract.c:PyObject_IsInstance decided at
   compile time from the promoted type of obj and the promoted class (or
   tuple of classes). 1 or 0 if known, -1 if it must be asked at run time.
   Only the rules for an exact 'type' cls are folded; a metaclass's
   __instancecheck__ is never guessed. A heap type's subtype relation can
   change (__bases__ assignment) and so is never folded; a static type's
   can't. A false answer also needs obj.__class__ to be the ordinary
   object.__class__, which recursive_isinstance consults next. */
static int isinstance_known(PyTypeObject *tp, PyObject *cls, int depth) {
  if ((PyObject *)tp == cls)
    return 1;
  if (PyTuple_Check(cls)) {
    Py_ssize_t i;
    if (depth > 8) /* CPython counts recursion here */
      return -1;
    for (i = 0; i < PyTuple_GET_SIZE(cls); i++) {
      int r = isinstance_known(tp, PyTuple_GET_ITEM(cls, i), depth + 1);
      if (r != 0)
        return r; /* found, or unknown: then CPython decides it all */
    }
    return 0;
  }
  if (!PyType_CheckExact(cls) || (tp->tp_flags & Py_TPFLAGS_HEAPTYPE))
    return -1;
  if (PyType_IsSubtype(tp, (PyTypeObject *)cls))
    return 1;
  if (tp->tp_getattro == PyObject_GenericGetAttr &&
      _PyType_Lookup(tp, str___class__) == object_class_descr)
    return 0;
  return -1;
}

/* the same for a class given as a vinfo: a virtual tuple (built here) is
   walked item by item, any other run-time tuple is left to CPython, and
   anything else (a class: long-lived) is promoted. Sets *r; returns false
   on error or promotion. */
static bool isinstance_known_v(PsycoObject *po, PyTypeObject *tp,
                               vinfo_t *vcls, int depth, int *r) {
  PyObject *cls;
  int m;
  if (Psyco_KnownType(vcls) == &PyTuple_Type && !is_compiletime(vcls->source) &&
      PsycoTuple_Load(vcls) >= 0) {
    int i, n = PsycoTuple_Load(vcls);
    *r = 0;
    if (depth > 8)
      *r = -1;
    for (i = 0; i < n && *r == 0; i++)
      if (!isinstance_known_v(po, tp, PsycoTuple_GET_ITEM(vcls, i), depth + 1,
                              r))
        return false;
    return true;
  }
  if (!is_compiletime(vcls->source)) {
    /* a tuple made at run time is a new object each time: promoting it
       would compile a new version per call (up to MEGAMORPHIC_MAX) */
    PyTypeObject *ctp = Psyco_NeedType(po, vcls);
    if (ctp == NULL)
      return false;
    if (PyType_IsSubtype(ctp, &PyTuple_Type)) {
      *r = -1;
      return true;
    }
  }
  m = psyco_pyobj_atcompiletime_mega(po, vcls, &cls);
  if (m < 0)
    return false;
  *r = m == 0 ? -1 : isinstance_known(tp, cls, depth);
  return true;
}

static vinfo_t *pbuiltin_isinstance(PsycoObject *po, vinfo_t *vself,
                                    vinfo_t *vargs) {
  vinfo_t *vobj, *vcls, *x;
  PyTypeObject *tp;
  int r;
  if (PsycoTuple_Load(vargs) != 2)
    return PsycoCFunction_CallFast(po, cimpl_isinstance, METH_FASTCALL, vself,
                                   vargs);
  vobj = PsycoTuple_GET_ITEM(vargs, 0);
  vcls = PsycoTuple_GET_ITEM(vargs, 1);
  tp = Psyco_NeedType(po, vobj);
  if (tp == NULL)
    return NULL;
  if (!isinstance_known_v(po, tp, vcls, 0, &r))
    return NULL;
  if (r == 1)
    return psyco_vi_Py_True();
  if (r == 0)
    return psyco_vi_Py_False();
  x = psyco_generic_call(po, PyObject_IsInstance, CfCommonIntBoolOrError,
                         "vv", vobj, vcls);
  return x != NULL ? PsycoBool_FROM_WORD(x) : NULL;
}

/* An exact float's whole part (trunc or round-half-even) as an int, or
   NULL in *out (with true returned) if it doesn't fit a long: then the
   caller asks CPython, which raises for NaN and infinities or builds a
   big int. */
static bool float_whole_to_int(PsycoObject *po, vinfo_t *vf, bool round_even,
                               vinfo_t **out) {
  vinfo_t *vd, *vw;
  int r;
  *out = NULL;
  vd = PsycoFloat_AS_DOUBLE(po, vf); /* borrowed */
  if (vd == NULL)
    return false;
  vw = round_even ? PsycoFloat_RoundEven(po, vd) : PsycoFloat_Trunc(po, vd);
  if (vw == NULL)
    return false;
  r = PsycoFloat_WholeToLong(po, vw, out);
  vinfo_decref(vw, po);
  return r >= 0;
}

/* builtin_round (Python/bltinmodule.c) without ndigits calls
   type(number).__round__(): for an exact int that is long_round ->
   long_long, the same object; for an exact float float___round___impl,
   round half to even then PyLong_FromDouble. */
static vinfo_t *pbuiltin_round(PsycoObject *po, vinfo_t *vself,
                               vinfo_t *vargs) {
  vinfo_t *v, *x;
  PyTypeObject *tp;
  if (PsycoTuple_Load(vargs) == 1) {
    v = PsycoTuple_GET_ITEM(vargs, 0);
    tp = Psyco_NeedType(po, v);
    if (tp == NULL)
      return NULL;
    if (tp == &PyLong_Type) {
      vinfo_incref(v);
      return v;
    }
    if (tp == &PyFloat_Type) {
      if (!float_whole_to_int(po, v, true, &x))
        return NULL;
      if (x != NULL)
        return x;
    }
  }
  return PsycoCFunction_CallFast(po, cimpl_round, METH_FASTCALL | METH_KEYWORDS,
                                 vself, vargs);
}

/* type_call (Objects/typeobject.c) for type(x), int(x) and float(x) with
   one positional argument of an exact type; everything else, including
   every class instantiation, is the residual type_call as before.
   type(x): the "ugly exception", the type itself. int(x): long_new_impl ->
   PyNumber_Long, which returns an exact int unchanged and uses nb_int
   (float___trunc___impl) for a float. float(x): float_new_impl ->
   PyNumber_Float, which returns an exact float unchanged and uses nb_float
   (long_float -> PyLong_AsDouble) for an int. The callee is compared with
   the three types at run time rather than promoted: a promoted class
   would be kept alive by the compiled code (freeing.namedtuple_code_dies).
   The comparisons only depend on earlier ones, as respawn requires. */
static vinfo_t *ptype_call(PsycoObject *po, vinfo_t *vtype, vinfo_t *varg,
                           vinfo_t *vkw) {
  static PyTypeObject *const handled[] = {&PyLong_Type, &PyFloat_Type,
                                           &PyType_Type};
  PyObject *t = NULL;
  PyTypeObject *tp;
  vinfo_t *v, *x;
  int i;
  if (!psyco_knowntobe(vkw, 0) || PsycoTuple_Load(varg) != 1)
    goto fallback;
  for (i = 0; i < 3 && t == NULL; i++) {
    condition_code_t cc =
        integer_cmp_i(po, vtype, (long)handled[i], Py_EQ);
    if (cc == CC_ERROR)
      return NULL;
    if (runtime_condition_f(po, cc))
      t = (PyObject *)handled[i];
  }
  if (t == NULL)
    goto fallback;
  v = PsycoTuple_GET_ITEM(varg, 0);
  if (t == (PyObject *)&PyType_Type)
    return psyco_get_field(po, v, OB_type);
  tp = Psyco_NeedType(po, v);
  if (tp == NULL)
    return NULL;
  if ((PyObject *)tp == t) {
    vinfo_incref(v);
    return v;
  }
  if (t == (PyObject *)&PyLong_Type && tp == &PyFloat_Type) {
    if (!float_whole_to_int(po, v, false, &x))
      return NULL;
    if (x != NULL)
      return x;
  }
  if (t == (PyObject *)&PyFloat_Type && tp == &PyLong_Type) {
    if (!PsycoLong_AsDouble(po, v, &x))
      return NULL;
    return PsycoFloat_FROM_DOUBLE(x);
  }
fallback:
  return psyco_generic_call(po, type_call, CfCommonNewRefPyObject, "vvv",
                            vtype, varg, vkw);
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
  DEFMETA(isinstance, METH_FASTCALL);
  DEFMETA(round, METH_FASTCALL | METH_KEYWORDS);
  type_call = PyType_Type.tp_call;
  Psyco_DefineMeta(type_call, ptype_call);
  str___class__ = PyUnicode_InternFromString("__class__");
  object_class_descr = _PyType_Lookup(&PyBaseObject_Type, str___class__);
#undef DEFMETA
  Py_XDECREF(md);
}
