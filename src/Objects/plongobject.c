/*
 * Virtual ints, following the one-digit fast paths of 3.7's
 * Objects/longobject.c. A result is a virtual int whose [iLONG_VALUE] holds
 * the value as a machine word; it is boxed with PyLong_FromLong only if it
 * escapes. An operand takes the fast path if it has one digit, i.e.
 * |value| <= PyLong_MASK: for an int in memory that is one digit at most
 * (|ob_size| <= 1, or lv_tag's count where ints have one), for
 * a virtual one a range test on the word. Both are run-time conditions
 * (respawn), and an operand that fails falls back to CPython's slot, as
 * longobject.c does.
 *
 * + - * unary - ~ + abs(), bool, / and ** are translated from
 * longobject.c with the word representation (below). Not translated: the
 * representation itself (plong_medium_value, MEDIUM_VALUE and its guard on a
 * virtual int, which has no ob_size; PsycoLong_Sign), and what 3.7's C has no
 * word form of: long_richcompare (long_compare is a digit loop), long_hash's
 * one-digit cases, & | ^ << >> (digit loops) and // % divmod() (3.7's fast
 * paths read the digit and the sign, which a virtual int doesn't have;
 * the helpers compute the same on words).
 */
#include "plongobject.h"
#include "pboolobject.h"
#include "pfloatobject.h"
#include "ptupleobject.h"

/* MEDIUM_VALUE(v) of an int that is one digit at run time, as a new
   reference in *pval. Returns 1; 0 if it isn't one digit (the caller falls
   back); -1 on error or promotion. */
static int
plong_medium_value(PsycoObject *po, vinfo_t *v, vinfo_t **pval)
{
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

#if LE_HAS_LONG_VALUE
    /* as _PyLong_CompactValue: at most one digit (there is always one,
       0 for zero), times 1 - the sign flags (0 positive, 1 zero, 2
       negative) */
    cc = PsycoLong_SizeWithin(po, v, 1);
    if (cc == CC_ERROR)
        return -1;
    if (!runtime_condition_t(po, cc))
        return 0;
    size = PsycoLong_Sign(po, v);
    if (size == NULL)
        return -1;
    digit0 = psyco_get_const(po, v, LONG_ob_digit0);
    if (digit0 == NULL) {
        vinfo_decref(size, po);
        return -1;
    }
    *pval = integer_mul(po, size, digit0, false);
    vinfo_decref(size, po);
    return *pval != NULL ? 1 : -1;
#else
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
#endif
}

DEFINEFN int
PsycoLong_MediumValue(PsycoObject *po, vinfo_t *v, vinfo_t **pval)
{
    return plong_medium_value(po, v, pval);
}

DEFINEFN vinfo_t *
PsycoLong_Sign(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *r;
#if LE_HAS_LONG_VALUE
    vinfo_t *tag, *three, *flags, *one;
#endif
    if (PsycoLong_IsVirtual(v)) {
        vinfo_incref(v->array->items[iLONG_VALUE]);
        return v->array->items[iLONG_VALUE];
    }
#if LE_HAS_LONG_VALUE
    /* 1 - the sign flags: 1, 0 or -1 */
    tag = psyco_get_const(po, v, LONG_lv_tag);
    if (tag == NULL)
        return NULL;
    three = vinfo_new(CompileTime_New(3));
    flags = integer_and(po, tag, three);
    vinfo_decref(three, po);
    if (flags == NULL)
        return NULL;
    one = vinfo_new(CompileTime_New(1));
    r = integer_sub(po, one, flags, false);
    vinfo_decref(one, po);
    vinfo_decref(flags, po);
    return r;
#else
    r = psyco_get_const(po, v, LONG_ob_size);
    if (r != NULL)
        vinfo_incref(r);
    return r;
#endif
}

DEFINEFN condition_code_t
PsycoLong_SizeWithin(PsycoObject *po, vinfo_t *v, long n)
{
    vinfo_t *size;
#if LE_HAS_LONG_VALUE
    /* the digit count is lv_tag >> 3 */
    size = psyco_get_const(po, v, LONG_lv_tag);
    if (size == NULL)
        return CC_ERROR;
    return integer_cmp_i(po, size, (n + 1) << 3, Py_LT | COMPARE_UNSIGNED);
#else
    vinfo_t *t;
    condition_code_t cc;
    /* -n <= ob_size <= n, as one unsigned comparison */
    size = psyco_get_const(po, v, LONG_ob_size);
    if (size == NULL)
        return CC_ERROR;
    t = integer_add_i(po, size, n, false);
    if (t == NULL)
        return CC_ERROR;
    cc = integer_cmp_i(po, t, 2 * n, Py_LE | COMPARE_UNSIGNED);
    vinfo_decref(t, po);
    return cc;
#endif
}

/* PyNumber_AsSsize_t: an int whose type is known (or a subclass) inline
   when virtual or one digit, which always fits, and an int constant at
   compile time; anything else, and a constant that doesn't fit, is the
   call. Another constant's __index__ is Python code, run each time */
DEFINEFN
vinfo_t *
PsycoNumber_AsSsize_t(PsycoObject *po, vinfo_t *v, PyObject *err)
{
    vinfo_t *w;
    PyTypeObject *tp;
    if (PsycoLong_IsVirtual(v)) {
        w = v->array->items[iLONG_VALUE];
        vinfo_incref(w);
        return w;
    }
    if (is_compiletime(v->source) &&
        PyLong_Check((PyObject *)CompileTime_Get(v->source)->value)) {
        Py_ssize_t i = PyNumber_AsSsize_t(
            (PyObject *)CompileTime_Get(v->source)->value, err);
        if (i != -1 || !PyErr_Occurred())
            return vinfo_new(CompileTime_New(i));
        PyErr_Clear();
    }
    else if ((tp = Psyco_KnownType(v)) != NULL &&
             PyType_TypeCheck(tp, &PyLong_Type)) {
        switch (plong_medium_value(po, v, &w)) {
            case 1:
                return w;
            case -1:
                return NULL;
        }
    }
    return psyco_generic_call(po, PyNumber_AsSsize_t,
                              CfReturnTypeNative | CfPyErrCheckMinus1, "vl", v,
                              (long)err);
}

DEFINEFN
vinfo_t *
PsycoLong_AsDouble(PsycoObject *po, vinfo_t *v)
{
    /* a virtual int's word converts directly; (double)long rounds to
       nearest even, as PyLong_AsDouble does */
    if (PsycoLong_IsVirtual(v)) {
        vinfo_t *word = v->array->items[iLONG_VALUE];
        if (is_runtime(word->source))
            return psyco_float_from_word(po, word);
        return psyco_generic_call(po, cimpl_fp_from_long,
                                  CfPure | CfReturnTypeDouble, "v", word);
    }
    return psyco_generic_call(po, PyLong_AsDouble,
                              CfPyErrCheck | CfReturnTypeDouble, "v", v);
}

DEFINEFN
vinfo_t *
PsycoLong_FromUnsignedLong(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *result = psyco_generic_call(po, PyLong_FromUnsignedLong,
                                         CfCommonNewRefPyObject, "v", v);
    if (result != NULL)
        Psyco_AssertType(po, result, &PyLong_Type);
    return result;
}

DEFINEFN
vinfo_t *
PsycoLong_FromLong(PsycoObject *po, vinfo_t *v)
{
    vinfo_incref(v);
    return PsycoLong_FROM_WORD(v);
}

/* a Py_ssize_t is a long */
DEFINEFN
vinfo_t *
PsycoLong_FromSsize_t(PsycoObject *po, vinfo_t *v)
{
    vinfo_incref(v);
    return PsycoLong_FROM_WORD(v);
}

DEFINEFN
vinfo_t *
PsycoLong_FROM_LONG(PsycoObject *po, vinfo_t *v)
{
    return PsycoLong_FROM_WORD(v);
}

/***************************************************************/
/*** computing a virtual int                                 ***/

static bool
compute_long(PsycoObject *po, vinfo_t *longobj)
{
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

static PyObject *
direct_compute_long(vinfo_t *longobj, char *data)
{
    long value = direct_read_vinfo(vinfo_getitem(longobj, iLONG_VALUE), data);
    if (value == -1 && PyErr_Occurred())
        return NULL;
    return PyLong_FromLong(value);
}

DEFINEVAR source_virtual_t psyco_computed_long;

static bool
compute_bool(PsycoObject *po, vinfo_t *boolobj)
{
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

static PyObject *
direct_compute_bool(vinfo_t *boolobj, char *data)
{
    long value = direct_read_vinfo(vinfo_getitem(boolobj, iLONG_VALUE), data);
    if (value == -1 && PyErr_Occurred())
        return NULL;
    return PyBool_FromLong(value);
}

DEFINEVAR source_virtual_t psyco_computed_bool;

/***************************************************************/
/*** int objects meta-implementation                         ***/

/* CHECK_BINOP: 1 if both are ints, 0 if not, -1 on error or promotion */
static int
both_longs(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    int r = Psyco_VerifyType(po, v, &PyLong_Type);
    if (r != 1)
        return r;
    return Psyco_VerifyType(po, w, &PyLong_Type);
}

/* the slow path: CPython's own slot, on ints */
static vinfo_t *
plong_fallback2(PsycoObject *po, void *slot, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *x =
        psyco_generic_call(po, slot, CfCommonNewRefPyObject, "vv", v, w);
    if (x != NULL)
        Psyco_AssertType(po, x, &PyLong_Type);
    return x;
}

/* + - * unary - ~ + abs() bool / **, as in Objects/longobject.c */
_Static_assert(sizeof(long) * 8 == 64, "words are longs");
_Static_assert(sizeof(int) * 8 == 32, "C ints are 32 bits");
_Static_assert(PyLong_MASK == 1073741823,
               "the word range of struct _longobject");

/* the flags of a C call that makes an int, and its result */
#define CF_LONG (CfCommonNewRefPyObject | CfNoPython | CfNoBlock)
static vinfo_t *
made_long(PsycoObject *po, vinfo_t *x)
{
    if (x != NULL)
        Psyco_AssertType(po, x, &PyLong_Type);
    return x;
}

/* CHECK_BINOP of longobject.c: true if both are ints, false with
 *result set (NotImplemented, or NULL on error) if not */
static bool
check_binop(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t **result)
{
    PyTypeObject *tp = Psyco_NeedType(po, v);
    *result = NULL;
    if (tp == NULL)
        return false;
    if ((tp->tp_flags & Py_TPFLAGS_LONG_SUBCLASS) != 0) {
        tp = Psyco_NeedType(po, w);
        if (tp == NULL)
            return false;
        if ((tp->tp_flags & Py_TPFLAGS_LONG_SUBCLASS) != 0)
            return true;
    }
    *result = psyco_vi_NotImplemented();
    return false;
}

static vinfo_t *
_PsycoLong_Copy(PsycoObject *po, vinfo_t *src)
{
    return psyco_generic_call(po, _PyLong_Copy, CF_LONG, "v", src);
}

static vinfo_t *
long_neg(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *w_v, *x;
    switch (PsycoLong_MediumValue(po, v, &w_v)) {
        case 1:
            break;
        case 0:
            return made_long(po, psyco_generic_call(
                                     po, PyLong_Type.tp_as_number->nb_negative,
                                     CF_LONG, "v", v));
        default:
            return NULL;
    }
    /* in [-1073741823, 1073741823]: no overflow */
    x = integer_neg(po, w_v, false);
    vinfo_decref(w_v, po);
    return x != NULL ? PsycoLong_FROM_WORD(x) : NULL;
}

static vinfo_t *
long_long(PsycoObject *po, vinfo_t *v)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    if (tp_v == &PyLong_Type) {
        vinfo_incref(v);
        return v;
    }
    return _PsycoLong_Copy(po, v);
}

static vinfo_t *
long_abs(PsycoObject *po, vinfo_t *v)
{
    condition_code_t cc;
    vinfo_t *sign = PsycoLong_Sign(po, v);
    if (sign == NULL)
        return NULL;
    cc = integer_cmp_i(po, sign, 0, Py_LT);
    vinfo_decref(sign, po);
    if (cc == CC_ERROR)
        return NULL;
    if (runtime_condition_f(po, cc))
        return made_long(po, long_neg(po, v));
    return made_long(po, long_long(po, v));
}

/* + - * of two medium ints, whose result fits in a word: 'slot' is the
   whole operation for the others, and 'cf' its flags */
static vinfo_t *
long_binop(PsycoObject *po, vinfo_t *a, vinfo_t *b, binaryfunc slot, int cf,
           vinfo_t *(*op)(PsycoObject *, vinfo_t *, vinfo_t *, bool))
{
    vinfo_t *w_a, *w_b, *x;
    if (!check_binop(po, a, b, &x))
        return x;
    switch (PsycoLong_MediumValue(po, a, &w_a)) {
        case 1:
            break;
        case 0:
            return made_long(po, psyco_generic_call(po, slot, cf, "vv", a, b));
        default:
            return NULL;
    }
    switch (PsycoLong_MediumValue(po, b, &w_b)) {
        case 1:
            break;
        case 0:
            x = made_long(po, psyco_generic_call(po, slot, cf, "vv", a, b));
            vinfo_decref(w_a, po);
            return x;
        default:
            vinfo_decref(w_a, po);
            return NULL;
    }
    x = op(po, w_a, w_b, false);
    vinfo_decref(w_a, po);
    vinfo_decref(w_b, po);
    return x != NULL ? PsycoLong_FROM_WORD(x) : NULL;
}

static vinfo_t *
long_add(PsycoObject *po, vinfo_t *a, vinfo_t *b)
{
    /* in [-2147483646, 2147483646]: no overflow */
    return long_binop(po, a, b, PyLong_Type.tp_as_number->nb_add, CF_LONG,
                      integer_add);
}

static vinfo_t *
long_sub(PsycoObject *po, vinfo_t *a, vinfo_t *b)
{
    /* in [-2147483646, 2147483646]: no overflow */
    return long_binop(po, a, b, PyLong_Type.tp_as_number->nb_subtract, CF_LONG,
                      integer_sub);
}

static vinfo_t *
long_mul(PsycoObject *po, vinfo_t *a, vinfo_t *b)
{
    /* in [-1152921502459363329, 1152921502459363329]: no overflow */
    return long_binop(po, a, b, PyLong_Type.tp_as_number->nb_multiply,
                      CfCommonNewRefPyObject, integer_mul);
}

static vinfo_t *
long_bool(PsycoObject *po, vinfo_t *v)
{
    condition_code_t cc;
    vinfo_t *sign = PsycoLong_Sign(po, v);
    if (sign == NULL)
        return NULL;
    cc = integer_non_null(po, sign);
    vinfo_decref(sign, po);
    if (cc == CC_ERROR)
        return NULL;
    return psyco_vinfo_condition(po, cc);
}

static vinfo_t *
long_float(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *result = PsycoLong_AsDouble(po, v);
    if (result == NULL)
        return NULL;
    return PsycoFloat_FROM_DOUBLE(result);
}

static vinfo_t *
long_invert(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *w_v, *x, *y;
    switch (PsycoLong_MediumValue(po, v, &w_v)) {
        case 1:
            break;
        case 0:
            return made_long(
                po, psyco_generic_call(po, PyLong_Type.tp_as_number->nb_invert,
                                       CF_LONG, "v", v));
        default:
            return NULL;
    }
    /* in [-1073741822, 1073741824]: no overflow */
    x = integer_add_i(po, w_v, 1, false);
    vinfo_decref(w_v, po);
    if (x == NULL)
        return NULL;
    /* in [-1073741824, 1073741822]: no overflow */
    y = integer_neg(po, x, false);
    vinfo_decref(x, po);
    return y != NULL ? PsycoLong_FROM_WORD(y) : NULL;
}

static vinfo_t *
long_pow(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t *x)
{
    return psyco_generic_call(po, PyLong_Type.tp_as_number->nb_power,
                              CfCommonCheckNotImplemented, "vvv", v, w, x);
}

static vinfo_t *
long_true_divide(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *x;
    if (!check_binop(po, v, w, &x))
        return x;
    x = psyco_generic_call(po, PyLong_Type.tp_as_number->nb_true_divide,
                           CfCommonCheckNotImplemented, "vv", v, w);
    if (x != NULL && !IS_NOTIMPLEMENTED(x))
        Psyco_AssertType(po, x, &PyFloat_Type);
    return x;
}

static void
gen_longobject_init(void)
{
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_absolute, long_abs);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_add, long_add);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_bool, long_bool);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_float, long_float);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_invert, long_invert);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_positive, long_long);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_multiply, long_mul);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_negative, long_neg);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_power, long_pow);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_subtract, long_sub);
    Psyco_DefineMeta(PyLong_Type.tp_as_number->nb_true_divide,
                     long_true_divide);
}

/* long_hash: a one-digit int hashes to its value, but -1 to -2 (-1 means
   an error) */
static long
cimpl_long_hash1(long v)
{
    return v == -1 ? -2 : v;
}

static vinfo_t *
plong_hash(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *a, *x;
    int r = plong_medium_value(po, v, &a);
    if (r < 0)
        return NULL;
    if (r == 0)
        return psyco_generic_call(po, PyLong_Type.tp_hash,
                                  CfReturnTypeLong | CfPyErrIfMinus1, "v", v);
    x = psyco_generic_call(po, cimpl_long_hash1, CfPure | CfReturnTypeLong,
                           "v", a);
    vinfo_decref(a, po);
    return x;
}

/* long_richcompare: for one-digit ints, long_compare is the comparison of
   their values */
static vinfo_t *
plong_richcompare(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op)
{
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
                                  CfCommonNewRefPyObject, "vvl", v, w,
                                  (long)op);
    cc = integer_cmp(po, a, b, op);
    vinfo_decref(b, po);
    vinfo_decref(a, po);
    if (cc == CC_ERROR)
        return NULL;
    return PsycoBool_FromCondition(po, cc);
}

/* & | ^ << >> on one-digit ints as word operations. Hand-written: 3.7's
   long_and etc. work on digit arrays with no one-digit fast path, but
   3.12's long_and/long_or/long_xor have one (_PyLong_IsCompact), which
   the word representation could translate there. A word shift has
   Python's meaning for >> (a count of 64 or more gives the sign, a
   negative one raises ValueError, as integer_rshift does); for << the
   count must be at most 32, which keeps a one-digit value inside a word. */
enum { OP_AND, OP_OR, OP_XOR, OP_LSHIFT, OP_RSHIFT };

static vinfo_t *
plong_bitop(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op, void *slot)
{
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
        condition_code_t cc =
            integer_cmp_i(po, b, 32, Py_LE | COMPARE_UNSIGNED);
        if (cc == CC_ERROR)
            goto done;
        if (!runtime_condition_t(po, cc)) {
            x = plong_fallback2(po, slot, v, w);
            goto done;
        }
    }
    switch (op) {
        case OP_AND:
            x = integer_and(po, a, b);
            break;
        case OP_OR:
            x = integer_or(po, a, b);
            break;
        case OP_XOR:
            x = integer_xor(po, a, b);
            break;
        case OP_LSHIFT:
            x = integer_lshift(po, a, b);
            break;
        default:
            x = integer_rshift(po, a, b);
            break;
    }
    if (x != NULL)
        x = PsycoLong_FROM_WORD(x);
done:
    vinfo_decref(a, po);
    vinfo_decref(b, po);
    return x;
}

#define BITOP(name, op, slotname)                                             \
    static vinfo_t *name(PsycoObject *po, vinfo_t *v, vinfo_t *w)             \
    {                                                                         \
        return plong_bitop(po, v, w, op, PyLong_Type.tp_as_number->slotname); \
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
static long
cimpl_floor_div(long a, long b)
{
    long q = a / b;
    if (a % b != 0 && (a < 0) != (b < 0))
        q--;
    return q;
}

static long
cimpl_floor_mod(long a, long b)
{
    long m = a % b;
    if (m != 0 && (m < 0) != (b < 0))
        m += b;
    return m;
}

/* the quotient and/or remainder of two one-digit words (*pq, *pm may be
   NULL when not wanted): 1, 0 if the divisor is zero (the caller takes the
   real slot, which raises), -1 on error */
static int
floor_divmod_words(PsycoObject *po, vinfo_t *a, vinfo_t *b, vinfo_t **pq,
                   vinfo_t **pm)
{
    condition_code_t cc;
    if (is_compiletime(b->source) && CompileTime_Get(b->source)->value > 0 &&
        (CompileTime_Get(b->source)->value &
         (CompileTime_Get(b->source)->value - 1)) == 0) {
        long d = CompileTime_Get(b->source)->value;
        int k = 0;
        while ((1L << k) != d) k++;
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
    if (pq != NULL && (*pq = psyco_generic_call(po, cimpl_floor_div,
                                                CfPure | CfReturnTypeLong,
                                                "vv", a, b)) == NULL)
        return -1;
    if (pm != NULL && (*pm = psyco_generic_call(po, cimpl_floor_mod,
                                                CfPure | CfReturnTypeLong,
                                                "vv", a, b)) == NULL) {
        if (pq != NULL)
            vinfo_decref(*pq, po);
        return -1;
    }
    return 1;
}

/* the real slot; divmod's returns a tuple, not an int */
static vinfo_t *
divmod_fallback(PsycoObject *po, int which, void *slot, vinfo_t *v, vinfo_t *w)
{
    if (which == 2)
        return psyco_generic_call(po, slot, CfCommonNewRefPyObject, "vv", v,
                                  w);
    return plong_fallback2(po, slot, v, w);
}

/* which: 0 //, 1 %, 2 divmod() */
static vinfo_t *
plong_divmod_op(PsycoObject *po, vinfo_t *v, vinfo_t *w, int which, void *slot)
{
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

static vinfo_t *
plong_div(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    return plong_divmod_op(po, v, w, 0,
                           PyLong_Type.tp_as_number->nb_floor_divide);
}

static vinfo_t *
plong_mod(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    return plong_divmod_op(po, v, w, 1,
                           PyLong_Type.tp_as_number->nb_remainder);
}

static vinfo_t *
plong_divmod(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    return plong_divmod_op(po, v, w, 2, PyLong_Type.tp_as_number->nb_divmod);
}

INITIALIZATIONFN
void
psy_longobject_init(void)
{
    PyNumberMethods *m = PyLong_Type.tp_as_number;

    gen_longobject_init();
    Psyco_DefineMeta(m->nb_remainder, plong_mod);
    Psyco_DefineMeta(m->nb_lshift, plong_lshift);
    Psyco_DefineMeta(m->nb_rshift, plong_rshift);
    Psyco_DefineMeta(m->nb_and, plong_and);
    Psyco_DefineMeta(m->nb_xor, plong_xor);
    Psyco_DefineMeta(m->nb_or, plong_or);
    Psyco_DefineMeta(m->nb_floor_divide, plong_div);
    Psyco_DefineMeta(m->nb_divmod, plong_divmod);
    Psyco_DefineMeta(PyLong_Type.tp_richcompare, plong_richcompare);
    Psyco_DefineMeta(PyLong_Type.tp_hash, plong_hash);

    INIT_SVIRTUAL(psyco_computed_long, compute_long, direct_compute_long, 0, 0,
                  0);
    INIT_SVIRTUAL(psyco_computed_bool, compute_bool, direct_compute_bool, 0, 0,
                  0);
}
