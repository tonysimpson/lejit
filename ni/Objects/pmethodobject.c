/*
 * Virtual builtin-method objects ([iCFUNC_M_ML] = PyMethodDef*,
 * [iCFUNC_M_SELF]) and the PyCFunction tp_call meta. The meta promotes m_ml,
 * switches on ml_flags exactly as CPython does, and dispatches to a meta for
 * ml_meth if one is registered. Without a meta, a METH_FASTCALL call with
 * no keywords and up to FAST_MAX arguments calls the C function directly
 * through a cimpl_fastN helper that puts the arguments in a C array, so no
 * tuple is built (KNOWN_BUGS F13). Like the METH_O and METH_VARARGS cases,
 * these direct calls skip PyCFunction_Call's Py_EnterRecursiveCall and
 * _Py_CheckFunctionResult. Keywords, or more arguments, take the residual
 * PyCFunction_Call / tuple paths.
 */
#include "pmethodobject.h"
#include "ptupleobject.h"

static bool compute_cfunction(PsycoObject *po, vinfo_t *methobj) {
  vinfo_t *newobj;
  vinfo_t *m_self;
  vinfo_t *m_ml;

  /* get the fields from the Python object 'methobj' */
  m_self = vinfo_getitem(methobj, iCFUNC_M_SELF);
  if (m_self == NULL)
    return false;
  m_ml = vinfo_getitem(methobj, iCFUNC_M_ML);
  if (m_ml == NULL)
    return false;

    /* call PyCFunction_New() */
#ifdef PyCFunction_New /* Python >= 2.3 introduced PyCFunction_NewEx() */
  newobj =
      psyco_generic_call(po, PyCFunction_NewEx, CfPure | CfCommonNewRefPyObject,
                         "vvl", m_ml, m_self, NULL);
#else
  newobj = psyco_generic_call(
      po, PyCFunction_New, CfPure | CfCommonNewRefPyObject, "vv", m_ml, m_self);
#endif
  if (newobj == NULL)
    return false;

  /* move the resulting non-virtual Python object back into 'methobj' */
  vinfo_move(po, methobj, newobj);
  return true;
}

static PyObject *direct_compute_cfunction(vinfo_t *methobj, char *data) {
  PyObject *m_self;
  long m_ml;
  PyObject *result = NULL;

  m_self = direct_xobj_vinfo(vinfo_getitem(methobj, iCFUNC_M_SELF), data);
  m_ml = direct_read_vinfo(vinfo_getitem(methobj, iCFUNC_M_ML), data);

  if (!PyErr_Occurred())
    result = PyCFunction_New((PyMethodDef *)m_ml, m_self);

  Py_XDECREF(m_self);
  return result;
}

DEFINEVAR source_virtual_t psyco_computed_cfunction;

/***************************************************************/
/*** C method objects meta-implementation                    ***/

DEFINEFN
vinfo_t *PsycoCFunction_Call(PsycoObject *po, vinfo_t *func, vinfo_t *args,
                             vinfo_t *kwargs) {
  long mllong;
  vinfo_t *vml = psyco_get_const(po, func, CFUNC_m_ml);
  if (vml == NULL)
    return NULL;

  /* promote to compile-time the function if we do not know which one
     it is yet */
  mllong = psyco_atcompiletime(po, vml);
  if (mllong == -1) {
    /* -1 is not a valid pointer */
    extra_assert(PycException_Occurred(po));
    return NULL;
  } else {
    PyMethodDef *ml = (PyMethodDef *)CompileTime_Get(vml->source)->value;
    int flags = ml->ml_flags & ~(METH_CLASS | METH_STATIC | METH_COEXIST);
    int tuplesize;

    vinfo_t *vself = psyco_get_const(po, func, CFUNC_m_self);
    if (vself == NULL)
      return NULL;

    if (!psyco_knowntobe(kwargs, (long)NULL))
      goto use_proxy;

    switch (flags) {
    case METH_O:
      tuplesize = PsycoTuple_Load(args);
      if (tuplesize != 1)
        goto use_proxy;
      return Psyco_META2(po, ml->ml_meth, CfCommonNewRefPyObject, "vv", vself,
                  PsycoTuple_GET_ITEM(args, 0));
    case METH_VARARGS:
      return Psyco_META2(po, ml->ml_meth, CfCommonNewRefPyObject, "vv", vself,
                         args);
    case METH_VARARGS | METH_KEYWORDS:
      return Psyco_META3(po, ml->ml_meth, CfCommonNewRefPyObject, "vvv", vself,
                         args, kwargs);
    case METH_NOARGS:
      tuplesize = PsycoTuple_Load(args);
      if (tuplesize != 0)
        goto use_proxy;
      return Psyco_META2(po, ml->ml_meth, CfCommonNewRefPyObject, "vl", vself,
                         NULL);
    case METH_FASTCALL:
    case METH_FASTCALL | METH_KEYWORDS: {
      vinfo_t *(*meta)(PsycoObject *, vinfo_t *, vinfo_t *) =
          Psyco_Lookup(ml->ml_meth);
      if (meta == NULL)
        return PsycoCFunction_CallFast(po, ml->ml_meth, flags, vself, args);
      return meta(po, vself, args); /* no keywords here */
    }
    }
  }

  /* default, slow version */
use_proxy:
  psyco_stat_cfunc_proxy++;
  return psyco_generic_call(po, PyCFunction_Call, CfCommonNewRefPyObject, "vvv",
                            func, args, kwargs);
}

DEFINEVAR long psyco_stat_cfunc_fast = 0;
DEFINEVAR long psyco_stat_cfunc_proxy = 0;

/* call a METH_FASTCALL function with a tuple's items, no keywords */
static PyObject *cimpl_call_fast(PyCFunction meth, PyObject *self,
                                 PyObject *args) {
  return ((_PyCFunctionFast)meth)(self, &PyTuple_GET_ITEM(args, 0),
                                  PyTuple_GET_SIZE(args));
}

static PyObject *cimpl_call_fast_kw(PyCFunction meth, PyObject *self,
                                    PyObject *args) {
  return ((_PyCFunctionFastWithKeywords)meth)(
      self, &PyTuple_GET_ITEM(args, 0), PyTuple_GET_SIZE(args), NULL);
}

/* the same with n arguments passed one by one (psyco_generic_call takes
   at most 7: meth, self and FAST_MAX arguments) */
#define FAST_MAX 5
#define FAST_FN(n, params, ...)                                                \
  static PyObject *cimpl_fast##n(PyCFunction meth, PyObject *self params) {    \
    PyObject *a[] = {__VA_ARGS__};                                             \
    return ((_PyCFunctionFast)meth)(self, a, n);                               \
  }                                                                            \
  static PyObject *cimpl_fastkw##n(PyCFunction meth, PyObject *self params) {  \
    PyObject *a[] = {__VA_ARGS__};                                             \
    return ((_PyCFunctionFastWithKeywords)meth)(self, a, n, NULL);             \
  }
#define P(x) , PyObject *x
FAST_FN(0, , NULL)
FAST_FN(1, P(a0), a0)
FAST_FN(2, P(a0) P(a1), a0, a1)
FAST_FN(3, P(a0) P(a1) P(a2), a0, a1, a2)
FAST_FN(4, P(a0) P(a1) P(a2) P(a3), a0, a1, a2, a3)
FAST_FN(5, P(a0) P(a1) P(a2) P(a3) P(a4), a0, a1, a2, a3, a4)
#undef P
#undef FAST_FN

static void *const cimpl_fast[2][FAST_MAX + 1] = {
    {cimpl_fast0, cimpl_fast1, cimpl_fast2, cimpl_fast3, cimpl_fast4,
     cimpl_fast5},
    {cimpl_fastkw0, cimpl_fastkw1, cimpl_fastkw2, cimpl_fastkw3,
     cimpl_fastkw4, cimpl_fastkw5}};

DEFINEFN
vinfo_t *PsycoCFunction_CallFast(PsycoObject *po, PyCFunction meth, int flags,
                                 vinfo_t *vself, vinfo_t *vargs) {
  static const char *const fmt[FAST_MAX + 1] = {"lv",    "lvv",    "lvvv",
                                                "lvvvv", "lvvvvv", "lvvvvvv"};
  int kw = (flags & METH_KEYWORDS) != 0;
  int n = PsycoTuple_Load(vargs);
  vinfo_t *a[FAST_MAX] = {NULL};
  int i;
  if (n < 0 || n > FAST_MAX) {
    void *fn = kw ? (void *)cimpl_call_fast_kw : (void *)cimpl_call_fast;
    return psyco_generic_call(po, fn, CfCommonNewRefPyObject, "lvv",
                              (long)meth, vself, vargs);
  }
  for (i = 0; i < n; i++)
    a[i] = PsycoTuple_GET_ITEM(vargs, i);
  psyco_stat_cfunc_fast++;
  /* fmt[n] stops before the unused (NULL) trailing arguments */
  return psyco_generic_call(po, cimpl_fast[kw][n], CfCommonNewRefPyObject,
                            fmt[n], (long)meth, vself, a[0], a[1], a[2], a[3],
                            a[4]);
}

INITIALIZATIONFN
void psy_methodobject_init(void) {
  Psyco_DefineMeta(PyCFunction_Type.tp_call, PsycoCFunction_Call);
  INIT_SVIRTUAL(psyco_computed_cfunction, compute_cfunction,
                direct_compute_cfunction, (1 << iCFUNC_M_SELF), 1, 1);
}
