/*
 * Virtual builtin-method objects ([iCFUNC_M_ML] = PyMethodDef*,
 * [iCFUNC_M_SELF]) and the PyCFunction tp_call meta. The meta promotes m_ml,
 * switches on ml_flags exactly as CPython does, and dispatches to a meta for
 * ml_meth if one is registered. METH_FASTCALL (very common in 3.7) is not
 * handled, so those calls always become a residual PyCFunction_Call.
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
    }
  }

  /* default, slow version */
use_proxy:
  return psyco_generic_call(po, PyCFunction_Call, CfCommonNewRefPyObject, "vvv",
                            func, args, kwargs);
}

INITIALIZATIONFN
void psy_methodobject_init(void) {
  Psyco_DefineMeta(PyCFunction_Type.tp_call, PsycoCFunction_Call);
  INIT_SVIRTUAL(psyco_computed_cfunction, compute_cfunction,
                direct_compute_cfunction, (1 << iCFUNC_M_SELF), 1, 1);
}
