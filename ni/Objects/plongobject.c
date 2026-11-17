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
#include "ptupleobject.h"

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

DEFINEFN int PsycoLong_MediumValue(PsycoObject *po, vinfo_t *v,
                               vinfo_t **pval) {
  return plong_medium_value(po, v, pval);
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
                              CfCommonNewRefPyObject, "vvl", v, w, (long)op);
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

RETLONG(1, plong_pos, nb_positive)
RETLONG(1, plong_abs, nb_absolute)
RETFLOAT(2, plong_true_divide, nb_true_divide)

#undef RETLONG
#undef RETFLOAT

/* & | ^ << >> on one-digit ints as word operations. Hand-written: 3.7's
   long_and etc. work on digit arrays with no one-digit fast path, but
   3.12's long_and/long_or/long_xor have one (_PyLong_IsCompact), which
   gen_long.py can translate there. A word shift has Python's meaning for
   >> (a count of 64 or more gives the sign, a negative one raises
   ValueError, as integer_rshift does); for << the count must be at most
   32, which keeps a one-digit value inside a word. */
enum { OP_AND, OP_OR, OP_XOR, OP_LSHIFT, OP_RSHIFT };

static vinfo_t *plong_bitop(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op,
                            void *slot) {
  vinfo_t *a, *b, *x = NULL;
  int r = both_longs(po, v, w);
  if (r <= 0)
    return r < 0 ? NULL : psyco_vi_NotImplemented();
  r = plong_medium_value(po, v, &a);
  if (r <= 0)
    return r < 0 ? NULL : plong_fallback2(po, slot, v, w);
  r = plong_medium_value(po, w, &b);
  if (r <= 0) {
    vinfo_decref(a, po);
    return r < 0 ? NULL : plong_fallback2(po, slot, v, w);
  }
  if (op == OP_LSHIFT) {
    condition_code_t cc = integer_cmp_i(po, b, 32, Py_LE | COMPARE_UNSIGNED);
    if (cc == CC_ERROR)
      goto done;
    if (!runtime_condition_t(po, cc)) {
      x = plong_fallback2(po, slot, v, w);
      goto done;
    }
  }
  switch (op) {
  case OP_AND: x = integer_and(po, a, b); break;
  case OP_OR: x = integer_or(po, a, b); break;
  case OP_XOR: x = integer_xor(po, a, b); break;
  case OP_LSHIFT: x = integer_lshift(po, a, b); break;
  default: x = integer_rshift(po, a, b); break;
  }
  if (x != NULL)
    x = PsycoLong_FROM_WORD(x);
done:
  vinfo_decref(a, po);
  vinfo_decref(b, po);
  return x;
}

#define BITOP(name, op, slotname)                                              \
  static vinfo_t *name(PsycoObject *po, vinfo_t *v, vinfo_t *w) {              \
    return plong_bitop(po, v, w, op, PyLong_Type.tp_as_number->slotname);      \
  }
BITOP(plong_and, OP_AND, nb_and)
BITOP(plong_or, OP_OR, nb_or)
BITOP(plong_xor, OP_XOR, nb_xor)
BITOP(plong_lshift, OP_LSHIFT, nb_lshift)
BITOP(plong_rshift, OP_RSHIFT, nb_rshift)
#undef BITOP

/* // and % on one-digit ints with Python's floor rounding (l_divmod).
   The backend has no divide, so they are pure C helpers on words (they
   fold on constants), except that a constant positive power-of-two
   divisor is a mask or an arithmetic shift. A zero divisor is the real
   slot, which raises. 3.7's fast_floor_div and fast_mod need exactly one
   digit each and so are the same thing. */
static long cimpl_floor_div(long a, long b) {
  long q = a / b;
  if (a % b != 0 && (a < 0) != (b < 0))
    q--;
  return q;
}

static long cimpl_floor_mod(long a, long b) {
  long m = a % b;
  if (m != 0 && (m < 0) != (b < 0))
    m += b;
  return m;
}

/* the quotient and/or remainder of two one-digit words (*pq, *pm may be
   NULL when not wanted): 1, 0 if the divisor is zero (the caller takes the
   real slot, which raises), -1 on error */
static int floor_divmod_words(PsycoObject *po, vinfo_t *a, vinfo_t *b,
                              vinfo_t **pq, vinfo_t **pm) {
  condition_code_t cc;
  if (is_compiletime(b->source) && CompileTime_Get(b->source)->value > 0 &&
      (CompileTime_Get(b->source)->value &
       (CompileTime_Get(b->source)->value - 1)) == 0) {
    long d = CompileTime_Get(b->source)->value;
    int k = 0;
    while ((1L << k) != d)
      k++;
    if (pq != NULL && (*pq = integer_rshift_i(po, a, k)) == NULL)
      return -1;
    if (pm != NULL) {
      vinfo_t *mask = vinfo_new(CompileTime_New(d - 1));
      *pm = integer_and(po, a, mask);
      vinfo_decref(mask, po);
      if (*pm == NULL) {
        if (pq != NULL)
          vinfo_decref(*pq, po);
        return -1;
      }
    }
    return 1;
  }
  cc = integer_cmp_i(po, b, 0, Py_NE);
  if (cc == CC_ERROR)
    return -1;
  if (!runtime_condition_t(po, cc))
    return 0;
  if (pq != NULL &&
      (*pq = psyco_generic_call(po, cimpl_floor_div, CfPure | CfReturnTypeLong,
                                "vv", a, b)) == NULL)
    return -1;
  if (pm != NULL &&
      (*pm = psyco_generic_call(po, cimpl_floor_mod, CfPure | CfReturnTypeLong,
                                "vv", a, b)) == NULL) {
    if (pq != NULL)
      vinfo_decref(*pq, po);
    return -1;
  }
  return 1;
}

/* the real slot; divmod's returns a tuple, not an int */
static vinfo_t *divmod_fallback(PsycoObject *po, int which, void *slot,
                                vinfo_t *v, vinfo_t *w) {
  if (which == 2)
    return psyco_generic_call(po, slot, CfCommonNewRefPyObject, "vv", v, w);
  return plong_fallback2(po, slot, v, w);
}

/* which: 0 //, 1 %, 2 divmod() */
static vinfo_t *plong_divmod_op(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                                int which, void *slot) {
  vinfo_t *a, *b, *q = NULL, *m = NULL, *x = NULL;
  int r = both_longs(po, v, w);
  if (r <= 0)
    return r < 0 ? NULL : psyco_vi_NotImplemented();
  r = plong_medium_value(po, v, &a);
  if (r <= 0)
    return r < 0 ? NULL : divmod_fallback(po, which, slot, v, w);
  r = plong_medium_value(po, w, &b);
  if (r <= 0) {
    vinfo_decref(a, po);
    return r < 0 ? NULL : divmod_fallback(po, which, slot, v, w);
  }
  r = floor_divmod_words(po, a, b, which != 1 ? &q : NULL,
                         which != 0 ? &m : NULL);
  if (r == 0)
    x = divmod_fallback(po, which, slot, v, w);
  else if (r > 0) {
    if (which == 0)
      x = PsycoLong_FROM_WORD(q);
    else if (which == 1)
      x = PsycoLong_FROM_WORD(m);
    else {
      vinfo_t *items[2];
      items[0] = PsycoLong_FROM_WORD(q);
      items[1] = PsycoLong_FROM_WORD(m);
      x = PsycoTuple_New(2, items);
      vinfo_decref(items[0], po);
      vinfo_decref(items[1], po);
    }
  }
  vinfo_decref(a, po);
  vinfo_decref(b, po);
  return x;
}

static vinfo_t *plong_div(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  return plong_divmod_op(po, v, w, 0,
                         PyLong_Type.tp_as_number->nb_floor_divide);
}

static vinfo_t *plong_mod(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  return plong_divmod_op(po, v, w, 1, PyLong_Type.tp_as_number->nb_remainder);
}

static vinfo_t *plong_divmod(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  return plong_divmod_op(po, v, w, 2, PyLong_Type.tp_as_number->nb_divmod);
}

static vinfo_t *plong_pow(PsycoObject *po, vinfo_t *v1, vinfo_t *v2,
                          vinfo_t *v3) {
  /* not a RET* macro: long_pow returns a float for a negative exponent
     without modulus (it calls float_pow), so the result type is unknown */
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
  Psyco_DefineMeta(m->nb_divmod, plong_divmod);
  Psyco_DefineMeta(m->nb_bool, plong_bool);
  Psyco_DefineMeta(PyLong_Type.tp_richcompare, plong_richcompare);
  Psyco_DefineMeta(PyLong_Type.tp_hash, plong_hash);

  INIT_SVIRTUAL(psyco_computed_long, compute_long, direct_compute_long, 0, 0,
                0);
  INIT_SVIRTUAL(psyco_computed_bool, compute_bool, direct_compute_bool, 0, 0,
                0);
}
