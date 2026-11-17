/*
 * The generic object protocol at compile time, plus the two primitives that
 * everything else uses to learn types: Psyco_NeedType (promote the type to a
 * constant; may raise the promotion pseudo-exception) and Psyco_VerifyType
 * (true / false / error-or-promotion). Psyco_SafelyDeleteVar decides which dead
 * locals can be dropped without a run-time decref, using a hard-coded list of
 * types that have no __del__. GenericGetAttr is specialised on the type
 * (see its comment); it is what reaches the tp_descr_get metas in
 * pdescrobject.c and pfuncobject.c.
 */
#include "pobject.h"
#include "../compat2to3.h"
#include "../pycodegen.h"
#include "pboolobject.h"
#include "pstringobject.h"

DEFINEFN
PyTypeObject *Psyco_NeedType(PsycoObject *po, vinfo_t *vi) {
  if (is_compiletime(vi->source)) {
    /* optimization for the type of a known object */
    PyObject *o = (PyObject *)CompileTime_Get(vi->source)->value;
    return o->ob_type;
  } else {
    vinfo_t *vtp = psyco_get_const(po, vi, OB_type);
    if (vtp == NULL)
      return NULL;
    return (PyTypeObject *)psyco_pyobj_atcompiletime(po, vtp);
  }
}

DEFINEFN
PyTypeObject *Psyco_KnownType(vinfo_t *vi) {
  if (is_compiletime(vi->source)) {
    PyObject *o = (PyObject *)CompileTime_Get(vi->source)->value;
    return o->ob_type;
  } else {
    vinfo_t *vtp = vinfo_getitem(vi, iOB_TYPE);
    if (vtp != NULL && is_compiletime(vtp->source))
      return (PyTypeObject *)CompileTime_Get(vtp->source)->value;
    else
      return NULL;
  }
}

DEFINEFN
vinfo_t *PsycoObject_IsTrue(PsycoObject *po, vinfo_t *vi) {
  PyTypeObject *tp;
  tp = (PyTypeObject *)Psyco_NeedType(po, vi);
  if (tp == NULL)
    return NULL;
  /* XXX tony: Need some way to check Py_True/Py_False quickly */
  /* XXX tony: Should make sure result is 0 or 1 like cPython */
  if (tp == Py_None->ob_type)
    return psyco_vi_Zero();
  else if (tp->tp_as_number != NULL && tp->tp_as_number->nb_bool != NULL)
    return Psyco_META1(po, tp->tp_as_number->nb_bool, CfCommonInquiry, "v", vi);
  else if (tp->tp_as_mapping != NULL && tp->tp_as_mapping->mp_length != NULL)
    return Psyco_META1(po, tp->tp_as_mapping->mp_length, CfCommonPySSizeTResult,
                       "v", vi);
  else if (tp->tp_as_sequence != NULL && tp->tp_as_sequence->sq_length != NULL)
    return Psyco_META1(po, tp->tp_as_sequence->sq_length,
                       CfCommonPySSizeTResult, "v", vi);
  else
    return psyco_vi_One();
}

DEFINEFN
vinfo_t *PsycoObject_Repr(PsycoObject *po, vinfo_t *vi) {
  /* XXX implement me */
  vinfo_t *vstr =
      psyco_generic_call(po, PyObject_Repr, CfCommonNewRefPyObject, "v", vi);
  if (vstr == NULL)
    return NULL;

  /* the result is a string */
  Psyco_AssertType(po, vstr, &NiCompatStr_Type);
  return vstr;
}

static vinfo_t *generic_getattr(PsycoObject *po, vinfo_t *obj, vinfo_t *vname,
                                void *generic);

/* The type of 'obj' as a constant, promoted for at most MEGAMORPHIC_MAX
   types at a site: 1 (*tp set), 0 (megamorphic: stay generic), or -1. A
   heap type made a constant stays alive with the code, so the bound also
   limits how many classes a site keeps. */
static int need_type_mega(PsycoObject *po, vinfo_t *obj, PyTypeObject **tp) {
  vinfo_t *vtp;
  if (is_compiletime(obj->source)) {
    *tp = Py_TYPE((PyObject *)CompileTime_Get(obj->source)->value);
    return 1;
  }
  vtp = psyco_get_const(po, obj, OB_type);
  if (vtp == NULL)
    return -1;
  return psyco_pyobj_atcompiletime_mega(po, vtp, (PyObject **)tp);
}

DEFINEFN
vinfo_t *PsycoObject_GetAttr(PsycoObject *po, vinfo_t *o, vinfo_t *attr_name) {
  PyTypeObject *tp;
  int r;
  if (!is_compiletime(attr_name->source) ||
      !PyUnicode_CheckExact(
          (PyObject *)CompileTime_Get(attr_name->source)->value))
    goto generic;
  r = need_type_mega(po, o, &tp);
  if (r < 0)
    return NULL;
  /* a heap type's tp_getattro can change (a new __getattr__), and a respawn
     replays this with the type as it is then: so don't test it here, before
     generic_getattr's guard, but after it */
  if (r > 0 && ((tp->tp_flags & Py_TPFLAGS_HEAPTYPE) ||
                tp->tp_getattro == PyObject_GenericGetAttr))
    return generic_getattr(po, o, attr_name, PyObject_GetAttr);
generic:
  return psyco_generic_call(po, PyObject_GetAttr, CfCommonNewRefPyObject, "vv",
                            o, attr_name);
}

DEFINEFN
bool PsycoObject_SetAttr(PsycoObject *po, vinfo_t *o, vinfo_t *vattrname,
                         vinfo_t *v) {
  PyObject *name;
  PyTypeObject *tp;
  vinfo_t *vresult;
  /* as in PsycoObject_GenericGetAttr() we don't try to analyse
     a non-constant vattrname */
  if (!is_compiletime(vattrname->source))
    goto generic;

  tp = (PyTypeObject *)Psyco_NeedType(po, o);
  if (tp == NULL)
    return false;

  name = (PyObject *)CompileTime_Get(vattrname->source)->value;
  if (!NiCompatStr_Check(name)) {
#ifdef IS_PY3K
    /* The Unicode to string conversion is done here because the
       existing tp_setattro slots expect a string object as name
       and we wouldn't want to break those. */
    if (PyUnicode_Check(name)) {
      goto generic;
    } else {
#endif
      PycException_SetFormat(po, PyExc_TypeError,
                             "attribute name must be string, not '%.200s'",
                             Py_TYPE(name)->tp_name);
      return false;
#ifdef IS_PY3K
    }
#endif
  } else
    Py_INCREF(name);

  NiCompatStr_InternInPlace(&name);
  if (tp->tp_setattro != NULL) {
    vresult = Psyco_META3(po, tp->tp_setattro, CfCommonIntZeroOk,
                          v ? "vlv" : "vll", o, name, v);
    Py_DECREF(name);
    return vresult != NULL;
  }
  if (tp->tp_setattr != NULL) {
    vresult =
        Psyco_META3(po, tp->tp_setattr, CfCommonIntZeroOk, v ? "vlv" : "vll", o,
                    (long)NiCompatStr_AS_STRING(name), v);
    Py_DECREF(name);
    return vresult != NULL;
  }
  Py_DECREF(name);

generic:
  /* fall-back implementation */
  return psyco_generic_call(po, PyObject_SetAttr, CfCommonIntZeroOk,
                            v ? "vvv" : "vvl", o, vattrname, v) != NULL;
}

/* PyObject_GenericGetAttr specialised on the type (A9). The steps are
   those of 3.7's _PyObject_GenericGetAttrWithDict: the type's attribute
   (descr) is looked up now, and is only valid while the type is unchanged,
   which a run-time check of its version tag guards (every change to a type
   or a base, including a new __getattr__, clears
   Py_TPFLAGS_VALID_VERSION_TAG; a new tag is assigned at the next lookup).
   A failed guard takes the generic path. Then, at run time: a data
   descriptor's __get__; else the instance __dict__; else a descriptor's
   __get__ (a function gives a virtual bound method, so a method call
   becomes a direct call); else the attribute itself.

   Respawns (dispatcher.h) replay compilation with the type as it is then,
   so, as for load_global_cached:
   - nothing before the guard depends on the type's mutable state (the
     expected tag is read from a per-site attr_guard_t, never freed);
   - the block ends right after the site (split_after_global), so the only
     replays through it are for its own conditions: the guard (the type
     changed: generic), or one just after a passed guard, with the type as
     the guard saw it. For that, only descriptors whose __get__ runs no
     Python code are specialised (a property getter could change the
     class and then raise, which is a respawn).
   - Only descriptors of built-in types: a heap descriptor type could gain
     or lose __get__/__set__ without the guard seeing it.
   - The C descriptor metas don't check the instance's type, as
     descr_check does, so they are used only when it is a subtype of the
     descriptor's d_type.
   - The compiled code holds a reference to descr (never released), so a
     bound method made from it stays valid if the class changes later.
   Known gap: an instance __dict__ key with a Python __eq__ that changes the
   class during the lookup (KNOWN_BUGS F34). */
typedef struct {
  unsigned int tag;
  int valid;
} attr_guard_t;

static int cimpl_type_unchanged(PyTypeObject *tp, attr_guard_t *g) {
  return g->valid && PyType_HasFeature(tp, Py_TPFLAGS_VALID_VERSION_TAG) &&
         tp->tp_version_tag == g->tag;
}

/* borrowed, or NULL with no exception set (PyDict_GetItem) */
static PyObject *cimpl_instance_dict_get(PyObject *obj, PyObject *name) {
  PyObject **dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr == NULL || *dictptr == NULL)
    return NULL;
  return PyDict_GetItem(*dictptr, name);
}

/* descriptor types whose __get__ is C code that runs no Python code */
static bool c_descriptor(PyTypeObject *dtp) {
  return dtp == &PyMemberDescr_Type || dtp == &PyMethodDescr_Type ||
         dtp == &PyGetSetDescr_Type || dtp == &PyClassMethodDescr_Type;
}
static bool safe_descriptor(PyTypeObject *dtp) {
  return c_descriptor(dtp) || dtp == &PyFunction_Type ||
         dtp == &PyStaticMethod_Type || dtp == &PyClassMethod_Type ||
         dtp->tp_descr_get == NULL;
}

/* 'generic' is the function the compiled code calls when the type has
   changed: PyObject_GetAttr from PsycoObject_GetAttr, because the change
   may have given the type a __getattr__ or __getattribute__. */
static vinfo_t *generic_getattr(PsycoObject *po, vinfo_t *obj, vinfo_t *vname,
                                void *generic) {
  static attr_guard_t respawn_guard;
  PyTypeObject *tp;
  PyObject *name, *descr;
  descrgetfunc f = NULL;
  attr_guard_t *guard;
  vinfo_t *v;
  condition_code_t cc;
  int r;

  if (!is_compiletime(vname->source))
    goto generic;
  name = (PyObject *)CompileTime_Get(vname->source)->value;
  if (!PyUnicode_CheckExact(name))
    goto generic;
  r = need_type_mega(po, obj, &tp);
  if (r < 0)
    return NULL;
  /* a static type's tp_getattro never changes; a heap type's is checked
     after the guard */
  if (r == 0 || (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE) &&
                 tp->tp_getattro != PyObject_GenericGetAttr))
    goto generic;

  if (is_respawning(po))
    guard = &respawn_guard;
  else {
    guard = PyMem_Malloc(sizeof(attr_guard_t));
    if (guard == NULL)
      OUT_OF_MEMORY();
    if (tp->tp_dict != NULL)
      _PyType_Lookup(tp, name); /* assigns the version tag */
    guard->tag = tp->tp_version_tag;
    guard->valid = PyType_HasFeature(tp, Py_TPFLAGS_VALID_VERSION_TAG) &&
                   tp->tp_getattro == PyObject_GenericGetAttr;
  }
  v = psyco_generic_call(po, cimpl_type_unchanged, CfReturnTypeInt, "ll",
                         (long)tp, (long)guard);
  if (v == NULL)
    return NULL;
  cc = integer_non_null(po, v);
  if (cc == CC_ERROR) {
    vinfo_decref(v, po);
    return NULL;
  }
  po->pr.split_after_global = 1;
  if (!runtime_condition_t(po, cc)) {
    vinfo_decref(v, po);
    goto generic;
  }
  vinfo_decref(v, po);

  /* the type is as the guard saw it */
  if (tp->tp_getattro != PyObject_GenericGetAttr || tp->tp_dict == NULL)
    goto generic;
  descr = _PyType_Lookup(tp, name); /* borrowed */
  if (descr != NULL) {
    PyTypeObject *dtp = Py_TYPE(descr);
    if ((dtp->tp_flags & Py_TPFLAGS_HEAPTYPE) || !safe_descriptor(dtp))
      goto generic;
    if (c_descriptor(dtp) && !PyType_IsSubtype(tp, PyDescr_TYPE(descr)))
      goto generic;
    f = dtp->tp_descr_get;
    Py_INCREF(descr); /* held by the compiled code */
  }

  if (f != NULL && PyDescr_IsData(descr))
    return Psyco_META3(po, f, CfCommonNewRefPyObject, "lvl", descr, obj, tp);

  if (tp->tp_dictoffset != 0) {
    bool found;
    v = psyco_generic_call(po, cimpl_instance_dict_get, CfReturnTypePtr, "vl",
                           obj, (long)name);
    if (v == NULL)
      return NULL;
    cc = integer_non_null(po, v);
    if (cc == CC_ERROR) {
      vinfo_decref(v, po);
      return NULL;
    }
    /* an attribute shadowing a method is rare; one without a class
       attribute is likely in the instance */
    found = descr != NULL ? runtime_condition_f(po, cc)
                          : runtime_condition_t(po, cc);
    if (found) {
      need_reference(po, v);
      return v;
    }
    vinfo_decref(v, po);
  }

  if (f != NULL)
    return Psyco_META3(po, f, CfCommonNewRefPyObject, "lvl", descr, obj, tp);
  if (descr != NULL) {
    Py_INCREF(descr);
    return vinfo_new(CompileTime_NewSk(sk_new((long)descr, SkFlagPyObj)));
  }
  /* not found: the generic call raises AttributeError */
generic:
  return psyco_generic_call(po, generic, CfCommonNewRefPyObject, "vv", obj,
                            vname);
}

DEFINEFN
vinfo_t *PsycoObject_GenericGetAttr(PsycoObject *po, vinfo_t *obj,
                                    vinfo_t *vname) {
  return generic_getattr(po, obj, vname, PyObject_GenericGetAttr);
}

/* Map rich comparison operators to their swapped version, e.g. LT --> GT */
static int swapped_op[] = {Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE};

static vinfo_t* try_rich_compare(PsycoObject* po, vinfo_t* v, vinfo_t* w, int op)
{
  bool swap;
  PyTypeObject* vtp;
  PyTypeObject* wtp;
  richcmpfunc fv;
  richcmpfunc fw;
  vinfo_t* res;

  vtp = (PyTypeObject*)Psyco_NeedType(po, v);
  if (vtp == NULL) return NULL;
  wtp = (PyTypeObject*) Psyco_NeedType(po, w);
  if (wtp == NULL) return NULL;
  fv = vtp->tp_richcompare;
  fw = wtp->tp_richcompare;

  swap = (vtp != wtp &&
          PyType_IsSubtype(wtp, vtp) &&
          fw != NULL);
  if (swap) {
          res = Psyco_META3(po, fw, CfCommonCheckNotImplemented,
                            "vvl", w, v, swapped_op[op]);
          if (IS_IMPLEMENTED(res))
                  return res;   /* 'res' might be NULL */
          vinfo_decref(res, po);
  }
  if (fv != NULL) {
          res = Psyco_META3(po, fv, CfCommonCheckNotImplemented,
                            "vvl", v, w, op);
          if (IS_IMPLEMENTED(res))
                  return res;
          vinfo_decref(res, po);
  }
  if (!swap && fw != NULL) {
          return Psyco_META3(po, fw, CfCommonCheckNotImplemented,
                              "vvl", w, v, swapped_op[op]);
  }
  return psyco_vi_NotImplemented();
}

DEFINEFN vinfo_t *PsycoObject_RichCompare(PsycoObject *po, vinfo_t *v,
                                          vinfo_t *w, int op) {
  vinfo_t *res = try_rich_compare(po, v, w, op);
  if (IS_IMPLEMENTED(res)) return res;
        vinfo_decref(res, po);
  return psyco_generic_call(po, PyObject_RichCompare, CfCommonNewRefPyObject,
                            "vvl", v, w, (long)op);
}

DEFINEFN
vinfo_t *PsycoObject_RichCompareBool(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                                     int op) {
  /* The result is a temporary, and only values in the frame can be
     promoted, so if its type isn't known its truth is a run-time call.
     Unlike CPython there is no v == w shortcut for Py_EQ/Py_NE. */
  vinfo_t *diff;
  vinfo_t *result = PsycoObject_RichCompare(po, v, w, op);
  if (result == NULL)
    return NULL;
  if (Psyco_KnownType(result) != NULL)
    diff = PsycoObject_IsTrue(po, result);
  else
    diff = psyco_generic_call(po, PyObject_IsTrue, CfCommonInquiry, "v",
                              result);
  vinfo_decref(result, po);
  return diff;
}

DEFINEFN
vinfo_t *PsycoObject_Hash(PsycoObject *po, vinfo_t *v) {
  /* -1 is never a hash: it means an error */
  PyTypeObject *tp = Psyco_NeedType(po, v);
  if (tp == NULL)
    return NULL;
  if (tp->tp_hash == NULL) /* a type not yet ready: CPython readies it */
    return psyco_generic_call(po, PyObject_Hash,
                              CfReturnTypeLong | CfPyErrIfMinus1, "v", v);
  return Psyco_META1(po, tp->tp_hash, CfReturnTypeLong | CfPyErrIfMinus1, "v",
                     v);
}

static vinfo_t *collect_undeletable_vars(vinfo_t *vi, vinfo_t *link) {
  int i;
  PyTypeObject *tp;
  switch (gettime(vi->source)) {

  case RunTime:
    if (vi->tmp != NULL || (vi->source & RunTime_NoRef) != 0)
      break; /* already seen or not holding a ref */
    tp = Psyco_KnownType(vi);
    if (tp &&
        (tp == &NiCompatStr_Type || tp == &PyBool_Type || tp == &PyFloat_Type ||
         tp == &PyLong_Type || tp == Py_None->ob_type || tp == &PyRange_Type))
      break; /* known safe type */
    /* make a linked list of results */
    vi->tmp = link;
    link = vi;
    break;

  case VirtualTime:
    for (i = vi->array->count; i--;)
      if (vi->array->items[i] != NULL)
        link = collect_undeletable_vars(vi->array->items[i], link);
    break;

  default:
    break;
  }
  return link;
}

DEFINEFN
vinfo_t *Psyco_SafelyDeleteVar(PsycoObject *po, vinfo_t *vi) {
  vinfo_t *result;
  vinfo_t *head;
  vinfo_t *queue = (vinfo_t *)1;
  vinfo_t *p;
  int count;
  vi->tmp = NULL;
  clear_tmp_marks(vi->array);

  head = collect_undeletable_vars(vi, queue);
  count = 0;
  for (p = head; p != queue; p = p->tmp) {
    vinfo_array_t *a = p->array;
    p->array = NullArray;
    array_delete(a, po);
    count++;
  }
  if (count == 0) {
    result = psyco_vi_Zero();
  } else if (count == 1) {
    result = head;
    vinfo_incref(result);
  } else {
    result = vinfo_new(VirtualTime_New(&psyco_vsource_not_important));
    count += iOB_TYPE + 1;
    result->array = array_new(count);
    for (p = head; p != queue; p = p->tmp) {
      vinfo_incref(p);
      result->array->items[--count] = p;
    }
  }
  return result;
}

INITIALIZATIONFN
void psy_object_init(void) {
  /* associate the Python implementation of some functions with
     the one from Psyco */
  Psyco_DefineMeta(PyObject_GenericGetAttr, PsycoObject_GenericGetAttr);
}
