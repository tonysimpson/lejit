/*
 * Virtual ranges and range iterators, so that 'for i in range(...)' is a
 * register loop, after Psyco's virtual xrange.
 *
 * A virtual range holds start, stop and step as machine words, at indices
 * past the real rangeobject's fields (which are PyObject *), so that any
 * read of a real field computes the object first. range(...) makes one only
 * when every argument is an exact int that is one digit at run time
 * (PsycoLong_MediumValue) and the step is non-zero; anything else is the
 * real call. GET_ITER on a virtual range gives a virtual range iterator
 * with rangeiterobject's layout (all words: index, start, step and len with
 * index mutable, or start, step and len with start and len mutable), and
 * the iternext meta works on virtual and real fast iterators alike. The long
 * iterator (longrangeiterobject, a different type) is never specialised.
 */
#include "prangeobject.h"
#include "plongobject.h"

/* rangeobject and rangeiterobject, private to Objects/rangeobject.c, are
   copied from the source into psyco.h */
#define RANGE_start CPY_FIELD(rangeobject, start, OB_type)
#define RANGE_stop CPY_FIELD(rangeobject, stop, RANGE_start)
#define RANGE_step CPY_FIELD(rangeobject, step, RANGE_stop)
#define RANGE_length CPY_FIELD(rangeobject, length, RANGE_step)
/* virtual ranges only: words, not C fields */
#define iVRANGE_START FIELDS_TOTAL(RANGE_length)
#define iVRANGE_STOP (iVRANGE_START + 1)
#define iVRANGE_STEP (iVRANGE_START + 2)
#define VRANGE_TOTAL (iVRANGE_START + 3)

#if LE_HAS_RANGE_ITER_INDEX
#define RANGEITER_index FMUT(CPY_FIELD(rangeiterobject, index, OB_type))
#define RANGEITER_start CPY_FIELD(rangeiterobject, start, RANGEITER_index)
#define RANGEITER_step CPY_FIELD(rangeiterobject, step, RANGEITER_start)
#define RANGEITER_len CPY_FIELD(rangeiterobject, len, RANGEITER_step)
#define iRANGEITER_INDEX FIELD_INDEX(RANGEITER_index)
#else
#define RANGEITER_start FMUT(CPY_FIELD(rangeiterobject, start, OB_type))
#define RANGEITER_step CPY_FIELD(rangeiterobject, step, RANGEITER_start)
#define RANGEITER_len FMUT(CPY_FIELD(rangeiterobject, len, RANGEITER_step))
#endif
#define iRANGEITER_START FIELD_INDEX(RANGEITER_start)
#define iRANGEITER_STEP FIELD_INDEX(RANGEITER_step)
#define iRANGEITER_LEN FIELD_INDEX(RANGEITER_len)
#define RANGEITER_TOTAL FIELDS_TOTAL(RANGEITER_len)

static source_virtual_t psyco_computed_range;
static source_virtual_t psyco_computed_rangeiter;
static getiterfunc range_iter;
/* whether the layouts copied match the types', so that ranges are virtual */
static int layouts_match;

/* rangeobject.c:get_len_of_range; the arguments are one-digit ints, so the
   result fits a long */
static long
cimpl_range_len(long lo, long hi, long step)
{
    if (step > 0 && lo < hi)
        return 1UL + (hi - 1UL - lo) / step;
    else if (step < 0 && lo > hi)
        return 1UL + (lo - 1UL - hi) / (0UL - step);
    return 0;
}

static PyObject *
cimpl_range_new(long start, long stop, long step)
{
    return PyObject_CallFunction((PyObject *)&PyRange_Type, "lll", start, stop,
                                 step);
}

#if LE_HAS_RANGE_ITER_INDEX
/* rangeobject.c:fast_range_iter with the index already advanced */
static PyObject *
cimpl_rangeiter_new(long index, long start, long step, long len)
{
    rangeiterobject *it = PyObject_New(rangeiterobject, &PyRangeIter_Type);
    if (it == NULL)
        return NULL;
    it->index = index;
    it->start = start;
    it->step = step;
    it->len = len;
    return (PyObject *)it;
}
#else
/* rangeobject.c:fast_range_iter with start and len already moved on */
static PyObject *
cimpl_rangeiter_new(long start, long step, long len)
{
    rangeiterobject *it = PyObject_New(rangeiterobject, &PyRangeIter_Type);
    if (it == NULL)
        return NULL;
    it->start = start;
    it->step = step;
    it->len = len;
    return (PyObject *)it;
}
#endif

static bool
compute_range(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *newobj = psyco_generic_call(
        po, cimpl_range_new, CfCommonNewRefPyObject, "vvv",
        v->array->items[iVRANGE_START], v->array->items[iVRANGE_STOP],
        v->array->items[iVRANGE_STEP]);
    if (newobj == NULL)
        return false;
    /* the words are not fields of the real object */
    vinfo_array_shrink(po, v, iVRANGE_START);
    vinfo_move(po, v, newobj);
    return true;
}

static bool
compute_rangeiter(PsycoObject *po, vinfo_t *v)
{
#if LE_HAS_RANGE_ITER_INDEX
    vinfo_t *newobj = psyco_generic_call(
        po, cimpl_rangeiter_new, CfCommonNewRefPyObject, "vvvv",
        v->array->items[iRANGEITER_INDEX], v->array->items[iRANGEITER_START],
        v->array->items[iRANGEITER_STEP], v->array->items[iRANGEITER_LEN]);
    if (newobj == NULL)
        return false;
    /* anyone may advance the real iterator from now on */
    psyco_forget_field(po, v, RANGEITER_index);
#else
    vinfo_t *newobj = psyco_generic_call(
        po, cimpl_rangeiter_new, CfCommonNewRefPyObject, "vvv",
        v->array->items[iRANGEITER_START], v->array->items[iRANGEITER_STEP],
        v->array->items[iRANGEITER_LEN]);
    if (newobj == NULL)
        return false;
    /* anyone may advance the real iterator from now on */
    psyco_forget_field(po, v, RANGEITER_start);
    psyco_forget_field(po, v, RANGEITER_len);
#endif
    vinfo_move(po, v, newobj);
    return true;
}

DEFINEFN
int
PsycoRange_New(PsycoObject *po, vinfo_t **args, int n, vinfo_t **result)
{
    vinfo_t *w[3] = {NULL, NULL, NULL};
    vinfo_t *r;
    int i, k;
    if (!layouts_match || n < 1 || n > 3)
        return 0;
    for (i = 0; i < n; i++) {
        PyTypeObject *tp = Psyco_NeedType(po, args[i]);
        if (tp == NULL)
            goto error;
        if (tp != &PyLong_Type)
            goto fallback;
        k = PsycoLong_MediumValue(po, args[i], &w[i]);
        if (k <= 0) {
            if (k < 0)
                goto error;
            goto fallback;
        }
    }
    if (n == 3) {
        /* step 0 raises ValueError: the real call does that */
        condition_code_t cc = integer_cmp_i(po, w[2], 0, Py_NE);
        if (cc == CC_ERROR)
            goto error;
        if (!runtime_condition_t(po, cc))
            goto fallback;
    }
    r = vinfo_new(VirtualTime_New(&psyco_computed_range));
    r->array = array_new(VRANGE_TOTAL);
    r->array->items[iOB_TYPE] =
        vinfo_new(CompileTime_New((long)&PyRange_Type));
    if (n == 1) {
        r->array->items[iVRANGE_START] = psyco_vi_Zero();
        r->array->items[iVRANGE_STOP] = w[0];
    }
    else {
        r->array->items[iVRANGE_START] = w[0];
        r->array->items[iVRANGE_STOP] = w[1];
    }
    r->array->items[iVRANGE_STEP] = n == 3 ? w[2] : psyco_vi_One();
    *result = r;
    return 1;

fallback:
    for (i = 0; i < 3; i++) vinfo_xdecref(w[i], po);
    return 0;
error:
    for (i = 0; i < 3; i++) vinfo_xdecref(w[i], po);
    return -1;
}

static vinfo_t *
prange_iter(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *it, *len;
#if LE_HAS_RANGE_ITER_INDEX
    vinfo_t *zero;
#endif
    vinfo_t **items;
    if (v->source != VirtualTime_New(&psyco_computed_range))
        return psyco_generic_call(po, range_iter, CfCommonNewRefPyObject, "v",
                                  v);
    items = v->array->items;
    len = psyco_generic_call(po, cimpl_range_len, CfPure | CfReturnTypeLong,
                             "vvv", items[iVRANGE_START], items[iVRANGE_STOP],
                             items[iVRANGE_STEP]);
    if (len == NULL)
        return NULL;
    it = vinfo_new(VirtualTime_New(&psyco_computed_rangeiter));
    it->array = array_new(RANGEITER_TOTAL);
    it->array->items[iOB_TYPE] =
        vinfo_new(CompileTime_New((long)&PyRangeIter_Type));
    /* run-time at once, so the loop counter isn't specialised on (as for
       the virtual seq-iterator) */
#if LE_HAS_RANGE_ITER_INDEX
    zero = psyco_vi_Zero();
    it->array->items[iRANGEITER_INDEX] = make_runtime_copy(po, zero);
    vinfo_decref(zero, po);
    vinfo_incref(items[iVRANGE_START]);
    it->array->items[iRANGEITER_START] = items[iVRANGE_START];
    it->array->items[iRANGEITER_LEN] = len;
#else
    it->array->items[iRANGEITER_START] =
        make_runtime_copy(po, items[iVRANGE_START]);
    it->array->items[iRANGEITER_LEN] = make_runtime_copy(po, len);
    vinfo_decref(len, po);
#endif
    vinfo_incref(items[iVRANGE_STEP]);
    it->array->items[iRANGEITER_STEP] = items[iVRANGE_STEP];
    return it;
}

/* rangeiter_next */
#if LE_HAS_RANGE_ITER_INDEX
static vinfo_t *
prangeiter_next(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *index, *len, *start, *step, *delta, *value, *next;
    condition_code_t cc;
    if ((len = psyco_get_const(po, v, RANGEITER_len)) == NULL ||
        (start = psyco_get_const(po, v, RANGEITER_start)) == NULL ||
        (step = psyco_get_const(po, v, RANGEITER_step)) == NULL)
        return NULL;
    index = psyco_get_field(po, v, RANGEITER_index);
    if (index == NULL)
        return NULL;
    cc = integer_cmp(po, index, len, Py_LT);
    if (cc == CC_ERROR)
        goto error;
    if (!runtime_condition_t(po, cc)) {
        /* exhausted: FOR_ITER and PsycoIter_Next's callers expect the
           exception that a NULL from tp_iternext stands for */
        vinfo_decref(index, po);
        PycException_SetVInfo(po, PyExc_StopIteration, psyco_vi_None());
        return NULL;
    }
    /* start + (unsigned long)index * step: the machine word wraps as the
       unsigned C does */
    delta = integer_mul(po, index, step, false);
    if (delta == NULL)
        goto error;
    value = integer_add(po, start, delta, false);
    vinfo_decref(delta, po);
    if (value == NULL)
        goto error;
    next = integer_add_i(po, index, 1, false);
    if (next == NULL || !psyco_put_field(po, v, RANGEITER_index, next)) {
        vinfo_xdecref(next, po);
        vinfo_decref(value, po);
        goto error;
    }
    vinfo_decref(next, po);
    vinfo_decref(index, po);
    return PsycoLong_FROM_WORD(value);
error:
    vinfo_decref(index, po);
    return NULL;
}
#else
static vinfo_t *
prangeiter_next(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *start, *step, *len, *next;
    condition_code_t cc;
    bool ok;
    if ((step = psyco_get_const(po, v, RANGEITER_step)) == NULL)
        return NULL;
    len = psyco_get_field(po, v, RANGEITER_len);
    if (len == NULL)
        return NULL;
    cc = integer_cmp_i(po, len, 0, Py_GT);
    if (cc == CC_ERROR) {
        vinfo_decref(len, po);
        return NULL;
    }
    if (!runtime_condition_t(po, cc)) {
        /* exhausted: FOR_ITER and PsycoIter_Next's callers expect the
           exception that a NULL from tp_iternext stands for */
        vinfo_decref(len, po);
        PycException_SetVInfo(po, PyExc_StopIteration, psyco_vi_None());
        return NULL;
    }
    start = psyco_get_field(po, v, RANGEITER_start);
    if (start == NULL) {
        vinfo_decref(len, po);
        return NULL;
    }
    /* the machine word wraps as the C does */
    next = integer_add(po, start, step, false);
    ok = next != NULL && psyco_put_field(po, v, RANGEITER_start, next);
    vinfo_xdecref(next, po);
    next = ok ? integer_add_i(po, len, -1, false) : NULL;
    ok = next != NULL && psyco_put_field(po, v, RANGEITER_len, next);
    vinfo_xdecref(next, po);
    vinfo_decref(len, po);
    if (!ok) {
        vinfo_decref(start, po);
        return NULL;
    }
    return PsycoLong_FROM_WORD(start);
}
#endif

INITIALIZATIONFN
void
psy_rangeobject_init(void)
{
    /* the layouts copied from the source (psyco.h) must be the types' */
    if (PyRange_Type.tp_basicsize != sizeof(rangeobject) ||
        PyRangeIter_Type.tp_basicsize != sizeof(rangeiterobject))
        return;
    layouts_match = 1;
    range_iter = PyRange_Type.tp_iter;
    Psyco_DefineMeta(range_iter, prange_iter);
    Psyco_DefineMeta(PyRangeIter_Type.tp_iternext, prangeiter_next);
    /* ranges are immutable, iterators not: both are forced across calls,
       so that f(r) is r holds */
    INIT_SVIRTUAL_NOCALL(psyco_computed_range, compute_range, 4);
    INIT_SVIRTUAL_NOCALL(psyco_computed_rangeiter, compute_rangeiter, 1);
}
