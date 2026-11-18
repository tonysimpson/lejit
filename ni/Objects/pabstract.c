/*
 * The hand-written part of the compile-time abstract.c. Most of it is
 * generated from CPython's Objects/abstract.c into pabstract_gen.h by
 * tools/metac/gen_abstract.py (D74), included below. What stays here is
 * what the translator leaves out: PyObject_Call (call.c), PyNumber_Power
 * (a type whitelist), PyObject_GetIter (a virtual iterator), PyIter_Next
 * (FOR_ITER's call, not a translation of it), and the subscripts of
 * tuples and lists.
 */
#include "pabstract.h"
#include "../compat2to3.h"
#include "pclassobject.h"
#include "pfuncobject.h"
#include "piterobject.h"
#include "plongobject.h"
#include "pmethodobject.h"
#include "ptupleobject.h"

/* abstract.c's type_error: msg has one %.200s for the type's name */
static vinfo_t *type_error_tp(PsycoObject *po, const char *msg,
                              PyTypeObject *tp) {
  PycException_SetFormat(po, PyExc_TypeError, msg, tp->tp_name);
  return NULL;
}


DEFINEFN
vinfo_t *PsycoObject_Call(PsycoObject *po, vinfo_t *callable_object,
                          vinfo_t *args,
                          vinfo_t *kw) { /* 'kw' may not be NULL */
  ternaryfunc call;
  PyTypeObject *tp = Psyco_NeedType(po, callable_object);
  if (tp == NULL)
    return NULL;

  switch (Psyco_TypeSlot(po, tp, &tp->tp_call, (void **)&call)) {
  case 1:
    break;
  case 0: /* a heap type's tp_call changed; kw is 0 for none */
    return psyco_generic_call(po, PyObject_Call, CfCommonNewRefPyObject,
                              "vvv", callable_object, args, kw);
  default:
    return NULL;
  }
  if (call != NULL) {
    return Psyco_META3(po, call, CfCommonNewRefPyObject, "vvv", callable_object,
                       args, kw);
  }
  return type_error_tp(po, "'%.200s' object is not callable", tp);
}

DEFINEFN
vinfo_t *psyco_generic_immut_ob_size(PsycoObject *po, vinfo_t *vi) {
  return psyco_get_field(po, vi, FIX_size);
}

DEFINEFN
vinfo_t *psyco_generic_mut_ob_size(PsycoObject *po, vinfo_t *vi) {
  return psyco_get_field(po, vi, VAR_size);
}

/* the metas generated from abstract.c (D74) */
#include "pabstract_gen.h"

DEFINEFN
vinfo_t *PsycoNumber_Power(PsycoObject *po, vinfo_t *v1, vinfo_t *v2,
                           vinfo_t *v3) {
  /* limited support -- the goal is only to support
     (long or float) ** (long or float) */
  vinfo_t *x;
  PyTypeObject *vtp;
  PyTypeObject *wtp;
  ternaryfunc f = NULL;
  vtp = Psyco_NeedType(po, v1);
  if (vtp == NULL)
    return NULL;
  if (vtp != &PyLong_Type && vtp != &PyFloat_Type)
    goto fallback;
  wtp = Psyco_NeedType(po, v2);
  if (wtp == NULL)
    return NULL;
  if (wtp != &PyLong_Type && wtp != &PyFloat_Type)
    goto fallback;
  if (vtp == &PyFloat_Type || wtp == &PyFloat_Type)
    f = PyFloat_Type.tp_as_number->nb_power;
  else if (vtp == &PyLong_Type || wtp == &PyLong_Type)
    f = PyLong_Type.tp_as_number->nb_power;
  if (f == NULL) {
    goto fallback;
  }
  x = Psyco_META3(po, f, CfCommonNewRefPyObject, "vvv", v1, v2, v3);
  if (IS_IMPLEMENTED(x))
    return x; /* may be NULL */
  vinfo_decref(x, po);

fallback:
  return psyco_generic_call(po, PyNumber_Power, CfCommonNewRefPyObject, "vvv",
                            v1, v2, v3);
}

DEFINEFN
vinfo_t *PsycoObject_GetIter(PsycoObject *po, vinfo_t *vi) {
  getiterfunc f;
  ssizeargfunc item = NULL;
  PyTypeObject *t = Psyco_NeedType(po, vi);
  if (t == NULL)
    return NULL;
  switch (Psyco_TypeSlot(po, t, &t->tp_iter, (void **)&f)) {
  case 1:
    break;
  case 0:
    goto generic;
  default:
    return NULL;
  }
  if (f == NULL) {
    if (t->tp_as_sequence != NULL)
      switch (Psyco_TypeSlot(po, t, &t->tp_as_sequence->sq_item,
                             (void **)&item)) {
      case 1:
        break;
      case 0:
        goto generic;
      default:
        return NULL;
      }
    if (item != NULL)
      return PsycoSeqIter_New(po, vi);
    return type_error_tp(po, "'%.200s' object is not iterable", t);
  }
  /* a heap type's __iter__ may return a non-iterator, which
     PyObject_GetIter checks */
  if (!Psyco_SlotsMutable(t))
    return Psyco_META1(po, f, CfCommonNewRefPyObject, "v", vi);
generic:
  return psyco_generic_call(po, PyObject_GetIter, CfCommonNewRefPyObject, "v",
                            vi);
}

/* ceval.c's FOR_ITER: GET_ITER made sure the slot was there, but a heap
   type's can have gone since (CPython leaves _PyObject_NextNotImplemented,
   which raises TypeError); NULL is tested only in case Ni's GET_ITER was
   bypassed. */
static PyObject *cimpl_iternext(PyObject *iter) {
  iternextfunc f = Py_TYPE(iter)->tp_iternext;
  if (f == NULL) {
    PyErr_Format(PyExc_TypeError, "'%.100s' object is not an iterator",
                 Py_TYPE(iter)->tp_name);
    return NULL;
  }
  return f(iter);
}

DEFINEFN
vinfo_t *PsycoIter_Next(PsycoObject *po, vinfo_t *iter) {
  iternextfunc f;
  PyTypeObject *tp = Psyco_NeedType(po, iter);
  if (tp == NULL)
    return NULL;
  switch (Psyco_TypeSlot(po, tp, &tp->tp_iternext, (void **)&f)) {
  case 1:
    break;
  case 0:
    return psyco_generic_call(po, cimpl_iternext, CfCommonIterNext, "v",
                              iter);
  default:
    return NULL;
  }
  if (f == NULL) {
    PycException_SetFormat(po, PyExc_TypeError,
                           "'%.100s' object is not an iterator", tp->tp_name);
    return NULL;
  }
  return Psyco_META1(po, f, CfCommonIterNext, "v", iter);
}

DEFINEFN
vinfo_t *psyco_generic_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key) {
  /* This is the meta-implementation of the mapping item assignment
     for sequences in Python >= 2.3, which is called for any
     expression of the form a[n]. It expects n to be an integer
     or an extended slice object. Regular slicing a[n:m] does not
     come here. */

  /* TypeSwitch */
  PyTypeObject *ktp = Psyco_NeedType(po, key);
  if (ktp == NULL)
    return NULL;

  if (PyType_TypeCheck(ktp, &PyLong_Type)) {
    vinfo_t *result;
    /* as list_subscript and tuplesubscript: PyNumber_AsSsize_t(item,
       PyExc_IndexError) (KNOWN_BUGS F102) */
    vinfo_t *key_value = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
    if (key_value == NULL)
      return NULL;
    result = PsycoSequence_GetItem(po, o, key_value);
    vinfo_decref(key_value, po);
    return result;
  } else {
    PyTypeObject *tp = Psyco_NeedType(po, o);
    if (tp == NULL)
      return NULL;
    extra_assert(tp->tp_as_mapping != NULL);
    extra_assert(tp->tp_as_mapping->mp_subscript != NULL);
    return psyco_generic_call(po, tp->tp_as_mapping->mp_subscript,
                              CfCommonNewRefPyObject, "vv", o, key);
  }
}

DEFINEFN
bool psyco_generic_ass_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key,
                                 vinfo_t *value) {
  /* see psyco_generic_subscript() for comments */

  /* TypeSwitch */
  PyTypeObject *ktp = Psyco_NeedType(po, key);
  if (ktp == NULL)
    return false;

  if (PyType_TypeCheck(ktp, &PyLong_Type)) {
    bool result;
    vinfo_t *key_value = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
    if (key_value == NULL)
      return false;
    result = (value != NULL)
                 ? PsycoSequence_SetItem(po, o, key_value, value)
                 : PsycoSequence_DelItem(po, o, key_value);
    vinfo_decref(key_value, po);
    return result;
  } else {
    char *vargs = (value != NULL) ? "vvv" : "vvl";
    PyTypeObject *tp = Psyco_NeedType(po, o);
    if (tp == NULL)
      return false;
    extra_assert(tp->tp_as_mapping != NULL);
    extra_assert(tp->tp_as_mapping->mp_ass_subscript != NULL);
    return psyco_generic_call(po, tp->tp_as_mapping->mp_ass_subscript,
                              CfCommonIntZeroOk, vargs, o, key, value) != NULL;
  }
}
