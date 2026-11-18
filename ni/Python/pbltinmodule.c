#include "pbltinmodule.h"
#include "../Objects/pboolobject.h"
#include "../Objects/pfloatobject.h"
#include "../Objects/plistobject.h"
#include "../Objects/plongobject.h"
#include "../Objects/pmethodobject.h"
#include "../Objects/prangeobject.h"
#include "../Objects/pstringobject.h"
#include "../Objects/ptupleobject.h"
#include "../psyfunc.h"
#include "../mergepoints.h"

/* the metas tools/metac/gen_builtins.py writes from Python/bltinmodule.c
   (policy_table.TRANSLATED_FUNCTIONS): abs, len, hash, id, divmod, pow,
   isinstance, issubclass, getattr, setattr; gen_bltinmodule_init
   registers them. The rest of this file is what stays by hand, each with
   its reason (DECISIONS D86). */
#include "pbltinmodule_gen.h"

static PyCFunction cimpl_globals;
static PyCFunction cimpl_min;
static PyCFunction cimpl_max;
static PyCFunction cimpl_round;
static PyCFunction cimpl_sum;
static PyCFunction cimpl_any;
static PyCFunction cimpl_all;

/* a pending generator expression (pfuncobject.h) consumed whole by a
   builtin: run its code fused with the consumer's step, so neither the
   generator nor its frame exists (psyfunc.c:psyco_call_genexpr). 'start'
   is sum()'s, or NULL. */
static vinfo_t *genexpr_consume(PsycoObject *po, vinfo_t *vgen, int mode,
                                PyTypeObject *tp, vinfo_t *start) {
  vinfo_t *acc, *result, *t;
  if (mode == GENEXPR_LIST)
    acc = psyco_generic_call(po, PyList_New, CfCommonNewRefPyObject, "l", 0L);
  else if (mode == GENEXPR_SET)
    acc = psyco_generic_call(po, PySet_New, CfCommonNewRefPyObject, "l", 0L);
  else if (mode == GENEXPR_SUM) {
    if (start != NULL) {
      vinfo_incref(start);
      acc = start;
    } else
      acc = PsycoLong_FROM_LONG(po, psyco_vi_Zero());
  } else
    acc = psyco_vi_Zero();
  if (acc == NULL)
    return NULL;
  result = psyco_call_genexpr(po, vgen, mode, acc);
  vinfo_decref(acc, po);
  if (result != NULL && tp == &PyTuple_Type) {
    t = psyco_generic_call(po, PyList_AsTuple, CfCommonNewRefPyObject, "v",
                           result);
    vinfo_decref(result, po);
    result = t;
  }
  if (result != NULL && tp != NULL)
    Psyco_AssertType(po, result, tp);
  return result;
}

static vinfo_t *pbuiltin_any(PsycoObject *po, vinfo_t *vself, vinfo_t *vobj) {
  switch (psyco_genexpr_fuse(po, vobj)) {
  case 1:
    return genexpr_consume(po, vobj, GENEXPR_ANY, &PyBool_Type, NULL);
  case -1:
    return NULL;
  }
  return psyco_generic_call(po, cimpl_any, CfCommonNewRefPyObject, "vv",
                            vself, vobj);
}

static vinfo_t *pbuiltin_all(PsycoObject *po, vinfo_t *vself, vinfo_t *vobj) {
  switch (psyco_genexpr_fuse(po, vobj)) {
  case 1:
    return genexpr_consume(po, vobj, GENEXPR_ALL, &PyBool_Type, NULL);
  case -1:
    return NULL;
  }
  return psyco_generic_call(po, cimpl_all, CfCommonNewRefPyObject, "vv",
                            vself, vobj);
}
static ternaryfunc type_call;
static PyObject *object_class_descr; /* object.__class__ */
static PyObject *str___class__;

/* globals() is the compiled function's own globals: the real builtin reads
   the nearest interpreted frame's (F47) */
static vinfo_t *pbuiltin_globals(PsycoObject *po, vinfo_t *vself,
                                 vinfo_t *vnull) {
  vinfo_incref(LOC_GLOBALS);
  return LOC_GLOBALS;
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
  int fuse = 0;
  if (n == 1 && psyco_knowntobe(vkw, (long)NULL) &&
      (fuse = psyco_genexpr_fuse(po, PsycoTuple_GET_ITEM(vargs, 0))) < 0)
    return NULL;
  if (fuse)
    return genexpr_consume(po, PsycoTuple_GET_ITEM(vargs, 0),
                           op == Py_LT ? GENEXPR_MIN : GENEXPR_MAX, NULL,
                           NULL);
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
   walked item by item; otherwise only a static type is promoted. Anything
   else at run time (a heap class, which a constant would keep alive, or a
   tuple made elsewhere, a new object each call) is left to CPython. Sets
   *r; returns false on error or promotion. */
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
  m = psyco_atcompiletime_static_type(po, vcls, &cls);
  if (m < 0)
    return false;
  *r = m == 0 ? -1 : isinstance_known(tp, cls, depth);
  return true;
}

/* Objects/abstract.c:PyObject_IsInstance's meta, by hand: the answer at
   compile time when isinstance_known gives one (policy_table's const_fold
   shortcut, its depth limit, never for a heap type), else the real call.
   Hand-written because the shortcut is judgement: the C has no
   compile-time form of it. A 0 or 1, or NULL on error. */
DEFINEFN
vinfo_t *PsycoObject_IsInstance(PsycoObject *po, vinfo_t *vobj,
                                vinfo_t *vcls) {
  vinfo_t *x;
  PyTypeObject *tp;
  int r;
  /* isinstance_known never folds for a heap type, so only a static type is
     promoted: a constant heap type would keep the class alive, and a class
     made per call would compile a new path each time */
  if (is_compiletime(vobj->source) && !Psyco_ConstantClassMutable(vobj)) {
    tp = Py_TYPE((PyObject *)CompileTime_Get(vobj->source)->value);
    r = 1;
  } else {
    x = psyco_get_const(po, vobj, OB_type);
    if (x == NULL)
      return NULL;
    r = psyco_atcompiletime_static_type(po, x, (PyObject **)&tp);
    if (r < 0)
      return NULL;
  }
  if (r == 0)
    r = -1;
  else if (!isinstance_known_v(po, tp, vcls, 0, &r))
    return NULL;
  if (r == 1)
    return psyco_vi_One();
  if (r == 0)
    return psyco_vi_Zero();
  return psyco_generic_call(po, PyObject_IsInstance, CfCommonIntBoolOrError,
                            "vv", vobj, vcls);
}

/* builtin_sum_impl (Python/bltinmodule.c) is a left fold of PyNumber_Add
   from start (default 0); its C fast paths for exact ints and floats give
   the same results. Compiled only when the length is known now: a tuple
   built here or a constant tuple. Every type is promoted before the first
   addition (restart rule, PATTERNS rule 1): a user __add__ must not run
   twice. A str, bytes or bytearray start raises, and anything else (lists,
   iterators: items that are not frame values can't be promoted) is
   CPython's, as before. */
static vinfo_t *pbuiltin_sum(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs) {
  int n = PsycoTuple_Load(vargs);
  int i, k;
  vinfo_t *vit, *result, *x;
  PyTypeObject *tp;
  if (n != 1 && n != 2)
    goto fallback;
  vit = PsycoTuple_GET_ITEM(vargs, 0);
  if (psyco_genexpr_fusable(vit)) {
    if (n == 2) {
      tp = Psyco_NeedType(po, PsycoTuple_GET_ITEM(vargs, 1));
      if (tp == NULL)
        return NULL;
      if (PyType_IsSubtype(tp, &PyUnicode_Type) ||
          PyType_IsSubtype(tp, &PyBytes_Type) ||
          PyType_IsSubtype(tp, &PyByteArray_Type))
        goto fallback; /* sum() raises its own TypeError */
    }
    switch (psyco_genexpr_fuse(po, vit)) {
    case 0:
      goto fallback;
    case -1:
      return NULL;
    }
    return genexpr_consume(po, vit, GENEXPR_SUM, NULL,
                           n == 2 ? PsycoTuple_GET_ITEM(vargs, 1) : NULL);
  }
  if (is_compiletime(vit->source)) {
    if (!PyTuple_CheckExact((PyObject *)CompileTime_Get(vit->source)->value))
      goto fallback;
  } else if (Psyco_KnownType(vit) != &PyTuple_Type)
    goto fallback;
  k = PsycoTuple_Load(vit);
  if (k < 0)
    goto fallback;
  if (n == 2) {
    tp = Psyco_NeedType(po, PsycoTuple_GET_ITEM(vargs, 1));
    if (tp == NULL)
      return NULL;
    if (PyType_IsSubtype(tp, &PyUnicode_Type) ||
        PyType_IsSubtype(tp, &PyBytes_Type) ||
        PyType_IsSubtype(tp, &PyByteArray_Type))
      goto fallback;
  }
  for (i = 0; i < k; i++)
    if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(vit, i)) == NULL)
      return NULL;
  if (n == 2) {
    result = PsycoTuple_GET_ITEM(vargs, 1);
    vinfo_incref(result);
  } else
    result = PsycoLong_FROM_LONG(po, psyco_vi_Zero());
  for (i = 0; i < k && result != NULL; i++) {
    vinfo_t *item = PsycoTuple_GET_ITEM(vit, i);
    if (Psyco_KnownType(result) != NULL)
      x = PsycoNumber_Add(po, result, item);
    else /* e.g. from a user __add__: a temporary, so its type can't be
            promoted; CPython's generic loop */
      x = psyco_generic_call(po, PyNumber_Add, CfCommonNewRefPyObject, "vv",
                             result, item);
    vinfo_decref(result, po);
    result = x;
  }
  return result;
fallback:
  return PsycoCFunction_CallFast(po, cimpl_sum, METH_FASTCALL, vself, vargs);
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

/* super() with no arguments: typeobject.c:super_init's frame lookup, with
   the frame being the compiled one, which a direct call (psyfunc.c) never
   makes real. 'argc' is the code's co_argcount, 'obj' its first local and
   'objcell' the cell holding the first argument instead, if any. */
static PyObject *cimpl_super_noargs(long argc, PyObject *obj,
                                    PyObject *objcell, PyObject *classcell) {
  PyObject *type;
  if (argc == 0) {
    PyErr_SetString(PyExc_RuntimeError, "super(): no arguments");
    return NULL;
  }
  if (obj == NULL && objcell != NULL)
    obj = PyCell_GET(objcell);
  if (obj == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "super(): arg[0] deleted");
    return NULL;
  }
  if (classcell == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "super(): __class__ cell not found");
    return NULL;
  }
  if (!PyCell_Check(classcell)) {
    PyErr_SetString(PyExc_RuntimeError, "super(): bad __class__ cell");
    return NULL;
  }
  type = PyCell_GET(classcell);
  if (type == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "super(): empty __class__ cell");
    return NULL;
  }
  if (!PyType_Check(type)) {
    PyErr_Format(PyExc_RuntimeError, "super(): __class__ is not a type (%s)",
                 Py_TYPE(type)->tp_name);
    return NULL;
  }
  return PyObject_CallFunctionObjArgs((PyObject *)&PySuper_Type, type, obj,
                                      NULL);
}

static vinfo_t *psuper_noargs(PsycoObject *po) {
  PyCodeObject *co = po->pr.co;
  Py_ssize_t i, ncells = PyTuple_GET_SIZE(co->co_cellvars);
  int fv = psyco_class_cell(co);
  vinfo_t *obj, *objcell = NULL, *classcell = NULL, *zero, *r;
  zero = psyco_vi_Zero();
  obj = co->co_argcount > 0 ? LOC_LOCALS_PLUS[0] : zero;
  if (obj == NULL)
    obj = zero;
  for (i = 0; co->co_cell2arg != NULL && i < ncells; i++)
    if (co->co_cell2arg[i] == 0)
      objcell = LOC_LOCALS_PLUS[co->co_nlocals + i];
  if (fv >= 0)
    classcell = LOC_LOCALS_PLUS[co->co_nlocals + ncells + fv];
  r = psyco_generic_call(po, cimpl_super_noargs, CfCommonNewRefPyObject,
                         "lvvv", (long)co->co_argcount, obj,
                         objcell ? objcell : zero,
                         classcell ? classcell : zero);
  vinfo_decref(zero, po);
  return r;
}

/* type_call (Objects/typeobject.c) for type(x), int(x) and float(x) with
   one positional argument of an exact type; everything else, including
   every class instantiation, is the residual type_call as before.
   type(x): the "ugly exception", the type itself. int(x): long_new_impl ->
   PyNumber_Long, which returns an exact int unchanged and uses nb_int
   (float___trunc___impl) for a float. float(x): float_new_impl ->
   PyNumber_Float, which returns an exact float unchanged and uses nb_float
   (long_float -> PyLong_AsDouble) for an int. Only a static type
   callee is promoted: a promoted class would be kept alive by the compiled
   code (freeing.namedtuple_code_dies). */
static vinfo_t *ptype_call(PsycoObject *po, vinfo_t *vtype, vinfo_t *varg,
                           vinfo_t *vkw) {
  PyObject *t;
  PyTypeObject *tp;
  vinfo_t *v, *x;
  int fuse;
  int m = psyco_atcompiletime_static_type(po, vtype, &t);
  if (m < 0)
    return NULL;
  /* super(): also when the callee isn't known, if this code has the
     __class__ cell super() would look for */
  if (psyco_knowntobe(vkw, 0) && PsycoTuple_Load(varg) == 0) {
    if (m != 0 && t == (PyObject *)&PySuper_Type)
      return psuper_noargs(po);
    if (m == 0 && psyco_class_cell(po->pr.co) >= 0) {
      condition_code_t cc =
          integer_cmp_i(po, vtype, (long)&PySuper_Type, Py_EQ);
      if (cc == CC_ERROR)
        return NULL;
      if (runtime_condition_f(po, cc))
        return psuper_noargs(po);
    }
  }
  if (m != 0 && t == (PyObject *)&PyRange_Type && psyco_knowntobe(vkw, 0)) {
    int n = PsycoTuple_Load(varg);
    if (n >= 1 && n <= 3) {
      int r = PsycoRange_New(po, &PsycoTuple_GET_ITEM(varg, 0), n, &x);
      if (r < 0)
        return NULL;
      if (r > 0)
        return x;
    }
    goto fallback;
  }
  if (m != 0 && psyco_knowntobe(vkw, 0) && PsycoTuple_Load(varg) == 1 &&
      (t == (PyObject *)&PyList_Type || t == (PyObject *)&PyTuple_Type ||
       t == (PyObject *)&PySet_Type) &&
      (fuse = psyco_genexpr_fuse(po, PsycoTuple_GET_ITEM(varg, 0))) != 0) {
    if (fuse < 0)
      return NULL;
    return genexpr_consume(po, PsycoTuple_GET_ITEM(varg, 0),
                           t == (PyObject *)&PySet_Type ? GENEXPR_SET
                                                        : GENEXPR_LIST,
                           (PyTypeObject *)t, NULL);
  }
  if (m == 0 || !psyco_knowntobe(vkw, 0) || PsycoTuple_Load(varg) != 1 ||
      (t != (PyObject *)&PyType_Type && t != (PyObject *)&PyLong_Type &&
       t != (PyObject *)&PyFloat_Type))
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
    if ((x = PsycoLong_AsDouble(po, v)) == NULL)
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
     from Python/bltinmodule.c and its clinic header), otherwise
     ni._unregistered_metas() lists it. A METH_FASTCALL meta takes a tuple
     (pmethodobject.h). No meta
     for iter(): PsycoObject_GetIter gives lists a virtual seq-iterator,
     right for a for loop but the wrong type for iter() to hand out. */
  PyObject *md = Psyco_DefineMetaModule("builtins");

  gen_bltinmodule_init(md);
#define DEFMETA(name, flags)                                                   \
  cimpl_##name = Psyco_DefineModuleFn(md, #name, flags, &pbuiltin_##name)
  DEFMETA(globals, METH_NOARGS);
  DEFMETA(min, METH_VARARGS | METH_KEYWORDS);
  DEFMETA(max, METH_VARARGS | METH_KEYWORDS);
  DEFMETA(round, METH_FASTCALL | METH_KEYWORDS);
  DEFMETA(sum, METH_FASTCALL);
  DEFMETA(any, METH_O);
  DEFMETA(all, METH_O);
  type_call = PyType_Type.tp_call;
  Psyco_DefineMeta(type_call, ptype_call);
  str___class__ = PyUnicode_InternFromString("__class__");
  object_class_descr = _PyType_Lookup(&PyBaseObject_Type, str___class__);
#undef DEFMETA
  Py_XDECREF(md);
}
