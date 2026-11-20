/*
 * Virtual floats. Every arithmetic result is a new virtual float whose
 * [iFLOAT_OB_FVAL] holds the double, so 'a + b * c' is never boxed unless it
 * escapes. The backend has no floating-point instructions, so operations
 * are tiny C helpers called with CfPure, which fold at compile time when
 * the operands are constants.
 *
 * + - * / % unary + - abs and bool are translated from floatobject.c
 * (below), their double code copied into helpers. Not translated: the
 * representation (compute functions), comparisons and hash, and the
 * conversions pbltinmodule.c uses.
 * The double is one word holding its bits, at run time (a stack slot) and
 * at compile time (source_known_t.value).
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

/* unreachable: nothing calls direct_xobj_vinfo from outside the
   direct_compute functions */
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
    /* fall back; NULL on MemoryError */
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

/* + - * / % unary + - abs bool hash, translated from Objects/floatobject.c */
/* the translation keeps CPython's unused variables */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
/* run-time copies: where a heap type's slot changed, or where the meta has no compile-time form, CPython's algorithm resumes in these */
static double
#if NI_HAS_FLOAT_ASDOUBLE_INDEX
cimpl_PyFloat_AsDouble_1(PyObject *res)
#else
cimpl_PyFloat_AsDouble_1(PyObject *op, PyObject *res)
#endif
{
#if !NI_HAS_FLOAT_ASDOUBLE_INDEX
    double val;
#endif
    Py_INCREF(res);
#if NI_HAS_FLOAT_ASDOUBLE_INDEX
    double val = PyLong_AsDouble(res);
#else
    if (!PyFloat_CheckExact(res)) {
        if (!PyFloat_Check(res)) {
            PyErr_Format(PyExc_TypeError,
                         "%.50s.__float__ returned non-float (type %.50s)",
                         op->ob_type->tp_name, res->ob_type->tp_name);
            Py_DECREF(res);
            return -1;
        }
        if (PyErr_WarnFormat(
                PyExc_DeprecationWarning, 1,
                "%.50s.__float__ returned non-float (type %.50s).  "
                "The ability to return an instance of a strict subclass of float "
                "is deprecated, and may be removed in a future version of Python.",
                op->ob_type->tp_name, res->ob_type->tp_name)) {
            Py_DECREF(res);
            return -1;
        }
    }
    val = PyFloat_AS_DOUBLE(res);
#endif
    Py_DECREF(res);
    return val;
}

static double
cimpl_PyFloat_AsDouble_2(PyObject *op, PyObject *res)
{
    double val;
    Py_INCREF(res);
#if NI_HAS_FLOAT_ASDOUBLE_INDEX
    if (!PyFloat_CheckExact(res)) {
        if (!PyFloat_Check(res)) {
            PyErr_Format(PyExc_TypeError,
                         "%.50s.__float__ returned non-float (type %.50s)",
                         Py_TYPE(op)->tp_name, Py_TYPE(res)->tp_name);
            Py_DECREF(res);
            return -1;
        }
        if (PyErr_WarnFormat(
                PyExc_DeprecationWarning, 1,
                "%.50s.__float__ returned non-float (type %.50s).  "
                "The ability to return an instance of a strict subclass of float "
                "is deprecated, and may be removed in a future version of Python.",
                Py_TYPE(op)->tp_name, Py_TYPE(res)->tp_name)) {
            Py_DECREF(res);
            return -1;
        }
    }
#else
    {
        if (!PyFloat_Check(res)) {
            PyErr_Format(PyExc_TypeError,
                         "%.50s.__float__ returned non-float (type %.50s)",
                         op->ob_type->tp_name, res->ob_type->tp_name);
            Py_DECREF(res);
            return -1;
        }
        if (PyErr_WarnFormat(
                PyExc_DeprecationWarning, 1,
                "%.50s.__float__ returned non-float (type %.50s).  "
                "The ability to return an instance of a strict subclass of float "
                "is deprecated, and may be removed in a future version of Python.",
                op->ob_type->tp_name, res->ob_type->tp_name)) {
            Py_DECREF(res);
            return -1;
        }
    }
#endif
    val = PyFloat_AS_DOUBLE(res);
    Py_DECREF(res);
    return val;
}

static double
cimpl_PyFloat_AsDouble_3(PyObject *op, PyObject *res)
{
    double val;
    Py_INCREF(res);
    {
#if NI_HAS_FLOAT_ASDOUBLE_INDEX
        if (!PyFloat_Check(res)) {
            PyErr_Format(PyExc_TypeError,
                         "%.50s.__float__ returned non-float (type %.50s)",
                         Py_TYPE(op)->tp_name, Py_TYPE(res)->tp_name);
            Py_DECREF(res);
            return -1;
        }
#endif
        if (PyErr_WarnFormat(
                PyExc_DeprecationWarning, 1,
                "%.50s.__float__ returned non-float (type %.50s).  "
                "The ability to return an instance of a strict subclass of float "
                "is deprecated, and may be removed in a future version of Python.",
                Py_TYPE(op)->tp_name, Py_TYPE(res)->tp_name)) {
            Py_DECREF(res);
            return -1;
        }
    }
    val = PyFloat_AS_DOUBLE(res);
    Py_DECREF(res);
    return val;
}

#if NI_HAS_FLOAT_ASDOUBLE_INDEX
static double
cimpl_PyFloat_AsDouble_4(PyObject *op, PyObject *res)
{
    double val;
    Py_INCREF(res);
    {
        if (PyErr_WarnFormat(
                PyExc_DeprecationWarning, 1,
                "%.50s.__float__ returned non-float (type %.50s).  "
                "The ability to return an instance of a strict subclass of float "
                "is deprecated, and may be removed in a future version of Python.",
                Py_TYPE(op)->tp_name, Py_TYPE(res)->tp_name)) {
            Py_DECREF(res);
            return -1;
        }
    }
    val = PyFloat_AS_DOUBLE(res);
    Py_DECREF(res);
    return val;
}
#endif

/* Objects/floatobject.c:float_abs: scalar code */
static double
cimpl_float_abs(double v_ob_fval)
{
    return fabs(v_ob_fval);
}

static int
cimpl_convert_to_double(PyObject **v, double *dbl)
{
    PyObject *obj = *v;
    if (PyLong_Check(obj)) {
        *dbl = PyLong_AsDouble(obj);
        if (*dbl == -1.0 && PyErr_Occurred()) {
            *v = NULL;
            return -1;
        }
    }
    else {
        Py_INCREF(Py_NotImplemented);
        *v = Py_NotImplemented;
        return -1;
    }
    return 0;
}

static PyObject *
cimpl_float_add_1(PyObject *w, double a)
{
    double b;
    if (PyFloat_Check(w))
        b = PyFloat_AS_DOUBLE(w);
    else if (cimpl_convert_to_double(&(w), &(b)) < 0)
        return w;
    a = a + b;
    return PyFloat_FromDouble(a);
}

/* Objects/floatobject.c:float_add: scalar code */
static double
cimpl_float_add(double a, double b)
{
    a = a + b;
    return a;
}

/* Objects/floatobject.c:float_bool: scalar code */
static int
cimpl_float_bool(double v_ob_fval)
{
    return v_ob_fval != 0.0;
}

static PyObject *
cimpl_float_div_1(PyObject *w, double a)
{
    double b;
    if (PyFloat_Check(w))
        b = PyFloat_AS_DOUBLE(w);
    else if (cimpl_convert_to_double(&(w), &(b)) < 0)
        return w;
    if (b == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError, "float division by zero");
        return NULL;
    }
    a = a / b;
    return PyFloat_FromDouble(a);
}

/* Objects/floatobject.c:float_div: scalar code */
static int
cimpl_float_div(double a, double b, double *_r)
{
    if (b == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError, "float division by zero");
        return -1;
    }
    a = a / b;
    {
        *_r = a;
        return 0;
    }
}

static PyObject *
cimpl_float_mul_1(PyObject *w, double a)
{
    double b;
    if (PyFloat_Check(w))
        b = PyFloat_AS_DOUBLE(w);
    else if (cimpl_convert_to_double(&(w), &(b)) < 0)
        return w;
    a = a * b;
    return PyFloat_FromDouble(a);
}

/* Objects/floatobject.c:float_mul: scalar code */
static double
cimpl_float_mul(double a, double b)
{
    a = a * b;
    return a;
}

/* Objects/floatobject.c:float_neg: scalar code */
static double
cimpl_float_neg(double v_ob_fval)
{
    return -v_ob_fval;
}

static PyObject *
cimpl_float_rem_1(PyObject *w, double vx)
{
    double mod;
    double wx;
    if (PyFloat_Check(w))
        wx = PyFloat_AS_DOUBLE(w);
    else if (cimpl_convert_to_double(&(w), &(wx)) < 0)
        return w;
    if (wx == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError, "float modulo");
        return NULL;
    }
    mod = fmod(vx, wx);
    if (mod) {
        if ((wx < 0) != (mod < 0)) {
            mod += wx;
        }
    }
    else {
        mod = copysign(0.0, wx);
    }
    return PyFloat_FromDouble(mod);
}

/* Objects/floatobject.c:float_rem: scalar code */
static int
cimpl_float_rem(double vx, double wx, double *_r)
{
    double mod;
    if (wx == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError, "float modulo");
        return -1;
    }
    mod = fmod(vx, wx);
    if (mod) {
        if ((wx < 0) != (mod < 0)) {
            mod += wx;
        }
    }
    else {
        mod = copysign(0.0, wx);
    }
    {
        *_r = mod;
        return 0;
    }
}

static PyObject *
cimpl_float_sub_1(PyObject *w, double a)
{
    double b;
    if (PyFloat_Check(w))
        b = PyFloat_AS_DOUBLE(w);
    else if (cimpl_convert_to_double(&(w), &(b)) < 0)
        return w;
    a = a - b;
    return PyFloat_FromDouble(a);
}

/* Objects/floatobject.c:float_sub: scalar code */
static double
cimpl_float_sub(double a, double b)
{
    a = a - b;
    return a;
}

static vinfo_t *
float_abs(PsycoObject *po, vinfo_t *);
static vinfo_t *
float_add(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
float_bool(PsycoObject *po, vinfo_t *);
static vinfo_t *
float_div(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
float_float(PsycoObject *po, vinfo_t *);
static vinfo_t *
float_hash(PsycoObject *po, vinfo_t *);
static vinfo_t *
float_mul(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
float_neg(PsycoObject *po, vinfo_t *);
static vinfo_t *
float_rem(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
float_sub(PsycoObject *po, vinfo_t *, vinfo_t *);

#if NI_HAS_FLOAT_ASDOUBLE_INDEX
DEFINEFN vinfo_t *
PsycoFloat_AsDouble(PsycoObject *po, vinfo_t *op)
{
    bool _t1;
    vinfo_t *_t10;
    vinfo_t *_t11;
    bool _t12;
    vinfo_t *_t13;
    int _t14;
    vinfo_t *_t15;
    vinfo_t *_t16;
    int _t2;
    vinfo_t *_t3;
    unaryfunc _t4;
    int _t5;
    unaryfunc _t6;
    int _t7;
    vinfo_t *_t8;
    vinfo_t *_t9;
    PyNumberMethods *nb;
    vinfo_t *res;
    PyTypeObject *tp_op;
    PyTypeObject *tp_res;
    vinfo_t *val;
    tp_op = Psyco_NeedType(po, op);
    if (tp_op == NULL) {
        return NULL;
    }
    _t2 = tp_op == (&PyFloat_Type);
    if (!_t2) {
        switch (Psyco_TypeIsSubtype(po, tp_op, &PyFloat_Type, &_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyFloat_AsDouble,
                                          CfReturnTypeDouble | CfPyErrCheck,
                                          "v", op);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = PsycoFloat_AS_DOUBLE(po, op);
        if (_t3 == NULL) {
            return NULL;
        }
        vinfo_incref(_t3);
        return _t3;
    }
    nb = tp_op->tp_as_number;
    _t5 = nb == NULL;
    if (!_t5) {
        switch (Psyco_TypeSlot(po, tp_op, &nb->nb_float, (void **)&_t4)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyFloat_AsDouble,
                                          CfReturnTypeDouble | CfPyErrCheck,
                                          "v", op);
            default:
                return NULL;
        }
        _t5 = _t4 == NULL;
    }
    if (_t5) {
        _t7 = nb != 0;
        if (_t7) {
            switch (Psyco_TypeSlot(po, tp_op, &nb->nb_index, (void **)&_t6)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(
                        po, PyFloat_AsDouble,
                        CfReturnTypeDouble | CfPyErrCheck, "v", op);
                default:
                    return NULL;
            }
            _t7 = _t6 != 0;
        }
        if (_t7) {
#if NI_HAS_PRIVATE_NUMBER_INDEX
            _t8 = psyco_generic_call(po, _PyNumber_Index,
                                     CfCommonNewRefPyObject, "v", op);
#else
            _t8 = psyco_generic_call(po, PyNumber_Index,
                                     CfCommonNewRefPyObject | CfNoBlock, "v",
                                     op);
#endif
            res = _t8;
            if (res == NULL) {
                return NULL;
            }
            _t9 = psyco_generic_call(po, cimpl_PyFloat_AsDouble_1,
                                     CfReturnTypeDouble | CfPyErrCheck, "v",
                                     res);
            vinfo_decref(res, po);
            return _t9;
        }
        PycException_SetFormat(po, PyExc_TypeError,
                               "must be real number, not %.50s",
                               tp_op->tp_name);
        return NULL;
    }
    _t10 = Psyco_META1(po, _t4, CfCommonNewRefPyObject, "v", op);
    res = _t10;
    if (res == NULL) {
        return NULL;
    }
    tp_res = Psyco_KnownType(res);
    if (tp_res == NULL) {
        _t11 = psyco_generic_call(po, cimpl_PyFloat_AsDouble_2,
                                  CfReturnTypeDouble | CfPyErrCheck, "vv", op,
                                  res);
        vinfo_decref(res, po);
        return _t11;
    }
    if (!(tp_res == (&PyFloat_Type))) {
        _t14 = tp_res == (&PyFloat_Type);
        if (!_t14) {
            switch (Psyco_TypeIsSubtype(po, tp_res, &PyFloat_Type, &_t12)) {
                case 1:
                    break;
                case 0:
                    _t13 = psyco_generic_call(
                        po, cimpl_PyFloat_AsDouble_3,
                        CfReturnTypeDouble | CfPyErrCheck, "vv", op, res);
                    vinfo_decref(res, po);
                    return _t13;
                default:
                    vinfo_decref(res, po);
                    return NULL;
            }
            _t14 = _t12 != 0;
        }
        if (!(_t14)) {
            PycException_SetFormat(
                po, PyExc_TypeError,
                "%.50s.__float__ returned non-float (type %.50s)",
                tp_op->tp_name, tp_res->tp_name);
            vinfo_decref(res, po);
            return NULL;
        }
        _t15 = psyco_generic_call(po, cimpl_PyFloat_AsDouble_4,
                                  CfReturnTypeDouble | CfPyErrCheck, "vv", op,
                                  res);
        vinfo_decref(res, po);
        return _t15;
    }
    _t16 = PsycoFloat_AS_DOUBLE(po, res);
    if (_t16 == NULL) {
        vinfo_decref(res, po);
        return NULL;
    }
    vinfo_incref(_t16);
    val = _t16;
    vinfo_decref(res, po);
    return val;
}
#else
DEFINEFN vinfo_t *
PsycoFloat_AsDouble(PsycoObject *po, vinfo_t *op)
{
    bool _t1;
    int _t10;
    vinfo_t *_t11;
    vinfo_t *_t12;
    int _t2;
    vinfo_t *_t3;
    unaryfunc _t4;
    int _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    bool _t8;
    vinfo_t *_t9;
    PyNumberMethods *nb;
    vinfo_t *res;
    PyTypeObject *tp_op;
    PyTypeObject *tp_res;
    vinfo_t *val;
    tp_op = Psyco_NeedType(po, op);
    if (tp_op == NULL) {
        return NULL;
    }
    _t2 = tp_op == (&PyFloat_Type);
    if (!_t2) {
        switch (Psyco_TypeIsSubtype(po, tp_op, &PyFloat_Type, &_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyFloat_AsDouble,
                                          CfReturnTypeDouble | CfPyErrCheck,
                                          "v", op);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = PsycoFloat_AS_DOUBLE(po, op);
        if (_t3 == NULL) {
            return NULL;
        }
        vinfo_incref(_t3);
        return _t3;
    }
    nb = tp_op->tp_as_number;
    _t5 = nb == NULL;
    if (!_t5) {
        switch (Psyco_TypeSlot(po, tp_op, &nb->nb_float, (void **)&_t4)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyFloat_AsDouble,
                                          CfReturnTypeDouble | CfPyErrCheck,
                                          "v", op);
            default:
                return NULL;
        }
        _t5 = _t4 == NULL;
    }
    if (_t5) {
        PycException_SetFormat(po, PyExc_TypeError,
                               "must be real number, not %.50s",
                               tp_op->tp_name);
        return NULL;
    }
    _t6 = Psyco_META1(po, _t4, CfCommonNewRefPyObject, "v", op);
    res = _t6;
    if (res == NULL) {
        return NULL;
    }
    tp_res = Psyco_KnownType(res);
    if (tp_res == NULL) {
        _t7 = psyco_generic_call(po, cimpl_PyFloat_AsDouble_1,
                                 CfReturnTypeDouble | CfPyErrCheck, "vv", op,
                                 res);
        vinfo_decref(res, po);
        return _t7;
    }
    if (!(tp_res == (&PyFloat_Type))) {
        _t10 = tp_res == (&PyFloat_Type);
        if (!_t10) {
            switch (Psyco_TypeIsSubtype(po, tp_res, &PyFloat_Type, &_t8)) {
                case 1:
                    break;
                case 0:
                    _t9 = psyco_generic_call(po, cimpl_PyFloat_AsDouble_2,
                                             CfReturnTypeDouble | CfPyErrCheck,
                                             "vv", op, res);
                    vinfo_decref(res, po);
                    return _t9;
                default:
                    vinfo_decref(res, po);
                    return NULL;
            }
            _t10 = _t8 != 0;
        }
        if (!(_t10)) {
            PycException_SetFormat(
                po, PyExc_TypeError,
                "%.50s.__float__ returned non-float (type %.50s)",
                tp_op->tp_name, tp_res->tp_name);
            vinfo_decref(res, po);
            return NULL;
        }
        _t11 = psyco_generic_call(po, cimpl_PyFloat_AsDouble_3,
                                  CfReturnTypeDouble | CfPyErrCheck, "vv", op,
                                  res);
        vinfo_decref(res, po);
        return _t11;
    }
    _t12 = PsycoFloat_AS_DOUBLE(po, res);
    if (_t12 == NULL) {
        vinfo_decref(res, po);
        return NULL;
    }
    vinfo_incref(_t12);
    val = _t12;
    vinfo_decref(res, po);
    return val;
}
#endif

static vinfo_t *
float_abs(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *_t1;
    vinfo_t *_t2;
    vinfo_t *_t3;
    _t1 = PsycoFloat_AS_DOUBLE(po, v);
    if (_t1 == NULL) {
        return NULL;
    }
    vinfo_incref(_t1);
    _t2 = psyco_generic_call(
        po, cimpl_float_abs,
        CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "d", _t1);
    vinfo_decref(_t1, po);
    if (_t2 == NULL) {
        return NULL;
    }
    _t3 = PsycoFloat_FROM_DOUBLE(_t2);
    return _t3;
}

static vinfo_t *
float_add(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    bool _t1;
    vinfo_t *_t10;
    vinfo_t *_t11;
    vinfo_t *_t12;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    bool _t5;
    vinfo_t *_t6;
    int _t7;
    vinfo_t *_t8;
    vinfo_t *_t9;
    vinfo_t *a;
    vinfo_t *b;
    vinfo_t *convert_to_double_obj;
    vinfo_t *convert_to_double_obj_2;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t2 = tp_v == (&PyFloat_Type);
    if (!_t2) {
        switch (Psyco_TypeIsSubtype(po, tp_v, &PyFloat_Type, &_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, PyFloat_Type.tp_as_number->nb_add,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vv",
                    v, w);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = PsycoFloat_AS_DOUBLE(po, v);
        if (_t3 == NULL) {
            return NULL;
        }
        vinfo_incref(_t3);
        a = _t3;
    }
    else {
        convert_to_double_obj = v;
        if (((tp_v->tp_flags & (1UL << 24))) != 0) {
            _t4 = PsycoLong_AsDouble(po, v);
            a = _t4;
            if (a == NULL) {
                return NULL;
            }
        }
        else {
            return psyco_vi_NotImplemented();
        }
    }
    _t7 = tp_w == (&PyFloat_Type);
    if (!_t7) {
        switch (Psyco_TypeIsSubtype(po, tp_w, &PyFloat_Type, &_t5)) {
            case 1:
                break;
            case 0:
                _t6 = psyco_generic_call(
                    po, cimpl_float_add_1,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vd",
                    w, a);
                vinfo_decref(a, po);
                return _t6;
            default:
                vinfo_decref(a, po);
                return NULL;
        }
        _t7 = _t5 != 0;
    }
    if (_t7) {
        _t8 = PsycoFloat_AS_DOUBLE(po, w);
        if (_t8 == NULL) {
            vinfo_decref(a, po);
            return NULL;
        }
        vinfo_incref(_t8);
        b = _t8;
    }
    else {
        convert_to_double_obj_2 = w;
        if (((tp_w->tp_flags & (1UL << 24))) != 0) {
            _t9 = PsycoLong_AsDouble(po, w);
            b = _t9;
            if (b == NULL) {
                vinfo_decref(a, po);
                return NULL;
            }
        }
        else {
            vinfo_decref(a, po);
            return psyco_vi_NotImplemented();
        }
    }
    _t10 = psyco_generic_call(
        po, cimpl_float_add,
        CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "dd", a, b);
    if (_t10 == NULL) {
        vinfo_decref(a, po);
        vinfo_decref(b, po);
        return NULL;
    }
    _t11 = PsycoFloat_FROM_DOUBLE(_t10);
    _t12 = _t11;
    vinfo_decref(a, po);
    vinfo_decref(b, po);
    return _t12;
}

static vinfo_t *
float_bool(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *_t1;
    vinfo_t *_t2;
    _t1 = PsycoFloat_AS_DOUBLE(po, v);
    if (_t1 == NULL) {
        return NULL;
    }
    vinfo_incref(_t1);
    _t2 = psyco_generic_call(po, cimpl_float_bool,
                             CfPure | CfReturnTypeInt | CfNoPython | CfNoBlock,
                             "d", _t1);
    vinfo_decref(_t1, po);
    if (_t2 == NULL) {
        return NULL;
    }
    return _t2;
}

static vinfo_t *
float_div(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    bool _t1;
    vinfo_array_t *_t10;
    vinfo_t *_t11;
    vinfo_t *_t12;
    vinfo_t *_t13;
    vinfo_t *_t14;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    bool _t5;
    vinfo_t *_t6;
    int _t7;
    vinfo_t *_t8;
    vinfo_t *_t9;
    vinfo_t *a;
    vinfo_t *b;
    vinfo_t *convert_to_double_obj;
    vinfo_t *convert_to_double_obj_2;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t2 = tp_v == (&PyFloat_Type);
    if (!_t2) {
        switch (Psyco_TypeIsSubtype(po, tp_v, &PyFloat_Type, &_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, PyFloat_Type.tp_as_number->nb_true_divide,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vv",
                    v, w);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = PsycoFloat_AS_DOUBLE(po, v);
        if (_t3 == NULL) {
            return NULL;
        }
        vinfo_incref(_t3);
        a = _t3;
    }
    else {
        convert_to_double_obj = v;
        if (((tp_v->tp_flags & (1UL << 24))) != 0) {
            _t4 = PsycoLong_AsDouble(po, v);
            a = _t4;
            if (a == NULL) {
                return NULL;
            }
        }
        else {
            return psyco_vi_NotImplemented();
        }
    }
    _t7 = tp_w == (&PyFloat_Type);
    if (!_t7) {
        switch (Psyco_TypeIsSubtype(po, tp_w, &PyFloat_Type, &_t5)) {
            case 1:
                break;
            case 0:
                _t6 = psyco_generic_call(
                    po, cimpl_float_div_1,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vd",
                    w, a);
                vinfo_decref(a, po);
                return _t6;
            default:
                vinfo_decref(a, po);
                return NULL;
        }
        _t7 = _t5 != 0;
    }
    if (_t7) {
        _t8 = PsycoFloat_AS_DOUBLE(po, w);
        if (_t8 == NULL) {
            vinfo_decref(a, po);
            return NULL;
        }
        vinfo_incref(_t8);
        b = _t8;
    }
    else {
        convert_to_double_obj_2 = w;
        if (((tp_w->tp_flags & (1UL << 24))) != 0) {
            _t9 = PsycoLong_AsDouble(po, w);
            b = _t9;
            if (b == NULL) {
                vinfo_decref(a, po);
                return NULL;
            }
        }
        else {
            vinfo_decref(a, po);
            return psyco_vi_NotImplemented();
        }
    }
    _t10 = array_new(1);
    _t11 = psyco_generic_call(po, cimpl_float_div,
                              CfCommonIntZeroOk | CfNoPython | CfNoBlock,
                              "dda", a, b, _t10);
    if (_t11 == NULL) {
        array_release(_t10);
        vinfo_decref(a, po);
        vinfo_decref(b, po);
        return NULL;
    }
    _t12 = _t10->items[0];
    array_release(_t10);
    _t13 = PsycoFloat_FROM_DOUBLE(_t12);
    _t14 = _t13;
    vinfo_decref(a, po);
    vinfo_decref(b, po);
    return _t14;
}

static vinfo_t *
float_float(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *_t1;
    vinfo_t *_t2;
    PyTypeObject *tp_v;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    if (tp_v == (&PyFloat_Type)) {
        vinfo_incref(v);
    }
    else {
        _t1 = PsycoFloat_AS_DOUBLE(po, v);
        if (_t1 == NULL) {
            return NULL;
        }
        vinfo_incref(_t1);
        _t2 = PsycoFloat_FROM_DOUBLE(_t1);
        v = _t2;
    }
    return v;
}

static vinfo_t *
float_hash(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *_t1;
    vinfo_t *_t2;
    _t1 = PsycoFloat_AS_DOUBLE(po, v);
    if (_t1 == NULL) {
        return NULL;
    }
    vinfo_incref(_t1);
#if NI_HAS_HASH_DOUBLE_OBJECT
    _t2 = psyco_generic_call(po, _Py_HashDouble,
                             CfReturnTypeNative | CfPure | CfNoBlock, "vd", v,
                             _t1);
#else
    _t2 = psyco_generic_call(
        po, _Py_HashDouble, CfReturnTypeNative | CfPure | CfNoBlock, "d", _t1);
#endif
    vinfo_decref(_t1, po);
    if (_t2 == NULL) {
        return NULL;
    }
    return _t2;
}

static vinfo_t *
float_mul(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    bool _t1;
    vinfo_t *_t10;
    vinfo_t *_t11;
    vinfo_t *_t12;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    bool _t5;
    vinfo_t *_t6;
    int _t7;
    vinfo_t *_t8;
    vinfo_t *_t9;
    vinfo_t *a;
    vinfo_t *b;
    vinfo_t *convert_to_double_obj;
    vinfo_t *convert_to_double_obj_2;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t2 = tp_v == (&PyFloat_Type);
    if (!_t2) {
        switch (Psyco_TypeIsSubtype(po, tp_v, &PyFloat_Type, &_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, PyFloat_Type.tp_as_number->nb_multiply,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vv",
                    v, w);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = PsycoFloat_AS_DOUBLE(po, v);
        if (_t3 == NULL) {
            return NULL;
        }
        vinfo_incref(_t3);
        a = _t3;
    }
    else {
        convert_to_double_obj = v;
        if (((tp_v->tp_flags & (1UL << 24))) != 0) {
            _t4 = PsycoLong_AsDouble(po, v);
            a = _t4;
            if (a == NULL) {
                return NULL;
            }
        }
        else {
            return psyco_vi_NotImplemented();
        }
    }
    _t7 = tp_w == (&PyFloat_Type);
    if (!_t7) {
        switch (Psyco_TypeIsSubtype(po, tp_w, &PyFloat_Type, &_t5)) {
            case 1:
                break;
            case 0:
                _t6 = psyco_generic_call(
                    po, cimpl_float_mul_1,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vd",
                    w, a);
                vinfo_decref(a, po);
                return _t6;
            default:
                vinfo_decref(a, po);
                return NULL;
        }
        _t7 = _t5 != 0;
    }
    if (_t7) {
        _t8 = PsycoFloat_AS_DOUBLE(po, w);
        if (_t8 == NULL) {
            vinfo_decref(a, po);
            return NULL;
        }
        vinfo_incref(_t8);
        b = _t8;
    }
    else {
        convert_to_double_obj_2 = w;
        if (((tp_w->tp_flags & (1UL << 24))) != 0) {
            _t9 = PsycoLong_AsDouble(po, w);
            b = _t9;
            if (b == NULL) {
                vinfo_decref(a, po);
                return NULL;
            }
        }
        else {
            vinfo_decref(a, po);
            return psyco_vi_NotImplemented();
        }
    }
    _t10 = psyco_generic_call(
        po, cimpl_float_mul,
        CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "dd", a, b);
    if (_t10 == NULL) {
        vinfo_decref(a, po);
        vinfo_decref(b, po);
        return NULL;
    }
    _t11 = PsycoFloat_FROM_DOUBLE(_t10);
    _t12 = _t11;
    vinfo_decref(a, po);
    vinfo_decref(b, po);
    return _t12;
}

static vinfo_t *
float_neg(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *_t1;
    vinfo_t *_t2;
    vinfo_t *_t3;
    _t1 = PsycoFloat_AS_DOUBLE(po, v);
    if (_t1 == NULL) {
        return NULL;
    }
    vinfo_incref(_t1);
    _t2 = psyco_generic_call(
        po, cimpl_float_neg,
        CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "d", _t1);
    vinfo_decref(_t1, po);
    if (_t2 == NULL) {
        return NULL;
    }
    _t3 = PsycoFloat_FROM_DOUBLE(_t2);
    return _t3;
}

static vinfo_t *
float_rem(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    bool _t1;
    vinfo_array_t *_t10;
    vinfo_t *_t11;
    vinfo_t *_t12;
    vinfo_t *_t13;
    vinfo_t *_t14;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    bool _t5;
    vinfo_t *_t6;
    int _t7;
    vinfo_t *_t8;
    vinfo_t *_t9;
    vinfo_t *convert_to_double_obj;
    vinfo_t *convert_to_double_obj_2;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    vinfo_t *vx;
    vinfo_t *wx;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t2 = tp_v == (&PyFloat_Type);
    if (!_t2) {
        switch (Psyco_TypeIsSubtype(po, tp_v, &PyFloat_Type, &_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, PyFloat_Type.tp_as_number->nb_remainder,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vv",
                    v, w);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = PsycoFloat_AS_DOUBLE(po, v);
        if (_t3 == NULL) {
            return NULL;
        }
        vinfo_incref(_t3);
        vx = _t3;
    }
    else {
        convert_to_double_obj = v;
        if (((tp_v->tp_flags & (1UL << 24))) != 0) {
            _t4 = PsycoLong_AsDouble(po, v);
            vx = _t4;
            if (vx == NULL) {
                return NULL;
            }
        }
        else {
            return psyco_vi_NotImplemented();
        }
    }
    _t7 = tp_w == (&PyFloat_Type);
    if (!_t7) {
        switch (Psyco_TypeIsSubtype(po, tp_w, &PyFloat_Type, &_t5)) {
            case 1:
                break;
            case 0:
                _t6 = psyco_generic_call(
                    po, cimpl_float_rem_1,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vd",
                    w, vx);
                vinfo_decref(vx, po);
                return _t6;
            default:
                vinfo_decref(vx, po);
                return NULL;
        }
        _t7 = _t5 != 0;
    }
    if (_t7) {
        _t8 = PsycoFloat_AS_DOUBLE(po, w);
        if (_t8 == NULL) {
            vinfo_decref(vx, po);
            return NULL;
        }
        vinfo_incref(_t8);
        wx = _t8;
    }
    else {
        convert_to_double_obj_2 = w;
        if (((tp_w->tp_flags & (1UL << 24))) != 0) {
            _t9 = PsycoLong_AsDouble(po, w);
            wx = _t9;
            if (wx == NULL) {
                vinfo_decref(vx, po);
                return NULL;
            }
        }
        else {
            vinfo_decref(vx, po);
            return psyco_vi_NotImplemented();
        }
    }
    _t10 = array_new(1);
    _t11 = psyco_generic_call(po, cimpl_float_rem,
                              CfCommonIntZeroOk | CfNoPython | CfNoBlock,
                              "dda", vx, wx, _t10);
    if (_t11 == NULL) {
        array_release(_t10);
        vinfo_decref(vx, po);
        vinfo_decref(wx, po);
        return NULL;
    }
    _t12 = _t10->items[0];
    array_release(_t10);
    _t13 = PsycoFloat_FROM_DOUBLE(_t12);
    _t14 = _t13;
    vinfo_decref(vx, po);
    vinfo_decref(wx, po);
    return _t14;
}

static vinfo_t *
float_sub(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    bool _t1;
    vinfo_t *_t10;
    vinfo_t *_t11;
    vinfo_t *_t12;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    bool _t5;
    vinfo_t *_t6;
    int _t7;
    vinfo_t *_t8;
    vinfo_t *_t9;
    vinfo_t *a;
    vinfo_t *b;
    vinfo_t *convert_to_double_obj;
    vinfo_t *convert_to_double_obj_2;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t2 = tp_v == (&PyFloat_Type);
    if (!_t2) {
        switch (Psyco_TypeIsSubtype(po, tp_v, &PyFloat_Type, &_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, PyFloat_Type.tp_as_number->nb_subtract,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vv",
                    v, w);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = PsycoFloat_AS_DOUBLE(po, v);
        if (_t3 == NULL) {
            return NULL;
        }
        vinfo_incref(_t3);
        a = _t3;
    }
    else {
        convert_to_double_obj = v;
        if (((tp_v->tp_flags & (1UL << 24))) != 0) {
            _t4 = PsycoLong_AsDouble(po, v);
            a = _t4;
            if (a == NULL) {
                return NULL;
            }
        }
        else {
            return psyco_vi_NotImplemented();
        }
    }
    _t7 = tp_w == (&PyFloat_Type);
    if (!_t7) {
        switch (Psyco_TypeIsSubtype(po, tp_w, &PyFloat_Type, &_t5)) {
            case 1:
                break;
            case 0:
                _t6 = psyco_generic_call(
                    po, cimpl_float_sub_1,
                    CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vd",
                    w, a);
                vinfo_decref(a, po);
                return _t6;
            default:
                vinfo_decref(a, po);
                return NULL;
        }
        _t7 = _t5 != 0;
    }
    if (_t7) {
        _t8 = PsycoFloat_AS_DOUBLE(po, w);
        if (_t8 == NULL) {
            vinfo_decref(a, po);
            return NULL;
        }
        vinfo_incref(_t8);
        b = _t8;
    }
    else {
        convert_to_double_obj_2 = w;
        if (((tp_w->tp_flags & (1UL << 24))) != 0) {
            _t9 = PsycoLong_AsDouble(po, w);
            b = _t9;
            if (b == NULL) {
                vinfo_decref(a, po);
                return NULL;
            }
        }
        else {
            vinfo_decref(a, po);
            return psyco_vi_NotImplemented();
        }
    }
    _t10 = psyco_generic_call(
        po, cimpl_float_sub,
        CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "dd", a, b);
    if (_t10 == NULL) {
        vinfo_decref(a, po);
        vinfo_decref(b, po);
        return NULL;
    }
    _t11 = PsycoFloat_FROM_DOUBLE(_t10);
    _t12 = _t11;
    vinfo_decref(a, po);
    vinfo_decref(b, po);
    return _t12;
}

static void
gen_floatobject_init(void)
{
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_absolute, float_abs);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_add, float_add);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_bool, float_bool);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_true_divide, float_div);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_positive, float_float);
    Psyco_DefineMeta(PyFloat_Type.tp_hash, float_hash);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_multiply, float_mul);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_negative, float_neg);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_remainder, float_rem);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_subtract, float_sub);
}
#pragma GCC diagnostic pop

INITIALIZATIONFN
void psy_floatobject_init(void) {
  gen_floatobject_init();
  Psyco_DefineMeta(PyFloat_Type.tp_richcompare, pfloat_richcompare);

  INIT_SVIRTUAL(psyco_computed_float, compute_float, direct_compute_float, 0, 0,
                0);
}

#else  /* !HAVE_FP_FN_CALLS */
INITIALIZATIONFN
void psy_floatobject_init(void) {}
#endif /* !HAVE_FP_FN_CALLS */
