/*
 * array.array items in compiled code. a[i] and a[i] = v, for an exact
 * array whose typecode is an int or float one (bBhHiIlLqQfd) and an exact
 * int i, read the item into a virtual int or float, or write an exact int
 * or float into it unboxed. The array's descriptor is promoted, so each
 * typecode met compiles its own code. Anything else (slices, other index
 * types, other typecodes, subclasses, deletion), and an index or value out
 * of range, takes array's own subscript, so every error and message is
 * array's.
 *
 * array's object is private to Modules/arraymodule.c: the fields read here
 * are checked against the running module once (array_probe), when an
 * exact array is first subscripted; if they differ nothing is done
 * inline. Descriptors are told apart by their addresses, taken from
 * arrays made by the probe, so their layout is only checked, never used.
 */
#include "parray.h"
#include "../Objects/pfloatobject.h"
#include "../Objects/plongobject.h"
#include "../pycodegen.h"

/* the start of array's arrayobject, the same from 3.7 to 3.16 */
typedef struct {
    PyObject_VAR_HEAD char *ob_item;
    Py_ssize_t allocated;
    const void *ob_descr;
} le_arrayobject_t;

#define ARRAY_ob_item FMUT(DEF_FIELD(le_arrayobject_t, ob_item, VAR_size))
#define ARRAY_ob_descr DEF_FIELD(le_arrayobject_t, ob_descr, ARRAY_ob_item)

/* what an item of one typecode is */
typedef struct {
    const void *descr;
    int size;
    bool is_signed;
    bool is_float;
} item_kind_t;

#define ITEM_CODES "bBhHiIlLqQfd"
static item_kind_t kinds[sizeof(ITEM_CODES) - 1];
static int nkinds;

/* 1 usable, -1 not, 0 not yet probed */
static int array_state = 0;
static PyTypeObject *array_type;
static binaryfunc array_subscr;
static objobjargproc array_ass_subscr;

/* the item kind of typecode 'code', from arrays of tp made with it; false
   if it isn't the one expected */
static bool
kind_probe(PyTypeObject *tp, char code, item_kind_t *k)
{
    char name[2] = {code, 0};
    bool is_float = code == 'f' || code == 'd';
    PyObject *a, *empty = NULL, *itemsize = NULL;
    le_arrayobject_t *arr;
    char *item;
    long size;
    bool ok = false;
    if (is_float)
        a = PyObject_CallFunction((PyObject *)tp, "s(dd)", name, 1.5, 2.5);
    else
        a = PyObject_CallFunction((PyObject *)tp, "s(ii)", name, 1, 2);
    if (a == NULL || Py_TYPE(a) != tp)
        goto done;
    empty = PyObject_CallFunction((PyObject *)tp, "s", name);
    itemsize = PyObject_GetAttrString(a, "itemsize");
    if (empty == NULL || itemsize == NULL || !PyLong_Check(itemsize))
        goto done;
    size = PyLong_AsLong(itemsize);
    arr = (le_arrayobject_t *)a;
    /* every descriptor starts with its typecode */
    if (Py_SIZE(a) != 2 || arr->ob_item == NULL || arr->ob_descr == NULL ||
        arr->ob_descr != ((le_arrayobject_t *)empty)->ob_descr ||
        *(const char *)arr->ob_descr != code)
        goto done;
    item = arr->ob_item + size;
    switch (size) {
        case 1:
            ok = !is_float && *(signed char *)item == 2;
            break;
        case 2:
            ok = !is_float && *(short *)item == 2;
            break;
        case 4:
            ok = is_float ? *(float *)item == 2.5f : *(int *)item == 2;
            break;
        case 8:
            ok = is_float ? *(double *)item == 2.5 : *(long *)item == 2;
            break;
    }
    if (ok) {
        k->descr = arr->ob_descr;
        k->size = (int)size;
        k->is_float = is_float;
        k->is_signed = code >= 'a' && code <= 'z';
    }
done:
    Py_XDECREF(itemsize);
    Py_XDECREF(empty);
    Py_XDECREF(a);
    PyErr_Clear();
    return ok;
}

static vinfo_t *
parray_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key);
static vinfo_t *
parray_ass_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key,
                     vinfo_t *value);

static int
array_probe(PyTypeObject *tp)
{
    PyObject *exc_type, *exc_value, *exc_tb;
    const char *code;
    if (tp->tp_basicsize < (Py_ssize_t)sizeof(le_arrayobject_t) ||
        tp->tp_itemsize != 0 || tp->tp_as_mapping == NULL ||
        tp->tp_as_mapping->mp_subscript == NULL ||
        tp->tp_as_mapping->mp_ass_subscript == NULL)
        return -1;
    PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
    for (code = ITEM_CODES; *code; code++)
        if (kind_probe(tp, *code, &kinds[nkinds]))
            nkinds++;
    PyErr_Restore(exc_type, exc_value, exc_tb);
    if (nkinds == 0)
        return -1;
    Py_INCREF(tp); /* kept for ever */
    array_type = tp;
    array_subscr = tp->tp_as_mapping->mp_subscript;
    array_ass_subscr = tp->tp_as_mapping->mp_ass_subscript;
    Psyco_DefineMeta(array_subscr, parray_subscript);
    Psyco_DefineMeta(array_ass_subscr, parray_ass_subscript);
    return 1;
}

DEFINEFN
void
PsycoArray_Seen(PyTypeObject *tp)
{
    if (array_state == 0 && strcmp(tp->tp_name, "array.array") == 0)
        array_state = array_probe(tp);
}

/* whether v's type is tp, a static type: 1 or 0, -1 on error or
   promotion. Only a static type is promoted, so a site that meets objects
   of many classes doesn't compile for each. */
static int
is_exactly(PsycoObject *po, vinfo_t *v, PyTypeObject *tp)
{
    PyObject *vtp_value;
    vinfo_t *vtp;
    int r;
    PyTypeObject *known = Psyco_KnownType(v);
    if (known != NULL)
        return known == tp;
    vtp = psyco_get_const(po, v, OB_type);
    if (vtp == NULL)
        return -1;
    r = psyco_atcompiletime_static_type(po, vtp, &vtp_value);
    if (r == 0 && is_runtime(v->source))
        /* flagged megamorphic: forgotten, so that a later use of v's type
           loads it again */
        psyco_forget_field(po, v, OB_type);
    return r == 1 ? vtp_value == (PyObject *)tp : r;
}

/* a[key] of an exact array indexed by an exact int, as array_subscr and
   array_ass_subscr find it: 1 with the item's kind in *pk and its index,
   in range, as a new reference in *pi; 0 where array's own subscript is
   to be called; -1 on error or promotion */
static int
item_index(PsycoObject *po, vinfo_t *o, vinfo_t *key, const item_kind_t **pk,
           vinfo_t **pi)
{
    PyTypeObject *tp = Psyco_NeedType(po, o);
    vinfo_t *vdescr, *i, *size, *t;
    condition_code_t cc;
    long descr;
    int n;
    if (tp == NULL)
        return -1;
    if (tp != array_type)
        return 0;
    if ((n = is_exactly(po, key, &PyLong_Type)) != 1)
        return n;
    vdescr = psyco_get_const(po, o, ARRAY_ob_descr);
    if (vdescr == NULL)
        return -1;
    descr = psyco_atcompiletime(po, vdescr);
    if (descr == -1)
        return -1;
    for (n = 0; n < nkinds && kinds[n].descr != (const void *)descr; n++);
    if (n == nkinds)
        return 0;
    *pk = &kinds[n];

    i = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
    if (i == NULL)
        return -1;
    size = psyco_get_field(po, o, VAR_size);
    if (size == NULL)
        goto fail;
    cc = integer_cmp_i(po, i, 0, Py_LT);
    if (cc == CC_ERROR)
        goto fail_size;
    if (runtime_condition_f(po, cc)) {
        t = integer_add(po, i, size, false);
        if (t == NULL)
            goto fail_size;
        vinfo_decref(i, po);
        i = t;
    }
    cc = integer_cmp(po, i, size, Py_GE | COMPARE_UNSIGNED);
    vinfo_decref(size, po);
    if (cc == CC_ERROR)
        goto fail;
    if (runtime_condition_f(po, cc)) {
        vinfo_decref(i, po);
        return 0;
    }
    assert_nonneg(i);
    *pi = i;
    return 1;

fail_size:
    vinfo_decref(size, po);
fail:
    vinfo_decref(i, po);
    return -1;
}

/* how an item of kind k is in memory: a float's bits, zero-extended; a
   'd', 'L' or 'Q' item as a signed word */
static defield_t
item_field(const item_kind_t *k)
{
    switch (k->size) {
        case 1:
            return k->is_signed ? FMUT(DEF_ARRAY(signed char, 0))
                                : FMUT(DEF_ARRAY(unsigned char, 0));
        case 2:
            return k->is_signed ? FMUT(DEF_ARRAY(short, 0))
                                : FMUT(DEF_ARRAY(unsigned short, 0));
        case 4:
            return k->is_signed ? FMUT(DEF_ARRAY(int, 0))
                                : FMUT(DEF_ARRAY(unsigned int, 0));
        default:
            return FMUT(DEF_ARRAY(long, 0));
    }
}

/* f_getitem's float, from its bits */
static double
cimpl_float_item(long bits)
{
    unsigned int u = (unsigned int)bits;
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/* f_setitem's (float)x, as its bits; -1 where f_setitem may do otherwise:
   for a NaN, and a finite x too large for a float, which some versions
   store as it rounds and others refuse (PyFloat_Pack4) */
static long
cimpl_float_item_bits(double x)
{
    float f = (float)x;
    unsigned int u;
    if (isnan(x) || (isinf(f) && !isinf(x)))
        return -1;
    memcpy(&u, &f, sizeof(u));
    return u;
}

/* the object array's getitem makes of the item w (consumed) */
static vinfo_t *
item_object(PsycoObject *po, const item_kind_t *k, vinfo_t *w)
{
    vinfo_t *x;
    condition_code_t cc;
    if (k->is_float && k->size == 4) {
        x = psyco_generic_call(
            po, cimpl_float_item,
            CfReturnTypeDouble | CfPure | CfNoPython | CfNoBlock, "v", w);
        vinfo_decref(w, po);
        return x == NULL ? NULL : PsycoFloat_FROM_DOUBLE(x);
    }
    if (k->is_float)
        return PsycoFloat_FROM_DOUBLE(w);
    if (k->size == 8 && !k->is_signed) {
        /* PyLong_FromUnsignedLong's, beyond a word's range */
        cc = integer_cmp_i(po, w, 0, Py_LT);
        if (cc == CC_ERROR) {
            vinfo_decref(w, po);
            return NULL;
        }
        if (runtime_condition_f(po, cc)) {
            x = PsycoLong_FromUnsignedLong(po, w);
            vinfo_decref(w, po);
            return x;
        }
    }
    return PsycoLong_FROM_WORD(w);
}

static vinfo_t *
parray_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key)
{
    const item_kind_t *k = NULL;
    vinfo_t *i = NULL, *items, *w;
    int r = item_index(po, o, key, &k, &i);
    if (r < 0)
        return NULL;
    if (r == 0)
        return psyco_generic_call(po, array_subscr, CfCommonNewRefPyObject,
                                  "vv", o, key);
    items = psyco_get_field(po, o, ARRAY_ob_item);
    w = items == NULL ? NULL
                      : psyco_get_field_array(po, items, item_field(k), i);
    vinfo_xdecref(items, po);
    vinfo_decref(i, po);
    return w == NULL ? NULL : item_object(po, k, w);
}

/* the word the setitem of kind k stores for v: 1 with it as a new
   reference in *pw; 0 where array's own setitem is to convert v (another
   type, or out of range); -1 on error or promotion */
static int
item_word(PsycoObject *po, const item_kind_t *k, vinfo_t *v, vinfo_t **pw)
{
    vinfo_t *w, *t;
    condition_code_t cc;
    int r = is_exactly(po, v, k->is_float ? &PyFloat_Type : &PyLong_Type);
    if (r != 1)
        return r;
    if (k->is_float) {
        w = PsycoFloat_AS_DOUBLE(po, v);
        if (w == NULL)
            return -1;
        if (k->size == 8) {
            vinfo_incref(w);
            *pw = w;
            return 1;
        }
        w = psyco_generic_call(
            po, cimpl_float_item_bits,
            CfReturnTypeLong | CfPure | CfNoPython | CfNoBlock, "d", w);
        if (w == NULL)
            return -1;
        cc = integer_cmp_i(po, w, -1, Py_EQ);
    }
    else {
        if (PsycoLong_IsVirtual(v)) {
            w = v->array->items[iLONG_VALUE];
            vinfo_incref(w);
        }
        else if ((r = PsycoLong_MediumValue(po, v, &w)) != 1)
            return r;
        if (k->size == 8 && k->is_signed) {
            *pw = w;
            return 1;
        }
        if (k->size == 8)
            cc = integer_cmp_i(po, w, 0, Py_LT);
        else {
            /* outside lo..hi, as one unsigned comparison */
            long bits = 8 * k->size;
            long lo = k->is_signed ? -(1L << (bits - 1)) : 0;
            t = integer_add_i(po, w, -lo, false);
            if (t == NULL) {
                vinfo_decref(w, po);
                return -1;
            }
            cc = integer_cmp_i(po, t, (1L << bits) - 1,
                               Py_GT | COMPARE_UNSIGNED);
            vinfo_decref(t, po);
        }
    }
    /* cc: array's own setitem is to convert v */
    if (cc == CC_ERROR) {
        vinfo_decref(w, po);
        return -1;
    }
    if (runtime_condition_f(po, cc)) {
        vinfo_decref(w, po);
        return 0;
    }
    *pw = w;
    return 1;
}

static vinfo_t *
parray_ass_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key, vinfo_t *value)
{
    const item_kind_t *k = NULL;
    vinfo_t *i = NULL, *w = NULL, *items;
    bool ok;
    int r;
    if (value == NULL)
        return psyco_generic_call(po, array_ass_subscr, CfCommonIntZeroOk,
                                  "vvl", o, key, 0L);
    r = item_index(po, o, key, &k, &i);
    if (r < 0)
        return NULL;
    if (r == 0)
        goto generic;
    r = item_word(po, k, value, &w);
    if (r <= 0) {
        vinfo_decref(i, po);
        if (r < 0)
            return NULL;
        goto generic;
    }
    items = psyco_get_field(po, o, ARRAY_ob_item);
    ok =
        items != NULL && psyco_put_field_array(po, items, item_field(k), i, w);
    vinfo_xdecref(items, po);
    vinfo_decref(w, po);
    vinfo_decref(i, po);
    return ok ? VINFO_OK : NULL;

generic:
    return psyco_generic_call(po, array_ass_subscr, CfCommonIntZeroOk, "vvv",
                              o, key, value);
}
