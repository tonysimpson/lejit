/*
 * Virtual slices, so that x[i:j] on str, bytes and list is one C call with
 * the bounds as words, instead of PySlice_New, the type's mp_subscript
 * unpacking it (PySlice_Unpack: _PyEval_SliceIndex on each bound, boxing
 * a virtual int first) and slice_dealloc.
 *
 * BUILD_SLICE always makes one. Its start, stop and step sit at the real
 * PySliceObject's field indices; the generated compute_slice emits
 * PySlice_New when the slice escapes (any meta that doesn't know it, or a
 * call) and then drops them (TYPES 'forced'), so nothing reads them as
 * fields of the real object; with constant bounds the slice is a constant. A type's meta
 * calls PsycoSlice_Unpack and, if that gives the bounds, its own C helper,
 * which does PySlice_AdjustIndices and the type's step-1 case exactly as
 * its mp_subscript does. Only step None is handled, and only bounds that
 * are None or ints: anything else (__index__, a step) is the real slice.
 */
#include "psliceobject.h"
#include "plongobject.h"

static source_virtual_t psyco_computed_slice;

/* SLICE_* descriptors: virtual_fields_gen.h; PsycoSlice_New, compute_slice
   and direct_compute_slice, generated from PySlice_New and
   policy_table.TYPES (D80) */
#include "psliceobject_virtual_gen.h"

static bool is_none(vinfo_t *v) {
  return is_compiletime(v->source) &&
         (PyObject *)CompileTime_Get(v->source)->value == Py_None;
}

/* _PyEval_SliceIndex for a bound that is None ('dflt') or an int */
static int slice_index(PsycoObject *po, vinfo_t *v, Py_ssize_t dflt,
                       vinfo_t **pw) {
  PyTypeObject *tp;
  if (is_compiletime(v->source)) {
    PyObject *o = (PyObject *)CompileTime_Get(v->source)->value;
    if (o == Py_None) {
      *pw = vinfo_new(CompileTime_New(dflt));
      return 1;
    }
    if (!PyLong_Check(o))
      return 0;
    /* an int can't fail; out of range it clips, as with a NULL exception */
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
int PsycoSlice_Unpack(PsycoObject *po, vinfo_t *slice, vinfo_t **pstart,
                      vinfo_t **pstop) {
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

INITIALIZATIONFN
void psy_sliceobject_init(void) {
  /* immutable, but forced across calls like ranges (identity, C17):
     policy_table.TYPES' weights */
  gen_virtual_slice_init();
}
