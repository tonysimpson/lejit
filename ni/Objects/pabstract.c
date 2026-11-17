/*
 * Compile-time copy of Objects/abstract.c (PyNumber_*, PySequence_*,
 * PyObject_Call/GetItem/...). Mostly a mechanical translation: promote the
 * type, then dispatch through the slot with Psyco_META*.
 */
#include "pabstract.h"
#include "../compat2to3.h"
#include "pclassobject.h"
#include "pfuncobject.h"
#include "piterobject.h"
#include "plongobject.h"
#include "pmethodobject.h"
#include "ptupleobject.h"

/*** This file is translated from the original 'abstract.c', see comments
     in the original file for the general ideas about the algorithms. ***/

/* PyNumber_AsSsize_t: -1 may be a valid result, and any result may be
   negative */
#define CF_AS_SSIZE_T (CfReturnTypeNative | CfPyErrCheckMinus1)

/* Shorthand to return certain errors */

/* abstract.c's type_error: msg has one %.200s for the type's name */
static vinfo_t *type_error_tp(PsycoObject *po, const char *msg,
                              PyTypeObject *tp) {
  PycException_SetFormat(po, PyExc_TypeError, msg, tp->tp_name);
  return NULL;
}

/* The slot at 'offset' in one of tp's tables (NULL if tp has no such
   table), as Psyco_TypeSlot reads it: 1, 0 if a heap type's slot has
   changed since it was first seen, -1 on error. */
static int table_slot(PsycoObject *po, PyTypeObject *tp, void *table,
                      size_t offset, void **f) {
  if (table == NULL) {
    *f = NULL;
    return 1;
  }
  return Psyco_TypeSlot(po, tp, (char *)table + offset, f);
}
#define NB_SLOT_OF(tp, offset, f)                                              \
  table_slot(po, tp, (tp)->tp_as_number, offset, (void **)(f))
#define SQ_SLOT_OF(tp, name, f)                                                \
  table_slot(po, tp, (tp)->tp_as_sequence, offsetof(PySequenceMethods, name),  \
             (void **)(f))
#define MP_SLOT_OF(tp, name, f)                                                \
  table_slot(po, tp, (tp)->tp_as_mapping, offsetof(PyMappingMethods, name),    \
             (void **)(f))
/* 'read' is one of the above: go to 'changed' if the slot has changed */
#define SLOT_OR_GOTO(read, changed, err)                                       \
  switch (read) {                                                              \
  case 1:                                                                      \
    break;                                                                     \
  case 0:                                                                      \
    goto changed;                                                              \
  default:                                                                     \
    return err;                                                                \
  }

DEFINEFN
vinfo_t *PsycoObject_Call(PsycoObject *po, vinfo_t *callable_object,
                          vinfo_t *args,
                          vinfo_t *kw) { /* 'kw' may not be NULL */
  ternaryfunc call;
  PyTypeObject *tp = Psyco_NeedType(po, callable_object);
  if (tp == NULL)
    return NULL;

  if ((call = tp->tp_call) != NULL) {
    return Psyco_META3(po, call, CfCommonNewRefPyObject, "vvv", callable_object,
                       args, kw);
  }
  PycException_SetFormat(po, PyExc_TypeError,
                         "object of type '%.100s' is not callable",
                         tp->tp_name);
  return NULL;
}

DEFINEFN
vinfo_t *PsycoEval_CallObjectWithKeywords(PsycoObject *po,
                                          vinfo_t *callable_object,
                                          vinfo_t *args, vinfo_t *kw) {
  vinfo_t *result;

  if (args == NULL)
    args = PsycoTuple_New(0, NULL);
  else {
    switch (Psyco_VerifyType(po, args, &PyTuple_Type)) {
    case true: /* args is a tuple */
      vinfo_incref(args);
      break;
    case false: /* args is not a tuple */
      goto use_proxy;
    default: /* error or promotion */
      return NULL;
    }
  }
  if (kw == NULL)
    kw = psyco_vi_Zero();
  else {
    switch (Psyco_VerifyType(po, kw, &PyDict_Type)) {
    case true: /* kw is a dict */
      vinfo_incref(kw);
      break;
    case false: /* kw is not a dict */
      vinfo_decref(args, po);
      goto use_proxy;
    default: /* error or promotion */
      return NULL;
    }
  }

  result = PsycoObject_Call(po, callable_object, args, kw);
  vinfo_decref(kw, po);
  vinfo_decref(args, po);
  return result;

use_proxy:
  return psyco_generic_call(po, PyEval_CallObjectWithKeywords,
                            CfCommonNewRefPyObject, "vvv", callable_object,
                            args, kw);
}

DEFINEFN
vinfo_t *PsycoObject_GetItem(PsycoObject *po, vinfo_t *o, vinfo_t *key) {
  binaryfunc subscript;
  ssizeargfunc item;
  PyTypeObject *tp = Psyco_NeedType(po, o);
  if (tp == NULL)
    return NULL;

  SLOT_OR_GOTO(MP_SLOT_OF(tp, mp_subscript, &subscript), changed, NULL);
  if (subscript != NULL)
    return Psyco_META2(po, subscript, CfCommonNewRefPyObject, "vv", o, key);

  /* sq_item too, as 3.7: every heap type has a tp_as_sequence (F42) */
  SLOT_OR_GOTO(SQ_SLOT_OF(tp, sq_item, &item), changed, NULL);
  if (item != NULL) {
    /* TypeSwitch */
    PyTypeObject *ktp = Psyco_NeedType(po, key);
    if (ktp == NULL)
      return NULL;

    if (PyType_TypeCheck(ktp, &PyLong_Type)) {
      vinfo_t *result;
      vinfo_t *key_value = PsycoLong_AsLong(po, key);
      if (key_value == NULL)
        return NULL;
      result = PsycoSequence_GetItem(po, o, key_value);
      vinfo_decref(key_value, po);
      return result;
    }
    if (PsycoIndex_Check(ktp)) {
      vinfo_t *result;
      vinfo_t *key_value;
      key_value =
          psyco_generic_call(po, PyNumber_AsSsize_t, CF_AS_SSIZE_T,
                             "vl", key, (long)PyExc_IndexError);
      if (key_value == NULL)
        return NULL;
      result = PsycoSequence_GetItem(po, o, key_value);
      vinfo_decref(key_value, po);
      return result;
    }
    type_error_tp(po, "sequence index must be integer, not '%.200s'", ktp);
    return false;
  }

  /* PEP 560: a class subscripted calls its __class_getitem__ (Generic[T]) */
  if (PyType_FastSubclass(tp, Py_TPFLAGS_TYPE_SUBCLASS))
    goto changed;

  type_error_tp(po, "'%.200s' object is not subscriptable", tp);
  return false;

changed:
  return psyco_generic_call(po, PyObject_GetItem, CfCommonNewRefPyObject, "vv",
                            o, key);
}

DEFINEFN
bool PsycoObject_SetItem(PsycoObject *po, vinfo_t *o, vinfo_t *key,
                         vinfo_t *value) {
  objobjargproc ass_subscript;
  ssizeobjargproc ass_item;
  PyTypeObject *tp = Psyco_NeedType(po, o);
  if (tp == NULL)
    return false;

  SLOT_OR_GOTO(MP_SLOT_OF(tp, mp_ass_subscript, &ass_subscript), changed,
               false);
  if (ass_subscript != NULL) {
    char *vargs = (value != NULL) ? "vvv" : "vvl";
    return Psyco_META3(po, ass_subscript, CfCommonIntZeroOk, vargs, o, key,
                       value) != NULL;
  }

  if (tp->tp_as_sequence) {
    /* TypeSwitch */
    PyTypeObject *ktp = Psyco_NeedType(po, key);
    if (ktp == NULL)
      return false;

    if (PyType_TypeCheck(ktp, &PyLong_Type)) {
      bool result;
      vinfo_t *key_value = PsycoLong_AsLong(po, key);
      if (key_value == NULL)
        return false;
      result = PsycoSequence_SetItem(po, o, key_value, value);
      vinfo_decref(key_value, po);
      return result;
    }
    if (PsycoIndex_Check(ktp)) {
      bool result;
      vinfo_t *key_value;
      key_value =
          psyco_generic_call(po, PyNumber_AsSsize_t, CF_AS_SSIZE_T,
                             "vl", key, (long)PyExc_IndexError);
      if (key_value == NULL)
        return false;
      result = PsycoSequence_SetItem(po, o, key_value, value);
      vinfo_decref(key_value, po);
      return result;
    }
    SLOT_OR_GOTO(SQ_SLOT_OF(tp, sq_ass_item, &ass_item), changed, false);
    if (ass_item != NULL) {
      type_error_tp(po, "sequence index must be integer, not '%.200s'", ktp);
      return false;
    }
  }

  type_error_tp(po, (value != NULL)
                        ? "'%.200s' object does not support item assignment"
                        : "'%.200s' object does not support item deletion",
                tp);
  return false;

changed:
  if (value != NULL)
    return psyco_generic_call(po, PyObject_SetItem, CfCommonIntZeroOk, "vvv",
                              o, key, value) != NULL;
  return psyco_generic_call(po, PyObject_DelItem, CfCommonIntZeroOk, "vv", o,
                            key) != NULL;
}

DEFINEFN
vinfo_t *PsycoObject_Size(PsycoObject *po, vinfo_t *vi) {
  lenfunc f;
  PyTypeObject *tp = Psyco_NeedType(po, vi);
  if (tp == NULL)
    return NULL;

  SLOT_OR_GOTO(SQ_SLOT_OF(tp, sq_length, &f), changed, NULL);
  if (f == NULL)
    SLOT_OR_GOTO(MP_SLOT_OF(tp, mp_length, &f), changed, NULL);
  if (f == NULL) {
    PycException_SetFormat(po, PyExc_TypeError,
                           "object of type '%.200s' has no len()",
                           tp->tp_name);
    return NULL;
  }
  return Psyco_META1(po, f, CfCommonPySSizeTResult, "v", vi);

changed:
  return psyco_generic_call(po, PyObject_Size, CfCommonPySSizeTResult, "v",
                            vi);
}

DEFINEFN
vinfo_t *psyco_generic_immut_ob_size(PsycoObject *po, vinfo_t *vi) {
  return psyco_get_field(po, vi, FIX_size);
}

DEFINEFN
vinfo_t *psyco_generic_mut_ob_size(PsycoObject *po, vinfo_t *vi) {
  return psyco_get_field(po, vi, VAR_size);
}

DEFINEFN
vinfo_t *PsycoSequence_GetItem(PsycoObject *po, vinfo_t *o, vinfo_t *i) {
  ssizeargfunc item;
  lenfunc length;
  PyTypeObject *tp = Psyco_NeedType(po, o);
  if (tp == NULL)
    return NULL;

  SLOT_OR_GOTO(SQ_SLOT_OF(tp, sq_item, &item), changed, NULL);
  if (item != NULL) {
    vinfo_t *result;
    vinfo_t *release_me = NULL;
    SLOT_OR_GOTO(SQ_SLOT_OF(tp, sq_length, &length), changed, NULL);
    if (length != NULL) {
      condition_code_t cc = integer_cmp_i(po, i, 0, Py_LT);
      if (cc == CC_ERROR)
        return NULL;
      if (runtime_condition_f(po, cc)) {
        vinfo_t *l = Psyco_META1(po, length, CfCommonPySSizeTResult, "v", o);
        if (l == NULL)
          return NULL;
        release_me = i = integer_add(po, i, l, false);
        vinfo_decref(l, po);
        if (i == NULL)
          return NULL;
      } else
        assert_nonneg(i);
    }
    result = Psyco_META2(po, item, CfCommonNewRefPyObject, "vv", o, i);
    vinfo_xdecref(release_me, po);
    return result;
  }

  return type_error_tp(po, "'%.200s' object does not support indexing", tp);

changed:
  return psyco_generic_call(po, PySequence_GetItem, CfCommonNewRefPyObject,
                            "vv", o, i);
}

DEFINEFN
bool PsycoSequence_SetItem(PsycoObject *po, vinfo_t *o, vinfo_t *i,
                           vinfo_t *value) {
  ssizeobjargproc ass_item;
  lenfunc length;
  PyTypeObject *tp = Psyco_NeedType(po, o);
  if (tp == NULL)
    return false;

  SLOT_OR_GOTO(SQ_SLOT_OF(tp, sq_ass_item, &ass_item), changed, false);
  if (ass_item != NULL) {
    bool result;
    char *vargs;
    vinfo_t *release_me = NULL;
    SLOT_OR_GOTO(SQ_SLOT_OF(tp, sq_length, &length), changed, false);
    if (length != NULL) {
      condition_code_t cc = integer_cmp_i(po, i, 0, Py_LT);
      if (cc == CC_ERROR)
        return false;
      if (runtime_condition_f(po, cc)) {
        vinfo_t *l = Psyco_META1(po, length, CfCommonPySSizeTResult, "v", o);
        if (l == NULL)
          return false;
        release_me = i = integer_add(po, i, l, false);
        vinfo_decref(l, po);
        if (i == NULL)
          return false;
      } else
        assert_nonneg(i);
    }
    vargs = (value != NULL) ? "vvv" : "vvl";
    result = Psyco_META3(po, ass_item, CfCommonIntZeroOk, vargs, o, i,
                         value) != NULL;
    vinfo_xdecref(release_me, po);
    return result;
  }

  /* PySequence_SetItem / PySequence_DelItem (their wording differs) */
  type_error_tp(po, (value != NULL)
                        ? "'%.200s' object does not support item assignment"
                        : "'%.200s' object doesn't support item deletion",
                tp);
  return false;

changed:
  if (value != NULL)
    return psyco_generic_call(po, PySequence_SetItem, CfCommonIntZeroOk, "vvv",
                              o, i, value) != NULL;
  return psyco_generic_call(po, PySequence_DelItem, CfCommonIntZeroOk, "vv", o,
                            i) != NULL;
}

DEFINEFN
vinfo_t *PsycoSequence_GetSlice(PsycoObject *po, vinfo_t *o, vinfo_t *i1,
                                vinfo_t *i2) {
  /* XXX call mp_subscript with sliceobj_from_intint */
  /* fallback */
  return psyco_generic_call(po, PySequence_GetSlice, CfCommonNewRefPyObject,
                            "vvv", o, i1, i2);
}

DEFINEFN
bool PsycoSequence_SetSlice(PsycoObject *po, vinfo_t *o, vinfo_t *ilow,
                            vinfo_t *ihigh, vinfo_t *value) {
  /* XXX implement me */
  if (value != NULL)
    return psyco_generic_call(po, PySequence_SetSlice, CfCommonIntZeroOk,
                              "vvvv", o, ilow, ihigh, value) != NULL;
  else
    return psyco_generic_call(po, PySequence_DelSlice, CfCommonIntZeroOk, "vvv",
                              o, ilow, ihigh) != NULL;
}

DEFINEFN
vinfo_t *PsycoSequence_Contains(PsycoObject *po, vinfo_t *seq, vinfo_t *ob) {
  objobjproc f;
  PyTypeObject *tp = Psyco_NeedType(po, seq);
  if (tp == NULL)
    return NULL;

  SLOT_OR_GOTO(SQ_SLOT_OF(tp, sq_contains, &f), changed, NULL);
  if (f != NULL)
    return Psyco_META2(po, f, CfCommonIntBoolOrError, "vv", seq, ob);

  /* XXX implement me */
  return psyco_generic_call(po, _PySequence_IterSearch, CfCommonIntBoolOrError,
                            "vvl", seq, ob, (long)PY_ITERSEARCH_CONTAINS);

changed:
  return psyco_generic_call(po, PySequence_Contains, CfCommonIntBoolOrError,
                            "vv", seq, ob);
}

DEFINEFN
vinfo_t *PsycoSequence_Tuple(PsycoObject *po, vinfo_t *seq) {
  /* XXX implement me */
  vinfo_t *v = psyco_generic_call(po, PySequence_Tuple, CfCommonNewRefPyObject,
                                  "v", seq);
  if (v == NULL)
    return NULL;

  /* the result is a tuple */
  Psyco_AssertType(po, v, &PyTuple_Type);
  return v;
}

#define NB_SLOT(x) offsetof(PyNumberMethods, x)
#define NB_BINOP(nb_methods, slot) ((binaryfunc *)(&((char *)nb_methods)[slot]))
#define NB_TERNOP(nb_methods, slot)                                            \
  ((ternaryfunc *)(&((char *)nb_methods)[slot]))

/* abstract.c's PyNumber_Positive etc.: 'generic' is what compiled code
   calls once a heap type's slot has changed */
static vinfo_t *unary_op(PsycoObject *po, vinfo_t *vi, size_t offset,
                         void *generic, const char *msg) {
  unaryfunc f;
  PyTypeObject *tp = Psyco_NeedType(po, vi);
  if (tp == NULL)
    return NULL;

  switch (NB_SLOT_OF(tp, offset, &f)) {
  case 1:
    break;
  case 0:
    return psyco_generic_call(po, generic, CfCommonNewRefPyObject, "v", vi);
  default:
    return NULL;
  }
  if (f != NULL)
    return Psyco_META1(po, f, CfCommonNewRefPyObject, "v", vi);
  return type_error_tp(po, msg, tp);
}

DEFINEFN
vinfo_t *PsycoNumber_Positive(PsycoObject *po, vinfo_t *vi) {
  return unary_op(po, vi, NB_SLOT(nb_positive), PyNumber_Positive,
                  "bad operand type for unary +: '%.200s'");
}

DEFINEFN
vinfo_t *PsycoNumber_Negative(PsycoObject *po, vinfo_t *vi) {
  return unary_op(po, vi, NB_SLOT(nb_negative), PyNumber_Negative,
                  "bad operand type for unary -: '%.200s'");
}

DEFINEFN
vinfo_t *PsycoNumber_Invert(PsycoObject *po, vinfo_t *vi) {
  return unary_op(po, vi, NB_SLOT(nb_invert), PyNumber_Invert,
                  "bad operand type for unary ~: '%.200s'");
}

DEFINEFN
vinfo_t *PsycoNumber_Absolute(PsycoObject *po, vinfo_t *vi) {
  return unary_op(po, vi, NB_SLOT(nb_absolute), PyNumber_Absolute,
                  "bad operand type for abs(): '%.200s'");
}

/* abstract.c's binary_op1, at run time: what compiled code calls once a
   heap type's slot has changed (KNOWN_BUGS F56) */
static PyObject *cimpl_binary_op1(PyObject *v, PyObject *w, long op_slot) {
  PyObject *x;
  binaryfunc slotv = NULL;
  binaryfunc slotw = NULL;
  if (Py_TYPE(v)->tp_as_number != NULL)
    slotv = *NB_BINOP(Py_TYPE(v)->tp_as_number, op_slot);
  if (Py_TYPE(w) != Py_TYPE(v) && Py_TYPE(w)->tp_as_number != NULL) {
    slotw = *NB_BINOP(Py_TYPE(w)->tp_as_number, op_slot);
    if (slotw == slotv)
      slotw = NULL;
  }
  if (slotv) {
    if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
      x = slotw(v, w);
      if (x != Py_NotImplemented)
        return x;
      Py_DECREF(x);
      slotw = NULL;
    }
    x = slotv(v, w);
    if (x != Py_NotImplemented)
      return x;
    Py_DECREF(x);
  }
  if (slotw)
    return slotw(v, w);
  Py_RETURN_NOTIMPLEMENTED;
}

/* 3.7 abstract.c binary_op1: try v's slot, then w's; w's first if w's type
   is a subtype of v's. */
static vinfo_t *binary_op1(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                           const int op_slot) {
  vinfo_t *x;
  binaryfunc slotv = NULL;
  binaryfunc slotw = NULL;

  PyTypeObject *vtp;
  PyTypeObject *wtp;
  vtp = Psyco_NeedType(po, v);
  if (vtp == NULL)
    return NULL;
  wtp = Psyco_NeedType(po, w);
  if (wtp == NULL)
    return NULL;

  switch (NB_SLOT_OF(vtp, op_slot, &slotv)) {
  case 1:
    break;
  case 0:
    goto changed;
  default:
    return NULL;
  }
  if (wtp != vtp) {
    switch (NB_SLOT_OF(wtp, op_slot, &slotw)) {
    case 1:
      break;
    case 0:
      goto changed;
    default:
      return NULL;
    }
    if (slotw == slotv)
      slotw = NULL;
  }
  if (slotv) {
    if (slotw && PyType_IsSubtype(wtp, vtp)) {
      x = Psyco_META2(po, slotw, CfCommonCheckNotImplemented, "vv", v, w);
      if (IS_IMPLEMENTED(x))
        return x;          /* may be NULL */
      vinfo_decref(x, po); /* can't do it */
      slotw = NULL;
    }
    x = Psyco_META2(po, slotv, CfCommonCheckNotImplemented, "vv", v, w);
    if (IS_IMPLEMENTED(x))
      return x;
    vinfo_decref(x, po); /* can't do it */
  }
  if (slotw) {
    x = Psyco_META2(po, slotw, CfCommonCheckNotImplemented, "vv", v, w);
    if (IS_IMPLEMENTED(x))
      return x;
    vinfo_decref(x, po); /* can't do it */
  }
  return psyco_vi_NotImplemented();

changed:
  return psyco_generic_call(po, cimpl_binary_op1, CfCommonCheckNotImplemented,
                            "vvl", v, w, (long)op_slot);
}

static vinfo_t *binop_type_error(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                                 const char *op_name) {
  PycException_SetFormat(
      po, PyExc_TypeError, "unsupported operand type(s) for %s: '%s' and '%s'",
      op_name, Psyco_FastType(v)->tp_name, Psyco_FastType(w)->tp_name);
  return NULL;
}

static vinfo_t *binary_op(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                          const int op_slot, const char *op_name) {
  vinfo_t *result = binary_op1(po, v, w, op_slot);
  if (!IS_IMPLEMENTED(result)) {
    vinfo_decref(result, po);
    return binop_type_error(po, v, w, op_name);
  }
  return result;
}

#define BINARY_FUNC(func, op, op_name)                                         \
  DEFINEFN vinfo_t *func(PsycoObject *po, vinfo_t *v, vinfo_t *w) {            \
    return binary_op(po, v, w, NB_SLOT(op), op_name);                          \
  }

BINARY_FUNC(PsycoNumber_Or, nb_or, "|")
BINARY_FUNC(PsycoNumber_Xor, nb_xor, "^")
BINARY_FUNC(PsycoNumber_And, nb_and, "&")
BINARY_FUNC(PsycoNumber_Lshift, nb_lshift, "<<")
/* abstract.c:PyNumber_Rshift's hint for Python 2's 'print >> f' (F46) */
static PyObject *cimpl_rshift_error(PyObject *v, PyObject *w) {
  if (PyCFunction_Check(v) &&
      strcmp(((PyCFunctionObject *)v)->m_ml->ml_name, "print") == 0)
    PyErr_Format(PyExc_TypeError,
                 "unsupported operand type(s) for >>: '%.100s' and '%.100s'. "
                 "Did you mean \"print(<message>, file=<output_stream>)\"?",
                 v->ob_type->tp_name, w->ob_type->tp_name);
  else
    PyErr_Format(PyExc_TypeError,
                 "unsupported operand type(s) for >>: '%.100s' and '%.100s'",
                 v->ob_type->tp_name, w->ob_type->tp_name);
  return NULL;
}

DEFINEFN
vinfo_t *PsycoNumber_Rshift(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t *result = binary_op1(po, v, w, NB_SLOT(nb_rshift));
  if (!IS_IMPLEMENTED(result)) {
    vinfo_decref(result, po);
    if (Psyco_FastType(v) == &PyCFunction_Type)
      return psyco_generic_call(po, cimpl_rshift_error,
                                CfCommonNewRefPyObject, "vv", v, w);
    return binop_type_error(po, v, w, ">>");
  }
  return result;
}
BINARY_FUNC(PsycoNumber_Subtract, nb_subtract, "-")
BINARY_FUNC(PsycoNumber_Divmod, nb_divmod, "divmod()")

/* XXX tp_flags test -- not done in Python either, check future releases */
BINARY_FUNC(PsycoNumber_FloorDivide, nb_floor_divide, "//")
BINARY_FUNC(PsycoNumber_TrueDivide, nb_true_divide, "/")
BINARY_FUNC(PsycoNumber_MatrixMultiply, nb_matrix_multiply, "@")

/* the sequence part of PyNumber_Add, _Multiply, _InPlaceAdd and
   _InPlaceMultiply, after the number slots gave NotImplemented, at run
   time: what compiled code calls once a heap type's sequence slot has
   changed */
enum { SEQ_ADD, SEQ_MUL, SEQ_IADD, SEQ_IMUL };

static PyObject *cimpl_sequence_repeat(ssizeargfunc f, PyObject *seq,
                                       PyObject *n) {
  Py_ssize_t count;
  if (!PyIndex_Check(n)) {
    PyErr_Format(PyExc_TypeError,
                 "can't multiply sequence by non-int of type '%.200s'",
                 Py_TYPE(n)->tp_name);
    return NULL;
  }
  count = PyNumber_AsSsize_t(n, PyExc_OverflowError);
  if (count == -1 && PyErr_Occurred())
    return NULL;
  return f(seq, count);
}

static PyObject *cimpl_sequence_op(PyObject *v, PyObject *w, long op) {
  PySequenceMethods *mv = Py_TYPE(v)->tp_as_sequence;
  PySequenceMethods *mw = Py_TYPE(w)->tp_as_sequence;
  static const char *const names[] = {"+", "*", "+=", "*="};
  switch (op) {
  case SEQ_ADD:
    if (mv && mv->sq_concat)
      return mv->sq_concat(v, w);
    break;
  case SEQ_IADD:
    if (mv && (mv->sq_inplace_concat || mv->sq_concat))
      return (mv->sq_inplace_concat ? mv->sq_inplace_concat
                                    : mv->sq_concat)(v, w);
    break;
  case SEQ_MUL:
    if (mv && mv->sq_repeat)
      return cimpl_sequence_repeat(mv->sq_repeat, v, w);
    if (mw && mw->sq_repeat)
      return cimpl_sequence_repeat(mw->sq_repeat, w, v);
    break;
  default: /* SEQ_IMUL */
    if (mv != NULL) {
      if (mv->sq_inplace_repeat || mv->sq_repeat)
        return cimpl_sequence_repeat(mv->sq_inplace_repeat
                                         ? mv->sq_inplace_repeat
                                         : mv->sq_repeat,
                                     v, w);
    } else if (mw && mw->sq_repeat)
      return cimpl_sequence_repeat(mw->sq_repeat, w, v);
    break;
  }
  PyErr_Format(PyExc_TypeError,
               "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'",
               names[op], Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
  return NULL;
}

static vinfo_t *sequence_op_changed(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                                    long op) {
  return psyco_generic_call(po, cimpl_sequence_op, CfCommonNewRefPyObject,
                            "vvl", v, w, op);
}

/* each sequence slot below: 1 (read), 0 (changed), -1 */
#define SQ_SLOT_OR(tp, name, f, changed_op)                                    \
  switch (SQ_SLOT_OF(tp, name, f)) {                                           \
  case 1:                                                                      \
    break;                                                                     \
  case 0:                                                                      \
    return sequence_op_changed(po, v, w, changed_op);                          \
  default:                                                                     \
    return NULL;                                                               \
  }

DEFINEFN
vinfo_t *PsycoNumber_Add(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t *result = binary_op1(po, v, w, NB_SLOT(nb_add));
  if (!IS_IMPLEMENTED(result)) {
    binaryfunc f;
    vinfo_decref(result, po);
    SQ_SLOT_OR(Psyco_FastType(v), sq_concat, &f, SEQ_ADD);
    if (f != NULL) {
      result = Psyco_META2(po, f, CfCommonNewRefPyObject, "vv", v, w);
    } else {
      return binop_type_error(po, v, w, "+");
    }
  }
  return result;
}

static vinfo_t *psequence_repeat(PsycoObject *po, void *repeatfunc,
                                 vinfo_t *vseq, vinfo_t *vn) {
  PyTypeObject *tp = Psyco_FastType(vn);
  vinfo_t *vcount;
  vinfo_t *result;

  if (PyType_TypeCheck(tp, &PyLong_Type)) {
    vcount = PsycoLong_AsLong(po, vn);
  } else if (PsycoIndex_Check(tp)) {
    vcount = psyco_generic_call(po, PyNumber_AsSsize_t, CF_AS_SSIZE_T,
                                "vl", vn, (long)PyExc_OverflowError);
  } else {
    return type_error_tp(po, "can't multiply sequence by non-int of type '%.200s'",
                         tp);
  }
  if (vcount == NULL)
    return NULL;

  result =
      Psyco_META2(po, repeatfunc, CfCommonNewRefPyObject, "vv", vseq, vcount);
  vinfo_decref(vcount, po);
  return result;
}

DEFINEFN
vinfo_t *PsycoNumber_Multiply(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  /* This has varied a lot over recent Python versions.
     2.2 has no special code here; instead, int_mul and long_mul
     check if they should delegate to the other argument's
     sq_repeat. In 2.3 there is code similar to the code below:
     int_mul and long_mul just return NotImplemented, and when
     they do we check if we should proceed with sq_repeat.
     I use the 2.3 semantics even with earlier Python versions.
     Only convoluted user code would see the difference. */

  vinfo_t *result = binary_op1(po, v, w, NB_SLOT(nb_multiply));
  if (!IS_IMPLEMENTED(result)) {
    ssizeargfunc f;
    vinfo_decref(result, po);
    SQ_SLOT_OR(Psyco_FastType(v), sq_repeat, &f, SEQ_MUL);
    if (f != NULL)
      return psequence_repeat(po, f, v, w);
    SQ_SLOT_OR(Psyco_FastType(w), sq_repeat, &f, SEQ_MUL);
    if (f != NULL)
      return psequence_repeat(po, f, w, v);
    result = binop_type_error(po, v, w, "*");
  }
  return result;
}

DEFINEFN
vinfo_t *PsycoNumber_Remainder(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  return binary_op(po, v, w, NB_SLOT(nb_remainder), "%");
}

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

/* abstract.c's binary_iop1 at run time, as cimpl_binary_op1 */
static PyObject *cimpl_binary_iop1(PyObject *v, PyObject *w, long iop_slot,
                                   long op_slot) {
  PyNumberMethods *mv = Py_TYPE(v)->tp_as_number;
  if (mv != NULL) {
    binaryfunc slot = *NB_BINOP(mv, iop_slot);
    if (slot) {
      PyObject *x = slot(v, w);
      if (x != Py_NotImplemented)
        return x;
      Py_DECREF(x);
    }
  }
  return cimpl_binary_op1(v, w, op_slot);
}

static vinfo_t *binary_iop1(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                            const int iop_slot, const int op_slot) {
  binaryfunc slot;
  PyTypeObject *vtp = Psyco_NeedType(po, v);
  if (vtp == NULL)
    return NULL;

  switch (NB_SLOT_OF(vtp, iop_slot, &slot)) {
  case 1:
    break;
  case 0:
    return psyco_generic_call(po, cimpl_binary_iop1,
                              CfCommonCheckNotImplemented, "vvll", v, w,
                              (long)iop_slot, (long)op_slot);
  default:
    return NULL;
  }
  if (slot) {
    vinfo_t *x = Psyco_META2(po, slot, CfCommonCheckNotImplemented, "vv", v, w);
    if (IS_IMPLEMENTED(x))
      return x;
    vinfo_decref(x, po);
  }
  return binary_op1(po, v, w, op_slot);
}

static vinfo_t *binary_iop(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                           const int iop_slot, const int op_slot,
                           const char *op_name) {
  vinfo_t *result = binary_iop1(po, v, w, iop_slot, op_slot);
  if (!IS_IMPLEMENTED(result)) {
    vinfo_decref(result, po);
    return binop_type_error(po, v, w, op_name);
  }
  return result;
}

#define INPLACE_BINOP(func, iop, op, op_name)                                  \
  DEFINEFN vinfo_t *func(PsycoObject *po, vinfo_t *v, vinfo_t *w) {            \
    return binary_iop(po, v, w, NB_SLOT(iop), NB_SLOT(op), op_name);           \
  }

INPLACE_BINOP(PsycoNumber_InPlaceOr, nb_inplace_or, nb_or, "|=")
INPLACE_BINOP(PsycoNumber_InPlaceXor, nb_inplace_xor, nb_xor, "^=")
INPLACE_BINOP(PsycoNumber_InPlaceAnd, nb_inplace_and, nb_and, "&=")
INPLACE_BINOP(PsycoNumber_InPlaceLshift, nb_inplace_lshift, nb_lshift, "<<=")
INPLACE_BINOP(PsycoNumber_InPlaceRshift, nb_inplace_rshift, nb_rshift, ">>=")
INPLACE_BINOP(PsycoNumber_InPlaceSubtract, nb_inplace_subtract, nb_subtract,
              "-=")
INPLACE_BINOP(PsycoNumber_InPlaceFloorDivide, nb_inplace_floor_divide,
              nb_floor_divide, "//=")
INPLACE_BINOP(PsycoNumber_InPlaceTrueDivide, nb_inplace_true_divide,
              nb_true_divide, "/=")
INPLACE_BINOP(PsycoNumber_InPlaceMatrixMultiply, nb_inplace_matrix_multiply,
              nb_matrix_multiply, "@=")

DEFINEFN
vinfo_t *PsycoNumber_InPlaceAdd(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t *result =
      binary_iop1(po, v, w, NB_SLOT(nb_inplace_add), NB_SLOT(nb_add));
  if (!IS_IMPLEMENTED(result)) {
    PyTypeObject *vtp = Psyco_FastType(v);
    binaryfunc f;
    vinfo_decref(result, po);
    SQ_SLOT_OR(vtp, sq_inplace_concat, &f, SEQ_IADD);
    if (f == NULL)
      SQ_SLOT_OR(vtp, sq_concat, &f, SEQ_IADD);
    if (f != NULL)
      return Psyco_META2(po, f, CfCommonNewRefPyObject, "vv", v, w);
    result = binop_type_error(po, v, w, "+=");
  }
  return result;
}

DEFINEFN
vinfo_t *PsycoNumber_InPlaceMultiply(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t *result =
      binary_iop1(po, v, w, NB_SLOT(nb_inplace_multiply), NB_SLOT(nb_multiply));
  if (!IS_IMPLEMENTED(result)) {
    PyTypeObject *vtp = Psyco_FastType(v);
    PyTypeObject *wtp = Psyco_FastType(w);
    ssizeargfunc f;
    vinfo_decref(result, po);
    if (vtp->tp_as_sequence != NULL) {
      SQ_SLOT_OR(vtp, sq_inplace_repeat, &f, SEQ_IMUL);
      if (f == NULL)
        SQ_SLOT_OR(vtp, sq_repeat, &f, SEQ_IMUL);
      if (f != NULL)
        return psequence_repeat(po, f, v, w);
    } else {
      /* Note that the right hand operand should not be
       * mutated in this case so sq_inplace_repeat is not
       * used. */
      SQ_SLOT_OR(wtp, sq_repeat, &f, SEQ_IMUL);
      if (f != NULL)
        return psequence_repeat(po, f, w, v);
    }
    result = binop_type_error(po, v, w, "*=");
  }
  return result;
}

DEFINEFN
vinfo_t *PsycoNumber_InPlaceRemainder(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  return binary_iop(po, v, w, NB_SLOT(nb_inplace_remainder),
                    NB_SLOT(nb_remainder), "%");
}

DEFINEFN
vinfo_t *PsycoNumber_InPlacePower(PsycoObject *po, vinfo_t *v1, vinfo_t *v2,
                                  vinfo_t *v3) {
  /* XXX implement the ternary operators */
  return psyco_generic_call(po, PyNumber_InPlacePower, CfCommonNewRefPyObject,
                            "vvv", v1, v2, v3);
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
    vinfo_t *key_value = PsycoLong_AsLong(po, key);
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
    vinfo_t *key_value = PsycoLong_AsLong(po, key);
    if (key_value == NULL)
      return false;
    result = PsycoSequence_SetItem(po, o, key_value, value);
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
