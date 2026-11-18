/*
 * str (PEP 393). Compiled code reads a str only through the fields every
 * kind shares (PyASCIIObject), and its characters inline only for a
 * compact ASCII string, the common case, or for a constant whose kind is
 * known now. Anything else (other kinds at run time; subclasses, which are
 * never compact; strings not yet "ready", whose length field is 0) goes to
 * a C call on a respawned branch. Strings are immutable, so constants fold.
 * A one-character result below 256 is CPython's own cached string (latin1[]
 * holds them, taken from CPython's cache at start-up), as unicode_char
 * returns, so identities match CPython's.
 *
 * Length, + and 'in' are generated (punicodeobject_gen.h) from
 * the C with the descriptors below (policy_table.TYPES); what stays here is
 * the representation the C doesn't state: the inline character read, the
 * slices and compare's constant fold and one-call equality.
 */
#include "pobject.h"
#include "pboolobject.h"
#include "plongobject.h"
#include "psliceobject.h"
#include "../pycodegen.h"

/* 'state' is read as its low byte, which holds the bit-fields ready, ascii,
   compact, kind and interned (their masks are generated: NI_BIT_, from
   the bit order policy_table.LAYOUTS checks, which puts ready, ascii and
   compact at the top, as state_at_least's unsigned >= needs). Only
   'interned' changes once a string is ready; the rest of the word is
   padding that CPython never initialises. */
#define UNICODE_length CPY_NONNEG_FIELD(PyASCIIObject, length, OB_type)
#define UNICODE_hash CPY_FIELD(PyASCIIObject, hash, UNICODE_length)
#define UNICODE_state CPY_FIELD(PyASCIIObject, state, UNICODE_hash)
#define STATE_COMPACT_ASCII                                                    \
  (NI_BIT_PyASCIIObject__state__ready | NI_BIT_PyASCIIObject__state__ascii |  \
   NI_BIT_PyASCIIObject__state__compact)

static PyObject *latin1[256];
static binaryfunc unicode_subscript;

/* unicode_length, PyUnicode_Concat, PyUnicode_Contains */
#include "punicodeobject_gen.h"

/* unicode_richcompare compares any two str instances, subclasses included
   (an override in a subclass is its own slot, tried by the caller); anything
   else is NotImplemented */
static bool both_str(vinfo_t *v, vinfo_t *w) {
  PyTypeObject *vtp = Psyco_KnownType(v);
  PyTypeObject *wtp = Psyco_KnownType(w);
  return vtp != NULL && wtp != NULL && PyType_IsSubtype(vtp, &PyUnicode_Type) &&
         PyType_IsSubtype(wtp, &PyUnicode_Type);
}

static int cimpl_unicode_eq(PyObject *a, PyObject *b) {
  return a == b || _PyUnicode_EQ(a, b);
}

/* unicode_richcompare. Strings are immutable, so two constants compare at
   compile time; == and != against a run-time string are a C call giving a
   known bool, not a new object to test with PyObject_IsTrue. */
static vinfo_t *punicode_richcompare(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                                     int op) {
  vinfo_t *x;
  condition_code_t cc;
  if (!both_str(v, w))
    return psyco_vi_NotImplemented();
  if (is_compiletime(v->source) && is_compiletime(w->source)) {
    PyObject *r = PyUnicode_Type.tp_richcompare(
        (PyObject *)CompileTime_Get(v->source)->value,
        (PyObject *)CompileTime_Get(w->source)->value, op);
    if (r == NULL) {
      psyco_virtualize_exception(po);
      return NULL;
    }
    cc = r == Py_True ? CC_ALWAYS_TRUE : CC_ALWAYS_FALSE;
    Py_DECREF(r);
    return PsycoBool_FromCondition(po, cc);
  }
  if (op != Py_EQ && op != Py_NE)
    return psyco_generic_call(po, PyUnicode_Type.tp_richcompare,
                              CfCommonNewRefPyObject, "vvl", v, w, (long)op);
  x = psyco_generic_call(po, cimpl_unicode_eq, CfPure | CfReturnTypeInt, "vv",
                         v, w);
  if (x == NULL)
    return NULL;
  cc = integer_non_null(po, x);
  vinfo_decref(x, po);
  if (cc == CC_ERROR)
    return NULL;
  if (op == Py_NE)
    cc = INVERT_CC(cc);
  return PsycoBool_FromCondition(po, cc);
}

/* the state byte is >= 'bits' at run time: a condition code */
static condition_code_t state_at_least(PsycoObject *po, vinfo_t *v, int bits) {
  vinfo_t *st = psyco_get_const(po, v, UNICODE_state);
  if (st == NULL)
    return CC_ERROR;
  return integer_cmp_i(po, st, bits, Py_GE | COMPARE_UNSIGNED);
}

/* unicode_subscript's integer case, for any str */
static PyObject *cimpl_unicode_item(PyObject *s, Py_ssize_t i) {
  if (PyUnicode_READY(s) == -1)
    return NULL;
  if (i < 0)
    i += PyUnicode_GET_LENGTH(s);
  if (i < 0 || i >= PyUnicode_GET_LENGTH(s)) {
    PyErr_SetString(PyExc_IndexError, "string index out of range");
    return NULL;
  }
  return PyUnicode_FromOrdinal(PyUnicode_READ_CHAR(s, i));
}

/* unicode_subscript's integer case then unicode_getitem, inline when the
   characters' place and size are known: 'data' bytes in, 'kind' bytes
   each. Every 1-byte character is below 256, so in latin1[]. */
static vinfo_t *punicode_item(PsycoObject *po, vinfo_t *v, vinfo_t *i) {
  int kind = PyUnicode_1BYTE_KIND;
  long data = sizeof(PyASCIIObject);
  vinfo_t *vlen, *ch, *r, *release_me = NULL;
  condition_code_t cc;
  defield_t df;

  if (is_compiletime(v->source)) {
    PyObject *s = (PyObject *)CompileTime_Get(v->source)->value;
    if (!PyUnicode_IS_READY(s) || !PyUnicode_IS_COMPACT(s))
      goto helper;
    kind = PyUnicode_KIND(s);
    data = (char *)PyUnicode_DATA(s) - (char *)s;
  } else {
    cc = state_at_least(po, v, STATE_COMPACT_ASCII);
    if (cc == CC_ERROR)
      return NULL;
    if (!runtime_condition_t(po, cc))
      goto helper;
  }

  vlen = psyco_get_const(po, v, UNICODE_length);
  if (vlen == NULL)
    return NULL;
  cc = integer_cmp_i(po, i, 0, Py_LT);
  if (cc == CC_ERROR)
    return NULL;
  if (runtime_condition_f(po, cc)) {
    release_me = i = integer_add(po, i, vlen, false);
    if (i == NULL)
      return NULL;
  } else
    assert_nonneg(i);
  cc = integer_cmp(po, i, vlen, Py_GE | COMPARE_UNSIGNED);
  if (cc == CC_ERROR || runtime_condition_f(po, cc)) {
    if (cc != CC_ERROR)
      PycException_SetString(po, PyExc_IndexError,
                             "string index out of range");
    vinfo_xdecref(release_me, po);
    return NULL;
  }

  df = kind == PyUnicode_1BYTE_KIND   ? UNSIGNED_ARRAY(Py_UCS1, data)
       : kind == PyUnicode_2BYTE_KIND ? UNSIGNED_ARRAY(Py_UCS2, data)
                                      : UNSIGNED_ARRAY(Py_UCS4, data);
  ch = psyco_get_field_array(po, v, df, i);
  vinfo_xdecref(release_me, po);
  if (ch == NULL)
    return NULL;
  if (kind == PyUnicode_1BYTE_KIND) {
    vinfo_t *table = vinfo_new(CompileTime_New((long)latin1));
    r = psyco_get_field_array(po, table, FPYREF(DEF_ARRAY(PyObject *, 0)), ch);
    vinfo_decref(table, po);
  } else
    r = psyco_generic_call(po, PyUnicode_FromOrdinal,
                           CfPure | CfCommonNewRefPyObject, "v", ch);
  vinfo_decref(ch, po);
  if (r != NULL && !is_compiletime(r->source))
    Psyco_AssertType(po, r, &PyUnicode_Type);
  return r;

helper:
  return psyco_generic_call(po, cimpl_unicode_item, CfCommonNewRefPyObject,
                            "vv", v, i);
}

/* unicode_subscript's slice case with step 1, after PySlice_Unpack. The
   empty string is CPython's shared one, and PyUnicode_Substring gives the
   string itself for a full slice of an exact str (unicode_result_unchanged),
   a copy for a subclass. */
static PyObject *cimpl_unicode_slice(PyObject *self, Py_ssize_t start,
                                     Py_ssize_t stop) {
  if (PyUnicode_READY(self) == -1)
    return NULL;
  if (PySlice_AdjustIndices(PyUnicode_GET_LENGTH(self), &start, &stop, 1) <=
      0)
    return PyUnicode_New(0, 0);
  return PyUnicode_Substring(self, start, stop);
}

/* unicode_subscript: an int index or a virtual slice here, anything else in
   C. A slice of a constant exact str folds. */
static vinfo_t *punicode_subscript(PsycoObject *po, vinfo_t *v,
                                   vinfo_t *key) {
  vinfo_t *i, *j, *r;
  PyTypeObject *ktp = Psyco_NeedType(po, key);
  if (ktp == NULL)
    return NULL;
  if (ktp == &PySlice_Type) {
    switch (PsycoSlice_Unpack(po, key, &i, &j)) {
    case -1:
      return NULL;
    case 1:
      r = psyco_generic_call(
          po, cimpl_unicode_slice,
          CfCommonNewRefPyObject |
              (Psyco_KnownType(v) == &PyUnicode_Type ? CfPure : 0),
          "vvv", v, i, j);
      vinfo_decref(j, po);
      vinfo_decref(i, po);
      if (r != NULL && !is_compiletime(r->source))
        Psyco_AssertType(po, r, &PyUnicode_Type);
      return r;
    }
  }
  if (!PyType_IsSubtype(ktp, &PyLong_Type))
    return psyco_generic_call(po, unicode_subscript, CfCommonNewRefPyObject,
                              "vv", v, key);
  i = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
  if (i == NULL)
    return NULL;
  r = punicode_item(po, v, i);
  vinfo_decref(i, po);
  return r;
}

INITIALIZATIONFN
void psy_unicodeobject_init(void) {
  int c;
  PyObject *a = PyUnicode_FromOrdinal('a');
  if (a == NULL ||
      (*(unsigned char *)&((PyASCIIObject *)a)->state & STATE_COMPACT_ASCII) !=
          STATE_COMPACT_ASCII ||
      PyUnicode_DATA(a) != (char *)a + sizeof(PyASCIIObject))
    Py_FatalError("ni: str layout is not what punicodeobject.c expects");
  Py_DECREF(a);
  for (c = 0; c < 256; c++)
    if ((latin1[c] = PyUnicode_FromOrdinal(c)) == NULL)
      Py_FatalError("ni: no one-character strings");
  unicode_subscript = PyUnicode_Type.tp_as_mapping->mp_subscript;
  Psyco_DefineMeta(PyUnicode_Type.tp_richcompare, punicode_richcompare);
  Psyco_DefineMeta(unicode_subscript, punicode_subscript);
  gen_unicodeobject_init();
}
