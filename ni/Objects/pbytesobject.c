#include "pobject.h"
#include "pabstract.h"
#include "plongobject.h"
#include "psliceobject.h"
#include "pmethodobject.h"
#include "ptupleobject.h"

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

/* the bytes slots translated from bytesobject.c (sq_length) */
/* the C functions, for the fallbacks: set by the registration */
static PyCFunction cfunc_bytes_lstrip;
static PyCFunction cfunc_bytes_rstrip;
static PyCFunction cfunc_bytes_strip;

/* run-time copies: where a heap type's slot changed, or where the meta has no compile-time form, CPython's algorithm resumes in these */
static PyObject *
cimpl_do_xstrip(PyBytesObject *self, int striptype, PyObject *sepobj)
{
    Py_buffer vsep;
    const char *s = PyBytes_AS_STRING(self);
    Py_ssize_t len = PyBytes_GET_SIZE(self);
    char *sep;
    Py_ssize_t seplen;
    Py_ssize_t i, j;
    if (PyObject_GetBuffer(sepobj, &vsep, PyBUF_SIMPLE) != 0)
        return NULL;
    sep = vsep.buf;
    seplen = vsep.len;
    i = 0;
    if (striptype != 1) {
        while (i < len && memchr(sep, Py_CHARMASK(s[i]), seplen)) {
            i++;
        }
    }
    j = len;
    if (striptype != 0) {
        do {
            j--;
        } while (j >= i && memchr(sep, Py_CHARMASK(s[j]), seplen));
        j++;
    }
    PyBuffer_Release(&vsep);
    if (i == 0 && j == len && PyBytes_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject *)self;
    }
    else
        return PyBytes_FromStringAndSize(s + i, j - i);
}

static PyObject *
cimpl_do_strip(PyBytesObject *self, int striptype)
{
    const char *s = PyBytes_AS_STRING(self);
    Py_ssize_t len = PyBytes_GET_SIZE(self), i, j;
    i = 0;
    if (striptype != 1) {
        while (i < len && Py_ISSPACE(s[i])) {
            i++;
        }
    }
    j = len;
    if (striptype != 0) {
        do {
            j--;
        } while (j >= i && Py_ISSPACE(s[j]));
        j++;
    }
    if (i == 0 && j == len && PyBytes_CheckExact(self)) {
        Py_INCREF(self);
        return (PyObject *)self;
    }
    else
        return PyBytes_FromStringAndSize(s + i, j - i);
}

static PyObject *
cimpl_do_argstrip(PyBytesObject *self, int striptype, PyObject *bytes)
{
#if NI_HAS_BYTES_STRIP_NONE_DEFAULT
    if (bytes != Py_None) {
        return cimpl_do_xstrip(self, striptype, bytes);
    }
#else
    if (bytes != NULL && bytes != Py_None) {
        return cimpl_do_xstrip(self, striptype, bytes);
    }
#endif
    return cimpl_do_strip(self, striptype);
}

static vinfo_t *
bytes_length(PsycoObject *po, vinfo_t *);
static vinfo_t *
do_argstrip(PsycoObject *po, vinfo_t *, int, vinfo_t *);
static vinfo_t *
bytes_lstrip_impl(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
bytes_lstrip(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
bytes_rstrip_impl(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
bytes_rstrip(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
bytes_strip_impl(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
bytes_strip(PsycoObject *po, vinfo_t *, vinfo_t *);

static vinfo_t *
bytes_length(PsycoObject *po, vinfo_t *a)
{
    vinfo_t *_t1;
    _t1 = psyco_get_field(po, a, FIX_size);
    if (_t1 == NULL) {
        return NULL;
    }
    return _t1;
}

static vinfo_t *
do_argstrip(PsycoObject *po, vinfo_t *self, int striptype, vinfo_t *bytes)
{
    return psyco_generic_call(po, cimpl_do_argstrip, CfCommonNewRefPyObject,
                              "vlv", self, (long)(striptype), bytes);
}

static vinfo_t *
bytes_lstrip_impl(PsycoObject *po, vinfo_t *self, vinfo_t *bytes)
{
    vinfo_t *_t1;
    _t1 = do_argstrip(po, self, 0, bytes);
    return _t1;
}

static vinfo_t *
bytes_lstrip(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    int _t1;
    vinfo_t *_t2;
    vinfo_t *_t3;
    vinfo_t *bytes;
    vinfo_t *return_value;
    _t1 = PsycoTuple_Load(vargs);
    if (_t1 < 0 || _t1 > 1) {
        return PsycoCFunction_CallFast(po, cfunc_bytes_lstrip, METH_FASTCALL,
                                       self, vargs);
    }
    if (_t1 == 0) {
        bytes = psyco_vi_None();
        _t2 = bytes_lstrip_impl(po, self, bytes);
        return_value = _t2;
        vinfo_decref(bytes, po);
        return return_value;
    }
    else {
        bytes = PsycoTuple_GET_ITEM(vargs, 0);
        _t3 = bytes_lstrip_impl(po, self, bytes);
        return_value = _t3;
        return return_value;
    }
}

static vinfo_t *
bytes_rstrip_impl(PsycoObject *po, vinfo_t *self, vinfo_t *bytes)
{
    vinfo_t *_t1;
    _t1 = do_argstrip(po, self, 1, bytes);
    return _t1;
}

static vinfo_t *
bytes_rstrip(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    int _t1;
    vinfo_t *_t2;
    vinfo_t *_t3;
    vinfo_t *bytes;
    vinfo_t *return_value;
    _t1 = PsycoTuple_Load(vargs);
    if (_t1 < 0 || _t1 > 1) {
        return PsycoCFunction_CallFast(po, cfunc_bytes_rstrip, METH_FASTCALL,
                                       self, vargs);
    }
    if (_t1 == 0) {
        bytes = psyco_vi_None();
        _t2 = bytes_rstrip_impl(po, self, bytes);
        return_value = _t2;
        vinfo_decref(bytes, po);
        return return_value;
    }
    else {
        bytes = PsycoTuple_GET_ITEM(vargs, 0);
        _t3 = bytes_rstrip_impl(po, self, bytes);
        return_value = _t3;
        return return_value;
    }
}

static vinfo_t *
bytes_strip_impl(PsycoObject *po, vinfo_t *self, vinfo_t *bytes)
{
    vinfo_t *_t1;
    _t1 = do_argstrip(po, self, 2, bytes);
    return _t1;
}

static vinfo_t *
bytes_strip(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    int _t1;
    vinfo_t *_t2;
    vinfo_t *_t3;
    vinfo_t *bytes;
    vinfo_t *return_value;
    _t1 = PsycoTuple_Load(vargs);
    if (_t1 < 0 || _t1 > 1) {
        return PsycoCFunction_CallFast(po, cfunc_bytes_strip, METH_FASTCALL,
                                       self, vargs);
    }
    if (_t1 == 0) {
        bytes = psyco_vi_None();
        _t2 = bytes_strip_impl(po, self, bytes);
        return_value = _t2;
        vinfo_decref(bytes, po);
        return return_value;
    }
    else {
        bytes = PsycoTuple_GET_ITEM(vargs, 0);
        _t3 = bytes_strip_impl(po, self, bytes);
        return_value = _t3;
        return return_value;
    }
}

static void
gen_bytesobject_init(void)
{
    Psyco_DefineMeta(PyBytes_Type.tp_as_sequence->sq_length, bytes_length);
}
static void
gen_bytesobject_methods_init(void)
{
    cfunc_bytes_lstrip = Psyco_DefineMethodFn(&PyBytes_Type, "lstrip",
                                              METH_FASTCALL, &bytes_lstrip);
    cfunc_bytes_rstrip = Psyco_DefineMethodFn(&PyBytes_Type, "rstrip",
                                              METH_FASTCALL, &bytes_rstrip);
    cfunc_bytes_strip = Psyco_DefineMethodFn(&PyBytes_Type, "strip",
                                             METH_FASTCALL, &bytes_strip);
}

INITIALIZATIONFN
void psy_bytesobject_init(void) {
  bytes_subscript = PyBytes_Type.tp_as_mapping->mp_subscript;
  gen_bytesobject_init();
  gen_bytesobject_methods_init();
  Psyco_DefineMeta(bytes_subscript, pbytes_subscript);
}
