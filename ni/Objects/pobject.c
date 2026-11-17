/*
 * The generic object protocol at compile time, plus the two primitives that
 * everything else uses to learn types: Psyco_NeedType (promote the type to a
 * constant; may raise the promotion pseudo-exception) and Psyco_VerifyType
 * (true / false / error-or-promotion). Psyco_SafelyDeleteVar decides which dead
 * locals can be dropped without a run-time decref, using a hard-coded list of
 * types that have no __del__. GetAttr and GenericGetAttr are plain residual
 * calls here (upstream specialised them), which is why the tp_descr_get metas
 * in pdescrobject.c and pfuncobject.c are never reached.
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

DEFINEFN
vinfo_t *PsycoObject_GetAttr(PsycoObject *po, vinfo_t *o, vinfo_t *attr_name) {
  /* XXX tony: Add unicode optimization */
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

DEFINEFN
vinfo_t *PsycoObject_GenericGetAttr(PsycoObject *po, vinfo_t *obj,
                                    vinfo_t *vname) {
  /* XXX tony: Optimize this but allow changes to type dict */
  return psyco_generic_call(po, PyObject_GenericGetAttr, CfCommonNewRefPyObject,
                            "vv", obj, vname);
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
