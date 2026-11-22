/*
 * Virtual tuples. vinfo->array layout: [iOB_TYPE] = &PyTuple_Type and
 * [iFIX_SIZE] = length (both constants), then the items from iTUPLE_OB_ITEM.
 * compute_tuple materialises a tuple when it escapes: if every item is a
 * constant the tuple is built now and becomes a constant, otherwise
 * PyTuple_New and item stores are emitted. Tuples are immutable, so they stay
 * virtual across calls. PsycoTuple_Load is how other code gets a compile-time
 * length and the item vinfos (it returns -1 if the length is unknown, and
 * refuses constant tuples longer than CT_TUPLE_LOAD_SIZE_LIMIT).
 */
#include "ptupleobject.h"

/***************************************************************/
/***   Virtual tuples                                        ***/

static source_virtual_t psyco_computed_tuple;

/* PsycoTuple_New, compute_tuple and direct_compute_tuple: a virtual
   PyTuple_New */
/* PyTuple_New(n) and PyTuple_SET_ITEM for each item, on raw values */
static PyObject *
direct_compute_tuple(vinfo_t *v, char *data)
{
    int i, n = v->array->count - iTUPLE_OB_ITEM;
    PyObject *o = PyTuple_New(n);
    if (o == NULL)
        return NULL;
    for (i = 0; i < n; i++) {
        PyObject *item =
            direct_xobj_vinfo(v->array->items[iTUPLE_OB_ITEM + i], data);
        if (item == NULL) {
            Py_DECREF(o);
            return NULL;
        }
        PyTuple_SET_ITEM(o, i, item);
    }
    return o;
}

/* forced: a constant if every item is one and CPython would share the
   object (rule allocator_pure), else PyTuple_New(n) at run time and the items
   stored */
static bool
compute_tuple(PsycoObject *po, vinfo_t *v)
{
    int i, n = v->array->count - iTUPLE_OB_ITEM;
    vinfo_t *newobj = NULL;
    extra_assert(n ==
                 CompileTime_Get(v->array->items[iFIX_SIZE]->source)->value);
    for (i = 0; i < n; i++)
        if (!is_compiletime(v->array->items[iTUPLE_OB_ITEM + i]->source))
            break;
    if (i == n) {
        PyObject *o = direct_compute_tuple(v, NULL);
        if (o == NULL)
            OUT_OF_MEMORY();
        newobj = psyco_shared_constant(o);
    }
    if (newobj == NULL) {
        newobj = psyco_generic_call(
            po, PyTuple_New, CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
            "l", (long)n);
        if (newobj == NULL)
            return false;
        for (i = 0; i < n; i++)
            if (!psyco_put_nth_field(po, newobj, FMUT(FPYREF(TUPLE_ob_item)),
                                     i, v->array->items[iTUPLE_OB_ITEM + i])) {
                vinfo_decref(newobj, po);
                return false;
            }
    }
    vinfo_move(po, v, newobj);
    return true;
}

/* a virtual tuple of count items, taking new references to source[0..count)
   (or filled by the caller when source is NULL) */
DEFINEFN
vinfo_t *
PsycoTuple_New(int count, vinfo_t **source)
{
    int i;
    vinfo_t *v = vinfo_new(VirtualTime_New(&psyco_computed_tuple));
    v->array = array_new(iTUPLE_OB_ITEM + count);
    v->array->items[iOB_TYPE] =
        vinfo_new(CompileTime_New((long)&PyTuple_Type));
    v->array->items[iFIX_SIZE] = vinfo_new(CompileTime_New(count));
    if (source != NULL)
        for (i = 0; i < count; i++) {
            vinfo_incref(source[i]);
            v->array->items[iTUPLE_OB_ITEM + i] = source[i];
        }
    return v;
}

/* the force and direct functions, with their weights */
static void
gen_virtual_tuple_init(void)
{
    INIT_SVIRTUAL(psyco_computed_tuple, compute_tuple, direct_compute_tuple,
                  (-1 << iTUPLE_OB_ITEM), NW_TUPLES_NORMAL, NW_TUPLES_FUNCALL);
}

/* do not load a constant tuple into a vinfo_array_t if longer than: */
#define CT_TUPLE_LOAD_SIZE_LIMIT 15

DEFINEFN
int
PsycoTuple_Load(vinfo_t *tuple)
{
    int size;
    /* if the tuple is virtual, then all items in its
       vinfo_array_t are already filled */
    if (tuple->source == VirtualTime_New(&psyco_computed_tuple))
        size = tuple->array->count - iTUPLE_OB_ITEM;
    else if (is_compiletime(tuple->source)) {
        /* a constant tuple means constant tuple items */
        int i;
        PyObject *o = (PyObject *)(CompileTime_Get(tuple->source)->value);
        /* BUILD_*_UNPACK asks about any constant, such as a global
           list */
        if (!PyTuple_Check(o))
            return -1;
        size = PyTuple_GET_SIZE(o);
        if (tuple->array->count < iTUPLE_OB_ITEM + size) {
            if (/*not_too_much &&*/ size > CT_TUPLE_LOAD_SIZE_LIMIT)
                return -1;
            vinfo_array_grow(tuple, iTUPLE_OB_ITEM + size);
        }
        /* load the tuple into the vinfo_array_t */
        for (i = 0; i < size; i++) {
            if (PsycoTuple_GET_ITEM(tuple, i) == NULL) {
                PyObject *item = PyTuple_GET_ITEM(o, i);
                source_known_t *sk;
                Py_INCREF(item);
                sk = sk_new((long)item, SkFlagPyObj);
                PsycoTuple_GET_ITEM(tuple, i) =
                    vinfo_new(CompileTime_NewSk(sk));
            }
        }
    }
    else
        return -1;
    return size;
}

/* the slots of tupleobject.c: sq_length, sq_item, sq_contains, sq_concat,
   sq_repeat, tp_richcompare */

/* the flags of a C call that makes a tuple, and its result */
#define CF_TUPLE (CfCommonNewRefPyObject | CfNoPython | CfNoBlock)
static vinfo_t *
made_tuple(PsycoObject *po, vinfo_t *t)
{
    if (t != NULL)
        Psyco_AssertType(po, t, &PyTuple_Type);
    return t;
}

#if LE_HAS_TUPLE_GET_EMPTY
static vinfo_t *
tuple_get_empty(PsycoObject *po)
{
    /* tupleobject.c's empty tuple, which PyTuple_New(0) gives */
    return vinfo_new(
        CompileTime_NewSk(sk_new((long)PyTuple_New(0), SkFlagPyObj)));
}
#endif

static vinfo_t *
tupleconcat(PsycoObject *po, vinfo_t *a, vinfo_t *bb)
{
    binaryfunc concat = PyTuple_Type.tp_as_sequence->sq_concat;
    PyTypeObject *tp_a, *tp_bb;
    int alen, blen;

    alen = PsycoTuple_Load(a);
    if (alen < 0 || alen > 16) {
        return made_tuple(
            po, psyco_generic_call(po, concat, CF_TUPLE, "vv", a, bb));
    }
    tp_bb = Psyco_NeedType(po, bb);
    if (tp_bb == NULL) {
        return NULL;
    }
    if (alen == 0 && tp_bb == &PyTuple_Type) {
        vinfo_incref(bb);
        return bb;
    }
    if ((tp_bb->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) == 0) {
        PycException_SetTypeFormat(po, PyExc_TypeError, LE_MSG_TUPLE_CONCAT,
                                   NULL, tp_bb, NULL);
        return NULL;
    }
    blen = PsycoTuple_Load(bb);
    if (blen < 0 || blen > 16) {
        return made_tuple(
            po, psyco_generic_call(po, concat, CF_TUPLE, "vv", a, bb));
    }
    if (blen == 0) {
        tp_a = Psyco_NeedType(po, a);
        if (tp_a == NULL) {
            return NULL;
        }
        if (tp_a == &PyTuple_Type) {
            vinfo_incref(a);
            return a;
        }
    }
#if LE_HAS_TUPLE_GET_EMPTY
    if (alen + blen == 0) {
        return made_tuple(po, tuple_get_empty(po));
    }
    return made_tuple(po,
                      psyco_generic_call(po, concat, CF_TUPLE, "vv", a, bb));
#else
    {
        vinfo_t *np, *v;
        int i;
        if (alen + blen > CT_TUPLE_LOAD_SIZE_LIMIT) {
            return made_tuple(
                po, psyco_generic_call(po, concat, CF_TUPLE, "vv", a, bb));
        }
        np = PsycoTuple_New(alen + blen, NULL);
        for (i = 0; i < alen; i++) {
            v = PsycoTuple_GET_ITEM(a, i);
            vinfo_incref(v);
            PsycoTuple_GET_ITEM(np, i) = v;
        }
        for (i = 0; i < blen; i++) {
            v = PsycoTuple_GET_ITEM(bb, i);
            vinfo_incref(v);
            PsycoTuple_GET_ITEM(np, alen + i) = v;
        }
        return np;
    }
#endif
}

/* the types of the items of a, a tuple of n known items */
static bool
need_item_types(PsycoObject *po, vinfo_t *a, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(a, i)) == NULL) {
            return false;
        }
    }
    return true;
}

static vinfo_t *
tuplecontains(PsycoObject *po, vinfo_t *a, vinfo_t *el)
{
    vinfo_t *cmp, *k;
    condition_code_t cc;
    int i, n;

    n = PsycoTuple_Load(a);
    if (n < 0 || n > 16) {
        return psyco_generic_call(po, PyTuple_Type.tp_as_sequence->sq_contains,
                                  CfCommonIntBoolOrError, "vv", a, el);
    }
#if LE_HAS_CONTAINS_ITEM_FIRST
    if (!need_item_types(po, a, n) || Psyco_NeedType(po, el) == NULL) {
        return NULL;
    }
#else
    if (Psyco_NeedType(po, el) == NULL || !need_item_types(po, a, n)) {
        return NULL;
    }
#endif
    cmp = vinfo_new(CompileTime_New(0));
    for (i = 0;; i++) {
        cc = integer_non_null(po, cmp);
        if (cc == CC_ERROR) {
            vinfo_decref(cmp, po);
            return NULL;
        }
        if (!(runtime_condition_t(po, INVERT_CC(cc)) && i < n))
            break;
#if LE_HAS_CONTAINS_ITEM_FIRST
        k = PsycoObject_RichCompareBool(po, PsycoTuple_GET_ITEM(a, i), el,
                                        Py_EQ);
#else
        k = PsycoObject_RichCompareBool(po, el, PsycoTuple_GET_ITEM(a, i),
                                        Py_EQ);
#endif
        vinfo_decref(cmp, po);
        cmp = k;
        if (cmp == NULL) {
            return NULL;
        }
    }
    return cmp;
}

static vinfo_t *
tupleitem(PsycoObject *po, vinfo_t *a, vinfo_t *i)
{
    condition_code_t cc;
    vinfo_t *size = psyco_get_field(po, a, FIX_size);
    if (size == NULL) {
        return NULL;
    }
    cc = integer_cmp(po, i, size, Py_GE | COMPARE_UNSIGNED);
    vinfo_decref(size, po);
    if (cc == CC_ERROR) {
        return NULL;
    }
    if (runtime_condition_f(po, cc)) {
        PycException_SetString(po, PyExc_IndexError, LE_MSG_TUPLE_INDEX_RANGE);
        return NULL;
    }
    return psyco_get_field_array(po, a, FPYREF(TUPLE_ob_item), i);
}

static vinfo_t *
tuplelength(PsycoObject *po, vinfo_t *a)
{
    return psyco_get_field(po, a, FIX_size);
}

static vinfo_t *
tuplerepeat(PsycoObject *po, vinfo_t *a, vinfo_t *n)
{
    ssizeargfunc repeat = PyTuple_Type.tp_as_sequence->sq_repeat;
    PyTypeObject *tp_a;
    Py_ssize_t n_k;
    int len;

    if (!is_compiletime(n->source)) {
        return made_tuple(
            po, psyco_generic_call(po, repeat, CF_TUPLE, "vv", a, n));
    }
    n_k = (Py_ssize_t)CompileTime_Get(n->source)->value;
#if !LE_HAS_TUPLE_GET_EMPTY
    if (n_k < 0) {
        n_k = 0;
    }
#endif
    len = PsycoTuple_Load(a);
    if (len < 0 || len > 16) {
        return made_tuple(
            po, psyco_generic_call(po, repeat, CF_TUPLE, "vl", a, (long)n_k));
    }
    if (len == 0 || n_k == 1) {
        tp_a = Psyco_NeedType(po, a);
        if (tp_a == NULL) {
            return NULL;
        }
        if (tp_a == &PyTuple_Type) {
            vinfo_incref(a);
            return a;
        }
#if !LE_HAS_TUPLE_GET_EMPTY
        if (len == 0) {
            return PsycoTuple_New(0, NULL);
        }
#endif
    }
#if LE_HAS_TUPLE_GET_EMPTY
    if (len == 0 || n_k <= 0) {
        return made_tuple(po, tuple_get_empty(po));
    }
#endif
    if (n_k > PY_SSIZE_T_MAX / len) {
        return made_tuple(
            po, psyco_generic_call(po, PyErr_NoMemory, CF_TUPLE, ""));
    }
#if !LE_HAS_TUPLE_GET_EMPTY
    if (len * n_k <= CT_TUPLE_LOAD_SIZE_LIMIT && n_k <= 16) {
        vinfo_t *np = PsycoTuple_New(len * n_k, NULL);
        Py_ssize_t i, j, p = 0;
        for (i = 0; i < n_k; i++) {
            for (j = 0; j < len; j++) {
                PsycoTuple_GET_ITEM(np, p) = PsycoTuple_GET_ITEM(a, j);
                vinfo_incref(PsycoTuple_GET_ITEM(np, p));
                p++;
            }
        }
        return np;
    }
#endif
    return made_tuple(
        po, psyco_generic_call(po, repeat, CF_TUPLE, "vl", a, (long)n_k));
}

static vinfo_t *
tuplerichcompare(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op)
{
    PyTypeObject *tp_v, *tp_w;
    vinfo_t *k;
    condition_code_t cc;
    Py_ssize_t i;
    int vlen, wlen;

    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    if ((tp_v->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) == 0) {
        return psyco_vi_NotImplemented();
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    if ((tp_w->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) == 0) {
        return psyco_vi_NotImplemented();
    }
    vlen = PsycoTuple_Load(v);
    wlen = vlen < 0 || vlen > 16 ? -1 : PsycoTuple_Load(w);
    if (wlen < 0 || wlen > 16) {
        return psyco_generic_call(po, PyTuple_Type.tp_richcompare,
                                  CfCommonCheckNotImplemented, "vvl", v, w,
                                  (long)op);
    }
    if (!need_item_types(po, v, vlen) || !need_item_types(po, w, wlen)) {
        return NULL;
    }
    for (i = 0; i < vlen && i < wlen; i++) {
        k = PsycoObject_RichCompareBool(po, PsycoTuple_GET_ITEM(v, i),
                                        PsycoTuple_GET_ITEM(w, i), Py_EQ);
        if (k == NULL) {
            return NULL;
        }
        cc = integer_non_null(po, k);
        if (cc == CC_ERROR) {
            vinfo_decref(k, po);
            return NULL;
        }
        if (!runtime_condition_t(po, cc)) {
            vinfo_decref(k, po);
            break;
        }
        vinfo_decref(k, po);
    }
    if (i >= vlen || i >= wlen) {
        bool res;
        switch (op) {
            case Py_EQ:
                res = vlen == wlen;
                break;
            case Py_NE:
                res = vlen != wlen;
                break;
            case Py_LT:
                res = vlen < wlen;
                break;
            case Py_GT:
                res = vlen > wlen;
                break;
            case Py_LE:
                res = vlen <= wlen;
                break;
            case Py_GE:
                res = vlen >= wlen;
                break;
            default:
                abort();
        }
        return res ? psyco_vi_Py_True() : psyco_vi_Py_False();
    }
    if (op == Py_EQ) {
        return psyco_vi_Py_False();
    }
    if (op == Py_NE) {
        return psyco_vi_Py_True();
    }
    return PsycoObject_RichCompare(po, PsycoTuple_GET_ITEM(v, i),
                                   PsycoTuple_GET_ITEM(w, i), op);
}

static void
gen_tupleobject_init(void)
{
    Psyco_DefineMeta(PyTuple_Type.tp_as_sequence->sq_concat, tupleconcat);
    Psyco_DefineMeta(PyTuple_Type.tp_as_sequence->sq_contains, tuplecontains);
    Psyco_DefineMeta(PyTuple_Type.tp_as_sequence->sq_item, tupleitem);
    Psyco_DefineMeta(PyTuple_Type.tp_as_sequence->sq_length, tuplelength);
    Psyco_DefineMeta(PyTuple_Type.tp_as_sequence->sq_repeat, tuplerepeat);
    Psyco_DefineMeta(PyTuple_Type.tp_richcompare, tuplerichcompare);
}

INITIALIZATIONFN
void
psy_tupleobject_init(void)
{
    PyMappingMethods *mm;
    gen_tupleobject_init();

    mm = PyTuple_Type.tp_as_mapping;
    if (mm) { /* Python >= 2.3 */
        Psyco_DefineMeta(mm->mp_subscript, psyco_generic_subscript);
    }

    gen_virtual_tuple_init();
}
