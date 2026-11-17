#include "pobject.h"
#include "pabstract.h"
#include "plongobject.h"

#define BYTES_sval UNSIGNED_ARRAY(unsigned char, offsetof(PyBytesObject, ob_sval))

static binaryfunc bytes_subscript;

/* bytes_subscript: an int index gives the byte as a virtual int; anything
   else (slices) in C */
static vinfo_t *pbytes_subscript(PsycoObject *po, vinfo_t *v, vinfo_t *key) {
  vinfo_t *i, *vlen, *r;
  condition_code_t cc;
  PyTypeObject *ktp = Psyco_NeedType(po, key);
  if (ktp == NULL)
    return NULL;
  if (!PyType_IsSubtype(ktp, &PyLong_Type))
    return psyco_generic_call(po, bytes_subscript, CfCommonNewRefPyObject,
                              "vv", v, key);
  i = PsycoLong_AsIndex(po, key);
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
