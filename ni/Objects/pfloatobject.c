/*
 * Virtual floats. Every arithmetic result is a new virtual float whose
 * [iFLOAT_OB_FVAL] holds the double, so 'a + b * c' is never boxed unless it
 * escapes. The backend has no floating-point instructions, so operations
 * are tiny C helpers called with CfPure, which fold at compile time when
 * the operands are constants.
 *
 * + - * / % unary - abs and bool are generated (pfloatobject_gen.h, by
 * tools/metac/gen_float.py), helpers included; unary + by the general
 * translator (pfloatslots_gen.h, D83). Hand-written here: the
 * representation (psyco_convert_to_double for CONVERT_TO_DOUBLE, compute
 * functions) and comparisons.
 * The double is one word holding its bits, at run time (a stack slot) and
 * at compile time (source_known_t.value); upstream (32-bit) used two words.
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

DEFINEFN double cimpl_fp_from_long(long value) { return (double)value; }

DEFINEFN double cimpl_fp_from_float(float value) { return (double)value; }

static int cimpl_fp_fits_long(double whole) {
  return LONG_MIN < whole && whole < LONG_MAX; /* false for NaN */
}

static long cimpl_fp_to_long(double whole) { return (long)whole; }

/* float___trunc___impl */
static double cimpl_fp_trunc(double x) {
  double wholepart;
  (void)modf(x, &wholepart);
  return wholepart;
}

/* float___round___impl without ndigits: round half to even */
static double cimpl_fp_round_even(double x) {
  double rounded = round(x);
  if (fabs(x - rounded) == 0.5)
    rounded = 2.0 * round(x / 2.0);
  return rounded;
}

DEFINEFN
vinfo_t *PsycoFloat_Trunc(PsycoObject *po, vinfo_t *vd) {
  return psyco_generic_call(po, cimpl_fp_trunc, CfPure | CfReturnTypeDouble,
                            "d", vd);
}

DEFINEFN
vinfo_t *PsycoFloat_RoundEven(PsycoObject *po, vinfo_t *vd) {
  return psyco_generic_call(po, cimpl_fp_round_even,
                            CfPure | CfReturnTypeDouble, "d", vd);
}

DEFINEFN
int PsycoFloat_WholeToLong(PsycoObject *po, vinfo_t *vwhole,
                           vinfo_t **result) {
  vinfo_t *x;
  int r;
  vinfo_t *fits = psyco_generic_call(po, cimpl_fp_fits_long,
                                     CfPure | CfReturnTypeInt, "d", vwhole);
  if (fits == NULL)
    return -1;
  r = runtime_NON_NULL_t(po, fits); /* consumes fits */
  if (r != true)
    return r == false ? 0 : -1;
  x = psyco_generic_call(po, cimpl_fp_to_long, CfPure | CfReturnTypeLong, "d",
                         vwhole);
  if (x == NULL)
    return -1;
  *result = PsycoLong_FROM_WORD(x);
  return 1;
}

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

/* unreachable in Ni: nothing calls direct_xobj_vinfo from outside the
   direct_compute functions since upstream's compactobject.c went
   (KNOWN_BUGS F6) */
static PyObject *direct_compute_float(vinfo_t *floatobj, char *data) {
  double value;
  long bits = direct_read_vinfo(vinfo_getitem(floatobj, iFLOAT_OB_FVAL), data);
  if (PyErr_Occurred())
    return NULL;
  memcpy(&value, &bits, sizeof(value));
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

#define RELEASE_DOUBLE(v) vinfo_decref(v, po);

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
    /* fall back; NULL on MemoryError (F104) */
    return psyco_generic_call(po, PyFloat_Type.tp_richcompare,
                              CfCommonNewRefPyObject | CfPure, "vvl", v, w,
                              (long)op);
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
    return r != NULL ? PsycoBool_FROM_WORD(r) : NULL;
  }

  return psyco_vi_NotImplemented();
}

/* float_hash: _Py_HashDouble(ob_fval), which never returns -1 */
static vinfo_t *pfloat_hash(PsycoObject *po, vinfo_t *v) {
  vinfo_t *d = PsycoFloat_AS_DOUBLE(po, v);
  if (d == NULL)
    return NULL;
  return psyco_generic_call(po, _Py_HashDouble, CfPure | CfReturnTypeLong, "d",
                            d);
}

/* + - * / % unary - abs bool, generated from Objects/floatobject.c */
#include "pfloatobject_gen.h"
/* unary + (float_float: an exact float is returned itself, F22) */
#include "pfloatslots_gen.h"

INITIALIZATIONFN
void psy_floatobject_init(void) {
  PyNumberMethods *m = PyFloat_Type.tp_as_number;

  pfloat_generated_init(m);
  gen_floatobject_init();


  Psyco_DefineMeta(PyFloat_Type.tp_richcompare, pfloat_richcompare);
  Psyco_DefineMeta(PyFloat_Type.tp_hash, pfloat_hash);

  INIT_SVIRTUAL(psyco_computed_float, compute_float, direct_compute_float, 0, 0,
                0);
}

#else  /* !HAVE_FP_FN_CALLS */
INITIALIZATIONFN
void psy_floatobject_init(void) {}
#endif /* !HAVE_FP_FN_CALLS */
