/*
 * Virtual floats. Every arithmetic result is a new virtual float whose
 * [iFLOAT_OB_FVAL] holds the double, so 'a + b * c' is never boxed unless it
 * escapes. + - * / are done inline in XMM registers
 * (x64/iencoding.c:psyco_float_op); the rest are C helpers called with
 * CfPure, which fold at compile time when the operands are constants.
 *
 * + - * / % unary + - abs and bool follow floatobject.c (below), their
 * double code copied into helpers. The representation (compute functions),
 * comparisons and the conversions pbltinmodule.c uses are LeJIT's own.
 * The double is one word holding its bits, at run time (a general or XMM
 * register, or a stack slot) and at compile time (source_known_t.value).
 */
#include "pfloatobject.h"
#include "pboolobject.h"
#include "plongobject.h"

#if HAVE_FP_FN_CALLS

#define CF_NONPURE_FP_HELPER \
    (CfPure | CfNoReturnValue | CfReturnTypeInt | CfPyErrIfNonNull)
#ifdef WANT_SIGFPE_HANDLER
#define CF_PURE_FP_HELPER \
    (CfPure | CfNoReturnValue | CfReturnTypeInt | CfPyErrIfNonNull)
#else
#define CF_PURE_FP_HELPER (CfPure | CfNoReturnValue)
#endif

static int
cimpl_fp_eq_fp(double a, double b)
{
    return a == b;
}
static int
cimpl_fp_ne_fp(double a, double b)
{
    return a != b;
}
static int
cimpl_fp_le_fp(double a, double b)
{
    return a <= b;
}
static int
cimpl_fp_lt_fp(double a, double b)
{
    return a < b;
}

DEFINEFN double
cimpl_fp_from_long(long value)
{
    return (double)value;
}

static int
cimpl_fp_fits_long(double whole)
{
    return LONG_MIN < whole && whole < LONG_MAX; /* false for NaN */
}

static long
cimpl_fp_to_long(double whole)
{
    return (long)whole;
}

/* float___trunc___impl */
static double
cimpl_fp_trunc(double x)
{
    double wholepart;
    (void)modf(x, &wholepart);
    return wholepart;
}

/* float___round___impl without ndigits: round half to even */
static double
cimpl_fp_round_even(double x)
{
    double rounded = round(x);
    if (fabs(x - rounded) == 0.5)
        rounded = 2.0 * round(x / 2.0);
    return rounded;
}

DEFINEFN
vinfo_t *
PsycoFloat_Trunc(PsycoObject *po, vinfo_t *vd)
{
    return psyco_generic_call(po, cimpl_fp_trunc, CfPure | CfReturnTypeDouble,
                              "d", vd);
}

DEFINEFN
vinfo_t *
PsycoFloat_RoundEven(PsycoObject *po, vinfo_t *vd)
{
    return psyco_generic_call(po, cimpl_fp_round_even,
                              CfPure | CfReturnTypeDouble, "d", vd);
}

DEFINEFN
int
PsycoFloat_WholeToLong(PsycoObject *po, vinfo_t *vwhole, vinfo_t **result)
{
    vinfo_t *x;
    int r;
    vinfo_t *fits = psyco_generic_call(po, cimpl_fp_fits_long,
                                       CfPure | CfReturnTypeInt, "d", vwhole);
    if (fits == NULL)
        return -1;
    r = runtime_NON_NULL_t(po, fits); /* consumes fits */
    if (r != true)
        return r == false ? 0 : -1;
    x = psyco_generic_call(po, cimpl_fp_to_long, CfPure | CfReturnTypeLong,
                           "d", vwhole);
    if (x == NULL)
        return -1;
    *result = PsycoLong_FROM_WORD(x);
    return 1;
}

static bool
compute_float(PsycoObject *po, vinfo_t *floatobj)
{
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
static PyObject *
direct_compute_float(vinfo_t *floatobj, char *data)
{
    double value;
    long bits =
        direct_read_vinfo(vinfo_getitem(floatobj, iFLOAT_OB_FVAL), data);
    if (PyErr_Occurred())
        return NULL;
    memcpy(&value, &bits, sizeof(value));
    return PyFloat_FromDouble(value);
}

DEFINEVAR source_virtual_t psyco_computed_float;

/***************************************************************/
/*** float objects meta-implementation                       ***/

static vinfo_t *
pfloat_richcompare(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op)
{
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
#define SWAP_A_B \
    r = a;       \
    a = b;       \
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

/* + - * / % unary + - abs bool hash, as in Objects/floatobject.c */

/* CPython's code, copied for the metas to call at run time */

/* the end of PyFloat_AsDouble: res, what nb_float gave for op, must be a
   float (a subclass with a warning) */
static double
cimpl_float_asdouble_result(PyObject *op, PyObject *res)
{
    double val;
    if (!PyFloat_CheckExact(res)) {
#ifdef LE_MSG_FLOAT_RETURNED_NON_FLOAT_QUALIFIED
        if (!PyFloat_Check(res)) {
            PyErr_Format(PyExc_TypeError,
                         LE_MSG_FLOAT_RETURNED_NON_FLOAT_QUALIFIED,
                         Py_TYPE(op), Py_TYPE(res));
            return -1;
        }
        if (PyErr_WarnFormat(PyExc_DeprecationWarning, 1,
                             LE_MSG_FLOAT_RETURNED_FLOAT_SUBCLASS_QUALIFIED,
                             Py_TYPE(op), Py_TYPE(res))) {
            return -1;
        }
#else
        if (!PyFloat_Check(res)) {
            PyErr_Format(PyExc_TypeError, LE_MSG_FLOAT_RETURNED_NON_FLOAT,
                         Py_TYPE(op)->tp_name, Py_TYPE(res)->tp_name);
            return -1;
        }
        if (PyErr_WarnFormat(PyExc_DeprecationWarning, 1,
                             LE_MSG_FLOAT_RETURNED_FLOAT_SUBCLASS,
                             Py_TYPE(op)->tp_name, Py_TYPE(res)->tp_name)) {
            return -1;
        }
#endif
    }
    val = PyFloat_AS_DOUBLE(res);
    return val;
}

static int
convert_to_double(PyObject **v, double *dbl)
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

#define CONVERT_TO_DOUBLE(obj, dbl)                 \
    if (PyFloat_Check(obj))                         \
        dbl = PyFloat_AS_DOUBLE(obj);               \
    else if (convert_to_double(&(obj), &(dbl)) < 0) \
        return obj;

/* the binary operations from CONVERT_TO_DOUBLE(w, b) on, a being v's value
 */
static PyObject *
cimpl_float_add_w(PyObject *w, double a)
{
    double b;
    CONVERT_TO_DOUBLE(w, b);
    a = a + b;
    return PyFloat_FromDouble(a);
}

static PyObject *
cimpl_float_sub_w(PyObject *w, double a)
{
    double b;
    CONVERT_TO_DOUBLE(w, b);
    a = a - b;
    return PyFloat_FromDouble(a);
}

static PyObject *
cimpl_float_mul_w(PyObject *w, double a)
{
    double b;
    CONVERT_TO_DOUBLE(w, b);
    a = a * b;
    return PyFloat_FromDouble(a);
}

static PyObject *
cimpl_float_div_w(PyObject *w, double a)
{
    double b;
    CONVERT_TO_DOUBLE(w, b);
    if (b == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError,
                        LE_MSG_FLOAT_DIVISION_BY_ZERO);
        return NULL;
    }
    a = a / b;
    return PyFloat_FromDouble(a);
}

static PyObject *
cimpl_float_rem_w(PyObject *w, double vx)
{
    double mod;
    double wx;
    CONVERT_TO_DOUBLE(w, wx);
    if (wx == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError, LE_MSG_FLOAT_MODULO_BY_ZERO);
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

/* their double code, and the unary operations' */
static double
cimpl_float_add(double a, double b)
{
    return a + b;
}

static double
cimpl_float_sub(double a, double b)
{
    return a - b;
}

static double
cimpl_float_mul(double a, double b)
{
    return a * b;
}

static int
cimpl_float_div(double a, double b, double *result)
{
    if (b == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError,
                        LE_MSG_FLOAT_DIVISION_BY_ZERO);
        return -1;
    }
    *result = a / b;
    return 0;
}

static int
cimpl_float_rem(double vx, double wx, double *result)
{
    double mod;
    if (wx == 0.0) {
        PyErr_SetString(PyExc_ZeroDivisionError, LE_MSG_FLOAT_MODULO_BY_ZERO);
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
    *result = mod;
    return 0;
}

#define DOUBLE_IS_ODD_INTEGER(x) (fmod(fabs(x), 2.0) == 1.0)

/* float_pow on two doubles where its result is a float: 1 with it in
   *result, or 0 for an error or a complex result, which float_pow itself
   then gives */
static int
cimpl_float_pow(double iv, double iw, double *result)
{
    double ix;
    bool negate_result = false;
    if (iw == 0) {
        *result = 1.0;
        return 1;
    }
    if (isnan(iv)) {
        *result = iv;
        return 1;
    }
    if (isnan(iw)) {
        *result = iv == 1.0 ? 1.0 : iw;
        return 1;
    }
    if (isinf(iw)) {
        iv = fabs(iv);
        if (iv == 1.0)
            *result = 1.0;
        else if ((iw > 0.0) == (iv > 1.0))
            *result = fabs(iw);
        else
            *result = 0.0;
        return 1;
    }
    if (isinf(iv)) {
        bool iw_is_odd = DOUBLE_IS_ODD_INTEGER(iw);
        if (iw > 0.0)
            *result = iw_is_odd ? iv : fabs(iv);
        else
            *result = iw_is_odd ? copysign(0.0, iv) : 0.0;
        return 1;
    }
    if (iv == 0.0) {
        if (iw < 0.0)
            return 0;
        *result = DOUBLE_IS_ODD_INTEGER(iw) ? iv : 0.0;
        return 1;
    }
    if (iv < 0.0) {
        if (iw != floor(iw))
            return 0;
        iv = -iv;
        negate_result = DOUBLE_IS_ODD_INTEGER(iw);
    }
    if (iv == 1.0) {
        *result = negate_result ? -1.0 : 1.0;
        return 1;
    }
    errno = 0;
    ix = pow(iv, iw);
    if (errno == 0) {
        if (ix == HUGE_VAL || ix == -HUGE_VAL)
            return 0;
    }
    else if (errno != ERANGE || ix != 0.0)
        return 0;
    *result = negate_result ? -ix : ix;
    return 1;
}

static double
cimpl_float_abs(double v)
{
    return fabs(v);
}

static double
cimpl_float_neg(double v)
{
    return -v;
}

static int
cimpl_float_bool(double v)
{
    return v != 0.0;
}

DEFINEFN vinfo_t *
PsycoFloat_AsDouble(PsycoObject *po, vinfo_t *op)
{
    PyTypeObject *tp_op, *tp_res;
    PyNumberMethods *nb;
    unaryfunc nb_float = NULL;
    vinfo_t *res, *val;
    bool is_float;

    tp_op = Psyco_NeedType(po, op);
    if (tp_op == NULL)
        return NULL;
    is_float = tp_op == &PyFloat_Type;
    if (!is_float) {
        switch (Psyco_TypeIsSubtype(po, tp_op, &PyFloat_Type, &is_float)) {
            case 1:
                break;
            case 0:
                goto at_run_time;
            default:
                return NULL;
        }
    }
    if (is_float) {
        val = PsycoFloat_AS_DOUBLE(po, op);
        if (val == NULL)
            return NULL;
        vinfo_incref(val);
        return val;
    }
    nb = tp_op->tp_as_number;
    if (nb != NULL) {
        switch (Psyco_TypeSlot(po, tp_op, &nb->nb_float, (void **)&nb_float)) {
            case 1:
                break;
            case 0:
                goto at_run_time;
            default:
                return NULL;
        }
    }
    if (nb_float == NULL) {
#if LE_HAS_FLOAT_ASDOUBLE_INDEX
        unaryfunc nb_index = NULL;
        if (nb != NULL) {
            switch (
                Psyco_TypeSlot(po, tp_op, &nb->nb_index, (void **)&nb_index)) {
                case 1:
                    break;
                case 0:
                    goto at_run_time;
                default:
                    return NULL;
            }
        }
        if (nb_index != NULL) {
#if LE_HAS_PRIVATE_NUMBER_INDEX
            res = psyco_generic_call(po, _PyNumber_Index,
                                     CfCommonNewRefPyObject, "v", op);
#else
            res = psyco_generic_call(po, PyNumber_Index,
                                     CfCommonNewRefPyObject | CfNoBlock, "v",
                                     op);
#endif
            if (res == NULL)
                return NULL;
            val = psyco_generic_call(po, PyLong_AsDouble,
                                     CfReturnTypeDouble | CfPyErrCheck, "v",
                                     res);
            vinfo_decref(res, po);
            return val;
        }
#endif
        PycException_SetTypeFormat(po, PyExc_TypeError,
                                   LE_MSG_MUST_BE_REAL_NUMBER, NULL, tp_op,
                                   NULL);
        return NULL;
    }
    res = Psyco_META1(po, nb_float, CfCommonNewRefPyObject, "v", op);
    if (res == NULL)
        return NULL;
    tp_res = Psyco_KnownType(res);
    if (tp_res == &PyFloat_Type) {
        val = PsycoFloat_AS_DOUBLE(po, res);
        if (val != NULL)
            vinfo_incref(val);
        vinfo_decref(res, po);
        return val;
    }
    /* a message naming the types by their qualified names, which can change
       after compiling, is made at run time, by cimpl_float_asdouble_result */
#ifndef LE_MSG_FLOAT_RETURNED_NON_FLOAT_QUALIFIED
    if (tp_res != NULL) {
        switch (Psyco_TypeIsSubtype(po, tp_res, &PyFloat_Type, &is_float)) {
            case 1:
                if (!is_float) {
                    PycException_SetTypeFormat(po, PyExc_TypeError,
                                               LE_MSG_FLOAT_RETURNED_NON_FLOAT,
                                               NULL, tp_op, tp_res);
                    vinfo_decref(res, po);
                    return NULL;
                }
                break;
            case 0:
                break;
            default:
                vinfo_decref(res, po);
                return NULL;
        }
    }
#endif
    val = psyco_generic_call(po, cimpl_float_asdouble_result,
                             CfReturnTypeDouble | CfPyErrCheck, "vv", op, res);
    vinfo_decref(res, po);
    return val;

at_run_time:
    return psyco_generic_call(po, PyFloat_AsDouble,
                              CfReturnTypeDouble | CfPyErrCheck, "v", op);
}

/* CONVERT_TO_DOUBLE(obj, dbl) at compile time: 1 with *dbl a new reference
   to obj's value, 0 where only the run time can tell obj's kind, 2 for
   NotImplemented, -1 on error */
static int
pconvert_to_double(PsycoObject *po, vinfo_t *obj, PyTypeObject *tp,
                   vinfo_t **dbl)
{
    bool is_float = tp == &PyFloat_Type;
    if (!is_float) {
        switch (Psyco_TypeIsSubtype(po, tp, &PyFloat_Type, &is_float)) {
            case 1:
                break;
            case 0:
                return 0;
            default:
                return -1;
        }
    }
    if (is_float) {
        *dbl = PsycoFloat_AS_DOUBLE(po, obj);
        if (*dbl == NULL)
            return -1;
        vinfo_incref(*dbl);
        return 1;
    }
    if ((tp->tp_flags & Py_TPFLAGS_LONG_SUBCLASS) != 0) {
        *dbl = PsycoLong_AsDouble(po, obj);
        return *dbl == NULL ? -1 : 1;
    }
    return 2;
}

/* a binary operation: 'slot' is the whole of it, 'rest' its run-time rest
   (cimpl_float_add_w...) and 'scalar' its double code, which fails only
   if 'can_fail'; 'op' (FLOAT_OP_ADD...) is done inline, or is -1 */
static vinfo_t *
float_binop(PsycoObject *po, vinfo_t *v, vinfo_t *w, binaryfunc slot,
            void *rest, void *scalar, bool can_fail, int op)
{
    PyTypeObject *tp_v, *tp_w;
    vinfo_t *a, *b, *x;

    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL)
        return NULL;
    switch (pconvert_to_double(po, v, tp_v, &a)) {
        case 1:
            break;
        case 0:
            return psyco_generic_call(
                po, slot, CfCommonCheckNotImplemented | CfNoPython | CfNoBlock,
                "vv", v, w);
        case 2:
            return psyco_vi_NotImplemented();
        default:
            return NULL;
    }
    switch (pconvert_to_double(po, w, tp_w, &b)) {
        case 1:
            break;
        case 0:
            x = psyco_generic_call(
                po, rest, CfCommonCheckNotImplemented | CfNoPython | CfNoBlock,
                "vd", w, a);
            vinfo_decref(a, po);
            return x;
        case 2:
            vinfo_decref(a, po);
            return psyco_vi_NotImplemented();
        default:
            vinfo_decref(a, po);
            return NULL;
    }
    if (op >= 0 && !(is_compiletime(a->source) && is_compiletime(b->source)) &&
        !(op == FLOAT_OP_DIV && is_compiletime(b->source) &&
          CompileTime_Get(b->source)->value << 1 == 0)) {
        /* a run-time divisor of zero raises, in the helper */
        if (op == FLOAT_OP_DIV && !is_compiletime(b->source) &&
            runtime_condition_f(po, psyco_float_is_zero(po, b)))
            goto call;
        x = psyco_float_op(po, op, a, b);
    }
    else if (!can_fail) {
        x = psyco_generic_call(
            po, scalar, CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock,
            "dd", a, b);
    }
    else {
        vinfo_array_t *result;
    call:
        result = array_new(1);
        x = NULL;
        if (psyco_generic_call(po, scalar,
                               CfCommonIntZeroOk | CfNoPython | CfNoBlock,
                               "dda", a, b, result) != NULL)
            x = result->items[0];
        array_release(result);
    }
    vinfo_decref(a, po);
    vinfo_decref(b, po);
    if (x == NULL)
        return NULL;
    return PsycoFloat_FROM_DOUBLE(x);
}

static vinfo_t *
float_add(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    return float_binop(po, v, w, PyFloat_Type.tp_as_number->nb_add,
                       cimpl_float_add_w, cimpl_float_add, false,
                       FLOAT_OP_ADD);
}

static vinfo_t *
float_sub(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    return float_binop(po, v, w, PyFloat_Type.tp_as_number->nb_subtract,
                       cimpl_float_sub_w, cimpl_float_sub, false,
                       FLOAT_OP_SUB);
}

static vinfo_t *
float_mul(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    return float_binop(po, v, w, PyFloat_Type.tp_as_number->nb_multiply,
                       cimpl_float_mul_w, cimpl_float_mul, false,
                       FLOAT_OP_MUL);
}

static vinfo_t *
float_div(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    return float_binop(po, v, w, PyFloat_Type.tp_as_number->nb_true_divide,
                       cimpl_float_div_w, cimpl_float_div, true, FLOAT_OP_DIV);
}

static vinfo_t *
float_rem(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    return float_binop(po, v, w, PyFloat_Type.tp_as_number->nb_remainder,
                       cimpl_float_rem_w, cimpl_float_rem, true, -1);
}

static vinfo_t *
float_pow(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t *z)
{
    ternaryfunc slot = PyFloat_Type.tp_as_number->nb_power;
    PyTypeObject *tp_v, *tp_w;
    vinfo_t *a, *b, *x;
    vinfo_array_t *result;
    int ok;

    if (!psyco_knowntobe(z, (long)Py_None))
        goto in_c;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL)
        return NULL;
    switch (pconvert_to_double(po, v, tp_v, &a)) {
        case 1:
            break;
        case 0:
            goto in_c;
        case 2:
            return psyco_vi_NotImplemented();
        default:
            return NULL;
    }
    switch (pconvert_to_double(po, w, tp_w, &b)) {
        case 1:
            break;
        case 0:
            vinfo_decref(a, po);
            goto in_c;
        case 2:
            vinfo_decref(a, po);
            return psyco_vi_NotImplemented();
        default:
            vinfo_decref(a, po);
            return NULL;
    }
    result = array_new(1);
    x = psyco_generic_call(po, cimpl_float_pow,
                           CfReturnTypeInt | CfNoPython | CfNoBlock, "dda", a,
                           b, result);
    vinfo_decref(a, po);
    vinfo_decref(b, po);
    if (x == NULL) {
        array_release(result);
        return NULL;
    }
    a = result->items[0];
    array_release(result);
    ok = runtime_NON_NULL_t(po, x); /* consumes x */
    if (ok == 1)
        return PsycoFloat_FROM_DOUBLE(a);
    vinfo_decref(a, po);
    if (ok < 0)
        return NULL;
in_c:
    return psyco_generic_call(
        po, slot, CfCommonCheckNotImplemented | CfNoPython | CfNoBlock, "vvv",
        v, w, z);
}

/* a unary operation whose double code is 'scalar', giving a 'flags' result
 */
static vinfo_t *
float_unary(PsycoObject *po, vinfo_t *v, void *scalar, int flags)
{
    vinfo_t *x = PsycoFloat_AS_DOUBLE(po, v);
    if (x == NULL)
        return NULL;
    vinfo_incref(x);
    v = psyco_generic_call(po, scalar, CfPure | flags | CfNoPython | CfNoBlock,
                           "d", x);
    vinfo_decref(x, po);
    return v;
}

static vinfo_t *
float_abs(PsycoObject *po, vinfo_t *v)
{
    v = float_unary(po, v, cimpl_float_abs, CfReturnTypeDouble);
    return v != NULL ? PsycoFloat_FROM_DOUBLE(v) : NULL;
}

static vinfo_t *
float_neg(PsycoObject *po, vinfo_t *v)
{
    v = float_unary(po, v, cimpl_float_neg, CfReturnTypeDouble);
    return v != NULL ? PsycoFloat_FROM_DOUBLE(v) : NULL;
}

static vinfo_t *
float_bool(PsycoObject *po, vinfo_t *v)
{
    return float_unary(po, v, cimpl_float_bool, CfReturnTypeInt);
}

static vinfo_t *
float_float(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *x;
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    if (tp_v == &PyFloat_Type) {
        vinfo_incref(v);
        return v;
    }
    x = PsycoFloat_AS_DOUBLE(po, v);
    if (x == NULL)
        return NULL;
    vinfo_incref(x);
    return PsycoFloat_FROM_DOUBLE(x);
}

static vinfo_t *
float_hash(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *x, *hash;
    x = PsycoFloat_AS_DOUBLE(po, v);
    if (x == NULL)
        return NULL;
    vinfo_incref(x);
#if LE_HAS_HASH_DOUBLE_OBJECT
    hash = psyco_generic_call(po, _Py_HashDouble,
                              CfReturnTypeNative | CfPure | CfNoBlock, "vd", v,
                              x);
#else
    hash = psyco_generic_call(po, _Py_HashDouble,
                              CfReturnTypeNative | CfPure | CfNoBlock, "d", x);
#endif
    vinfo_decref(x, po);
    return hash;
}

static void
gen_floatobject_init(void)
{
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_absolute, float_abs);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_add, float_add);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_bool, float_bool);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_true_divide, float_div);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_positive, float_float);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_power, float_pow);
    Psyco_DefineMeta(PyFloat_Type.tp_hash, float_hash);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_multiply, float_mul);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_negative, float_neg);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_remainder, float_rem);
    Psyco_DefineMeta(PyFloat_Type.tp_as_number->nb_subtract, float_sub);
}

INITIALIZATIONFN
void
psy_floatobject_init(void)
{
    gen_floatobject_init();
    Psyco_DefineMeta(PyFloat_Type.tp_richcompare, pfloat_richcompare);

    INIT_SVIRTUAL(psyco_computed_float, compute_float, direct_compute_float, 0,
                  0, 0);
}

#else  /* !HAVE_FP_FN_CALLS */
INITIALIZATIONFN
void
psy_floatobject_init(void)
{
}
#endif /* !HAVE_FP_FN_CALLS */
