/*
 * Virtual slices, so that x[i:j] on str, bytes and list is one C call with
 * the bounds as words, instead of PySlice_New, the type's mp_subscript
 * unpacking it (PySlice_Unpack: _PyEval_SliceIndex on each bound, boxing
 * a virtual int first) and slice_dealloc.
 *
 * BUILD_SLICE always makes one. Its start, stop and step sit at the real
 * PySliceObject's field indices; compute_slice emits
 * PySlice_New when the slice escapes (any meta that doesn't know it, or a
 * call) and then drops them (TYPES 'forced'), so nothing reads them as
 * fields of the real object; with constant bounds the slice is a constant. A
 * type's meta calls PsycoSlice_Unpack and, if that gives the bounds, its own C
 * helper, which does PySlice_AdjustIndices and the type's step-1 case exactly
 * as its mp_subscript does. Only step None is handled (and -1 for a list's
 * pieces, plistobject.c), and only bounds that are None or ints: anything
 * else (__index__, another step) is the real slice. A slice made from a
 * slice constant (PsycoSlice_FromConstant) keeps that object one past the
 * fields, and is that object when forced, as CPython uses the constant.
 */
#include "psliceobject.h"
#include "plongobject.h"
#include "../initialize.h"

static source_virtual_t psyco_computed_slice;

#define iSLICE_CONSTANT SLICE_TOTAL

/* the slice constant v was made from, or NULL */
static PyObject *
slice_constant(vinfo_t *v)
{
    if (v->array->count <= iSLICE_CONSTANT)
        return NULL;
    return (PyObject *)CompileTime_Get(
               v->array->items[iSLICE_CONSTANT]->source)
        ->value;
}

/* SLICE_* descriptors: pobject.h; PsycoSlice_New, compute_slice and
   direct_compute_slice: a virtual PySlice_New */
/* PySlice_New(start, stop, step) on raw values */
static PyObject *
direct_compute_slice(vinfo_t *v, char *data)
{
    PyObject *o = slice_constant(v);
    PyObject *start, *stop, *step;
    if (o != NULL) {
        Py_INCREF(o);
        return o;
    }
    start = direct_xobj_vinfo(v->array->items[iSLICE_START], data);
    stop = direct_xobj_vinfo(v->array->items[iSLICE_STOP], data);
    step = direct_xobj_vinfo(v->array->items[iSLICE_STEP], data);
    if (start != NULL && stop != NULL && step != NULL)
        o = PySlice_New(start, stop, step);
    Py_XDECREF(step);
    Py_XDECREF(stop);
    Py_XDECREF(start);
    return o;
}

/* forced: the slice constant it was made from; a constant if every field is
   one and CPython would share the object (rule allocator_pure); else
   PySlice_New at run time */
static bool
compute_slice(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *newobj = NULL;
    PyObject *constant = slice_constant(v);
    if (constant != NULL)
        newobj = vinfo_new(CompileTime_New((long)constant));
    else if (is_compiletime(v->array->items[iSLICE_START]->source) &&
             is_compiletime(v->array->items[iSLICE_STOP]->source) &&
             is_compiletime(v->array->items[iSLICE_STEP]->source)) {
        PyObject *o = direct_compute_slice(v, NULL);
        if (o == NULL)
            OUT_OF_MEMORY();
        newobj = psyco_shared_constant(o);
    }
    if (newobj == NULL) {
        newobj = psyco_generic_call(
            po, PySlice_New, CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
            "vvv", v->array->items[iSLICE_START], v->array->items[iSLICE_STOP],
            v->array->items[iSLICE_STEP]);
        if (newobj == NULL)
            return false;
        vinfo_array_shrink(po, v, iSLICE_START);
    }
    vinfo_move(po, v, newobj);
    return true;
}

/* a virtual slice: PySlice_New's arguments, each a new reference; NULL is None
 * for start, stop, step */
DEFINEFN
vinfo_t *
PsycoSlice_New(vinfo_t *start, vinfo_t *stop, vinfo_t *step)
{
    vinfo_t *v = vinfo_new(VirtualTime_New(&psyco_computed_slice));
    v->array = array_new(SLICE_TOTAL);
    v->array->items[iOB_TYPE] =
        vinfo_new(CompileTime_New((long)&PySlice_Type));
    if (start == NULL)
        start = psyco_vi_None();
    else
        vinfo_incref(start);
    v->array->items[iSLICE_START] = start;
    if (stop == NULL)
        stop = psyco_vi_None();
    else
        vinfo_incref(stop);
    v->array->items[iSLICE_STOP] = stop;
    if (step == NULL)
        step = psyco_vi_None();
    else
        vinfo_incref(step);
    v->array->items[iSLICE_STEP] = step;
    return v;
}

/* a virtual slice of a slice constant's start, stop and step, which is the
   constant itself when forced; 'slice' is borrowed and must outlive the
   compiled code, as a code object's constants do */
DEFINEFN
vinfo_t *
PsycoSlice_FromConstant(PyObject *slice)
{
    PySliceObject *s = (PySliceObject *)slice;
    vinfo_t *start = vinfo_new(CompileTime_New((long)s->start));
    vinfo_t *stop = vinfo_new(CompileTime_New((long)s->stop));
    vinfo_t *step = vinfo_new(CompileTime_New((long)s->step));
    vinfo_t *v = PsycoSlice_New(start, stop, step);
    vinfo_decref(step, NULL);
    vinfo_decref(stop, NULL);
    vinfo_decref(start, NULL);
    v->array = array_grow1(v->array, iSLICE_CONSTANT + 1);
    v->array->items[iSLICE_CONSTANT] = vinfo_new(CompileTime_New((long)slice));
    return v;
}

/* the force and direct functions, with their weights */
static void
gen_virtual_slice_init(void)
{
    INIT_SVIRTUAL(
        psyco_computed_slice, compute_slice, direct_compute_slice,
        (1 << iSLICE_START) | (1 << iSLICE_STOP) | (1 << iSLICE_STEP), 4,
        NW_FORCE);
}

static bool
is_none(vinfo_t *v)
{
    return is_compiletime(v->source) &&
           (PyObject *)CompileTime_Get(v->source)->value == Py_None;
}

/* _PyEval_SliceIndex for a bound that is None ('dflt') or an int */
static int
slice_index(PsycoObject *po, vinfo_t *v, Py_ssize_t dflt, vinfo_t **pw)
{
    PyTypeObject *tp;
    if (is_compiletime(v->source)) {
        PyObject *o = (PyObject *)CompileTime_Get(v->source)->value;
        if (o == Py_None) {
            *pw = vinfo_new(CompileTime_New(dflt));
            return 1;
        }
        if (!PyLong_Check(o))
            return 0;
        /* an int can't fail; out of range it clips, as with a NULL exception
         */
        *pw = vinfo_new(CompileTime_New(PyNumber_AsSsize_t(o, NULL)));
        return 1;
    }
    if (PsycoLong_IsVirtual(v)) {
        *pw = v->array->items[iLONG_VALUE];
        vinfo_incref(*pw);
        return 1;
    }
    tp = Psyco_NeedType(po, v);
    if (tp == NULL)
        return -1;
    if (tp == Py_None->ob_type) {
        *pw = vinfo_new(CompileTime_New(dflt));
        return 1;
    }
    if (!PyType_TypeCheck(tp, &PyLong_Type))
        return 0;
    switch (PsycoLong_MediumValue(po, v, pw)) {
        case 1:
            return 1;
        case -1:
            return -1;
    }
    /* more than one digit, on a respawned branch */
    *pw = psyco_generic_call(po, PyNumber_AsSsize_t,
                             CfReturnTypeNative | CfPyErrCheckMinus1, "vl", v,
                             (long)NULL);
    return *pw != NULL ? 1 : -1;
}

DEFINEFN
int
PsycoSlice_Unpack(PsycoObject *po, vinfo_t *slice, vinfo_t **pstart,
                  vinfo_t **pstop)
{
    int k;
    if (slice->source != VirtualTime_New(&psyco_computed_slice) ||
        !is_none(slice->array->items[iSLICE_STEP]))
        return 0;
    k = slice_index(po, slice->array->items[iSLICE_START], 0, pstart);
    if (k <= 0)
        return k;
    k = slice_index(po, slice->array->items[iSLICE_STOP], PY_SSIZE_T_MAX,
                    pstop);
    if (k <= 0)
        vinfo_decref(*pstart, po);
    return k;
}

/* the same, only for bounds that are None or exact ints, and also for a
   step that is the constant -1, with PySlice_Unpack's bounds for a negative
   step when they are None; *pstep is 1 or -1 */
DEFINEFN
int
PsycoSlice_UnpackStep(PsycoObject *po, vinfo_t *slice, vinfo_t **pstart,
                      vinfo_t **pstop, int *pstep)
{
    vinfo_t *step;
    PyObject *o;
    int k, overflow;
    if (slice->source != VirtualTime_New(&psyco_computed_slice))
        return 0;
    for (k = iSLICE_START; k <= iSLICE_STOP; k++)
        if (!is_none(slice->array->items[k]) &&
            Psyco_KnownType(slice->array->items[k]) != &PyLong_Type)
            return 0;
    step = slice->array->items[iSLICE_STEP];
    if (is_none(step)) {
        *pstep = 1;
        return PsycoSlice_Unpack(po, slice, pstart, pstop);
    }
    if (!is_compiletime(step->source))
        return 0;
    o = (PyObject *)CompileTime_Get(step->source)->value;
    if (!PyLong_CheckExact(o) ||
        PyLong_AsLongAndOverflow(o, &overflow) != -1 || overflow)
        return 0;
    *pstep = -1;
    k = slice_index(po, slice->array->items[iSLICE_START], PY_SSIZE_T_MAX,
                    pstart);
    if (k <= 0)
        return k;
    k = slice_index(po, slice->array->items[iSLICE_STOP], PY_SSIZE_T_MIN,
                    pstop);
    if (k <= 0)
        vinfo_decref(*pstart, po);
    return k;
}

INITIALIZATIONFN
void
psy_sliceobject_init(void)
{
    /* immutable, but forced across calls like ranges, for identity:
       their weights */
    gen_virtual_slice_init();
}
