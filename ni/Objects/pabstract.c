/*
 * The compile-time abstract.c. Most of it is translated from CPython's
 * Objects/abstract.c, after what the translation leaves out:
 * PyObject_Call (call.c), PyNumber_Power (a type whitelist), PyObject_GetIter (a virtual iterator), PyIter_Next
 * (FOR_ITER's call, not a translation of it), and the subscripts of
 * tuples and lists.
 */
#include "pabstract.h"
#include "pclassobject.h"
#include "pfuncobject.h"
#include "piterobject.h"
#include "plongobject.h"
#include "pmethodobject.h"
#include "ptupleobject.h"

/* abstract.c's type_error: msg has one %.200s for the type's name */
static vinfo_t *type_error_tp(PsycoObject *po, const char *msg,
                              PyTypeObject *tp) {
  PycException_SetFormat(po, PyExc_TypeError, msg, tp->tp_name);
  return NULL;
}


/* From 3.9 a function's, a method's and a method descriptor's tp_call is
   the same PyVectorcall_Call, so a call meta is looked up by type, not
   by the C function the slot holds. */
#define CALL_METAS_MAX 8
static struct {
  PyTypeObject *tp;
  ternaryfunc call;
  call_meta_fn meta;
} call_metas[CALL_METAS_MAX];
static int ncall_metas = 0;

DEFINEFN
void Psyco_DefineCallMeta(PyTypeObject *tp, call_meta_fn meta) {
  if (ncall_metas == CALL_METAS_MAX)
    Py_FatalError("Ni: CALL_METAS_MAX call metas");
  call_metas[ncall_metas].tp = tp;
  call_metas[ncall_metas].call = tp->tp_call;
  call_metas[ncall_metas].meta = meta;
  ncall_metas++;
}

DEFINEFN
vinfo_t *PsycoObject_Call(PsycoObject *po, vinfo_t *callable_object,
                          vinfo_t *args,
                          vinfo_t *kw) { /* 'kw' may not be NULL */
  ternaryfunc call;
  PyTypeObject *tp = Psyco_NeedType(po, callable_object);
  if (tp == NULL)
    return NULL;

  switch (Psyco_TypeSlot(po, tp, &tp->tp_call, (void **)&call)) {
  case 1:
    break;
  case 0: /* a heap type's tp_call changed; kw is 0 for none */
    return psyco_generic_call(po, PyObject_Call, CfCommonNewRefPyObject,
                              "vvv", callable_object, args, kw);
  default:
    return NULL;
  }
  if (call != NULL) {
    int i;
    for (i = 0; i < ncall_metas; i++)
      if (call_metas[i].tp == tp && call_metas[i].call == call)
        return call_metas[i].meta(po, callable_object, args, kw);
    return Psyco_META3(po, call, CfCommonNewRefPyObject, "vvv", callable_object,
                       args, kw);
  }
  return type_error_tp(po, "'%.200s' object is not callable", tp);
}

/* the metas translated from abstract.c */
#if NI_HAS_CHECK_SLOT_RESULT
/* run-time copies: where a heap type's slot changed, or where the meta has no compile-time form, CPython's algorithm resumes in these */
extern int
_Py_CheckSlotResult(PyObject *obj, const char *slot_name, int success);
#endif

#if !NI_HAS_CHECK_SLOT_RESULT
/* run-time copies: where a heap type's slot changed, or where the meta has no compile-time form, CPython's algorithm resumes in these */
#endif
static PyObject *
cimpl_binary_op1(PyObject *v, PyObject *w, const int op_slot)
{
#if NI_HAS_CHECK_SLOT_RESULT
    binaryfunc slotv;
    if (Py_TYPE(v)->tp_as_number != NULL) {
        slotv =
            (*(binaryfunc *)(&((char *)Py_TYPE(v)->tp_as_number)[op_slot]));
    }
    else {
        slotv = NULL;
    }
    binaryfunc slotw;
    if (!Py_IS_TYPE(w, Py_TYPE(v)) && Py_TYPE(w)->tp_as_number != NULL) {
        slotw =
            (*(binaryfunc *)(&((char *)Py_TYPE(w)->tp_as_number)[op_slot]));
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    else {
        slotw = NULL;
    }
#elif NI_HAS_PY_IS_TYPE
    PyObject *x;
    binaryfunc slotv = NULL;
    binaryfunc slotw = NULL;
    if (Py_TYPE(v)->tp_as_number != NULL)
        slotv =
            (*(binaryfunc *)(&((char *)Py_TYPE(v)->tp_as_number)[op_slot]));
    if (!Py_IS_TYPE(w, Py_TYPE(v)) && Py_TYPE(w)->tp_as_number != NULL) {
        slotw =
            (*(binaryfunc *)(&((char *)Py_TYPE(w)->tp_as_number)[op_slot]));
        if (slotw == slotv)
            slotw = NULL;
    }
#else
    PyObject *x;
    binaryfunc slotv = NULL;
    binaryfunc slotw = NULL;
    if (v->ob_type->tp_as_number != NULL)
        slotv =
            (*(binaryfunc *)(&((char *)v->ob_type->tp_as_number)[op_slot]));
    if (w->ob_type != v->ob_type && w->ob_type->tp_as_number != NULL) {
        slotw =
            (*(binaryfunc *)(&((char *)w->ob_type->tp_as_number)[op_slot]));
        if (slotw == slotv)
            slotw = NULL;
    }
#endif
    if (slotv) {
#if NI_HAS_CHECK_SLOT_RESULT
        PyObject *x;
        if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
            x = slotw(v, w);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x);
            slotw = NULL;
        }
#elif NI_HAS_PY_IS_TYPE
        if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
            x = slotw(v, w);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x);
            slotw = NULL;
        }
#else
        if (slotw && PyType_IsSubtype(w->ob_type, v->ob_type)) {
            x = slotw(v, w);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x);
            slotw = NULL;
        }
#endif
        x = slotv(v, w);
#if NI_HAS_CHECK_SLOT_RESULT
        assert(_Py_CheckSlotResult(v, op_name, x != NULL));
        if (x != Py_NotImplemented) {
            return x;
        }
#else
        if (x != Py_NotImplemented)
            return x;
#endif
        Py_DECREF(x);
    }
    if (slotw) {
#if NI_HAS_CHECK_SLOT_RESULT
        PyObject *x = slotw(v, w);
        assert(_Py_CheckSlotResult(w, op_name, x != NULL));
        if (x != Py_NotImplemented) {
            return x;
        }
#else
        x = slotw(v, w);
        if (x != Py_NotImplemented)
            return x;
#endif
        Py_DECREF(x);
    }
    Py_RETURN_NOTIMPLEMENTED;
}

static PyObject *
cimpl_binop_type_error(PyObject *v, PyObject *w, const char *op_name)
#if NI_HAS_PY_IS_TYPE
{
    PyErr_Format(PyExc_TypeError,
                 "unsupported operand type(s) for %.100s: "
                 "'%.100s' and '%.100s'",
                 op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    return NULL;
}
#else
{
    PyErr_Format(PyExc_TypeError,
                 "unsupported operand type(s) for %.100s: "
                 "'%.100s' and '%.100s'",
                 op_name, v->ob_type->tp_name, w->ob_type->tp_name);
    return NULL;
}
#endif

static PyObject *
cimpl_PyNumber_Add_1(PyObject *v, PyObject *w, PySequenceMethods *m)
#if NI_HAS_CHECK_SLOT_RESULT
{
    PyObject *result;
    if (m && m->sq_concat) {
        result = (*m->sq_concat)(v, w);
        assert(_Py_CheckSlotResult(v, "+", result != NULL));
        return result;
    }
    return cimpl_binop_type_error(v, w, "+");
}
#else
{
    PyObject *result;
    {
        if (m && m->sq_concat) {
            return (*m->sq_concat)(v, w);
        }
        result = cimpl_binop_type_error(v, w, "+");
    }
    return result;
}
#endif

static PyObject *
cimpl_binary_op_1(PyObject *v, PyObject *w, const int op_slot,
                  const char *op_name)
#if NI_HAS_PY_IS_TYPE
{
    if (op_slot == offsetof(PyNumberMethods, nb_rshift) &&
        PyCFunction_CheckExact(v) &&
        strcmp(((PyCFunctionObject *)v)->m_ml->ml_name, "print") == 0) {
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: "
                     "'%.100s' and '%.100s'. Did you mean \"print(<message>, "
                     "file=<output_stream>)\"?",
                     op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
        return NULL;
    }
    return cimpl_binop_type_error(v, w, op_name);
}
#else
{
    if (op_slot == offsetof(PyNumberMethods, nb_rshift) &&
        PyCFunction_Check(v) &&
        strcmp(((PyCFunctionObject *)v)->m_ml->ml_name, "print") == 0) {
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: "
                     "'%.100s' and '%.100s'. Did you mean \"print(<message>, "
                     "file=<output_stream>)\"?",
                     op_name, v->ob_type->tp_name, w->ob_type->tp_name);
        return NULL;
    }
    return cimpl_binop_type_error(v, w, op_name);
}
#endif

static PyObject *
cimpl_binary_iop1(PyObject *v, PyObject *w, const int iop_slot,
                  const int op_slot)
{
#if NI_HAS_PY_IS_TYPE
    PyNumberMethods *mv = Py_TYPE(v)->tp_as_number;
#else
    PyNumberMethods *mv = v->ob_type->tp_as_number;
#endif
    if (mv != NULL) {
        binaryfunc slot = (*(binaryfunc *)(&((char *)mv)[iop_slot]));
        if (slot) {
            PyObject *x = (slot)(v, w);
#if NI_HAS_CHECK_SLOT_RESULT
            assert(_Py_CheckSlotResult(v, op_name, x != NULL));
#endif
            if (x != Py_NotImplemented) {
                return x;
            }
            Py_DECREF(x);
        }
    }
    return cimpl_binary_op1(v, w, op_slot);
}

static PyObject *
cimpl_PyNumber_InPlaceAdd_1(PyObject *v, PyObject *w, PySequenceMethods *m)
{
#if !NI_HAS_CHECK_SLOT_RESULT
    binaryfunc f;
#endif
    PyObject *result;
    {
        {
#if NI_HAS_CHECK_SLOT_RESULT
            binaryfunc func = m->sq_inplace_concat;
            if (func == NULL)
                func = m->sq_concat;
            if (func != NULL) {
                result = func(v, w);
                assert(_Py_CheckSlotResult(v, "+=", result != NULL));
                return result;
            }
#else
            f = m->sq_inplace_concat;
            if (f == NULL)
                f = m->sq_concat;
            if (f != NULL)
                return (*f)(v, w);
#endif
        }
        result = cimpl_binop_type_error(v, w, "+=");
    }
    return result;
}

static PyObject *
cimpl_PyNumber_InPlaceAdd_3(PyObject *v, PyObject *w, PySequenceMethods *m)
{
#if NI_HAS_CHECK_SLOT_RESULT
    binaryfunc func;
#else
    binaryfunc f;
#endif
    PyObject *result;
    {
        {
            {
#if NI_HAS_CHECK_SLOT_RESULT
                func = m->sq_concat;
#else
                f = m->sq_concat;
#endif
            }
#if NI_HAS_CHECK_SLOT_RESULT
            if (func != NULL) {
                result = func(v, w);
                assert(_Py_CheckSlotResult(v, "+=", result != NULL));
                return result;
            }
#else
            if (f != NULL)
                return (*f)(v, w);
#endif
        }
        result = cimpl_binop_type_error(v, w, "+=");
    }
    return result;
}

static PyObject *
cimpl_type_error(const char *msg, PyObject *obj)
{
#if NI_HAS_PY_IS_TYPE
    PyErr_Format(PyExc_TypeError, msg, Py_TYPE(obj)->tp_name);
#else
    PyErr_Format(PyExc_TypeError, msg, obj->ob_type->tp_name);
#endif
    return NULL;
}

static PyObject *
cimpl_sequence_repeat(ssizeargfunc repeatfunc, PyObject *seq, PyObject *n)
{
    Py_ssize_t count;
#if NI_HAS_CHECK_SLOT_RESULT
    if (((((PyObject *)(n))->ob_type->tp_as_number != ((void *)0)) &&
         (((PyObject *)(n))->ob_type->tp_as_number->nb_index !=
          ((void *)0)))) {
        count = PyNumber_AsSsize_t(n, PyExc_OverflowError);
        if (count == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
#elif NI_HAS_PY_IS_TYPE
    if (((((PyObject *)(n))->ob_type->tp_as_number != ((void *)0)) &&
         (((PyObject *)(n))->ob_type->tp_as_number->nb_index !=
          ((void *)0)))) {
        count = PyNumber_AsSsize_t(n, PyExc_OverflowError);
        if (count == -1 && PyErr_Occurred())
            return NULL;
    }
#else
    if (PyIndex_Check(n)) {
        count = PyNumber_AsSsize_t(n, PyExc_OverflowError);
        if (count == -1 && PyErr_Occurred())
            return NULL;
    }
#endif
    else {
        return cimpl_type_error(
            "can't multiply sequence by "
            "non-int of type '%.200s'",
            n);
    }
#if NI_HAS_CHECK_SLOT_RESULT
    PyObject *res = (*repeatfunc)(seq, count);
    assert(_Py_CheckSlotResult(seq, "*", res != NULL));
    return res;
#else
    return (*repeatfunc)(seq, count);
#endif
}

static PyObject *
cimpl_PyNumber_InPlaceMultiply_1(PyObject *v, PyObject *w,
                                 PySequenceMethods *mv)
{
    ssizeargfunc f;
    PyObject *result;
    {
        {
            f = mv->sq_inplace_repeat;
            if (f == NULL)
                f = mv->sq_repeat;
            if (f != NULL)
                return cimpl_sequence_repeat(f, v, w);
        }
        result = cimpl_binop_type_error(v, w, "*=");
    }
    return result;
}

static PyObject *
cimpl_PyNumber_InPlaceMultiply_4(PyObject *v, PyObject *w,
                                 PySequenceMethods *mv)
{
    ssizeargfunc f;
    PyObject *result;
    {
        {
            {
                f = mv->sq_repeat;
            }
            if (f != NULL)
                return cimpl_sequence_repeat(f, v, w);
        }
        result = cimpl_binop_type_error(v, w, "*=");
    }
    return result;
}

static PyObject *
cimpl_PyNumber_InPlaceMultiply_5(PyObject *v, PyObject *w,
                                 PySequenceMethods *mw)
{
    PyObject *result;
    {
        {
            {
                if (mw->sq_repeat)
                    return cimpl_sequence_repeat(mw->sq_repeat, w, v);
            }
        }
        result = cimpl_binop_type_error(v, w, "*=");
    }
    return result;
}

static PyObject *
cimpl_ternary_op(PyObject *v, PyObject *w, PyObject *z, const int op_slot,
                 const char *op_name)
{
#if NI_HAS_CHECK_SLOT_RESULT
    PyNumberMethods *mv = Py_TYPE(v)->tp_as_number;
    PyNumberMethods *mw = Py_TYPE(w)->tp_as_number;
    ternaryfunc slotv;
    if (mv != NULL) {
        slotv = (*(ternaryfunc *)(&((char *)mv)[op_slot]));
    }
    else {
        slotv = NULL;
    }
    ternaryfunc slotw;
    if (!Py_IS_TYPE(w, Py_TYPE(v)) && mw != NULL) {
        slotw = (*(ternaryfunc *)(&((char *)mw)[op_slot]));
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    else {
        slotw = NULL;
    }
#elif NI_HAS_PY_IS_TYPE
    PyNumberMethods *mv, *mw, *mz;
    PyObject *x = NULL;
    ternaryfunc slotv = NULL;
    ternaryfunc slotw = NULL;
    ternaryfunc slotz = NULL;
    mv = Py_TYPE(v)->tp_as_number;
    mw = Py_TYPE(w)->tp_as_number;
    if (mv != NULL)
        slotv = (*(ternaryfunc *)(&((char *)mv)[op_slot]));
    if (!Py_IS_TYPE(w, Py_TYPE(v)) && mw != NULL) {
        slotw = (*(ternaryfunc *)(&((char *)mw)[op_slot]));
        if (slotw == slotv)
            slotw = NULL;
    }
#else
    PyNumberMethods *mv, *mw, *mz;
    PyObject *x = NULL;
    ternaryfunc slotv = NULL;
    ternaryfunc slotw = NULL;
    ternaryfunc slotz = NULL;
    mv = v->ob_type->tp_as_number;
    mw = w->ob_type->tp_as_number;
    if (mv != NULL)
        slotv = (*(ternaryfunc *)(&((char *)mv)[op_slot]));
    if (w->ob_type != v->ob_type && mw != NULL) {
        slotw = (*(ternaryfunc *)(&((char *)mw)[op_slot]));
        if (slotw == slotv)
            slotw = NULL;
    }
#endif
    if (slotv) {
#if NI_HAS_CHECK_SLOT_RESULT
        PyObject *x;
        if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
            x = slotw(v, w, z);
            if (x != Py_NotImplemented) {
                return x;
            }
            Py_DECREF(x);
            slotw = NULL;
        }
#elif NI_HAS_PY_IS_TYPE
        if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
            x = slotw(v, w, z);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x);
            slotw = NULL;
        }
#else
        if (slotw && PyType_IsSubtype(w->ob_type, v->ob_type)) {
            x = slotw(v, w, z);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x);
            slotw = NULL;
        }
#endif
        x = slotv(v, w, z);
#if NI_HAS_CHECK_SLOT_RESULT
        assert(_Py_CheckSlotResult(v, op_name, x != NULL));
        if (x != Py_NotImplemented) {
            return x;
        }
#else
        if (x != Py_NotImplemented)
            return x;
#endif
        Py_DECREF(x);
    }
    if (slotw) {
#if NI_HAS_CHECK_SLOT_RESULT
        PyObject *x = slotw(v, w, z);
        assert(_Py_CheckSlotResult(w, op_name, x != NULL));
        if (x != Py_NotImplemented) {
            return x;
        }
#else
        x = slotw(v, w, z);
        if (x != Py_NotImplemented)
            return x;
#endif
        Py_DECREF(x);
    }
#if NI_HAS_CHECK_SLOT_RESULT
    PyNumberMethods *mz = Py_TYPE(z)->tp_as_number;
#elif NI_HAS_PY_IS_TYPE
    mz = Py_TYPE(z)->tp_as_number;
#else
    mz = z->ob_type->tp_as_number;
#endif
    if (mz != NULL) {
#if NI_HAS_CHECK_SLOT_RESULT
        ternaryfunc slotz = (*(ternaryfunc *)(&((char *)mz)[op_slot]));
        if (slotz == slotv || slotz == slotw) {
            slotz = NULL;
        }
#else
        slotz = (*(ternaryfunc *)(&((char *)mz)[op_slot]));
        if (slotz == slotv || slotz == slotw)
            slotz = NULL;
#endif
        if (slotz) {
#if NI_HAS_CHECK_SLOT_RESULT
            PyObject *x = slotz(v, w, z);
            assert(_Py_CheckSlotResult(z, op_name, x != NULL));
            if (x != Py_NotImplemented) {
                return x;
            }
#else
            x = slotz(v, w, z);
            if (x != Py_NotImplemented)
                return x;
#endif
            Py_DECREF(x);
        }
    }
#if NI_HAS_CHECK_SLOT_RESULT
    if (z == Py_None) {
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: "
                     "'%.100s' and '%.100s'",
                     op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: "
                     "'%.100s', '%.100s', '%.100s'",
                     op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name,
                     Py_TYPE(z)->tp_name);
    }
#elif NI_HAS_PY_IS_TYPE
    if (z == Py_None)
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for ** or pow(): "
                     "'%.100s' and '%.100s'",
                     Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    else
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for pow(): "
                     "'%.100s', '%.100s', '%.100s'",
                     Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name,
                     Py_TYPE(z)->tp_name);
#else
    if (z == Py_None)
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for ** or pow(): "
                     "'%.100s' and '%.100s'",
                     v->ob_type->tp_name, w->ob_type->tp_name);
    else
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for pow(): "
                     "'%.100s', '%.100s', '%.100s'",
                     v->ob_type->tp_name, w->ob_type->tp_name,
                     z->ob_type->tp_name);
#endif
    return NULL;
}

static PyObject *
#if NI_HAS_CHECK_SLOT_RESULT
cimpl_ternary_op_1(PyObject *v, PyObject *w, PyObject *z, const int op_slot,
                   const char *op_name, PyNumberMethods *mz, ternaryfunc slotv,
                   ternaryfunc slotw)
{
    {
        ternaryfunc slotz = (*(ternaryfunc *)(&((char *)mz)[op_slot]));
        if (slotz == slotv || slotz == slotw) {
            slotz = NULL;
        }
        if (slotz) {
            PyObject *x = slotz(v, w, z);
            assert(_Py_CheckSlotResult(z, op_name, x != NULL));
            if (x != Py_NotImplemented) {
                return x;
            }
            Py_DECREF(x);
        }
    }
    if (z == Py_None) {
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: "
                     "'%.100s' and '%.100s'",
                     op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: "
                     "'%.100s', '%.100s', '%.100s'",
                     op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name,
                     Py_TYPE(z)->tp_name);
    }
    return NULL;
}
#elif NI_HAS_PY_IS_TYPE
cimpl_ternary_op_1(PyObject *v, PyObject *w, PyObject *z, const int op_slot,
                   PyNumberMethods *mz, ternaryfunc slotv, ternaryfunc slotw)
{
    ternaryfunc slotz;
    PyObject *x;
    {
        slotz = (*(ternaryfunc *)(&((char *)mz)[op_slot]));
        if (slotz == slotv || slotz == slotw)
            slotz = NULL;
        if (slotz) {
            x = slotz(v, w, z);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x);
        }
    }
    if (z == Py_None)
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for ** or pow(): "
                     "'%.100s' and '%.100s'",
                     Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    else
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for pow(): "
                     "'%.100s', '%.100s', '%.100s'",
                     Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name,
                     Py_TYPE(z)->tp_name);
    return NULL;
}
#else
cimpl_ternary_op_1(PyObject *v, PyObject *w, PyObject *z, const int op_slot,
                   PyNumberMethods *mz, ternaryfunc slotv, ternaryfunc slotw)
{
    ternaryfunc slotz;
    PyObject *x;
    {
        slotz = (*(ternaryfunc *)(&((char *)mz)[op_slot]));
        if (slotz == slotv || slotz == slotw)
            slotz = NULL;
        if (slotz) {
            x = slotz(v, w, z);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x);
        }
    }
    if (z == Py_None)
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for ** or pow(): "
                     "'%.100s' and '%.100s'",
                     v->ob_type->tp_name, w->ob_type->tp_name);
    else
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for pow(): "
                     "'%.100s', '%.100s', '%.100s'",
                     v->ob_type->tp_name, w->ob_type->tp_name,
                     z->ob_type->tp_name);
    return NULL;
}
#endif

static PyObject *
#if NI_HAS_CHECK_SLOT_RESULT
cimpl_ternary_op_2(PyObject *v, PyObject *w, PyObject *z, const char *op_name)
#else
cimpl_ternary_op_2(PyObject *v, PyObject *w, PyObject *z)
#endif
{
#if NI_HAS_CHECK_SLOT_RESULT
    if (z == Py_None) {
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: "
                     "'%.100s' and '%.100s'",
                     op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    }
    else {
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for %.100s: "
                     "'%.100s', '%.100s', '%.100s'",
                     op_name, Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name,
                     Py_TYPE(z)->tp_name);
    }
#elif NI_HAS_PY_IS_TYPE
    if (z == Py_None)
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for ** or pow(): "
                     "'%.100s' and '%.100s'",
                     Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    else
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for pow(): "
                     "'%.100s', '%.100s', '%.100s'",
                     Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name,
                     Py_TYPE(z)->tp_name);
#else
    if (z == Py_None)
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for ** or pow(): "
                     "'%.100s' and '%.100s'",
                     v->ob_type->tp_name, w->ob_type->tp_name);
    else
        PyErr_Format(PyExc_TypeError,
                     "unsupported operand type(s) for pow(): "
                     "'%.100s', '%.100s', '%.100s'",
                     v->ob_type->tp_name, w->ob_type->tp_name,
                     z->ob_type->tp_name);
#endif
    return NULL;
}

#if NI_HAS_TERNARY_IOP
static PyObject *
cimpl_ternary_iop(PyObject *v, PyObject *w, PyObject *z, const int iop_slot,
                  const int op_slot, const char *op_name)
{
    PyNumberMethods *mv = Py_TYPE(v)->tp_as_number;
    if (mv != NULL) {
        ternaryfunc slot = (*(ternaryfunc *)(&((char *)mv)[iop_slot]));
        if (slot) {
            PyObject *x = (slot)(v, w, z);
            if (x != Py_NotImplemented) {
                return x;
            }
            Py_DECREF(x);
        }
    }
    return cimpl_ternary_op(v, w, z, op_slot, op_name);
}
#endif

static PyObject *
cimpl_PyNumber_Multiply_1(PyObject *v, PyObject *w, PySequenceMethods *mv,
                          PySequenceMethods *mw)
{
    PyObject *result;
    {
        if (mv && mv->sq_repeat) {
            return cimpl_sequence_repeat(mv->sq_repeat, v, w);
        }
        else if (mw && mw->sq_repeat) {
            return cimpl_sequence_repeat(mw->sq_repeat, w, v);
        }
        result = cimpl_binop_type_error(v, w, "*");
    }
    return result;
}

static PyObject *
cimpl_PyNumber_Multiply_3(PyObject *v, PyObject *w, PySequenceMethods *mw)
{
    PyObject *result;
    {
        {
            if (mw && mw->sq_repeat) {
                return cimpl_sequence_repeat(mw->sq_repeat, w, v);
            }
        }
        result = cimpl_binop_type_error(v, w, "*");
    }
    return result;
}

static int
cimpl_PySequence_DelItem_1(PyObject *s, Py_ssize_t i, PySequenceMethods *m)
{
#if NI_HAS_CHECK_SLOT_RESULT
    int res = m->sq_ass_item(s, i, (PyObject *)NULL);
    assert(_Py_CheckSlotResult(s, "__delitem__", res >= 0));
    return res;
#else
    return m->sq_ass_item(s, i, (PyObject *)NULL);
#endif
}

static PyObject *
cimpl_PySequence_GetItem_1(PyObject *s, Py_ssize_t i, PySequenceMethods *m)
{
#if NI_HAS_CHECK_SLOT_RESULT
    PyObject *res = m->sq_item(s, i);
    assert(_Py_CheckSlotResult(s, "__getitem__", res != NULL));
    return res;
#else
    return m->sq_item(s, i);
#endif
}

static int
cimpl_PySequence_SetItem_1(PyObject *s, Py_ssize_t i, PyObject *o,
                           PySequenceMethods *m)
{
#if NI_HAS_CHECK_SLOT_RESULT
    int res = m->sq_ass_item(s, i, o);
    assert(_Py_CheckSlotResult(s, "__setitem__", res >= 0));
    return res;
#else
    return m->sq_ass_item(s, i, o);
#endif
}

static PyObject *
cimpl_PySequence_GetSlice_1(PyObject *s, PyMappingMethods *mp, PyObject *slice)
{
#if !NI_HAS_CHECK_SLOT_RESULT
    PyObject *res;
#endif
    Py_INCREF(slice);
#if NI_HAS_CHECK_SLOT_RESULT
    PyObject *res = mp->mp_subscript(s, slice);
    assert(_Py_CheckSlotResult(s, "__getitem__", res != NULL));
#else
    res = mp->mp_subscript(s, slice);
#endif
    Py_DECREF(slice);
    return res;
}

static int
cimpl_PySequence_SetSlice_1(PyObject *s, PyObject *o, PyMappingMethods *mp,
                            PyObject *slice)
{
#if !NI_HAS_CHECK_SLOT_RESULT
    int res;
#endif
    Py_INCREF(slice);
#if NI_HAS_CHECK_SLOT_RESULT
    int res = mp->mp_ass_subscript(s, slice, o);
    assert(_Py_CheckSlotResult(s, "__setitem__", res >= 0));
#else
    res = mp->mp_ass_subscript(s, slice, o);
#endif
    Py_DECREF(slice);
    return res;
}

static vinfo_t *
type_error(PsycoObject *po, const char *, vinfo_t *);
static vinfo_t *
binary_op1(PsycoObject *po, vinfo_t *, vinfo_t *, int);
static vinfo_t *
binop_type_error(PsycoObject *po, vinfo_t *, vinfo_t *, const char *);
static vinfo_t *
binary_op(PsycoObject *po, vinfo_t *, vinfo_t *, int, const char *);
static vinfo_t *
binary_iop1(PsycoObject *po, vinfo_t *, vinfo_t *, int, int);
static vinfo_t *
binary_iop(PsycoObject *po, vinfo_t *, vinfo_t *, int, int, const char *);
static vinfo_t *
sequence_repeat(PsycoObject *po, ssizeargfunc, vinfo_t *, vinfo_t *);
static vinfo_t *
ternary_op(PsycoObject *po, vinfo_t *, vinfo_t *, vinfo_t *, int,
           const char *);

#if NI_HAS_TERNARY_IOP
static vinfo_t *
ternary_iop(PsycoObject *po, vinfo_t *, vinfo_t *, vinfo_t *, int, int,
            const char *);
#endif

#if NI_HAS_ITER_CHECK_FUNCTION
DEFINEFN vinfo_t *
PsycoIter_Check(PsycoObject *po, vinfo_t *obj)
{
    iternextfunc _t1;
    int _t2;
#if NI_HAS_ITER_CHECK_TYPE_LOCAL
    PyTypeObject *tp;
#endif
    PyTypeObject *tp_obj;
    tp_obj = Psyco_NeedType(po, obj);
    if (tp_obj == NULL) {
        return NULL;
    }
#if NI_HAS_ITER_CHECK_TYPE_LOCAL
    tp = tp_obj;
    switch (Psyco_TypeSlot(po, tp_obj, &tp->tp_iternext, (void **)&_t1)) {
        case 1:
            break;
        case 0:
            return psyco_generic_call(
                po, PyIter_Check,
                CfCommonIntBoolOrError | CfNoPython | CfNoBlock, "v", obj);
        default:
            return NULL;
    }
#else
    switch (Psyco_TypeSlot(po, tp_obj, &tp_obj->tp_iternext, (void **)&_t1)) {
        case 1:
            break;
        case 0:
            return psyco_generic_call(
                po, PyIter_Check,
                CfCommonIntBoolOrError | CfNoPython | CfNoBlock, "v", obj);
        default:
            return NULL;
    }
#endif
    _t2 = _t1 != NULL;
    if (_t2) {
        return psyco_generic_call(
            po, PyIter_Check, CfCommonIntBoolOrError | CfNoPython | CfNoBlock,
            "v", obj);
    }
    return vinfo_new(CompileTime_New((long)(_t2)));
}
#endif

static vinfo_t *
type_error(PsycoObject *po, const char *msg, vinfo_t *obj)
{
    PyTypeObject *tp_obj;
    tp_obj = Psyco_NeedType(po, obj);
    if (tp_obj == NULL) {
        return NULL;
    }
    PycException_SetFormat(po, PyExc_TypeError, msg, tp_obj->tp_name);
    return NULL;
}

DEFINEFN vinfo_t *
PsycoMapping_Size(PsycoObject *po, vinfo_t *o)
{
    lenfunc _t1;
    int _t2;
    vinfo_t *_t3;
#if NI_HAS_NOT_A_SEQUENCE_ERROR
    lenfunc _t4;
    int _t5;
#endif
    vinfo_t *len;
    PyMappingMethods *m;
    PyTypeObject *tp_o;
    tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL) {
        return NULL;
    }
    m = tp_o->tp_as_mapping;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_o, &m->mp_length, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyMapping_Size,
                                          CfCommonPySSizeTResult, "v", o);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = Psyco_META1(po, _t1, CfCommonPySSizeTResult, "v", o);
        len = _t3;
        return len;
    }
#if NI_HAS_NOT_A_SEQUENCE_ERROR
    _t5 = (tp_o->tp_as_sequence) != 0;
    if (_t5) {
        switch (Psyco_TypeSlot(po, tp_o, &(tp_o->tp_as_sequence)->sq_length,
                               (void **)&_t4)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyMapping_Size,
                                          CfCommonPySSizeTResult, "v", o);
            default:
                return NULL;
        }
        _t5 = _t4 != 0;
    }
    if (_t5) {
        type_error(po, "%.200s is not a mapping", o);
        return NULL;
    }
#endif
    type_error(po, "object of type '%.200s' has no len()", o);
    return NULL;
}

DEFINEFN vinfo_t *
PsycoNumber_Absolute(PsycoObject *po, vinfo_t *o)
{
    unaryfunc _t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    PyNumberMethods *m;
#if NI_HAS_CHECK_SLOT_RESULT
    vinfo_t *res;
#endif
    PyTypeObject *tp_o;
    tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL) {
        return NULL;
    }
    m = tp_o->tp_as_number;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_o, &m->nb_absolute, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyNumber_Absolute,
                                          CfCommonNewRefPyObject, "v", o);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = Psyco_META1(po, _t1, CfCommonNewRefPyObject, "v", o);
#if NI_HAS_CHECK_SLOT_RESULT
        res = _t3;
        return res;
#else
        return _t3;
#endif
    }
    _t4 = type_error(po, "bad operand type for abs(): '%.200s'", o);
    return _t4;
}

static vinfo_t *
binary_op1(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op_slot)
{
    binaryfunc _t1;
    binaryfunc _t2;
    bool _t3;
    int _t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    binaryfunc slotv;
    binaryfunc slotw;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    vinfo_t *x;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
#if !NI_HAS_CHECK_SLOT_RESULT
    slotv = NULL;
    slotw = NULL;
#endif
    if ((tp_v->tp_as_number) != NULL) {
        switch (Psyco_TypeSlot(
            po, tp_v, (void *)&(((char *)tp_v->tp_as_number))[op_slot],
            (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_binary_op1,
                                          CfCommonCheckNotImplemented, "vvl",
                                          v, w, (long)(op_slot));
            default:
                return NULL;
        }
        slotv = _t1;
    }
#if NI_HAS_CHECK_SLOT_RESULT
    else {
        slotv = NULL;
    }
    if ((!(tp_w == tp_v) && (tp_w->tp_as_number) != NULL)) {
        switch (Psyco_TypeSlot(
            po, tp_w, (void *)&(((char *)tp_w->tp_as_number))[op_slot],
            (void **)&_t2)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_binary_op1,
                                          CfCommonCheckNotImplemented, "vvl",
                                          v, w, (long)(op_slot));
            default:
                return NULL;
        }
        slotw = _t2;
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    else {
        slotw = NULL;
    }
#elif NI_HAS_PY_IS_TYPE
    if ((!(tp_w == tp_v) && (tp_w->tp_as_number) != NULL)) {
        switch (Psyco_TypeSlot(
            po, tp_w, (void *)&(((char *)tp_w->tp_as_number))[op_slot],
            (void **)&_t2)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_binary_op1,
                                          CfCommonCheckNotImplemented, "vvl",
                                          v, w, (long)(op_slot));
            default:
                return NULL;
        }
        slotw = _t2;
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
#else
    if ((tp_w != tp_v && (tp_w->tp_as_number) != NULL)) {
        switch (Psyco_TypeSlot(
            po, tp_w, (void *)&(((char *)tp_w->tp_as_number))[op_slot],
            (void **)&_t2)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_binary_op1,
                                          CfCommonCheckNotImplemented, "vvl",
                                          v, w, (long)(op_slot));
            default:
                return NULL;
        }
        slotw = _t2;
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
#endif
    if (slotv != 0) {
        _t4 = slotw != 0;
        if (_t4) {
            switch (Psyco_TypeIsSubtype(po, tp_w, tp_v, &_t3)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, cimpl_binary_op1,
                                              CfCommonCheckNotImplemented,
                                              "vvl", v, w, (long)(op_slot));
                default:
                    return NULL;
            }
            _t4 = _t3 != 0;
        }
        if (_t4) {
            _t5 = Psyco_META2(po, slotw, CfCommonCheckNotImplemented, "vv", v,
                              w);
            x = _t5;
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
            slotw = NULL;
        }
        _t6 = Psyco_META2(po, slotv, CfCommonCheckNotImplemented, "vv", v, w);
        x = _t6;
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po);
    }
    if (slotw != 0) {
        _t7 = Psyco_META2(po, slotw, CfCommonCheckNotImplemented, "vv", v, w);
        x = _t7;
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po);
    }
    return psyco_vi_NotImplemented();
}

static vinfo_t *
binop_type_error(PsycoObject *po, vinfo_t *v, vinfo_t *w, const char *op_name)
{
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    PycException_SetFormat(
        po, PyExc_TypeError,
        "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'",
        op_name, tp_v->tp_name, tp_w->tp_name);
    return NULL;
}

#if NI_HAS_CHECK_SLOT_RESULT
DEFINEFN vinfo_t *
PsycoNumber_Add(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    binaryfunc _t2;
    int _t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    PySequenceMethods *m;
    vinfo_t *result;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op1(po, v, w, offsetof(PyNumberMethods, nb_add));
    result = _t1;
    if (IS_IMPLEMENTED(result)) {
        return result;
    }
    vinfo_decref(result, po);
    m = tp_v->tp_as_sequence;
    _t3 = m != 0;
    if (_t3) {
        switch (Psyco_TypeSlot(po, tp_v, &m->sq_concat, (void **)&_t2)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_PyNumber_Add_1,
                                          CfCommonNewRefPyObject, "vvl", v, w,
                                          (long)(m));
            default:
                return NULL;
        }
        _t3 = _t2 != 0;
    }
    if (_t3) {
        _t4 = Psyco_META2(po, _t2, CfCommonNewRefPyObject, "vv", v, w);
        result = _t4;
        return result;
    }
    _t5 = binop_type_error(po, v, w, "+");
    return _t5;
}
#else
DEFINEFN vinfo_t *
PsycoNumber_Add(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    binaryfunc _t2;
    int _t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
    PySequenceMethods *m;
    vinfo_t *result;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op1(po, v, w, offsetof(PyNumberMethods, nb_add));
    result = _t1;
    if (!IS_IMPLEMENTED(result)) {
        m = tp_v->tp_as_sequence;
        vinfo_decref(result, po);
        _t3 = m != 0;
        if (_t3) {
            switch (Psyco_TypeSlot(po, tp_v, &m->sq_concat, (void **)&_t2)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, cimpl_PyNumber_Add_1,
                                              CfCommonNewRefPyObject, "vvl", v,
                                              w, (long)(m));
                default:
                    return NULL;
            }
            _t3 = _t2 != 0;
        }
        if (_t3) {
            _t4 = Psyco_META2(po, _t2, CfCommonNewRefPyObject, "vv", v, w);
            return _t4;
        }
        _t5 = binop_type_error(po, v, w, "+");
        result = _t5;
        return result;
    }
    return result;
}
#endif

static vinfo_t *
binary_op(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op_slot,
          const char *op_name)
{
    vinfo_t *_t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *result;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op1(po, v, w, op_slot);
    result = _t1;
    if (!IS_IMPLEMENTED(result)) {
        vinfo_decref(result, po);
        _t2 = (op_slot == (offsetof(PyNumberMethods, nb_rshift)) &&
               tp_v == (&PyCFunction_Type));
        if (_t2) {
            return psyco_generic_call(po, cimpl_binary_op_1,
                                      CfCommonNewRefPyObject, "vvll", v, w,
                                      (long)(op_slot), (long)(op_name));
        }
        if (_t2) {
            PycException_SetFormat(
                po, PyExc_TypeError,
                "unsupported operand type(s) for %.100s: '%.100s' and '%.100s'. Did you mean \"print(<message>, file=<output_stream>)\"?",
                op_name, tp_v->tp_name, tp_w->tp_name);
            return NULL;
        }
        _t3 = binop_type_error(po, v, w, op_name);
        return _t3;
    }
    return result;
}

DEFINEFN vinfo_t *
PsycoNumber_And(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op(po, v, w, offsetof(PyNumberMethods, nb_and), "&");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_Divmod(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 =
        binary_op(po, v, w, offsetof(PyNumberMethods, nb_divmod), "divmod()");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_FloorDivide(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 =
        binary_op(po, v, w, offsetof(PyNumberMethods, nb_floor_divide), "//");
    return _t1;
}

static vinfo_t *
binary_iop1(PsycoObject *po, vinfo_t *v, vinfo_t *w, int iop_slot, int op_slot)
{
    binaryfunc _t1;
    vinfo_t *_t2;
    vinfo_t *_t3;
    PyNumberMethods *mv;
    binaryfunc slot;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    vinfo_t *x;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    mv = tp_v->tp_as_number;
    if (mv != NULL) {
        switch (Psyco_TypeSlot(po, tp_v, (void *)&(((char *)mv))[iop_slot],
                               (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_binary_iop1, CfCommonCheckNotImplemented, "vvll",
                    v, w, (long)(iop_slot), (long)(op_slot));
            default:
                return NULL;
        }
        slot = _t1;
        if (slot != 0) {
            _t2 =
                Psyco_META2(po, slot, CfCommonCheckNotImplemented, "vv", v, w);
            x = _t2;
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
        }
    }
    _t3 = binary_op1(po, v, w, op_slot);
    return _t3;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceAdd(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    binaryfunc _t2;
    binaryfunc _t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
#if NI_HAS_CHECK_SLOT_RESULT
    binaryfunc func;
#else
    binaryfunc f;
#endif
    PySequenceMethods *m;
    vinfo_t *result;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop1(po, v, w, offsetof(PyNumberMethods, nb_inplace_add),
                      offsetof(PyNumberMethods, nb_add));
    result = _t1;
    if (!IS_IMPLEMENTED(result)) {
        m = tp_v->tp_as_sequence;
        vinfo_decref(result, po);
        if (m != NULL) {
#if !NI_HAS_CHECK_SLOT_RESULT
            f = NULL;
#endif
            switch (Psyco_TypeSlot(po, tp_v, &m->sq_inplace_concat,
                                   (void **)&_t2)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, cimpl_PyNumber_InPlaceAdd_1,
                                              CfCommonNewRefPyObject, "vvl", v,
                                              w, (long)(m));
                default:
                    return NULL;
            }
#if NI_HAS_CHECK_SLOT_RESULT
            func = _t2;
            if (func == NULL) {
                switch (
                    Psyco_TypeSlot(po, tp_v, &m->sq_concat, (void **)&_t3)) {
                    case 1:
                        break;
                    case 0:
                        return psyco_generic_call(
                            po, cimpl_PyNumber_InPlaceAdd_3,
                            CfCommonNewRefPyObject, "vvl", v, w, (long)(m));
                    default:
                        return NULL;
                }
                func = _t3;
            }
            if (func != NULL) {
                _t4 =
                    Psyco_META2(po, func, CfCommonNewRefPyObject, "vv", v, w);
                result = _t4;
                return result;
            }
#else
            f = _t2;
            if (f == NULL) {
                switch (
                    Psyco_TypeSlot(po, tp_v, &m->sq_concat, (void **)&_t3)) {
                    case 1:
                        break;
                    case 0:
                        return psyco_generic_call(
                            po, cimpl_PyNumber_InPlaceAdd_3,
                            CfCommonNewRefPyObject, "vvl", v, w, (long)(m));
                    default:
                        return NULL;
                }
                f = _t3;
            }
            if (f != NULL) {
                _t4 = Psyco_META2(po, f, CfCommonNewRefPyObject, "vv", v, w);
                return _t4;
            }
#endif
        }
        _t5 = binop_type_error(po, v, w, "+=");
        result = _t5;
        return result;
    }
    return result;
}

static vinfo_t *
binary_iop(PsycoObject *po, vinfo_t *v, vinfo_t *w, int iop_slot, int op_slot,
           const char *op_name)
{
    vinfo_t *_t1;
    vinfo_t *_t2;
    vinfo_t *result;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop1(po, v, w, iop_slot, op_slot);
    result = _t1;
    if (!IS_IMPLEMENTED(result)) {
        vinfo_decref(result, po);
        _t2 = binop_type_error(po, v, w, op_name);
        return _t2;
    }
    return result;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceAnd(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop(po, v, w, offsetof(PyNumberMethods, nb_inplace_and),
                     offsetof(PyNumberMethods, nb_and), "&=");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceFloorDivide(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop(po, v, w,
                     offsetof(PyNumberMethods, nb_inplace_floor_divide),
                     offsetof(PyNumberMethods, nb_floor_divide), "//=");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceLshift(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop(po, v, w, offsetof(PyNumberMethods, nb_inplace_lshift),
                     offsetof(PyNumberMethods, nb_lshift), "<<=");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceMatrixMultiply(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop(po, v, w,
                     offsetof(PyNumberMethods, nb_inplace_matrix_multiply),
                     offsetof(PyNumberMethods, nb_matrix_multiply), "@=");
    return _t1;
}

static vinfo_t *
sequence_repeat(PsycoObject *po, ssizeargfunc repeatfunc, vinfo_t *seq,
                vinfo_t *n)
{
    unaryfunc _t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *_t5;
#if !NI_HAS_CHECK_SLOT_RESULT
    vinfo_t *_t6;
#endif
    vinfo_t *count;
#if NI_HAS_CHECK_SLOT_RESULT
    vinfo_t *res;
#endif
    PyTypeObject *tp_n;
    tp_n = Psyco_NeedType(po, n);
    if (tp_n == NULL) {
        return NULL;
    }
    _t2 = (tp_n->tp_as_number) != NULL;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_n, &(tp_n->tp_as_number)->nb_index,
                               (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_sequence_repeat,
                                          CfCommonNewRefPyObject, "lvv",
                                          (long)(repeatfunc), seq, n);
            default:
                return NULL;
        }
        _t2 = _t1 != NULL;
    }
    if (_t2) {
        _t3 = PsycoNumber_AsSsize_t(po, n, PyExc_OverflowError);
        count = _t3;
        if (count == NULL) {
            return NULL;
        }
    }
    else {
        _t4 = type_error(
            po, "can't multiply sequence by non-int of type '%.200s'", n);
        return _t4;
    }
    _t5 =
        Psyco_META2(po, repeatfunc, CfCommonNewRefPyObject, "vv", seq, count);
#if NI_HAS_CHECK_SLOT_RESULT
    res = _t5;
#else
    _t6 = _t5;
#endif
    vinfo_decref(count, po);
#if NI_HAS_CHECK_SLOT_RESULT
    return res;
#else
    return _t6;
#endif
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceMultiply(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    ssizeargfunc _t2;
    ssizeargfunc _t3;
    vinfo_t *_t4;
    ssizeargfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeargfunc f;
    PySequenceMethods *mv;
    PySequenceMethods *mw;
    vinfo_t *result;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop1(po, v, w, offsetof(PyNumberMethods, nb_inplace_multiply),
                      offsetof(PyNumberMethods, nb_multiply));
    result = _t1;
    if (!IS_IMPLEMENTED(result)) {
        f = NULL;
        mv = tp_v->tp_as_sequence;
        mw = tp_w->tp_as_sequence;
        vinfo_decref(result, po);
        if (mv != NULL) {
            switch (Psyco_TypeSlot(po, tp_v, &mv->sq_inplace_repeat,
                                   (void **)&_t2)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(
                        po, cimpl_PyNumber_InPlaceMultiply_1,
                        CfCommonNewRefPyObject, "vvl", v, w, (long)(mv));
                default:
                    return NULL;
            }
            f = _t2;
            if (f == NULL) {
                switch (
                    Psyco_TypeSlot(po, tp_v, &mv->sq_repeat, (void **)&_t3)) {
                    case 1:
                        break;
                    case 0:
                        return psyco_generic_call(
                            po, cimpl_PyNumber_InPlaceMultiply_4,
                            CfCommonNewRefPyObject, "vvl", v, w, (long)(mv));
                    default:
                        return NULL;
                }
                f = _t3;
            }
            if (f != NULL) {
                _t4 = sequence_repeat(po, f, v, w);
                return _t4;
            }
        }
        else {
            if (mw != NULL) {
                switch (
                    Psyco_TypeSlot(po, tp_w, &mw->sq_repeat, (void **)&_t5)) {
                    case 1:
                        break;
                    case 0:
                        return psyco_generic_call(
                            po, cimpl_PyNumber_InPlaceMultiply_5,
                            CfCommonNewRefPyObject, "vvl", v, w, (long)(mw));
                    default:
                        return NULL;
                }
                if (_t5 != 0) {
                    _t6 = sequence_repeat(po, _t5, w, v);
                    return _t6;
                }
            }
        }
        _t7 = binop_type_error(po, v, w, "*=");
        result = _t7;
        return result;
    }
    return result;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceOr(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop(po, v, w, offsetof(PyNumberMethods, nb_inplace_or),
                     offsetof(PyNumberMethods, nb_or), "|=");
    return _t1;
}

static vinfo_t *
ternary_op(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t *z, int op_slot,
           const char *op_name)
#if NI_HAS_CHECK_SLOT_RESULT
{
    ternaryfunc _t1;
    ternaryfunc _t2;
    bool _t3;
    int _t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ternaryfunc _t8;
    vinfo_t *_t9;
    PyNumberMethods *mv;
    PyNumberMethods *mw;
    PyNumberMethods *mz;
    ternaryfunc slotv;
    ternaryfunc slotw;
    ternaryfunc slotz;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    PyTypeObject *tp_z;
    vinfo_t *x;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    tp_z = Psyco_NeedType(po, z);
    if (tp_z == NULL) {
        return NULL;
    }
    mv = tp_v->tp_as_number;
    mw = tp_w->tp_as_number;
    if (mv != NULL) {
        switch (Psyco_TypeSlot(po, tp_v, (void *)&(((char *)mv))[op_slot],
                               (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll", v,
                    w, z, (long)(op_slot), (long)(op_name));
            default:
                return NULL;
        }
        slotv = _t1;
    }
    else {
        slotv = NULL;
    }
    if ((!(tp_w == tp_v) && mw != NULL)) {
        switch (Psyco_TypeSlot(po, tp_w, (void *)&(((char *)mw))[op_slot],
                               (void **)&_t2)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll", v,
                    w, z, (long)(op_slot), (long)(op_name));
            default:
                return NULL;
        }
        slotw = _t2;
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    else {
        slotw = NULL;
    }
    if (slotv != 0) {
        _t4 = slotw != 0;
        if (_t4) {
            switch (Psyco_TypeIsSubtype(po, tp_w, tp_v, &_t3)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(
                        po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll",
                        v, w, z, (long)(op_slot), (long)(op_name));
                default:
                    return NULL;
            }
            _t4 = _t3 != 0;
        }
        if (_t4) {
            _t5 = Psyco_META3(po, slotw, CfCommonCheckNotImplemented, "vvv", v,
                              w, z);
            x = _t5;
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
            slotw = NULL;
        }
        _t6 = Psyco_META3(po, slotv, CfCommonCheckNotImplemented, "vvv", v, w,
                          z);
        x = _t6;
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po);
    }
    if (slotw != 0) {
        _t7 = Psyco_META3(po, slotw, CfCommonCheckNotImplemented, "vvv", v, w,
                          z);
        x = _t7;
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po);
    }
    mz = tp_z->tp_as_number;
    if (mz != NULL) {
        switch (Psyco_TypeSlot(po, tp_z, (void *)&(((char *)mz))[op_slot],
                               (void **)&_t8)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_op_1, CfCommonNewRefPyObject, "vvvlllll",
                    v, w, z, (long)(op_slot), (long)(op_name), (long)(mz),
                    (long)(slotv), (long)(slotw));
            default:
                return NULL;
        }
        slotz = _t8;
        if ((slotz == slotv || slotz == slotw)) {
            slotz = NULL;
        }
        if (slotz != 0) {
            _t9 = Psyco_META3(po, slotz, CfCommonCheckNotImplemented, "vvv", v,
                              w, z);
            x = _t9;
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
        }
    }
    return psyco_generic_call(po, cimpl_ternary_op_2, CfCommonNewRefPyObject,
                              "vvvl", v, w, z, (long)(op_name));
}
#elif NI_HAS_PY_IS_TYPE
{
    ternaryfunc _t1;
    ternaryfunc _t2;
    bool _t3;
    int _t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ternaryfunc _t8;
    vinfo_t *_t9;
    PyNumberMethods *mv;
    PyNumberMethods *mw;
    PyNumberMethods *mz;
    ternaryfunc slotv;
    ternaryfunc slotw;
    ternaryfunc slotz;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    PyTypeObject *tp_z;
    vinfo_t *x;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    tp_z = Psyco_NeedType(po, z);
    if (tp_z == NULL) {
        return NULL;
    }
    slotv = NULL;
    slotw = NULL;
    slotz = NULL;
    mv = tp_v->tp_as_number;
    mw = tp_w->tp_as_number;
    if (mv != NULL) {
        switch (Psyco_TypeSlot(po, tp_v, (void *)&(((char *)mv))[op_slot],
                               (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll", v,
                    w, z, (long)(op_slot), (long)(op_name));
            default:
                return NULL;
        }
        slotv = _t1;
    }
    if ((!(tp_w == tp_v) && mw != NULL)) {
        switch (Psyco_TypeSlot(po, tp_w, (void *)&(((char *)mw))[op_slot],
                               (void **)&_t2)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll", v,
                    w, z, (long)(op_slot), (long)(op_name));
            default:
                return NULL;
        }
        slotw = _t2;
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    if (slotv != 0) {
        _t4 = slotw != 0;
        if (_t4) {
            switch (Psyco_TypeIsSubtype(po, tp_w, tp_v, &_t3)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(
                        po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll",
                        v, w, z, (long)(op_slot), (long)(op_name));
                default:
                    return NULL;
            }
            _t4 = _t3 != 0;
        }
        if (_t4) {
            _t5 = Psyco_META3(po, slotw, CfCommonCheckNotImplemented, "vvv", v,
                              w, z);
            x = _t5;
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
            slotw = NULL;
        }
        _t6 = Psyco_META3(po, slotv, CfCommonCheckNotImplemented, "vvv", v, w,
                          z);
        x = _t6;
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po);
    }
    if (slotw != 0) {
        _t7 = Psyco_META3(po, slotw, CfCommonCheckNotImplemented, "vvv", v, w,
                          z);
        x = _t7;
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po);
    }
    mz = tp_z->tp_as_number;
    if (mz != NULL) {
        switch (Psyco_TypeSlot(po, tp_z, (void *)&(((char *)mz))[op_slot],
                               (void **)&_t8)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_ternary_op_1,
                                          CfCommonNewRefPyObject, "vvvllll", v,
                                          w, z, (long)(op_slot), (long)(mz),
                                          (long)(slotv), (long)(slotw));
            default:
                return NULL;
        }
        slotz = _t8;
        if ((slotz == slotv || slotz == slotw)) {
            slotz = NULL;
        }
        if (slotz != 0) {
            _t9 = Psyco_META3(po, slotz, CfCommonCheckNotImplemented, "vvv", v,
                              w, z);
            x = _t9;
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
        }
    }
    return psyco_generic_call(po, cimpl_ternary_op_2, CfCommonNewRefPyObject,
                              "vvv", v, w, z);
}
#else
{
    ternaryfunc _t1;
    ternaryfunc _t2;
    bool _t3;
    int _t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ternaryfunc _t8;
    vinfo_t *_t9;
    PyNumberMethods *mv;
    PyNumberMethods *mw;
    PyNumberMethods *mz;
    ternaryfunc slotv;
    ternaryfunc slotw;
    ternaryfunc slotz;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    PyTypeObject *tp_z;
    vinfo_t *x;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    tp_z = Psyco_NeedType(po, z);
    if (tp_z == NULL) {
        return NULL;
    }
    slotv = NULL;
    slotw = NULL;
    slotz = NULL;
    mv = tp_v->tp_as_number;
    mw = tp_w->tp_as_number;
    if (mv != NULL) {
        switch (Psyco_TypeSlot(po, tp_v, (void *)&(((char *)mv))[op_slot],
                               (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll", v,
                    w, z, (long)(op_slot), (long)(op_name));
            default:
                return NULL;
        }
        slotv = _t1;
    }
    if ((tp_w != tp_v && mw != NULL)) {
        switch (Psyco_TypeSlot(po, tp_w, (void *)&(((char *)mw))[op_slot],
                               (void **)&_t2)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll", v,
                    w, z, (long)(op_slot), (long)(op_name));
            default:
                return NULL;
        }
        slotw = _t2;
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    if (slotv != 0) {
        _t4 = slotw != 0;
        if (_t4) {
            switch (Psyco_TypeIsSubtype(po, tp_w, tp_v, &_t3)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(
                        po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll",
                        v, w, z, (long)(op_slot), (long)(op_name));
                default:
                    return NULL;
            }
            _t4 = _t3 != 0;
        }
        if (_t4) {
            _t5 = Psyco_META3(po, slotw, CfCommonCheckNotImplemented, "vvv", v,
                              w, z);
            x = _t5;
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
            slotw = NULL;
        }
        _t6 = Psyco_META3(po, slotv, CfCommonCheckNotImplemented, "vvv", v, w,
                          z);
        x = _t6;
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po);
    }
    if (slotw != 0) {
        _t7 = Psyco_META3(po, slotw, CfCommonCheckNotImplemented, "vvv", v, w,
                          z);
        x = _t7;
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po);
    }
    mz = tp_z->tp_as_number;
    if (mz != NULL) {
        switch (Psyco_TypeSlot(po, tp_z, (void *)&(((char *)mz))[op_slot],
                               (void **)&_t8)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_ternary_op_1,
                                          CfCommonNewRefPyObject, "vvvllll", v,
                                          w, z, (long)(op_slot), (long)(mz),
                                          (long)(slotv), (long)(slotw));
            default:
                return NULL;
        }
        slotz = _t8;
        if ((slotz == slotv || slotz == slotw)) {
            slotz = NULL;
        }
        if (slotz != 0) {
            _t9 = Psyco_META3(po, slotz, CfCommonCheckNotImplemented, "vvv", v,
                              w, z);
            x = _t9;
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
        }
    }
    return psyco_generic_call(po, cimpl_ternary_op_2, CfCommonNewRefPyObject,
                              "vvv", v, w, z);
}
#endif

#if NI_HAS_TERNARY_IOP
static vinfo_t *
ternary_iop(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t *z, int iop_slot,
            int op_slot, const char *op_name)
{
    ternaryfunc _t1;
    vinfo_t *_t2;
    vinfo_t *_t3;
    PyNumberMethods *mv;
    ternaryfunc slot;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    PyTypeObject *tp_z;
    vinfo_t *x;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    tp_z = Psyco_NeedType(po, z);
    if (tp_z == NULL) {
        return NULL;
    }
    mv = tp_v->tp_as_number;
    if (mv != NULL) {
        switch (Psyco_TypeSlot(po, tp_v, (void *)&(((char *)mv))[iop_slot],
                               (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_iop, CfCommonNewRefPyObject, "vvvlll", v,
                    w, z, (long)(iop_slot), (long)(op_slot), (long)(op_name));
            default:
                return NULL;
        }
        slot = _t1;
        if (slot != 0) {
            _t2 = Psyco_META3(po, slot, CfCommonCheckNotImplemented, "vvv", v,
                              w, z);
            x = _t2;
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
        }
    }
    _t3 = ternary_op(po, v, w, z, op_slot, op_name);
    return _t3;
}
#endif

#if NI_HAS_TERNARY_IOP
DEFINEFN vinfo_t *
PsycoNumber_InPlacePower(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t *z)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    PyTypeObject *tp_z;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    tp_z = Psyco_NeedType(po, z);
    if (tp_z == NULL) {
        return NULL;
    }
    _t1 = ternary_iop(po, v, w, z, offsetof(PyNumberMethods, nb_inplace_power),
                      offsetof(PyNumberMethods, nb_power), "**=");
    return _t1;
}
#else
DEFINEFN vinfo_t *
PsycoNumber_InPlacePower(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t *z)
{
    ternaryfunc _t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    PyTypeObject *tp_v;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    _t2 = (tp_v->tp_as_number) != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_v,
                               &(tp_v->tp_as_number)->nb_inplace_power,
                               (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyNumber_InPlacePower,
                                          CfCommonNewRefPyObject, "vvv", v, w,
                                          z);
            default:
                return NULL;
        }
        _t2 = _t1 != NULL;
    }
    if (_t2) {
        _t3 = ternary_op(po, v, w, z,
                         offsetof(PyNumberMethods, nb_inplace_power), "**=");
        return _t3;
    }
    else {
        _t4 = ternary_op(po, v, w, z, offsetof(PyNumberMethods, nb_power),
                         "**=");
        return _t4;
    }
}
#endif

DEFINEFN vinfo_t *
PsycoNumber_InPlaceRemainder(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop(po, v, w, offsetof(PyNumberMethods, nb_inplace_remainder),
                     offsetof(PyNumberMethods, nb_remainder), "%=");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceRshift(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop(po, v, w, offsetof(PyNumberMethods, nb_inplace_rshift),
                     offsetof(PyNumberMethods, nb_rshift), ">>=");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceSubtract(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop(po, v, w, offsetof(PyNumberMethods, nb_inplace_subtract),
                     offsetof(PyNumberMethods, nb_subtract), "-=");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceTrueDivide(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 =
        binary_iop(po, v, w, offsetof(PyNumberMethods, nb_inplace_true_divide),
                   offsetof(PyNumberMethods, nb_true_divide), "/=");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceXor(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_iop(po, v, w, offsetof(PyNumberMethods, nb_inplace_xor),
                     offsetof(PyNumberMethods, nb_xor), "^=");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_Invert(PsycoObject *po, vinfo_t *o)
{
    unaryfunc _t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    PyNumberMethods *m;
#if NI_HAS_CHECK_SLOT_RESULT
    vinfo_t *res;
#endif
    PyTypeObject *tp_o;
    tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL) {
        return NULL;
    }
    m = tp_o->tp_as_number;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_o, &m->nb_invert, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyNumber_Invert,
                                          CfCommonNewRefPyObject, "v", o);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = Psyco_META1(po, _t1, CfCommonNewRefPyObject, "v", o);
#if NI_HAS_CHECK_SLOT_RESULT
        res = _t3;
        return res;
#else
        return _t3;
#endif
    }
    _t4 = type_error(po, "bad operand type for unary ~: '%.200s'", o);
    return _t4;
}

DEFINEFN vinfo_t *
PsycoNumber_Lshift(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op(po, v, w, offsetof(PyNumberMethods, nb_lshift), "<<");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_MatrixMultiply(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op(po, v, w, offsetof(PyNumberMethods, nb_matrix_multiply),
                    "@");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_Multiply(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    ssizeargfunc _t2;
    int _t3;
    vinfo_t *_t4;
    ssizeargfunc _t5;
    int _t6;
    vinfo_t *_t7;
    vinfo_t *_t8;
    PySequenceMethods *mv;
    PySequenceMethods *mw;
    vinfo_t *result;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op1(po, v, w, offsetof(PyNumberMethods, nb_multiply));
    result = _t1;
    if (!IS_IMPLEMENTED(result)) {
        mv = tp_v->tp_as_sequence;
        mw = tp_w->tp_as_sequence;
        vinfo_decref(result, po);
        _t3 = mv != 0;
        if (_t3) {
            switch (Psyco_TypeSlot(po, tp_v, &mv->sq_repeat, (void **)&_t2)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, cimpl_PyNumber_Multiply_1,
                                              CfCommonNewRefPyObject, "vvll",
                                              v, w, (long)(mv), (long)(mw));
                default:
                    return NULL;
            }
            _t3 = _t2 != 0;
        }
        if (_t3) {
            _t4 = sequence_repeat(po, _t2, v, w);
            return _t4;
        }
        else {
            _t6 = mw != 0;
            if (_t6) {
                switch (
                    Psyco_TypeSlot(po, tp_w, &mw->sq_repeat, (void **)&_t5)) {
                    case 1:
                        break;
                    case 0:
                        return psyco_generic_call(
                            po, cimpl_PyNumber_Multiply_3,
                            CfCommonNewRefPyObject, "vvl", v, w, (long)(mw));
                    default:
                        return NULL;
                }
                _t6 = _t5 != 0;
            }
            if (_t6) {
                _t7 = sequence_repeat(po, _t5, w, v);
                return _t7;
            }
        }
        _t8 = binop_type_error(po, v, w, "*");
        result = _t8;
        return result;
    }
    return result;
}

DEFINEFN vinfo_t *
PsycoNumber_Negative(PsycoObject *po, vinfo_t *o)
{
    unaryfunc _t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    PyNumberMethods *m;
#if NI_HAS_CHECK_SLOT_RESULT
    vinfo_t *res;
#endif
    PyTypeObject *tp_o;
    tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL) {
        return NULL;
    }
    m = tp_o->tp_as_number;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_o, &m->nb_negative, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyNumber_Negative,
                                          CfCommonNewRefPyObject, "v", o);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = Psyco_META1(po, _t1, CfCommonNewRefPyObject, "v", o);
#if NI_HAS_CHECK_SLOT_RESULT
        res = _t3;
        return res;
#else
        return _t3;
#endif
    }
    _t4 = type_error(po, "bad operand type for unary -: '%.200s'", o);
    return _t4;
}

DEFINEFN vinfo_t *
PsycoNumber_Or(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op(po, v, w, offsetof(PyNumberMethods, nb_or), "|");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_Positive(PsycoObject *po, vinfo_t *o)
{
    unaryfunc _t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    PyNumberMethods *m;
#if NI_HAS_CHECK_SLOT_RESULT
    vinfo_t *res;
#endif
    PyTypeObject *tp_o;
    tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL) {
        return NULL;
    }
    m = tp_o->tp_as_number;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_o, &m->nb_positive, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyNumber_Positive,
                                          CfCommonNewRefPyObject, "v", o);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = Psyco_META1(po, _t1, CfCommonNewRefPyObject, "v", o);
#if NI_HAS_CHECK_SLOT_RESULT
        res = _t3;
        return res;
#else
        return _t3;
#endif
    }
    _t4 = type_error(po, "bad operand type for unary +: '%.200s'", o);
    return _t4;
}

DEFINEFN vinfo_t *
PsycoNumber_Remainder(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op(po, v, w, offsetof(PyNumberMethods, nb_remainder), "%");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_Rshift(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op(po, v, w, offsetof(PyNumberMethods, nb_rshift), ">>");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_Subtract(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op(po, v, w, offsetof(PyNumberMethods, nb_subtract), "-");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_TrueDivide(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op(po, v, w, offsetof(PyNumberMethods, nb_true_divide), "/");
    return _t1;
}

DEFINEFN vinfo_t *
PsycoNumber_Xor(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    vinfo_t *_t1;
    PyTypeObject *tp_v;
    PyTypeObject *tp_w;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL) {
        return NULL;
    }
    _t1 = binary_op(po, v, w, offsetof(PyNumberMethods, nb_xor), "^");
    return _t1;
}

#if NI_HAS_CHECK_SLOT_RESULT
DEFINEFN bool
PsycoSequence_DelItem(PsycoObject *po, vinfo_t *s, vinfo_t *i)
{
    ssizeobjargproc _t1;
    objobjargproc _t10;
    int _t11;
    int _t2;
    condition_code_t _t3;
    int _t4;
    lenfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeobjargproc _t8;
    bool _t9;
    vinfo_t *l;
    PySequenceMethods *m;
    vinfo_t *own_i;
    vinfo_t *res;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return false;
    }
    m = tp_s->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_DelItem,
                                          CfCommonIntZeroOk, "vv", s,
                                          i) != NULL;
            default:
                return false;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = integer_cmp_i(po, i, 0, Py_LT);
        if (_t3 == CC_ERROR) {
            return false;
        }
        _t4 = runtime_condition_f(po, _t3);
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&_t5)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_DelItem,
                                              CfCommonIntZeroOk, "vv", s,
                                              i) != NULL;
                default:
                    return false;
            }
            if (_t5 != 0) {
                _t6 = Psyco_META1(po, _t5, CfCommonPySSizeTResult, "v", s);
                l = _t6;
                if (l == NULL) {
                    return false;
                }
                _t7 = integer_add(po, i, l, false);
                i = _t7;
                if (i == NULL) {
                    vinfo_decref(l, po);
                    return false;
                }
                vinfo_decref(l, po);
                own_i = i;
            }
            else {
                own_i = NULL;
            }
        }
        else {
            assert_nonneg(i);
            own_i = NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t8)) {
            case 1:
                break;
            case 0:
                _t9 = psyco_generic_call(po, cimpl_PySequence_DelItem_1,
                                         CfCommonIntZeroOk, "vvl", s, i,
                                         (long)(m)) != NULL;
                vinfo_xdecref(own_i, po);
                return _t9;
            default:
                vinfo_xdecref(own_i, po);
                return false;
        }
        res = Psyco_META3(po, _t8, CfCommonIntZeroOk, "vvl", s, i,
                          (long)(((PyObject *)NULL)));
        vinfo_xdecref(own_i, po);
        return res != NULL;
    }
    _t11 = (tp_s->tp_as_mapping) != 0;
    if (_t11) {
        switch (Psyco_TypeSlot(po, tp_s,
                               &(tp_s->tp_as_mapping)->mp_ass_subscript,
                               (void **)&_t10)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_DelItem,
                                          CfCommonIntZeroOk, "vv", s,
                                          i) != NULL;
            default:
                return false;
        }
        _t11 = _t10 != 0;
    }
    if (_t11) {
        type_error(po, "%.200s is not a sequence", s);
        return false;
    }
    type_error(po, "'%.200s' object doesn't support item deletion", s);
    return false;
}
#elif NI_HAS_NOT_A_SEQUENCE_ERROR
DEFINEFN bool
PsycoSequence_DelItem(PsycoObject *po, vinfo_t *s, vinfo_t *i)
{
    ssizeobjargproc _t1;
    bool _t10;
    objobjargproc _t11;
    int _t12;
    int _t2;
    condition_code_t _t3;
    int _t4;
    lenfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeobjargproc _t8;
    bool _t9;
    vinfo_t *l;
    PySequenceMethods *m;
    vinfo_t *own_i;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return false;
    }
    m = tp_s->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_DelItem,
                                          CfCommonIntZeroOk, "vv", s,
                                          i) != NULL;
            default:
                return false;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = integer_cmp_i(po, i, 0, Py_LT);
        if (_t3 == CC_ERROR) {
            return false;
        }
        _t4 = runtime_condition_f(po, _t3);
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&_t5)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_DelItem,
                                              CfCommonIntZeroOk, "vv", s,
                                              i) != NULL;
                default:
                    return false;
            }
            if (_t5 != 0) {
                _t6 = Psyco_META1(po, _t5, CfCommonPySSizeTResult, "v", s);
                l = _t6;
                if (l == NULL) {
                    return false;
                }
                _t7 = integer_add(po, i, l, false);
                i = _t7;
                if (i == NULL) {
                    vinfo_decref(l, po);
                    return false;
                }
                vinfo_decref(l, po);
                own_i = i;
            }
            else {
                own_i = NULL;
            }
        }
        else {
            assert_nonneg(i);
            own_i = NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t8)) {
            case 1:
                break;
            case 0:
                _t9 = psyco_generic_call(po, cimpl_PySequence_DelItem_1,
                                         CfCommonIntZeroOk, "vvl", s, i,
                                         (long)(m)) != NULL;
                vinfo_xdecref(own_i, po);
                return _t9;
            default:
                vinfo_xdecref(own_i, po);
                return false;
        }
        _t10 = Psyco_META3(po, _t8, CfCommonIntZeroOk, "vvl", s, i,
                           (long)(((PyObject *)NULL))) != NULL;
        vinfo_xdecref(own_i, po);
        return _t10;
    }
    _t12 = (tp_s->tp_as_mapping) != 0;
    if (_t12) {
        switch (Psyco_TypeSlot(po, tp_s,
                               &(tp_s->tp_as_mapping)->mp_ass_subscript,
                               (void **)&_t11)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_DelItem,
                                          CfCommonIntZeroOk, "vv", s,
                                          i) != NULL;
            default:
                return false;
        }
        _t12 = _t11 != 0;
    }
    if (_t12) {
        type_error(po, "%.200s is not a sequence", s);
        return false;
    }
    type_error(po, "'%.200s' object doesn't support item deletion", s);
    return false;
}
#else
DEFINEFN bool
PsycoSequence_DelItem(PsycoObject *po, vinfo_t *s, vinfo_t *i)
{
    ssizeobjargproc _t1;
    bool _t10;
    int _t2;
    condition_code_t _t3;
    int _t4;
    lenfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeobjargproc _t8;
    bool _t9;
    vinfo_t *l;
    PySequenceMethods *m;
    vinfo_t *own_i;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return false;
    }
    m = tp_s->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_DelItem,
                                          CfCommonIntZeroOk, "vv", s,
                                          i) != NULL;
            default:
                return false;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = integer_cmp_i(po, i, 0, Py_LT);
        if (_t3 == CC_ERROR) {
            return false;
        }
        _t4 = runtime_condition_f(po, _t3);
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&_t5)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_DelItem,
                                              CfCommonIntZeroOk, "vv", s,
                                              i) != NULL;
                default:
                    return false;
            }
            if (_t5 != 0) {
                _t6 = Psyco_META1(po, _t5, CfCommonPySSizeTResult, "v", s);
                l = _t6;
                if (l == NULL) {
                    return false;
                }
                _t7 = integer_add(po, i, l, false);
                i = _t7;
                if (i == NULL) {
                    vinfo_decref(l, po);
                    return false;
                }
                vinfo_decref(l, po);
                own_i = i;
            }
            else {
                own_i = NULL;
            }
        }
        else {
            assert_nonneg(i);
            own_i = NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t8)) {
            case 1:
                break;
            case 0:
                _t9 = psyco_generic_call(po, cimpl_PySequence_DelItem_1,
                                         CfCommonIntZeroOk, "vvl", s, i,
                                         (long)(m)) != NULL;
                vinfo_xdecref(own_i, po);
                return _t9;
            default:
                vinfo_xdecref(own_i, po);
                return false;
        }
        _t10 = Psyco_META3(po, _t8, CfCommonIntZeroOk, "vvl", s, i,
                           (long)(((PyObject *)NULL))) != NULL;
        vinfo_xdecref(own_i, po);
        return _t10;
    }
    type_error(po, "'%.200s' object doesn't support item deletion", s);
    return false;
}
#endif

DEFINEFN bool
PsycoObject_DelItem(PsycoObject *po, vinfo_t *o, vinfo_t *key)
{
    objobjargproc _t1;
    int _t2;
    unaryfunc _t3;
    int _t4;
    vinfo_t *_t5;
    bool _t6;
    ssizeobjargproc _t7;
    vinfo_t *key_value;
    PyMappingMethods *m;
#if NI_HAS_CHECK_SLOT_RESULT
    vinfo_t *res;
#endif
    PyTypeObject *tp_key;
    PyTypeObject *tp_o;
    tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL) {
        return false;
    }
    m = tp_o->tp_as_mapping;
    _t2 = m != 0;
    if (_t2) {
        switch (
            Psyco_TypeSlot(po, tp_o, &m->mp_ass_subscript, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_DelItem,
                                          CfCommonIntZeroOk, "vv", o,
                                          key) != NULL;
            default:
                return false;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
#if NI_HAS_CHECK_SLOT_RESULT
        res = Psyco_META3(po, _t1, CfCommonIntZeroOk, "vvl", o, key,
                          (long)(((PyObject *)NULL)));
        return res != NULL;
#else
        return Psyco_META3(po, _t1, CfCommonIntZeroOk, "vvl", o, key,
                           (long)(((PyObject *)NULL))) != NULL;
#endif
    }
    if ((tp_o->tp_as_sequence) != 0) {
        tp_key = Psyco_NeedType(po, key);
        if (tp_key == NULL) {
            return false;
        }
        _t4 = (tp_key->tp_as_number) != NULL;
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_key,
                                   &(tp_key->tp_as_number)->nb_index,
                                   (void **)&_t3)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_DelItem,
                                              CfCommonIntZeroOk, "vv", o,
                                              key) != NULL;
                default:
                    return false;
            }
            _t4 = _t3 != NULL;
        }
        if (_t4) {
            _t5 = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
            key_value = _t5;
            if (key_value == NULL) {
                return false;
            }
            _t6 = PsycoSequence_DelItem(po, o, key_value);
            vinfo_decref(key_value, po);
            return _t6;
        }
        else {
            switch (Psyco_TypeSlot(po, tp_o,
                                   &(tp_o->tp_as_sequence)->sq_ass_item,
                                   (void **)&_t7)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_DelItem,
                                              CfCommonIntZeroOk, "vv", o,
                                              key) != NULL;
                default:
                    return false;
            }
            if (_t7 != 0) {
                type_error(po, "sequence index must be integer, not '%.200s'",
                           key);
                return false;
            }
        }
    }
    type_error(po, "'%.200s' object does not support item deletion", o);
    return false;
}

#if NI_HAS_CHECK_SLOT_RESULT
DEFINEFN vinfo_t *
PsycoSequence_GetItem(PsycoObject *po, vinfo_t *s, vinfo_t *i)
{
    ssizeargfunc _t1;
    vinfo_t *_t10;
    binaryfunc _t11;
    int _t12;
    vinfo_t *_t13;
    vinfo_t *_t14;
    int _t2;
    condition_code_t _t3;
    int _t4;
    lenfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeargfunc _t8;
    vinfo_t *_t9;
    vinfo_t *l;
    PySequenceMethods *m;
    vinfo_t *own_i;
    vinfo_t *res;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return NULL;
    }
    m = tp_s->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_item, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_GetItem,
                                          CfCommonNewRefPyObject, "vv", s, i);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = integer_cmp_i(po, i, 0, Py_LT);
        if (_t3 == CC_ERROR) {
            return NULL;
        }
        _t4 = runtime_condition_f(po, _t3);
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&_t5)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_GetItem,
                                              CfCommonNewRefPyObject, "vv", s,
                                              i);
                default:
                    return NULL;
            }
            if (_t5 != 0) {
                _t6 = Psyco_META1(po, _t5, CfCommonPySSizeTResult, "v", s);
                l = _t6;
                if (l == NULL) {
                    return NULL;
                }
                _t7 = integer_add(po, i, l, false);
                i = _t7;
                if (i == NULL) {
                    vinfo_decref(l, po);
                    return NULL;
                }
                vinfo_decref(l, po);
                own_i = i;
            }
            else {
                own_i = NULL;
            }
        }
        else {
            assert_nonneg(i);
            own_i = NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_item, (void **)&_t8)) {
            case 1:
                break;
            case 0:
                _t9 = psyco_generic_call(po, cimpl_PySequence_GetItem_1,
                                         CfCommonNewRefPyObject, "vvl", s, i,
                                         (long)(m));
                vinfo_xdecref(own_i, po);
                return _t9;
            default:
                vinfo_xdecref(own_i, po);
                return NULL;
        }
        _t10 = Psyco_META2(po, _t8, CfCommonNewRefPyObject, "vv", s, i);
        res = _t10;
        vinfo_xdecref(own_i, po);
        return res;
    }
    _t12 = (tp_s->tp_as_mapping) != 0;
    if (_t12) {
        switch (Psyco_TypeSlot(po, tp_s, &(tp_s->tp_as_mapping)->mp_subscript,
                               (void **)&_t11)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_GetItem,
                                          CfCommonNewRefPyObject, "vv", s, i);
            default:
                return NULL;
        }
        _t12 = _t11 != 0;
    }
    if (_t12) {
        _t13 = type_error(po, "%.200s is not a sequence", s);
        return _t13;
    }
    _t14 = type_error(po, "'%.200s' object does not support indexing", s);
    return _t14;
}
#elif NI_HAS_NOT_A_SEQUENCE_ERROR
DEFINEFN vinfo_t *
PsycoSequence_GetItem(PsycoObject *po, vinfo_t *s, vinfo_t *i)
{
    ssizeargfunc _t1;
    vinfo_t *_t10;
    vinfo_t *_t11;
    binaryfunc _t12;
    int _t13;
    vinfo_t *_t14;
    vinfo_t *_t15;
    int _t2;
    condition_code_t _t3;
    int _t4;
    lenfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeargfunc _t8;
    vinfo_t *_t9;
    vinfo_t *l;
    PySequenceMethods *m;
    vinfo_t *own_i;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return NULL;
    }
    m = tp_s->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_item, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_GetItem,
                                          CfCommonNewRefPyObject, "vv", s, i);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = integer_cmp_i(po, i, 0, Py_LT);
        if (_t3 == CC_ERROR) {
            return NULL;
        }
        _t4 = runtime_condition_f(po, _t3);
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&_t5)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_GetItem,
                                              CfCommonNewRefPyObject, "vv", s,
                                              i);
                default:
                    return NULL;
            }
            if (_t5 != 0) {
                _t6 = Psyco_META1(po, _t5, CfCommonPySSizeTResult, "v", s);
                l = _t6;
                if (l == NULL) {
                    return NULL;
                }
                _t7 = integer_add(po, i, l, false);
                i = _t7;
                if (i == NULL) {
                    vinfo_decref(l, po);
                    return NULL;
                }
                vinfo_decref(l, po);
                own_i = i;
            }
            else {
                own_i = NULL;
            }
        }
        else {
            assert_nonneg(i);
            own_i = NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_item, (void **)&_t8)) {
            case 1:
                break;
            case 0:
                _t9 = psyco_generic_call(po, cimpl_PySequence_GetItem_1,
                                         CfCommonNewRefPyObject, "vvl", s, i,
                                         (long)(m));
                vinfo_xdecref(own_i, po);
                return _t9;
            default:
                vinfo_xdecref(own_i, po);
                return NULL;
        }
        _t10 = Psyco_META2(po, _t8, CfCommonNewRefPyObject, "vv", s, i);
        _t11 = _t10;
        vinfo_xdecref(own_i, po);
        return _t11;
    }
    _t13 = (tp_s->tp_as_mapping) != 0;
    if (_t13) {
        switch (Psyco_TypeSlot(po, tp_s, &(tp_s->tp_as_mapping)->mp_subscript,
                               (void **)&_t12)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_GetItem,
                                          CfCommonNewRefPyObject, "vv", s, i);
            default:
                return NULL;
        }
        _t13 = _t12 != 0;
    }
    if (_t13) {
        _t14 = type_error(po, "%.200s is not a sequence", s);
        return _t14;
    }
    _t15 = type_error(po, "'%.200s' object does not support indexing", s);
    return _t15;
}
#else
DEFINEFN vinfo_t *
PsycoSequence_GetItem(PsycoObject *po, vinfo_t *s, vinfo_t *i)
{
    ssizeargfunc _t1;
    vinfo_t *_t10;
    vinfo_t *_t11;
    vinfo_t *_t12;
    int _t2;
    condition_code_t _t3;
    int _t4;
    lenfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeargfunc _t8;
    vinfo_t *_t9;
    vinfo_t *l;
    PySequenceMethods *m;
    vinfo_t *own_i;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return NULL;
    }
    m = tp_s->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_item, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_GetItem,
                                          CfCommonNewRefPyObject, "vv", s, i);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = integer_cmp_i(po, i, 0, Py_LT);
        if (_t3 == CC_ERROR) {
            return NULL;
        }
        _t4 = runtime_condition_f(po, _t3);
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&_t5)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_GetItem,
                                              CfCommonNewRefPyObject, "vv", s,
                                              i);
                default:
                    return NULL;
            }
            if (_t5 != 0) {
                _t6 = Psyco_META1(po, _t5, CfCommonPySSizeTResult, "v", s);
                l = _t6;
                if (l == NULL) {
                    return NULL;
                }
                _t7 = integer_add(po, i, l, false);
                i = _t7;
                if (i == NULL) {
                    vinfo_decref(l, po);
                    return NULL;
                }
                vinfo_decref(l, po);
                own_i = i;
            }
            else {
                own_i = NULL;
            }
        }
        else {
            assert_nonneg(i);
            own_i = NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_item, (void **)&_t8)) {
            case 1:
                break;
            case 0:
                _t9 = psyco_generic_call(po, cimpl_PySequence_GetItem_1,
                                         CfCommonNewRefPyObject, "vvl", s, i,
                                         (long)(m));
                vinfo_xdecref(own_i, po);
                return _t9;
            default:
                vinfo_xdecref(own_i, po);
                return NULL;
        }
        _t10 = Psyco_META2(po, _t8, CfCommonNewRefPyObject, "vv", s, i);
        _t11 = _t10;
        vinfo_xdecref(own_i, po);
        return _t11;
    }
    _t12 = type_error(po, "'%.200s' object does not support indexing", s);
    return _t12;
}
#endif

DEFINEFN vinfo_t *
PsycoObject_GetItem(PsycoObject *po, vinfo_t *o, vinfo_t *key)
{
    binaryfunc _t1;
    vinfo_t *_t10;
    vinfo_t *_t11;
    vinfo_t *_t12;
    int _t2;
    vinfo_t *_t3;
    ssizeargfunc _t4;
    int _t5;
    unaryfunc _t6;
    int _t7;
    vinfo_t *_t8;
    vinfo_t *_t9;
    vinfo_t *item;
    vinfo_t *key_value;
    PyMappingMethods *m;
    PySequenceMethods *ms;
    PyTypeObject *tp_key;
    PyTypeObject *tp_o;
    tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL) {
        return NULL;
    }
    m = tp_o->tp_as_mapping;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_o, &m->mp_subscript, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_GetItem,
                                          CfCommonNewRefPyObject, "vv", o,
                                          key);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = Psyco_META2(po, _t1, CfCommonNewRefPyObject, "vv", o, key);
        item = _t3;
        return item;
    }
    ms = tp_o->tp_as_sequence;
    _t5 = ms != 0;
    if (_t5) {
        switch (Psyco_TypeSlot(po, tp_o, &ms->sq_item, (void **)&_t4)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_GetItem,
                                          CfCommonNewRefPyObject, "vv", o,
                                          key);
            default:
                return NULL;
        }
        _t5 = _t4 != 0;
    }
    if (_t5) {
        tp_key = Psyco_NeedType(po, key);
        if (tp_key == NULL) {
            return NULL;
        }
        _t7 = (tp_key->tp_as_number) != NULL;
        if (_t7) {
            switch (Psyco_TypeSlot(po, tp_key,
                                   &(tp_key->tp_as_number)->nb_index,
                                   (void **)&_t6)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_GetItem,
                                              CfCommonNewRefPyObject, "vv", o,
                                              key);
                default:
                    return NULL;
            }
            _t7 = _t6 != NULL;
        }
        if (_t7) {
            _t8 = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
            key_value = _t8;
            if (key_value == NULL) {
                return NULL;
            }
            _t9 = PsycoSequence_GetItem(po, o, key_value);
            _t10 = _t9;
            vinfo_decref(key_value, po);
            return _t10;
        }
        else {
            _t11 = type_error(
                po, "sequence index must be integer, not '%.200s'", key);
            return _t11;
        }
    }
    if (((tp_o->tp_flags & (1UL << 31))) != 0) {
        return psyco_generic_call(po, PyObject_GetItem, CfCommonNewRefPyObject,
                                  "vv", o, key);
    }
    _t12 = type_error(po, "'%.200s' object is not subscriptable", o);
    return _t12;
}

#if NI_HAS_CHECK_SLOT_RESULT
DEFINEFN bool
PsycoSequence_SetItem(PsycoObject *po, vinfo_t *s, vinfo_t *i, vinfo_t *o)
{
    ssizeobjargproc _t1;
    objobjargproc _t10;
    int _t11;
    int _t2;
    condition_code_t _t3;
    int _t4;
    lenfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeobjargproc _t8;
    bool _t9;
    vinfo_t *l;
    PySequenceMethods *m;
    vinfo_t *own_i;
    vinfo_t *res;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return false;
    }
    m = tp_s->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_SetItem,
                                          CfCommonIntZeroOk, "vvv", s, i,
                                          o) != NULL;
            default:
                return false;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = integer_cmp_i(po, i, 0, Py_LT);
        if (_t3 == CC_ERROR) {
            return false;
        }
        _t4 = runtime_condition_f(po, _t3);
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&_t5)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_SetItem,
                                              CfCommonIntZeroOk, "vvv", s, i,
                                              o) != NULL;
                default:
                    return false;
            }
            if (_t5 != 0) {
                _t6 = Psyco_META1(po, _t5, CfCommonPySSizeTResult, "v", s);
                l = _t6;
                if (l == NULL) {
                    return false;
                }
                _t7 = integer_add(po, i, l, false);
                i = _t7;
                if (i == NULL) {
                    vinfo_decref(l, po);
                    return false;
                }
                vinfo_decref(l, po);
                own_i = i;
            }
            else {
                own_i = NULL;
            }
        }
        else {
            assert_nonneg(i);
            own_i = NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t8)) {
            case 1:
                break;
            case 0:
                _t9 = psyco_generic_call(po, cimpl_PySequence_SetItem_1,
                                         CfCommonIntZeroOk, "vvvl", s, i, o,
                                         (long)(m)) != NULL;
                vinfo_xdecref(own_i, po);
                return _t9;
            default:
                vinfo_xdecref(own_i, po);
                return false;
        }
        res = Psyco_META3(po, _t8, CfCommonIntZeroOk, "vvv", s, i, o);
        vinfo_xdecref(own_i, po);
        return res != NULL;
    }
    _t11 = (tp_s->tp_as_mapping) != 0;
    if (_t11) {
        switch (Psyco_TypeSlot(po, tp_s,
                               &(tp_s->tp_as_mapping)->mp_ass_subscript,
                               (void **)&_t10)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_SetItem,
                                          CfCommonIntZeroOk, "vvv", s, i,
                                          o) != NULL;
            default:
                return false;
        }
        _t11 = _t10 != 0;
    }
    if (_t11) {
        type_error(po, "%.200s is not a sequence", s);
        return false;
    }
    type_error(po, "'%.200s' object does not support item assignment", s);
    return false;
}
#elif NI_HAS_NOT_A_SEQUENCE_ERROR
DEFINEFN bool
PsycoSequence_SetItem(PsycoObject *po, vinfo_t *s, vinfo_t *i, vinfo_t *o)
{
    ssizeobjargproc _t1;
    bool _t10;
    objobjargproc _t11;
    int _t12;
    int _t2;
    condition_code_t _t3;
    int _t4;
    lenfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeobjargproc _t8;
    bool _t9;
    vinfo_t *l;
    PySequenceMethods *m;
    vinfo_t *own_i;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return false;
    }
    m = tp_s->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_SetItem,
                                          CfCommonIntZeroOk, "vvv", s, i,
                                          o) != NULL;
            default:
                return false;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = integer_cmp_i(po, i, 0, Py_LT);
        if (_t3 == CC_ERROR) {
            return false;
        }
        _t4 = runtime_condition_f(po, _t3);
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&_t5)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_SetItem,
                                              CfCommonIntZeroOk, "vvv", s, i,
                                              o) != NULL;
                default:
                    return false;
            }
            if (_t5 != 0) {
                _t6 = Psyco_META1(po, _t5, CfCommonPySSizeTResult, "v", s);
                l = _t6;
                if (l == NULL) {
                    return false;
                }
                _t7 = integer_add(po, i, l, false);
                i = _t7;
                if (i == NULL) {
                    vinfo_decref(l, po);
                    return false;
                }
                vinfo_decref(l, po);
                own_i = i;
            }
            else {
                own_i = NULL;
            }
        }
        else {
            assert_nonneg(i);
            own_i = NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t8)) {
            case 1:
                break;
            case 0:
                _t9 = psyco_generic_call(po, cimpl_PySequence_SetItem_1,
                                         CfCommonIntZeroOk, "vvvl", s, i, o,
                                         (long)(m)) != NULL;
                vinfo_xdecref(own_i, po);
                return _t9;
            default:
                vinfo_xdecref(own_i, po);
                return false;
        }
        _t10 = Psyco_META3(po, _t8, CfCommonIntZeroOk, "vvv", s, i, o) != NULL;
        vinfo_xdecref(own_i, po);
        return _t10;
    }
    _t12 = (tp_s->tp_as_mapping) != 0;
    if (_t12) {
        switch (Psyco_TypeSlot(po, tp_s,
                               &(tp_s->tp_as_mapping)->mp_ass_subscript,
                               (void **)&_t11)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_SetItem,
                                          CfCommonIntZeroOk, "vvv", s, i,
                                          o) != NULL;
            default:
                return false;
        }
        _t12 = _t11 != 0;
    }
    if (_t12) {
        type_error(po, "%.200s is not a sequence", s);
        return false;
    }
    type_error(po, "'%.200s' object does not support item assignment", s);
    return false;
}
#else
DEFINEFN bool
PsycoSequence_SetItem(PsycoObject *po, vinfo_t *s, vinfo_t *i, vinfo_t *o)
{
    ssizeobjargproc _t1;
    bool _t10;
    int _t2;
    condition_code_t _t3;
    int _t4;
    lenfunc _t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    ssizeobjargproc _t8;
    bool _t9;
    vinfo_t *l;
    PySequenceMethods *m;
    vinfo_t *own_i;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return false;
    }
    m = tp_s->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_SetItem,
                                          CfCommonIntZeroOk, "vvv", s, i,
                                          o) != NULL;
            default:
                return false;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = integer_cmp_i(po, i, 0, Py_LT);
        if (_t3 == CC_ERROR) {
            return false;
        }
        _t4 = runtime_condition_f(po, _t3);
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&_t5)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_SetItem,
                                              CfCommonIntZeroOk, "vvv", s, i,
                                              o) != NULL;
                default:
                    return false;
            }
            if (_t5 != 0) {
                _t6 = Psyco_META1(po, _t5, CfCommonPySSizeTResult, "v", s);
                l = _t6;
                if (l == NULL) {
                    return false;
                }
                _t7 = integer_add(po, i, l, false);
                i = _t7;
                if (i == NULL) {
                    vinfo_decref(l, po);
                    return false;
                }
                vinfo_decref(l, po);
                own_i = i;
            }
            else {
                own_i = NULL;
            }
        }
        else {
            assert_nonneg(i);
            own_i = NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&_t8)) {
            case 1:
                break;
            case 0:
                _t9 = psyco_generic_call(po, cimpl_PySequence_SetItem_1,
                                         CfCommonIntZeroOk, "vvvl", s, i, o,
                                         (long)(m)) != NULL;
                vinfo_xdecref(own_i, po);
                return _t9;
            default:
                vinfo_xdecref(own_i, po);
                return false;
        }
        _t10 = Psyco_META3(po, _t8, CfCommonIntZeroOk, "vvv", s, i, o) != NULL;
        vinfo_xdecref(own_i, po);
        return _t10;
    }
    type_error(po, "'%.200s' object does not support item assignment", s);
    return false;
}
#endif

DEFINEFN bool
PsycoObject_SetItem(PsycoObject *po, vinfo_t *o, vinfo_t *key, vinfo_t *value)
{
    objobjargproc _t1;
    int _t2;
    unaryfunc _t3;
    int _t4;
    vinfo_t *_t5;
    bool _t6;
    ssizeobjargproc _t7;
    vinfo_t *key_value;
    PyMappingMethods *m;
#if NI_HAS_CHECK_SLOT_RESULT
    vinfo_t *res;
#endif
    PyTypeObject *tp_key;
    PyTypeObject *tp_o;
    tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL) {
        return false;
    }
    m = tp_o->tp_as_mapping;
    _t2 = m != 0;
    if (_t2) {
        switch (
            Psyco_TypeSlot(po, tp_o, &m->mp_ass_subscript, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_SetItem,
                                          CfCommonIntZeroOk, "vvv", o, key,
                                          value) != NULL;
            default:
                return false;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
#if NI_HAS_CHECK_SLOT_RESULT
        res = Psyco_META3(po, _t1, CfCommonIntZeroOk, "vvv", o, key, value);
        return res != NULL;
#else
        return Psyco_META3(po, _t1, CfCommonIntZeroOk, "vvv", o, key, value) !=
               NULL;
#endif
    }
    if ((tp_o->tp_as_sequence) != 0) {
        tp_key = Psyco_NeedType(po, key);
        if (tp_key == NULL) {
            return false;
        }
        _t4 = (tp_key->tp_as_number) != NULL;
        if (_t4) {
            switch (Psyco_TypeSlot(po, tp_key,
                                   &(tp_key->tp_as_number)->nb_index,
                                   (void **)&_t3)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_SetItem,
                                              CfCommonIntZeroOk, "vvv", o, key,
                                              value) != NULL;
                default:
                    return false;
            }
            _t4 = _t3 != NULL;
        }
        if (_t4) {
            _t5 = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
            key_value = _t5;
            if (key_value == NULL) {
                return false;
            }
            _t6 = PsycoSequence_SetItem(po, o, key_value, value);
            vinfo_decref(key_value, po);
            return _t6;
        }
        else {
            switch (Psyco_TypeSlot(po, tp_o,
                                   &(tp_o->tp_as_sequence)->sq_ass_item,
                                   (void **)&_t7)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_SetItem,
                                              CfCommonIntZeroOk, "vvv", o, key,
                                              value) != NULL;
                default:
                    return false;
            }
            if (_t7 != 0) {
                type_error(po, "sequence index must be integer, not '%.200s'",
                           key);
                return false;
            }
        }
    }
    type_error(po, "'%.200s' object does not support item assignment", o);
    return false;
}

DEFINEFN vinfo_t *
PsycoObject_Size(PsycoObject *po, vinfo_t *o)
{
    lenfunc _t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
    vinfo_t *len;
    PySequenceMethods *m;
    PyTypeObject *tp_o;
    tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL) {
        return NULL;
    }
    m = tp_o->tp_as_sequence;
    _t2 = m != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_o, &m->sq_length, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_Size,
                                          CfCommonPySSizeTResult, "v", o);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = Psyco_META1(po, _t1, CfCommonPySSizeTResult, "v", o);
        len = _t3;
        return len;
    }
    _t4 = PsycoMapping_Size(po, o);
    return _t4;
}

DEFINEFN vinfo_t *
_PsycoSequence_IterSearch(PsycoObject *po, vinfo_t *seq, vinfo_t *obj,
                          int operation)
{
    return psyco_generic_call(po, _PySequence_IterSearch,
                              CfCommonPySSizeTResult, "vvl", seq, obj,
                              (long)(operation));
}

DEFINEFN vinfo_t *
PsycoSequence_Contains(PsycoObject *po, vinfo_t *seq, vinfo_t *ob)
{
    objobjproc _t1;
    int _t2;
    vinfo_t *_t3;
    vinfo_t *_t4;
#if NI_HAS_CHECK_SLOT_RESULT
    vinfo_t *res;
#endif
    vinfo_t *result;
    PySequenceMethods *sqm;
    PyTypeObject *tp_seq;
    tp_seq = Psyco_NeedType(po, seq);
    if (tp_seq == NULL) {
        return NULL;
    }
    sqm = tp_seq->tp_as_sequence;
    _t2 = sqm != NULL;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_seq, &sqm->sq_contains, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_Contains,
                                          CfCommonIntBoolOrError, "vv", seq,
                                          ob);
            default:
                return NULL;
        }
        _t2 = _t1 != NULL;
    }
    if (_t2) {
        _t3 = Psyco_META2(po, _t1, CfCommonIntBoolOrError, "vv", seq, ob);
#if NI_HAS_CHECK_SLOT_RESULT
        res = _t3;
        return res;
#else
        return _t3;
#endif
    }
    _t4 = _PsycoSequence_IterSearch(po, seq, ob, 3);
    result = _t4;
    return result;
}

DEFINEFN vinfo_t *
PsycoSequence_List(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *_t1;
    _t1 = psyco_generic_call(po, PySequence_List, CfCommonNewRefPyObject, "v",
                             v);
    if (_t1 != NULL)
        Psyco_AssertType(po, _t1, &PyList_Type);
    return _t1;
}

DEFINEFN vinfo_t *
PsycoSequence_GetSlice(PsycoObject *po, vinfo_t *s, vinfo_t *i1, vinfo_t *i2)
{
    binaryfunc _t1;
    int _t2;
    vinfo_t *_t3;
    binaryfunc _t4;
    vinfo_t *_t5;
    vinfo_t *_t6;
    vinfo_t *_t7;
    PyMappingMethods *mp;
    vinfo_t *res;
    vinfo_t *slice;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return NULL;
    }
    mp = tp_s->tp_as_mapping;
    _t2 = mp != 0;
    if (_t2) {
        switch (Psyco_TypeSlot(po, tp_s, &mp->mp_subscript, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_GetSlice,
                                          CfCommonNewRefPyObject, "vvv", s, i1,
                                          i2);
            default:
                return NULL;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = psyco_generic_call(po, _PySlice_FromIndices,
                                 CfCommonNewRefPyObject, "vv", i1, i2);
        slice = _t3;
        if (slice == NULL) {
            return NULL;
        }
        switch (Psyco_TypeSlot(po, tp_s, &mp->mp_subscript, (void **)&_t4)) {
            case 1:
                break;
            case 0:
                _t5 = psyco_generic_call(po, cimpl_PySequence_GetSlice_1,
                                         CfCommonNewRefPyObject, "vlv", s,
                                         (long)(mp), slice);
                vinfo_decref(slice, po);
                return _t5;
            default:
                vinfo_decref(slice, po);
                return NULL;
        }
        _t6 = Psyco_META2(po, _t4, CfCommonNewRefPyObject, "vv", s, slice);
        res = _t6;
        vinfo_decref(slice, po);
        return res;
    }
    _t7 = type_error(po, "'%.200s' object is unsliceable", s);
    return _t7;
}

DEFINEFN bool
PsycoSequence_SetSlice(PsycoObject *po, vinfo_t *s, vinfo_t *i1, vinfo_t *i2,
                       vinfo_t *o)
{
    objobjargproc _t1;
    int _t2;
    vinfo_t *_t3;
    objobjargproc _t4;
    bool _t5;
    PyMappingMethods *mp;
    vinfo_t *res;
    vinfo_t *slice;
    PyTypeObject *tp_s;
    tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL) {
        return false;
    }
    mp = tp_s->tp_as_mapping;
    _t2 = mp != 0;
    if (_t2) {
        switch (
            Psyco_TypeSlot(po, tp_s, &mp->mp_ass_subscript, (void **)&_t1)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_SetSlice,
                                          CfCommonIntZeroOk, "vvvv", s, i1, i2,
                                          o) != NULL;
            default:
                return false;
        }
        _t2 = _t1 != 0;
    }
    if (_t2) {
        _t3 = psyco_generic_call(po, _PySlice_FromIndices,
                                 CfCommonNewRefPyObject, "vv", i1, i2);
        slice = _t3;
        if (slice == NULL) {
            return false;
        }
        switch (
            Psyco_TypeSlot(po, tp_s, &mp->mp_ass_subscript, (void **)&_t4)) {
            case 1:
                break;
            case 0:
                _t5 = psyco_generic_call(po, cimpl_PySequence_SetSlice_1,
                                         CfCommonIntZeroOk, "vvlv", s, o,
                                         (long)(mp), slice) != NULL;
                vinfo_decref(slice, po);
                return _t5;
            default:
                vinfo_decref(slice, po);
                return false;
        }
        res = Psyco_META3(po, _t4, CfCommonIntZeroOk, "vvv", s, slice, o);
        vinfo_decref(slice, po);
        return res != NULL;
    }
    type_error(po, "'%.200s' object doesn't support slice assignment", s);
    return false;
}

DEFINEFN vinfo_t *
PsycoSequence_Tuple(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *_t1;
    vinfo_t *_t2;
    PyTypeObject *tp_v;
    tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL) {
        return NULL;
    }
    if (tp_v == (&PyTuple_Type)) {
        vinfo_incref(v);
        return v;
    }
    if (tp_v == (&PyList_Type)) {
        _t1 = psyco_generic_call(
            po, PyList_AsTuple,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v", v);
        if (_t1 != NULL)
            Psyco_AssertType(po, _t1, &PyTuple_Type);
        return _t1;
    }
    _t2 = psyco_generic_call(po, PySequence_Tuple, CfCommonNewRefPyObject, "v",
                             v);
    if (_t2 != NULL)
        Psyco_AssertType(po, _t2, &PyTuple_Type);
    return _t2;
}

DEFINEFN
vinfo_t *PsycoNumber_Power(PsycoObject *po, vinfo_t *v1, vinfo_t *v2,
                           vinfo_t *v3) {
  /* limited support -- the goal is only to support
     (long or float) ** (long or float) */
  vinfo_t *x;
  PyTypeObject *vtp;
  PyTypeObject *wtp;
  ternaryfunc f = NULL;
  vtp = Psyco_NeedType(po, v1);
  if (vtp == NULL)
    return NULL;
  if (vtp != &PyLong_Type && vtp != &PyFloat_Type)
    goto fallback;
  wtp = Psyco_NeedType(po, v2);
  if (wtp == NULL)
    return NULL;
  if (wtp != &PyLong_Type && wtp != &PyFloat_Type)
    goto fallback;
  if (vtp == &PyFloat_Type || wtp == &PyFloat_Type)
    f = PyFloat_Type.tp_as_number->nb_power;
  else if (vtp == &PyLong_Type || wtp == &PyLong_Type)
    f = PyLong_Type.tp_as_number->nb_power;
  if (f == NULL) {
    goto fallback;
  }
  x = Psyco_META3(po, f, CfCommonNewRefPyObject, "vvv", v1, v2, v3);
  if (IS_IMPLEMENTED(x))
    return x; /* may be NULL */
  vinfo_decref(x, po);

fallback:
  return psyco_generic_call(po, PyNumber_Power, CfCommonNewRefPyObject, "vvv",
                            v1, v2, v3);
}

DEFINEFN
vinfo_t *PsycoObject_GetIter(PsycoObject *po, vinfo_t *vi) {
  getiterfunc f;
  ssizeargfunc item = NULL;
  PyTypeObject *t = Psyco_NeedType(po, vi);
  if (t == NULL)
    return NULL;
  switch (Psyco_TypeSlot(po, t, &t->tp_iter, (void **)&f)) {
  case 1:
    break;
  case 0:
    goto generic;
  default:
    return NULL;
  }
  if (f == NULL) {
    if (t->tp_as_sequence != NULL)
      switch (Psyco_TypeSlot(po, t, &t->tp_as_sequence->sq_item,
                             (void **)&item)) {
      case 1:
        break;
      case 0:
        goto generic;
      default:
        return NULL;
      }
    if (item != NULL)
      return PsycoSeqIter_New(po, vi);
    return type_error_tp(po, "'%.200s' object is not iterable", t);
  }
  /* a heap type's __iter__ may return a non-iterator, which
     PyObject_GetIter checks */
  if (!Psyco_SlotsMutable(t))
    return Psyco_META1(po, f, CfCommonNewRefPyObject, "v", vi);
generic:
  return psyco_generic_call(po, PyObject_GetIter, CfCommonNewRefPyObject, "v",
                            vi);
}

/* ceval.c's FOR_ITER: GET_ITER made sure the slot was there, but a heap
   type's can have gone since (CPython leaves _PyObject_NextNotImplemented,
   which raises TypeError); NULL is tested only in case Ni's GET_ITER was
   bypassed. */
static PyObject *cimpl_iternext(PyObject *iter) {
  iternextfunc f = Py_TYPE(iter)->tp_iternext;
  if (f == NULL) {
    PyErr_Format(PyExc_TypeError, "'%.100s' object is not an iterator",
                 Py_TYPE(iter)->tp_name);
    return NULL;
  }
  return f(iter);
}

DEFINEFN
vinfo_t *PsycoIter_Next(PsycoObject *po, vinfo_t *iter) {
  iternextfunc f;
  PyTypeObject *tp = Psyco_NeedType(po, iter);
  if (tp == NULL)
    return NULL;
  switch (Psyco_TypeSlot(po, tp, &tp->tp_iternext, (void **)&f)) {
  case 1:
    break;
  case 0:
    return psyco_generic_call(po, cimpl_iternext, CfCommonIterNext, "v",
                              iter);
  default:
    return NULL;
  }
  if (f == NULL) {
    PycException_SetFormat(po, PyExc_TypeError,
                           "'%.100s' object is not an iterator", tp->tp_name);
    return NULL;
  }
  return Psyco_META1(po, f, CfCommonIterNext, "v", iter);
}

/* tuplesubscript, list_subscript and list_ass_subscript count a negative
   index from the object's own size and call their own item functions, not
   the type's slots, which a subclass's __len__ replaces */
static PyTypeObject *subscript_base(PsycoObject *po, vinfo_t *o) {
  PyTypeObject *tp = Psyco_NeedType(po, o);
  if (tp == NULL)
    return NULL;
  if (PyType_TypeCheck(tp, &PyList_Type))
    return &PyList_Type;
  extra_assert(PyType_TypeCheck(tp, &PyTuple_Type));
  return &PyTuple_Type;
}

/* the index plus the size if it is negative: a new reference */
static vinfo_t *own_size_index(PsycoObject *po, PyTypeObject *base,
                               vinfo_t *o, vinfo_t *i) {
  vinfo_t *l, *r;
  condition_code_t cc = integer_cmp_i(po, i, 0, Py_LT);
  if (cc == CC_ERROR)
    return NULL;
  if (!runtime_condition_f(po, cc)) {
    assert_nonneg(i);
    vinfo_incref(i);
    return i;
  }
  l = Psyco_META1(po, base->tp_as_sequence->sq_length,
                  CfCommonPySSizeTResult, "v", o);
  if (l == NULL)
    return NULL;
  r = integer_add(po, i, l, false);
  vinfo_decref(l, po);
  return r;
}

DEFINEFN
vinfo_t *psyco_generic_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key) {
  /* This is the meta-implementation of the mapping item assignment
     for sequences in Python >= 2.3, which is called for any
     expression of the form a[n]. It expects n to be an integer
     or an extended slice object. Regular slicing a[n:m] does not
     come here. */

  /* TypeSwitch */
  PyTypeObject *ktp = Psyco_NeedType(po, key);
  if (ktp == NULL)
    return NULL;

  if (PyType_TypeCheck(ktp, &PyLong_Type)) {
    vinfo_t *result;
    /* as list_subscript and tuplesubscript: PyNumber_AsSsize_t(item,
       PyExc_IndexError) */
    vinfo_t *index;
    PyTypeObject *base = subscript_base(po, o);
    vinfo_t *key_value;
    if (base == NULL)
      return NULL;
    key_value = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
    if (key_value == NULL)
      return NULL;
    index = own_size_index(po, base, o, key_value);
    vinfo_decref(key_value, po);
    if (index == NULL)
      return NULL;
    result = Psyco_META2(po, base->tp_as_sequence->sq_item,
                         CfCommonNewRefPyObject, "vv", o, index);
    vinfo_decref(index, po);
    return result;
  } else {
    PyTypeObject *tp = Psyco_NeedType(po, o);
    if (tp == NULL)
      return NULL;
    extra_assert(tp->tp_as_mapping != NULL);
    extra_assert(tp->tp_as_mapping->mp_subscript != NULL);
    return psyco_generic_call(po, tp->tp_as_mapping->mp_subscript,
                              CfCommonNewRefPyObject, "vv", o, key);
  }
}

DEFINEFN
bool psyco_generic_ass_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key,
                                 vinfo_t *value) {
  /* see psyco_generic_subscript() for comments */

  /* TypeSwitch */
  PyTypeObject *ktp = Psyco_NeedType(po, key);
  if (ktp == NULL)
    return false;

  if (PyType_TypeCheck(ktp, &PyLong_Type)) {
    bool result;
    vinfo_t *index;
    PyTypeObject *base = subscript_base(po, o);
    vinfo_t *key_value;
    if (base == NULL)
      return false;
    key_value = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
    if (key_value == NULL)
      return false;
    index = own_size_index(po, base, o, key_value);
    vinfo_decref(key_value, po);
    if (index == NULL)
      return false;
    if (value != NULL)
      result = Psyco_META3(po, base->tp_as_sequence->sq_ass_item,
                           CfCommonIntZeroOk, "vvv", o, index, value) != NULL;
    else
      result = Psyco_META3(po, base->tp_as_sequence->sq_ass_item,
                           CfCommonIntZeroOk, "vvl", o, index,
                           (long)NULL) != NULL;
    vinfo_decref(index, po);
    return result;
  } else {
    char *vargs = (value != NULL) ? "vvv" : "vvl";
    PyTypeObject *tp = Psyco_NeedType(po, o);
    if (tp == NULL)
      return false;
    extra_assert(tp->tp_as_mapping != NULL);
    extra_assert(tp->tp_as_mapping->mp_ass_subscript != NULL);
    return psyco_generic_call(po, tp->tp_as_mapping->mp_ass_subscript,
                              CfCommonIntZeroOk, vargs, o, key, value) != NULL;
  }
}
