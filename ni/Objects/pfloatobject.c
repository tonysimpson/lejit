/*
 * Virtual floats. Every arithmetic result is a new virtual float whose
 * [iFLOAT_OB_FVAL] holds the double, so 'a + b * c' is never boxed unless it
 * escapes. The operations are tiny C helpers (cimpl_fp_*) called with CfPure,
 * so they fold at compile time when both operands are constants.
 * Suspected bugs: direct_compute_float converts the stored 64-bit word
 * numerically instead of reinterpreting its bits; pfloat_div has no
 * ZeroDivisionError check.
 */
#include "pfloatobject.h"
#include "pboolobject.h"
#include "plongobject.h"

#if HAVE_FP_FN_CALLS

#define CF_NONPURE_FP_HELPER                                                   \
  (CfPure | CfNoReturnValue | CfReturnTypeInt | CfPyErrIfNonNull)
#ifdef WANT_SIGFPE_HANDLER
#define CF_PURE_FP_HELPER                                                      \
  (CfPure | CfNoReturnValue | CfReturnTypeInt | CfPyErrIfNonNull)
#else
#define CF_PURE_FP_HELPER (CfPure | CfNoReturnValue)
#endif

static int cimpl_fp_eq_fp(double a, double b) { return a == b; }
static int cimpl_fp_ne_fp(double a, double b) { return a != b; }
static int cimpl_fp_le_fp(double a, double b) { return a <= b; }
static int cimpl_fp_lt_fp(double a, double b) { return a < b; }

static double cimpl_fp_add(double a, double b) { return a + b; }

static double cimpl_fp_sub(double a, double b) { return a - b; }

static double cimpl_fp_mul(double a, double b) { return a * b; }

static double cimpl_fp_div(double a, double b) { return a / b; }

static double cimpl_fp_neg(double a) { return -a; }

static double cimpl_fp_abs(double a) { return fabs(a); }

DEFINEFN double cimpl_fp_from_long(long value) { return (double)value; }

DEFINEFN double cimpl_fp_from_float(float value) { return (double)value; }

DEFINEFN
vinfo_t *PsycoFloat_FromFloat(PsycoObject *po, vinfo_t *vfloat) {
  vinfo_t *x;
  x = psyco_generic_call(po, cimpl_fp_from_float, CfReturnTypeDouble | CfPure,
                         "d", vfloat);
  if (x != NULL) {
    x = PsycoFloat_FROM_DOUBLE(x);
  }
  return x;
}

DEFINEFN
bool PsycoFloat_AsDouble(PsycoObject *po, vinfo_t *v, vinfo_t **result) {
  PyNumberMethods *nb;
  PyTypeObject *tp;

  tp = Psyco_NeedType(po, v);
  if (tp == NULL)
    return false;

  if (PsycoFloat_Check(tp)) {
    *result = PsycoFloat_AS_DOUBLE(po, v);
    if (*result == NULL)
      return false;
    vinfo_incref(*result);
    return true;
  }

  if ((nb = tp->tp_as_number) == NULL || nb->nb_float == NULL) {
    PycException_SetString(po, PyExc_TypeError, "a float is required");
    return false;
  }

  v = Psyco_META1(po, nb->nb_float, CfCommonNewRefPyObject, "v", v);
  if (v == NULL)
    return false;

  /* silently assumes the result is a float object */
  *result = PsycoFloat_AS_DOUBLE(po, v);
  if (*result == NULL) {
    vinfo_decref(v, po);
    return false;
  }
  vinfo_incref(*result);
  vinfo_decref(v, po);
  return true;
}

static bool compute_float(PsycoObject *po, vinfo_t *floatobj) {
  vinfo_t *newobj;
  vinfo_t *fval;

  /* get the field 'ob_fval' from the Python object 'floatobj' */
  fval = vinfo_getitem(floatobj, iFLOAT_OB_FVAL);
  if (fval == NULL)
    return false;

  /* call PyFloat_FromDouble() */
  newobj = psyco_generic_call(po, PyFloat_FromDouble,
                              CfPure | CfCommonNewRefPyObject, "d", fval);

  if (newobj == NULL)
    return false;

  /* move the resulting non-virtual Python object back into 'floatobj' */
  vinfo_move(po, floatobj, newobj);
  return true;
}

static PyObject *direct_compute_float(vinfo_t *floatobj, char *data) {
  double value;
  value = direct_read_vinfo(vinfo_getitem(floatobj, iFLOAT_OB_FVAL), data);
  if (PyErr_Occurred())
    return NULL;
  return PyFloat_FromDouble(value);
}

DEFINEVAR source_virtual_t psyco_computed_float;

/***************************************************************/
/*** float objects meta-implementation                       ***/

#define CONVERT_TO_DOUBLE_CORE(vobj, v, ERRORACTION)                           \
  switch (psyco_convert_to_double(po, vobj, &v)) {                             \
  case true:                                                                   \
    break; /* fine */                                                          \
  case false:                                                                  \
    return ERRORACTION; /* error or promotion */                               \
  default:                                                                     \
    ERRORACTION;                                                               \
    return psyco_vi_NotImplemented(); /* cannot do it */                       \
  }

#define CONVERT_TO_DOUBLE(vobj, v)                                             \
  CONVERT_TO_DOUBLE_CORE(vobj, v, return_null())

#define CONVERT_TO_DOUBLE2(uobj, u, vobj, v)                                   \
  CONVERT_TO_DOUBLE_CORE(uobj, u, return_null());                              \
  CONVERT_TO_DOUBLE_CORE(vobj, v, release_double(po, u))

#define RELEASE_DOUBLE(v) vinfo_decref(v, po);

#define RELEASE_DOUBLE2(v, u)                                                  \
  vinfo_decref(v, po);                                                         \
  vinfo_decref(u, po);

static vinfo_t *release_double(PsycoObject *po, vinfo_t *u) {
  vinfo_decref(u, po);
  return NULL;
}

static vinfo_t *return_null(void) { return NULL; }

DEFINEFN
int psyco_convert_to_double(PsycoObject *po, vinfo_t *vobj, vinfo_t **pv) {
  /* TypeSwitch */
  PyTypeObject *vtp = Psyco_NeedType(po, vobj);
  if (vtp == NULL)
    return false;

  if (PyType_TypeCheck(vtp, &PyLong_Type)) {
    return PsycoLong_AsDouble(po, vobj, pv);
  }
  if (PyType_TypeCheck(vtp, &PyFloat_Type)) {
    *pv = PsycoFloat_AS_DOUBLE(po, vobj);
    if (*pv == NULL)
      return false;
    vinfo_incref(*pv);
    return true;
  }
  return -1; /* cannot do it */
}

static vinfo_t *pfloat_richcompare(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                                   int op) {
  /* Python >= 2.4 */
  /* TypeSwitch */
  PyTypeObject *wtp;
  vinfo_t *a, *b, *r;
  void *fn;

  wtp = Psyco_NeedType(po, w);
  if (wtp == NULL)
    return NULL;

  a = PsycoFloat_AS_DOUBLE(po, v);
  if (a == NULL)
    return NULL;

  if (PyType_TypeCheck(wtp, &PyLong_Type)) {
    /* fall back */
    return psyco_generic_call(po, PyFloat_Type.tp_richcompare,
                              CfCommonNewRefPyObjectNoError | CfPure, "vvl", v,
                              w, op);
  }
  if (PyType_TypeCheck(wtp, &PyFloat_Type)) {
    b = PsycoFloat_AS_DOUBLE(po, w);
    if (b == NULL)
      return NULL;
#define SWAP_A_B                                                               \
  r = a;                                                                       \
  a = b;                                                                       \
  b = r;
    switch (op) {
    case Py_EQ:
      fn = cimpl_fp_eq_fp;
      break;
    case Py_NE:
      fn = cimpl_fp_ne_fp;
      break;
    case Py_LE:
      fn = cimpl_fp_le_fp;
      break;
    case Py_GE:
      fn = cimpl_fp_le_fp;
      SWAP_A_B;
      break;
    case Py_LT:
      fn = cimpl_fp_lt_fp;
      break;
    case Py_GT:
      fn = cimpl_fp_lt_fp;
      SWAP_A_B;
      break;
    default:
      Py_FatalError("bad richcmp op");
      return NULL;
    }
#undef SWAP_A_B
    r = psyco_generic_call(po, fn, CfReturnTypeInt | CfPure, "dd", a, b);
    if (r != NULL) {
      vinfo_t *cmp = r;
      r = psyco_generic_call(po, PyBool_FromLong, CfCommonNewRefPyObject, "v",
                             cmp);
      vinfo_decref(cmp, po);
      if (r != NULL)
        Psyco_AssertType(po, r, &PyBool_Type);
    }
    return r;
  }

  return psyco_vi_NotImplemented();
}

static vinfo_t *pfloat_pos(PsycoObject *po, vinfo_t *v) {
  vinfo_t *a, *x;
  CONVERT_TO_DOUBLE(v, a);
  x = PsycoFloat_FromDouble(a);
  RELEASE_DOUBLE(a);
  return x;
}

static vinfo_t *pfloat_neg(PsycoObject *po, vinfo_t *v) {
  vinfo_t *a, *x;
  CONVERT_TO_DOUBLE(v, a);
  x = psyco_generic_call(po, cimpl_fp_neg, CfPure | CfReturnTypeDouble, "d", a);
  RELEASE_DOUBLE(a);
  return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;
}

static vinfo_t *pfloat_abs(PsycoObject *po, vinfo_t *v) {
  vinfo_t *a, *x;
  CONVERT_TO_DOUBLE(v, a);
  x = psyco_generic_call(po, cimpl_fp_abs, CfPure | CfReturnTypeDouble, "d", a);
  RELEASE_DOUBLE(a);
  return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;
}

static vinfo_t *pfloat_add(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t *a, *b, *x;
  CONVERT_TO_DOUBLE2(v, a, w, b);
  x = psyco_generic_call(po, cimpl_fp_add, CfPure | CfReturnTypeDouble, "dd", a,
                         b);
  RELEASE_DOUBLE2(a, b);
  return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;
}

static vinfo_t *pfloat_sub(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t *a, *b, *x;
  CONVERT_TO_DOUBLE2(v, a, w, b);
  x = psyco_generic_call(po, cimpl_fp_sub, CfPure | CfReturnTypeDouble, "dd", a,
                         b);
  RELEASE_DOUBLE2(a, b);
  return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;
}

static vinfo_t *pfloat_mul(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t *a, *b, *x;
  CONVERT_TO_DOUBLE2(v, a, w, b);
  x = psyco_generic_call(po, cimpl_fp_mul, CfPure | CfReturnTypeDouble, "dd", a,
                         b);
  RELEASE_DOUBLE2(a, b);
  return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;
}

static vinfo_t *pfloat_div(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t *a, *b, *x;
  CONVERT_TO_DOUBLE2(v, a, w, b);
  x = psyco_generic_call(po, cimpl_fp_div, CfPure | CfReturnTypeDouble, "dd", a,
                         b);
  RELEASE_DOUBLE2(a, b);
  return x != NULL ? PsycoFloat_FROM_DOUBLE(x) : NULL;
}

INITIALIZATIONFN
void psy_floatobject_init(void) {
  PyNumberMethods *m = PyFloat_Type.tp_as_number;

  Psyco_DefineMeta(m->nb_positive, pfloat_pos);
  Psyco_DefineMeta(m->nb_negative, pfloat_neg);
  Psyco_DefineMeta(m->nb_absolute, pfloat_abs);

  Psyco_DefineMeta(m->nb_add, pfloat_add);
  Psyco_DefineMeta(m->nb_subtract, pfloat_sub);
  Psyco_DefineMeta(m->nb_multiply, pfloat_mul);
  Psyco_DefineMeta(m->nb_true_divide, pfloat_div);

  Psyco_DefineMeta(PyFloat_Type.tp_richcompare, pfloat_richcompare);

  INIT_SVIRTUAL(psyco_computed_float, compute_float, direct_compute_float, 0, 0,
                0);
}

#else  /* !HAVE_FP_FN_CALLS */
INITIALIZATIONFN
void psy_floatobject_init(void) {}
#endif /* !HAVE_FP_FN_CALLS */
