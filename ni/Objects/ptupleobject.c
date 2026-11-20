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
#define CT_TUPLE_LOAD_SIZE_LIMIT    15

DEFINEFN
int PsycoTuple_Load(vinfo_t* tuple)
{
	int size;
	/* if the tuple is virtual, then all items in its
	   vinfo_array_t are already filled */
	if (tuple->source == VirtualTime_New(&psyco_computed_tuple))
		size = tuple->array->count - iTUPLE_OB_ITEM;
	else if (is_compiletime(tuple->source)) {
		/* a constant tuple means constant tuple items */
		int i;
		PyObject* o = (PyObject*)(CompileTime_Get(tuple->source)->value);
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
		for (i=0; i<size; i++) {
			if (PsycoTuple_GET_ITEM(tuple, i) == NULL) {
				PyObject* item = PyTuple_GET_ITEM(o, i);
				source_known_t* sk;
				Py_INCREF(item);
				sk = sk_new((long) item, SkFlagPyObj);
				PsycoTuple_GET_ITEM(tuple, i) =
					vinfo_new(CompileTime_NewSk(sk));
			}
		}
	}
	else
		return -1;
	return size;
}

/* the slots translated from tupleobject.c: sq_length,
   sq_item, sq_contains, sq_concat, sq_repeat, tp_richcompare */
/* the translation keeps CPython's unused variables */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#if NI_HAS_TUPLE_GET_EMPTY
PyObject *
cimpl_tuple_get_empty(void);
#endif

#if NI_HAS_TUPLE_GET_EMPTY
static vinfo_t *
tuple_get_empty(PsycoObject *po);
#endif
static vinfo_t *
tupleconcat(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
tuplecontains(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
tupleitem(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
tuplelength(PsycoObject *po, vinfo_t *);
static vinfo_t *
tuplerepeat(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
tuplerichcompare(PsycoObject *po, vinfo_t *, vinfo_t *, int);

#if NI_HAS_TUPLE_GET_EMPTY
static vinfo_t *
tuple_get_empty(PsycoObject *po)
{
    return vinfo_new(
        CompileTime_NewSk(sk_new((long)cimpl_tuple_get_empty(), SkFlagPyObj)));
}
#endif

#if NI_HAS_TUPLE_GET_EMPTY
static vinfo_t *
tupleconcat(PsycoObject *po, vinfo_t *a, vinfo_t *bb)
{
    int _t1;
    vinfo_t *_t2;
    int _t3;
    int _t4;
    vinfo_t *_t5;
    int _t6;
    vinfo_t *_t7;
    vinfo_t *_t8;
    vinfo_t *b;
    Py_ssize_t size;
    PyTypeObject *tp_a;
    PyTypeObject *tp_bb;
    _t1 = PsycoTuple_Load(a);
    if (_t1 < 0 || _t1 > 16) {
        _t2 = psyco_generic_call(
            po, PyTuple_Type.tp_as_sequence->sq_concat,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "vv", a, bb);
        if (_t2 != NULL)
            Psyco_AssertType(po, _t2, &PyTuple_Type);
        return _t2;
    }
    _t3 = _t1 == 0;
    if (_t3) {
        tp_bb = Psyco_NeedType(po, bb);
        if (tp_bb == NULL) {
            return NULL;
        }
        _t3 = tp_bb == &PyTuple_Type;
    }
    if (_t3) {
        vinfo_incref(bb);
        return bb;
    }
    tp_bb = Psyco_NeedType(po, bb);
    if (tp_bb == NULL) {
        return NULL;
    }
    if ((tp_bb->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) == 0) {
        PycException_SetFormat(
            po, PyExc_TypeError,
            NI_MSG_TUPLE_CONCAT,
            tp_bb->tp_name);
        return NULL;
    }
    b = bb;
    _t4 = PsycoTuple_Load(bb);
    if (_t4 < 0 || _t4 > 16) {
        _t5 = psyco_generic_call(
            po, PyTuple_Type.tp_as_sequence->sq_concat,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "vv", a, bb);
        if (_t5 != NULL)
            Psyco_AssertType(po, _t5, &PyTuple_Type);
        return _t5;
    }
    _t6 = _t4 == 0;
    if (_t6) {
        tp_a = Psyco_NeedType(po, a);
        if (tp_a == NULL) {
            return NULL;
        }
        _t6 = tp_a == &PyTuple_Type;
    }
    if (_t6) {
        vinfo_incref(a);
        return a;
    }
    size = (_t1 + _t4);
    if (size == 0) {
        _t7 = tuple_get_empty(po);
        if (_t7 != NULL)
            Psyco_AssertType(po, _t7, &PyTuple_Type);
        return _t7;
    }
    _t8 = psyco_generic_call(po, PyTuple_Type.tp_as_sequence->sq_concat,
                             CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                             "vv", a, bb);
    if (_t8 != NULL)
        Psyco_AssertType(po, _t8, &PyTuple_Type);
    return _t8;
}
#else
static vinfo_t *
tupleconcat(PsycoObject *po, vinfo_t *a, vinfo_t *bb)
{
    int _t1;
    vinfo_t *_t2;
    int _t3;
    int _t4;
    vinfo_t *_t5;
    int _t6;
    vinfo_t *_t7;
    Py_ssize_t _t8;
    vinfo_t *_t9;
    Py_ssize_t dest;
    Py_ssize_t i;
    vinfo_t *np;
    Py_ssize_t size;
    Py_ssize_t src;
    PyTypeObject *tp_a;
    PyTypeObject *tp_bb;
    vinfo_t *v;
    _t1 = PsycoTuple_Load(a);
    if (_t1 < 0 || _t1 > 16) {
        _t2 = psyco_generic_call(
            po, PyTuple_Type.tp_as_sequence->sq_concat,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "vv", a, bb);
        if (_t2 != NULL)
            Psyco_AssertType(po, _t2, &PyTuple_Type);
        return _t2;
    }
    _t3 = _t1 == 0;
    if (_t3) {
        tp_bb = Psyco_NeedType(po, bb);
        if (tp_bb == NULL) {
            return NULL;
        }
        _t3 = tp_bb == &PyTuple_Type;
    }
    if (_t3) {
        vinfo_incref(bb);
        return bb;
    }
    tp_bb = Psyco_NeedType(po, bb);
    if (tp_bb == NULL) {
        return NULL;
    }
    if ((tp_bb->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) == 0) {
        PycException_SetFormat(
            po, PyExc_TypeError,
            NI_MSG_TUPLE_CONCAT,
            tp_bb->tp_name);
        return NULL;
    }
    _t4 = PsycoTuple_Load(bb);
    if (_t4 < 0 || _t4 > 16) {
        _t5 = psyco_generic_call(
            po, PyTuple_Type.tp_as_sequence->sq_concat,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "vv", a, bb);
        if (_t5 != NULL)
            Psyco_AssertType(po, _t5, &PyTuple_Type);
        return _t5;
    }
    _t6 = _t4 == 0;
    if (_t6) {
        tp_a = Psyco_NeedType(po, a);
        if (tp_a == NULL) {
            return NULL;
        }
        _t6 = tp_a == &PyTuple_Type;
    }
    if (_t6) {
        vinfo_incref(a);
        return a;
    }
    if (_t1 > ((PY_SSIZE_T_MAX - _t4))) {
        _t7 = psyco_generic_call(
            po, PyErr_NoMemory,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "");
        if (_t7 != NULL)
            Psyco_AssertType(po, _t7, &PyTuple_Type);
        return _t7;
    }
    size = (_t1 + _t4);
    _t8 = size;
    if (_t8 < 0 || _t8 > CT_TUPLE_LOAD_SIZE_LIMIT) {
        _t9 = psyco_generic_call(
            po, PyTuple_Type.tp_as_sequence->sq_concat,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "vv", a, bb);
        if (_t9 != NULL)
            Psyco_AssertType(po, _t9, &PyTuple_Type);
        return _t9;
    }
    np = PsycoTuple_New(_t8, NULL);
    src = 0;
    dest = 0;
    i = 0;
    for (;;) {
        if (!(i < _t1))
            break;
        v = PsycoTuple_GET_ITEM(a, src + i);
        vinfo_incref(v);
        PsycoTuple_GET_ITEM(np, dest + i) = v;
        i++;
    }
    src = 0;
    dest = (0 + _t1);
    i = 0;
    for (;;) {
        if (!(i < _t4))
            break;
        v = PsycoTuple_GET_ITEM(bb, src + i);
        vinfo_incref(v);
        PsycoTuple_GET_ITEM(np, dest + i) = v;
        i++;
    }
    return np;
}
#endif

static vinfo_t *
tuplecontains(PsycoObject *po, vinfo_t *a, vinfo_t *el)
{
    int _t1;
    int _t2;
    condition_code_t _t3;
    int _t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *cmp;
    Py_ssize_t i;
    PyTypeObject *tp_el;
    _t1 = PsycoTuple_Load(a);
    if (_t1 < 0 || _t1 > 16) {
        return psyco_generic_call(po, PyTuple_Type.tp_as_sequence->sq_contains,
                                  CfCommonIntBoolOrError, "vv", a, el);
    }
    i = 0;
    cmp = vinfo_new(CompileTime_New((long)(0)));
#if NI_HAS_CONTAINS_ITEM_FIRST
    for (_t2 = 0; _t2 < _t1; _t2++) {
        if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(a, _t2)) == NULL) {
            vinfo_decref(cmp, po);
            return NULL;
        }
    }
    tp_el = Psyco_NeedType(po, el);
    if (tp_el == NULL) {
        vinfo_decref(cmp, po);
        return NULL;
    }
#else
    tp_el = Psyco_NeedType(po, el);
    if (tp_el == NULL) {
        vinfo_decref(cmp, po);
        return NULL;
    }
    for (_t2 = 0; _t2 < _t1; _t2++) {
        if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(a, _t2)) == NULL) {
            vinfo_decref(cmp, po);
            return NULL;
        }
    }
#endif
    for (;;) {
        _t3 = integer_non_null(po, cmp);
        if (_t3 == CC_ERROR) {
            vinfo_decref(cmp, po);
            return NULL;
        }
        _t3 = INVERT_CC(_t3);
        _t4 = runtime_condition_t(po, _t3);
        if (!((_t4 && i < _t1)))
            break;
        _t5 =
#if NI_HAS_CONTAINS_ITEM_FIRST
            PsycoObject_RichCompareBool(po, PsycoTuple_GET_ITEM(a, i), el, 2);
#else
            PsycoObject_RichCompareBool(po, el, PsycoTuple_GET_ITEM(a, i), 2);
#endif
        _t6 = _t5;
        vinfo_decref(cmp, po);
        cmp = _t6;
        if (cmp == NULL) {
            return NULL;
        }
        ++i;
    }
    return cmp;
}

static vinfo_t *
tupleitem(PsycoObject *po, vinfo_t *a, vinfo_t *i)
{
    vinfo_t *_t1;
    condition_code_t _t2;
    int _t3;
    vinfo_t *_t4;
    _t1 = psyco_get_field(po, a, FIX_size);
    if (_t1 == NULL) {
        return NULL;
    }
    _t2 = integer_cmp(po, i, _t1, Py_GE | COMPARE_UNSIGNED);
    vinfo_decref(_t1, po);
    if (_t2 == CC_ERROR) {
        return NULL;
    }
    _t3 = runtime_condition_f(po, _t2);
    if (_t3) {
        PycException_SetString(po, PyExc_IndexError,
                               NI_MSG_TUPLE_INDEX_RANGE);
        return NULL;
    }
    _t4 = psyco_get_field_array(po, a, FPYREF(TUPLE_ob_item), i);
    if (_t4 == NULL) {
        return NULL;
    }
    return _t4;
}

static vinfo_t *
tuplelength(PsycoObject *po, vinfo_t *a)
{
    vinfo_t *_t1;
    _t1 = psyco_get_field(po, a, FIX_size);
    if (_t1 == NULL) {
        return NULL;
    }
    return _t1;
}

#if NI_HAS_TUPLE_GET_EMPTY
static vinfo_t *
tuplerepeat(PsycoObject *po, vinfo_t *a, vinfo_t *n)
{
    vinfo_t *_t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    Py_ssize_t n_k;
    Py_ssize_t size;
    PyTypeObject *tp_a;
    if (!is_compiletime(n->source)) {
        _t1 = psyco_generic_call(
            po, PyTuple_Type.tp_as_sequence->sq_repeat,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "vv", a, n);
        if (_t1 != NULL)
            Psyco_AssertType(po, _t1, &PyTuple_Type);
        return _t1;
    }
    n_k = (Py_ssize_t)CompileTime_Get(n->source)->value;
    _t2 = PsycoTuple_Load(a);
    if (_t2 < 0 || _t2 > 16) {
        _t3 =
            psyco_generic_call(po, PyTuple_Type.tp_as_sequence->sq_repeat,
                               CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                               "vl", a, (long)(n_k));
        if (_t3 != NULL)
            Psyco_AssertType(po, _t3, &PyTuple_Type);
        return _t3;
    }
    if ((_t2 == 0 || n_k == 1)) {
        tp_a = Psyco_NeedType(po, a);
        if (tp_a == NULL) {
            return NULL;
        }
        if (tp_a == &PyTuple_Type) {
            vinfo_incref(a);
            return a;
        }
    }
    if ((_t2 == 0 || n_k <= 0)) {
        _t4 = tuple_get_empty(po);
        if (_t4 != NULL)
            Psyco_AssertType(po, _t4, &PyTuple_Type);
        return _t4;
    }
    if (n_k > ((PY_SSIZE_T_MAX / _t2))) {
        _t5 = psyco_generic_call(
            po, PyErr_NoMemory,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "");
        if (_t5 != NULL)
            Psyco_AssertType(po, _t5, &PyTuple_Type);
        return _t5;
    }
    size = (_t2 * n_k);
    _t6 = psyco_generic_call(po, PyTuple_Type.tp_as_sequence->sq_repeat,
                             CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                             "vl", a, (long)(n_k));
    if (_t6 != NULL)
        Psyco_AssertType(po, _t6, &PyTuple_Type);
    return _t6;
}
#else
static vinfo_t *
tuplerepeat(PsycoObject *po, vinfo_t *a, vinfo_t *n)
{
    vinfo_t *_t1;
    vinfo_t *_t10;
    int _t2;
    vinfo_t *_t3;
    Py_ssize_t _t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    Py_ssize_t _t7;
    vinfo_t *_t8;
    Py_ssize_t i;
    Py_ssize_t items;
    Py_ssize_t j;
    Py_ssize_t n_k;
    vinfo_t *np;
    Py_ssize_t p;
    Py_ssize_t size;
    PyTypeObject *tp_a;
    if (!is_compiletime(n->source)) {
        _t1 = psyco_generic_call(
            po, PyTuple_Type.tp_as_sequence->sq_repeat,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "vv", a, n);
        if (_t1 != NULL)
            Psyco_AssertType(po, _t1, &PyTuple_Type);
        return _t1;
    }
    n_k = (Py_ssize_t)CompileTime_Get(n->source)->value;
    if (n_k < 0) {
        n_k = 0;
    }
    _t2 = PsycoTuple_Load(a);
    if (_t2 < 0 || _t2 > 16) {
        _t3 =
            psyco_generic_call(po, PyTuple_Type.tp_as_sequence->sq_repeat,
                               CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                               "vl", a, (long)(n_k));
        if (_t3 != NULL)
            Psyco_AssertType(po, _t3, &PyTuple_Type);
        return _t3;
    }
    if ((_t2 == 0 || n_k == 1)) {
        tp_a = Psyco_NeedType(po, a);
        if (tp_a == NULL) {
            return NULL;
        }
        if (tp_a == &PyTuple_Type) {
            vinfo_incref(a);
            return a;
        }
        if (_t2 == 0) {
            _t4 = 0;
            _t5 = PsycoTuple_New(_t4, NULL);
            return _t5;
        }
    }
    if (n_k > ((PY_SSIZE_T_MAX / _t2))) {
        _t6 = psyco_generic_call(
            po, PyErr_NoMemory,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "");
        if (_t6 != NULL)
            Psyco_AssertType(po, _t6, &PyTuple_Type);
        return _t6;
    }
    size = (_t2 * n_k);
    _t7 = size;
    if (_t7 < 0 || _t7 > CT_TUPLE_LOAD_SIZE_LIMIT) {
        _t8 =
            psyco_generic_call(po, PyTuple_Type.tp_as_sequence->sq_repeat,
                               CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                               "vl", a, (long)(n_k));
        if (_t8 != NULL)
            Psyco_AssertType(po, _t8, &PyTuple_Type);
        return _t8;
    }
    np = PsycoTuple_New(_t7, NULL);
    p = 0;
    items = 0;
    if (n_k > 16) {
        _t10 =
            psyco_generic_call(po, PyTuple_Type.tp_as_sequence->sq_repeat,
                               CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                               "vl", a, (long)(n_k));
        if (_t10 != NULL)
            Psyco_AssertType(po, _t10, &PyTuple_Type);
        vinfo_decref(np, po);
        return _t10;
    }
    i = 0;
    for (;;) {
        if (!(i < n_k))
            break;
        j = 0;
        for (;;) {
            if (!(j < _t2))
                break;
            PsycoTuple_GET_ITEM(np, p) = PsycoTuple_GET_ITEM(a, items + j);
            vinfo_incref(PsycoTuple_GET_ITEM(np, p));
            p++;
            j++;
        }
        i++;
    }
    return np;
}
#endif

static vinfo_t *
tuplerichcompare(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op)
{
    int _t1;
    int _t2;
    int _t3;
    int _t4;
    int _t5;
    vinfo_t *_t6;
    condition_code_t _t7;
    int _t8;
    vinfo_t *_t9;
    Py_ssize_t i;
    vinfo_t *k;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    Py_ssize_t vlen;
    vinfo_t *vt;
    Py_ssize_t wlen;
    vinfo_t *wt;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    _t1 = (tp_v->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) == 0;
    if (!_t1) {
        tp_w = Psyco_NeedType(po, w);
        if (tp_w == NULL) {
            return NULL;
        }
        _t1 = (tp_w->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) == 0;
    }
    if (_t1) {
        return psyco_vi_NotImplemented();
    }
    vt = v;
    wt = w;
    _t2 = PsycoTuple_Load(v);
    if (_t2 < 0 || _t2 > 16) {
        return psyco_generic_call(po, PyTuple_Type.tp_richcompare,
                                  CfCommonCheckNotImplemented, "vvl", v, w,
                                  (long)(op));
    }
    vlen = _t2;
    _t3 = PsycoTuple_Load(w);
    if (_t3 < 0 || _t3 > 16) {
        return psyco_generic_call(po, PyTuple_Type.tp_richcompare,
                                  CfCommonCheckNotImplemented, "vvl", v, w,
                                  (long)(op));
    }
    wlen = _t3;
    i = 0;
    for (_t4 = 0; _t4 < _t2; _t4++) {
        if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(v, _t4)) == NULL) {
            return NULL;
        }
    }
    for (_t5 = 0; _t5 < _t3; _t5++) {
        if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(w, _t5)) == NULL) {
            return NULL;
        }
    }
    for (;;) {
        if (!((i < vlen && i < wlen)))
            break;
        _t6 = PsycoObject_RichCompareBool(po, PsycoTuple_GET_ITEM(v, i),
                                          PsycoTuple_GET_ITEM(w, i), 2);
        k = _t6;
        if (k == NULL) {
            return NULL;
        }
        _t7 = integer_non_null(po, k);
        if (_t7 == CC_ERROR) {
            vinfo_decref(k, po);
            return NULL;
        }
        _t8 = runtime_condition_t(po, _t7);
        if (!(_t8)) {
            vinfo_decref(k, po);
            break;
        }
        else {
            vinfo_decref(k, po);
        }
        i++;
    }
    if ((i >= vlen || i >= wlen)) {
        switch (op) {
            case 2:
                if (vlen == wlen) {
                    return psyco_vi_Py_True();
                }
                return psyco_vi_Py_False();
            case 3:
                if (vlen != wlen) {
                    return psyco_vi_Py_True();
                }
                return psyco_vi_Py_False();
            case 0:
                if (vlen < wlen) {
                    return psyco_vi_Py_True();
                }
                return psyco_vi_Py_False();
            case 4:
                if (vlen > wlen) {
                    return psyco_vi_Py_True();
                }
                return psyco_vi_Py_False();
            case 1:
                if (vlen <= wlen) {
                    return psyco_vi_Py_True();
                }
                return psyco_vi_Py_False();
            case 5:
                if (vlen >= wlen) {
                    return psyco_vi_Py_True();
                }
                return psyco_vi_Py_False();
            default:
                abort();
        }
    }
    if (op == 2) {
        return psyco_vi_Py_False();
    }
    if (op == 3) {
        return psyco_vi_Py_True();
    }
    _t9 = PsycoObject_RichCompare(po, PsycoTuple_GET_ITEM(v, i),
                                  PsycoTuple_GET_ITEM(w, i), op);
    return _t9;
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
#pragma GCC diagnostic pop


INITIALIZATIONFN
void psy_tupleobject_init(void)
{
	PyMappingMethods *mm;
	gen_tupleobject_init();

	mm = PyTuple_Type.tp_as_mapping;
	if (mm) {  /* Python >= 2.3 */
		Psyco_DefineMeta(mm->mp_subscript, psyco_generic_subscript);
	}

	gen_virtual_tuple_init();
}
