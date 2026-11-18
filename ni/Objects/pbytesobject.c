#include "pobject.h"
#include "pabstract.h"
#include "plongobject.h"
#include "psliceobject.h"

#define BYTES_sval CPY_ARRAY(PyBytesObject, ob_sval)

static binaryfunc bytes_subscript;

/* bytes_subscript's slice case with step 1, after PySlice_Unpack */
static PyObject *cimpl_bytes_slice(PyObject *self, Py_ssize_t start,
                                   Py_ssize_t stop) {
  Py_ssize_t slicelength =
      PySlice_AdjustIndices(PyBytes_GET_SIZE(self), &start, &stop, 1);
  if (slicelength <= 0)
    return PyBytes_FromStringAndSize("", 0);
  if (start == 0 && slicelength == PyBytes_GET_SIZE(self) &&
      PyBytes_CheckExact(self)) {
    Py_INCREF(self);
    return self;
  }
  return PyBytes_FromStringAndSize(PyBytes_AS_STRING(self) + start,
                                   slicelength);
}

/* bytes_subscript: an int index gives the byte as a virtual int, a virtual
   slice is cimpl_bytes_slice (folding for a constant exact bytes); anything
   else in C */
static vinfo_t *pbytes_subscript(PsycoObject *po, vinfo_t *v, vinfo_t *key) {
  vinfo_t *i, *j, *vlen, *r;
  condition_code_t cc;
  PyTypeObject *ktp = Psyco_NeedType(po, key);
  if (ktp == NULL)
    return NULL;
  if (ktp == &PySlice_Type) {
    switch (PsycoSlice_Unpack(po, key, &i, &j)) {
    case -1:
      return NULL;
    case 1:
      r = psyco_generic_call(
          po, cimpl_bytes_slice,
          CfCommonNewRefPyObject |
              (Psyco_KnownType(v) == &PyBytes_Type ? CfPure : 0),
          "vvv", v, i, j);
      vinfo_decref(j, po);
      vinfo_decref(i, po);
      if (r != NULL && !is_compiletime(r->source))
        Psyco_AssertType(po, r, &PyBytes_Type);
      return r;
    }
  }
  if (!PyType_IsSubtype(ktp, &PyLong_Type))
    return psyco_generic_call(po, bytes_subscript, CfCommonNewRefPyObject,
                              "vv", v, key);
  i = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
  if (i == NULL)
    return NULL;
  vlen = psyco_get_const(po, v, FIX_size);
  if (vlen == NULL)
    goto fail;
  cc = integer_cmp_i(po, i, 0, Py_LT);
  if (cc == CC_ERROR)
    goto fail;
  if (runtime_condition_f(po, cc)) {
    vinfo_t *j = integer_add(po, i, vlen, false);
    vinfo_decref(i, po);
    if (j == NULL)
      return NULL;
    i = j;
  } else
    assert_nonneg(i);
  cc = integer_cmp(po, i, vlen, Py_GE | COMPARE_UNSIGNED);
  if (cc == CC_ERROR)
    goto fail;
  if (runtime_condition_f(po, cc)) {
    PycException_SetString(po, PyExc_IndexError, "index out of range");
    goto fail;
  }
  r = psyco_get_field_array(po, v, BYTES_sval, i);
  vinfo_decref(i, po);
  if (r == NULL)
    return NULL;
  return PsycoLong_FROM_LONG(po, r);

fail:
  vinfo_decref(i, po);
  return NULL;
}

INITIALIZATIONFN
void psy_bytesobject_init(void) {
  bytes_subscript = PyBytes_Type.tp_as_mapping->mp_subscript;
  Psyco_DefineMeta(PyBytes_Type.tp_as_sequence->sq_length,
                   psyco_generic_immut_ob_size);
  Psyco_DefineMeta(bytes_subscript, pbytes_subscript);
}
