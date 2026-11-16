/*
 * Virtual ints, following the one-digit fast paths of 3.7's
 * Objects/longobject.c. A result is a virtual int whose [iLONG_VALUE] holds
 * the value as a machine word; it is boxed with PyLong_FromLong only if it
 * escapes. An operand takes the fast path if it has one digit, i.e.
 * |value| <= PyLong_MASK: for an int in memory that is |ob_size| <= 1, for
 * a virtual one a range test on the word. Both are run-time conditions
 * (respawn), and an operand that fails falls back to CPython's slot, as
 * longobject.c does.
 *
 * + - * unary - and ~ are generated (plongobject_gen.h, by
 * tools/metac/gen_long.py; QUESTIONS 26). Hand-written here: what the
 * generator can't derive from the C, i.e. plong_medium_value (MEDIUM_VALUE
 * and its guard on a virtual int, which has no ob_size), the fallbacks,
 * long_bool and long_richcompare (long_compare reduced to one digit).
 */
#include "plongobject.h"
#include "pboolobject.h"
#include "pfloatobject.h"

/* MEDIUM_VALUE(v) of an int that is one digit at run time, as a new
   reference in *pval. Returns 1; 0 if it isn't one digit (the caller falls
   back); -1 on error or promotion. */
static int plong_medium_value(PsycoObject *po, vinfo_t *v, vinfo_t **pval) {
  vinfo_t *size, *digit0, *t;
  condition_code_t cc;

  if (PsycoLong_IsVirtual(v)) {
    vinfo_t *w = v->array->items[iLONG_VALUE];
    /* -PyLong_MASK <= w <= PyLong_MASK, as one unsigned comparison */
    t = integer_add_i(po, w, PyLong_MASK, false);
    if (t == NULL)
      return -1;
    cc = integer_cmp_i(po, t, 2 * PyLong_MASK, Py_LE | COMPARE_UNSIGNED);
    vinfo_decref(t, po);
    if (cc == CC_ERROR)
      return -1;
    if (!runtime_condition_t(po, cc))
      return 0;
    vinfo_incref(w);
    *pval = w;
    return 1;
  }

  size = psyco_get_const(po, v, LONG_ob_size);
  if (size == NULL)
    return -1;
  /* -1 <= ob_size <= 1 */
  t = integer_add_i(po, size, 1, false);
  if (t == NULL)
    return -1;
  cc = integer_cmp_i(po, t, 2, Py_LE | COMPARE_UNSIGNED);
  vinfo_decref(t, po);
  if (cc == CC_ERROR)
    return -1;
  if (!runtime_condition_t(po, cc))
    return 0;
  /* zero has no digit to read */
  cc = integer_cmp_i(po, size, 0, Py_EQ);
  if (cc == CC_ERROR)
    return -1;
  if (runtime_condition_f(po, cc)) {
    *pval = vinfo_new(CompileTime_New(0));
    return 1;
  }
  digit0 = psyco_get_const(po, v, LONG_ob_digit0);
  if (digit0 == NULL)
    return -1;
  /* ob_size is -1 or 1 here */
  *pval = integer_mul(po, size, digit0, false);
  return *pval != NULL ? 1 : -1;
}

DEFINEFN
vinfo_t *PsycoLong_AsLong(PsycoObject *po, vinfo_t *v) {
  if (PsycoLong_IsVirtual(v)) {
    vinfo_t *w = v->array->items[iLONG_VALUE];
    vinfo_incref(w);
    return w;
  }
  return psyco_generic_call(po, PyLong_AsLong,
                            CfReturnTypeLong | CfPyErrCheckMinus1, "v", v);
}

DEFINEFN
bool PsycoLong_AsDouble(PsycoObject *po, vinfo_t *v, vinfo_t **vd) {
  /* the backend has no int-to-double conversion: a C helper. A virtual
     int's word converts directly; (double)long rounds to nearest even,
     as PyLong_AsDouble does. */
  if (PsycoLong_IsVirtual(v)) {
    *vd = psyco_generic_call(po, cimpl_fp_from_long, CfPure | CfReturnTypeDouble,
                             "v", v->array->items[iLONG_VALUE]);
    return *vd != NULL;
  }
  if ((*vd = psyco_generic_call(po, PyLong_AsDouble,
                                CfPyErrCheck | CfReturnTypeDouble, "v", v)) ==
      NULL) {
    return false;
  }
  return true;
}

DEFINEFN
vinfo_t *PsycoLong_FromUnsignedLong(PsycoObject *po, vinfo_t *v) {
  vinfo_t *result = psyco_generic_call(po, PyLong_FromUnsignedLong,
                                       CfCommonNewRefPyObject, "v", v);
  if (result != NULL)
    Psyco_AssertType(po, result, &PyLong_Type);
  return result;
}

DEFINEFN
vinfo_t *PsycoLong_FromLong(PsycoObject *po, vinfo_t *v) {
  vinfo_incref(v);
  return PsycoLong_FROM_WORD(v);
}

DEFINEFN
vinfo_t *PsycoLong_FROM_LONG(PsycoObject *po, vinfo_t *v) {
  return PsycoLong_FROM_WORD(v);
}

/***************************************************************/
/*** computing a virtual int                                 ***/

static bool compute_long(PsycoObject *po, vinfo_t *longobj) {
  vinfo_t *newobj;
  vinfo_t *x = vinfo_getitem(longobj, iLONG_VALUE);
  if (x == NULL)
    return false;
  newobj = psyco_generic_call(po, PyLong_FromLong,
                              CfPure | CfCommonNewRefPyObject, "v", x);
  if (newobj == NULL)
    return false;
  /* move the resulting non-virtual Python object back into 'longobj' */
  vinfo_move(po, longobj, newobj);
  return true;
}

static PyObject *direct_compute_long(vinfo_t *longobj, char *data) {
  long value = direct_read_vinfo(vinfo_getitem(longobj, iLONG_VALUE), data);
  if (value == -1 && PyErr_Occurred())
    return NULL;
  return PyLong_FromLong(value);
}

DEFINEVAR source_virtual_t psyco_computed_long;

static bool compute_bool(PsycoObject *po, vinfo_t *boolobj) {
  vinfo_t *newobj;
  vinfo_t *x = vinfo_getitem(boolobj, iLONG_VALUE);
  if (x == NULL)
    return false;
  newobj = psyco_generic_call(po, PyBool_FromLong,
                              CfPure | CfCommonNewRefPyObject, "v", x);
  if (newobj == NULL)
    return false;
  vinfo_move(po, boolobj, newobj);
  return true;
}

static PyObject *direct_compute_bool(vinfo_t *boolobj, char *data) {
  long value = direct_read_vinfo(vinfo_getitem(boolobj, iLONG_VALUE), data);
  if (value == -1 && PyErr_Occurred())
    return NULL;
  return PyBool_FromLong(value);
}

DEFINEVAR source_virtual_t psyco_computed_bool;

/***************************************************************/
/*** int objects meta-implementation                         ***/

/* CHECK_BINOP: 1 if both are ints, 0 if not, -1 on error or promotion */
static int both_longs(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  int r = Psyco_VerifyType(po, v, &PyLong_Type);
  if (r != 1)
    return r;
  return Psyco_VerifyType(po, w, &PyLong_Type);
}

/* the slow path: CPython's own slot, on ints */
static vinfo_t *plong_fallback2(PsycoObject *po, void *slot, vinfo_t *v,
                                vinfo_t *w) {
  vinfo_t *x = psyco_generic_call(po, slot, CfCommonNewRefPyObject, "vv", v, w);
  if (x != NULL)
    Psyco_AssertType(po, x, &PyLong_Type);
  return x;
}

static vinfo_t *plong_fallback1(PsycoObject *po, void *slot, vinfo_t *v) {
  vinfo_t *x = psyco_generic_call(po, slot, CfCommonNewRefPyObject, "v", v);
  if (x != NULL)
    Psyco_AssertType(po, x, &PyLong_Type);
  return x;
}

/* + - * unary - ~, generated from Objects/longobject.c */
#include "plongobject_gen.h"

/* long_hash: a one-digit int hashes to its value, but -1 to -2 (-1 means
   an error) */
static long cimpl_long_hash1(long v) { return v == -1 ? -2 : v; }

static vinfo_t *plong_hash(PsycoObject *po, vinfo_t *v) {
  vinfo_t *a, *x;
  int r = plong_medium_value(po, v, &a);
  if (r < 0)
    return NULL;
  if (r == 0)
    return psyco_generic_call(po, PyLong_Type.tp_hash,
                              CfReturnTypeLong | CfPyErrIfMinus1, "v", v);
  x = psyco_generic_call(po, cimpl_long_hash1, CfPure | CfReturnTypeLong, "v",
                         a);
  vinfo_decref(a, po);
  return x;
}

/* long_bool: ob_size != 0, i.e. the value isn't zero */
static vinfo_t *plong_bool(PsycoObject *po, vinfo_t *v) {
  condition_code_t cc;
  if (PsycoLong_IsVirtual(v))
    cc = integer_cmp_i(po, v->array->items[iLONG_VALUE], 0, Py_NE);
  else {
    vinfo_t *size = psyco_get_const(po, v, LONG_ob_size);
    if (size == NULL)
      return NULL;
    cc = integer_cmp_i(po, size, 0, Py_NE);
  }
  if (cc == CC_ERROR)
    return NULL;
  return psyco_vinfo_condition(po, cc);
}

/* long_richcompare: for one-digit ints, long_compare is the comparison of
   their values */
static vinfo_t *plong_richcompare(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                                  int op) {
  vinfo_t *a, *b;
  condition_code_t cc;
  int r = both_longs(po, v, w);
  if (r < 0)
    return NULL;
  if (r == 0)
    return psyco_vi_NotImplemented();

  r = plong_medium_value(po, v, &a);
  if (r < 0)
    return NULL;
  if (r > 0) {
    r = plong_medium_value(po, w, &b);
    if (r <= 0)
      vinfo_decref(a, po);
    if (r < 0)
      return NULL;
  }
  if (r == 0)
    return psyco_generic_call(po, PyLong_Type.tp_richcompare,
                              CfCommonNewRefPyObject, "vvl", v, w, op);
  cc = integer_cmp(po, a, b, op);
  vinfo_decref(b, po);
  vinfo_decref(a, po);
  if (cc == CC_ERROR)
    return NULL;
  return PsycoBool_FromCondition(po, cc);
}

#define RETLONG(arity, cname, slotname)                                        \
  DEF_KNOWN_RET_TYPE_##arity(cname, PyLong_Type.tp_as_number->slotname,        \
                             (arity) == 1 ? (CfCommonNewRefPyObject)           \
                                          : (CfCommonCheckNotImplemented),     \
                             &PyLong_Type)

#define RETFLOAT(arity, cname, slotname)                                       \
  DEF_KNOWN_RET_TYPE_##arity(cname, PyLong_Type.tp_as_number->slotname,        \
                             (arity) == 1 ? (CfCommonNewRefPyObject)           \
                                          : (CfCommonCheckNotImplemented),     \
                             &PyFloat_Type)

RETLONG(2, plong_mod, nb_remainder)
RETLONG(1, plong_pos, nb_positive)
RETLONG(1, plong_abs, nb_absolute)
RETLONG(2, plong_lshift, nb_lshift)
RETLONG(2, plong_rshift, nb_rshift)
RETLONG(2, plong_and, nb_and)
RETLONG(2, plong_xor, nb_xor)
RETLONG(2, plong_or, nb_or)
RETLONG(2, plong_div, nb_floor_divide)
RETFLOAT(2, plong_true_divide, nb_true_divide)

#undef RETLONG
#undef RETFLOAT

static vinfo_t *plong_pow(PsycoObject *po, vinfo_t *v1, vinfo_t *v2,
                          vinfo_t *v3) {
  /* XXX tony: this is done rather then using a RET* macro because
  nb_power can return PyLongObject or PyFloatObject? Is this true.
   */
  return psyco_generic_call(po, PyLong_Type.tp_as_number->nb_power,
                            CfCommonCheckNotImplemented, "vvv", v1, v2, v3);
}

INITIALIZATIONFN
void psy_longobject_init(void) {
  PyNumberMethods *m = PyLong_Type.tp_as_number;

  plong_generated_init(m);
  Psyco_DefineMeta(m->nb_true_divide, plong_true_divide);
  Psyco_DefineMeta(m->nb_remainder, plong_mod);
  Psyco_DefineMeta(m->nb_power, plong_pow);
  Psyco_DefineMeta(m->nb_positive, plong_pos);
  Psyco_DefineMeta(m->nb_absolute, plong_abs);
  Psyco_DefineMeta(m->nb_lshift, plong_lshift);
  Psyco_DefineMeta(m->nb_rshift, plong_rshift);
  Psyco_DefineMeta(m->nb_and, plong_and);
  Psyco_DefineMeta(m->nb_xor, plong_xor);
  Psyco_DefineMeta(m->nb_or, plong_or);
  Psyco_DefineMeta(m->nb_floor_divide, plong_div);
  Psyco_DefineMeta(m->nb_bool, plong_bool);
  Psyco_DefineMeta(PyLong_Type.tp_richcompare, plong_richcompare);
  Psyco_DefineMeta(PyLong_Type.tp_hash, plong_hash);

  INIT_SVIRTUAL(psyco_computed_long, compute_long, direct_compute_long, 0, 0,
                0);
  INIT_SVIRTUAL(psyco_computed_bool, compute_bool, direct_compute_bool, 0, 0,
                0);
}
