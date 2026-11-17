#include "pobject.h"
#include "pboolobject.h"
#include "../pycodegen.h"

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

INITIALIZATIONFN
void psy_unicodeobject_init(void) {
  Psyco_DefineMeta(PyUnicode_Type.tp_richcompare, punicode_richcompare);
}
