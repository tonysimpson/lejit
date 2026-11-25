/*
 * The compile-time abstract.c. Most of it is translated from CPython's
 * Objects/abstract.c, after what the translation leaves out:
 * PyObject_Call (call.c), PyNumber_Power (a type whitelist), PyObject_GetIter
 * (a virtual iterator), PyIter_Next (FOR_ITER's call, not a translation of
 * it), and the subscripts of tuples and lists.
 */
#include "pabstract.h"
#include "pclassobject.h"
#include "pdictobject.h"
#include "pfuncobject.h"
#include "piterobject.h"
#include "plongobject.h"
#include "pmethodobject.h"
#include "ptupleobject.h"
#include "../Modules/pctypes.h"

/* abstract.c's type_error: msg has one %.200s for the type's name */
static vinfo_t *
type_error_tp(PsycoObject *po, const char *msg, PyTypeObject *tp)
{
    PycException_SetTypeFormat(po, PyExc_TypeError, msg, NULL, tp, NULL);
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
void
Psyco_DefineCallMeta(PyTypeObject *tp, call_meta_fn meta)
{
    if (ncall_metas == CALL_METAS_MAX)
        Py_FatalError("LeJIT: CALL_METAS_MAX call metas");
    call_metas[ncall_metas].tp = tp;
    call_metas[ncall_metas].call = tp->tp_call;
    call_metas[ncall_metas].meta = meta;
    ncall_metas++;
}

#if LE_HAS_VECTORCALL
/* ceval's call: the values in an array with a spare slot in front, which
   PY_VECTORCALL_ARGUMENTS_OFFSET lets the callee use; the last
   len(kwnames) values are the keyword ones */
static PyObject *
cimpl_vectorcall(PyObject *callable, PyObject *kwnames, PyObject **slots,
                 Py_ssize_t n)
{
    Py_ssize_t nargs = n - (kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames));
    return PyObject_Vectorcall(
        callable, slots + 1, nargs | PY_VECTORCALL_ARGUMENTS_OFFSET, kwnames);
}
#endif

DEFINEFN
vinfo_t *
psyco_vectorcall(PsycoObject *po, vinfo_t *callable, vinfo_t *self,
                 vinfo_t **args, int nargs, PyObject *kwnames,
                 vinfo_t **kwvalues)
{
#if LE_HAS_VECTORCALL
    int i, k = kwnames == NULL ? 0 : (int)PyTuple_GET_SIZE(kwnames);
    int first = self != NULL ? 2 : 1, n = first - 1 + nargs + k;
    /* items[0] stays NULL: the spare slot */
    vinfo_array_t *slots = array_new(n + 1);
    vinfo_t *vnames, *r;
    if (self != NULL)
        slots->items[1] = self;
    for (i = 0; i < nargs; i++) slots->items[first + i] = args[i];
    for (i = 0; i < k; i++) slots->items[first + nargs + i] = kwvalues[i];
    if (kwnames == NULL)
        vnames = psyco_vi_Zero();
    else {
        Py_INCREF(kwnames);
        vnames =
            vinfo_new(CompileTime_NewSk(sk_new((long)kwnames, SkFlagPyObj)));
    }
    r = psyco_generic_call(po, cimpl_vectorcall, CfCommonNewRefPyObject,
                           "vvil", callable, vnames, slots, (long)n);
    vinfo_decref(vnames, po);
    array_release(slots);
    return r;
#else
    return NULL;
#endif
}

DEFINEFN
vinfo_t *
psyco_vectorcall_tuple(PsycoObject *po, vinfo_t *callable, vinfo_t *args,
                       vinfo_t *kw)
{
    PyObject *kwnames = NULL;
    vinfo_t **kwvalues = NULL;
    int n;
    if (!psyco_knowntobe(kw, 0)) {
        /* pending keywords with no names are the empty dict of
           f(*a, **{}), which CPython passes on as a dict */
        kwnames = PsycoDict_PendingNames(kw, &kwvalues);
        if (kwnames == NULL || PyTuple_GET_SIZE(kwnames) == 0)
            return NULL;
    }
    if (!is_virtualtime(args->source))
        return NULL;
    n = PsycoTuple_Load(args);
    if (n < 0)
        return NULL;
    return psyco_vectorcall(po, callable, NULL, &PsycoTuple_GET_ITEM(args, 0),
                            n, kwnames, kwvalues);
}

DEFINEFN
vinfo_t *
psyco_residual_call(PsycoObject *po, void *call, vinfo_t *callable,
                    vinfo_t *args, vinfo_t *kw)
{
    vinfo_t *r = psyco_vectorcall_tuple(po, callable, args, kw);
    if (r != NULL || PycException_Occurred(po))
        return r;
    return psyco_generic_call(po, call, CfCommonNewRefPyObject, "vvv",
                              callable, args, kw);
}
DEFINEFN
vinfo_t *
PsycoObject_Call(PsycoObject *po, vinfo_t *callable_object, vinfo_t *args,
                 vinfo_t *kw)
{ /* 'kw' may not be NULL */
    ternaryfunc call;
    PyTypeObject *tp = Psyco_NeedType(po, callable_object);
    if (tp == NULL)
        return NULL;

    switch (Psyco_TypeSlot(po, tp, &tp->tp_call, (void **)&call)) {
        case 1:
            break;
        case 0: /* a heap type's tp_call changed; kw is 0 for none */
            return psyco_residual_call(po, PyObject_Call, callable_object,
                                       args, kw);
        default:
            return NULL;
    }
    if (call != NULL) {
        int i;
        for (i = 0; i < ncall_metas; i++)
            if (call_metas[i].tp == tp && call_metas[i].call == call)
                return call_metas[i].meta(po, callable_object, args, kw);
#ifdef Py_TPFLAGS_HAVE_VECTORCALL
        /* CPython calls such an object through the vectorcall function it
           holds, which may not do what tp_call does */
        if ((tp->tp_flags & Py_TPFLAGS_HAVE_VECTORCALL) &&
            Psyco_Lookup((void *)call) == NULL)
            return psyco_residual_call(po, PyObject_Call, callable_object,
                                       args, kw);
#endif
        if (PsycoCtypes_IsFunction(tp, call)) {
            vinfo_t *r = PsycoCtypes_Call(po, tp, callable_object, args, kw);
            if (r != NULL || PycException_Occurred(po))
                return r;
        }
        return Psyco_META3(po, call, CfCommonNewRefPyObject, "vvv",
                           callable_object, args, kw);
    }
    return type_error_tp(po, LE_MSG_NOT_CALLABLE, tp);
}

#define NB_SLOT(x) offsetof(PyNumberMethods, x)
#define NB_BINOP(nb_methods, slot) \
    (*(binaryfunc *)(&((char *)nb_methods)[slot]))
#define NB_TERNOP(nb_methods, slot) \
    (*(ternaryfunc *)(&((char *)nb_methods)[slot]))

/* CPython's code, copied for the metas to call at run time. A copy of part
   of a CPython function, for where a slot read when compiling has changed,
   starts where the meta has got to and takes what it has read so far. */

static PyObject *
cimpl_type_error(const char *msg, PyObject *obj)
{
    PyErr_Format(PyExc_TypeError, msg, Py_TYPE(obj)->tp_name);
    return NULL;
}

static PyObject *
cimpl_binary_op1(PyObject *v, PyObject *w, const int op_slot)
{
    binaryfunc slotv;
    if (Py_TYPE(v)->tp_as_number != NULL) {
        slotv = NB_BINOP(Py_TYPE(v)->tp_as_number, op_slot);
    }
    else {
        slotv = NULL;
    }

    binaryfunc slotw;
    if (Py_TYPE(w) != Py_TYPE(v) && Py_TYPE(w)->tp_as_number != NULL) {
        slotw = NB_BINOP(Py_TYPE(w)->tp_as_number, op_slot);
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    else {
        slotw = NULL;
    }

    if (slotv) {
        PyObject *x;
        if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
            x = slotw(v, w);
            if (x != Py_NotImplemented)
                return x;
            Py_DECREF(x); /* can't do it */
            slotw = NULL;
        }
        x = slotv(v, w);
        if (x != Py_NotImplemented) {
            return x;
        }
        Py_DECREF(x); /* can't do it */
    }
    if (slotw) {
        PyObject *x = slotw(v, w);
        if (x != Py_NotImplemented) {
            return x;
        }
        Py_DECREF(x); /* can't do it */
    }
    Py_RETURN_NOTIMPLEMENTED;
}

static PyObject *
cimpl_binop_type_error(PyObject *v, PyObject *w, const char *op_name)
{
    PyErr_Format(PyExc_TypeError, LE_MSG_BINOP_TYPE_ERROR, op_name,
                 Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    return NULL;
}

/* binary_op once binary_op1 has declined */
static PyObject *
cimpl_binary_op_error(PyObject *v, PyObject *w, const int op_slot,
                      const char *op_name)
{
    if (op_slot == NB_SLOT(nb_rshift) && Py_TYPE(v) == &PyCFunction_Type &&
        strcmp(((PyCFunctionObject *)v)->m_ml->ml_name, "print") == 0) {
        PyErr_Format(PyExc_TypeError, LE_MSG_BINOP_PRINT_TYPE_ERROR, op_name,
                     Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
        return NULL;
    }
    return cimpl_binop_type_error(v, w, op_name);
}

/* ternary_op once v's and w's slots have declined: mz is z's
   tp_as_number, or NULL to go straight to the error */
static PyObject *
cimpl_ternary_op_rest(PyObject *v, PyObject *w, PyObject *z, const int op_slot,
                      const char *op_name, PyNumberMethods *mz,
                      ternaryfunc slotv, ternaryfunc slotw)
{
    if (mz != NULL) {
        ternaryfunc slotz = NB_TERNOP(mz, op_slot);
        if (slotz == slotv || slotz == slotw) {
            slotz = NULL;
        }
        if (slotz) {
            PyObject *x = slotz(v, w, z);
            if (x != Py_NotImplemented) {
                return x;
            }
            Py_DECREF(x); /* can't do it */
        }
    }

    /* op_name is the name a type error of **= shows (LE_MSG_OP_IPOW: before
       3.10, "** or pow()"). z is INPLACE_POWER's None: pow()'s message of
       three operands stands for the case no compiled code reaches. */
    if (z == Py_None) {
        PyErr_Format(PyExc_TypeError, LE_MSG_BINOP_TYPE_ERROR, op_name,
                     Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name);
    }
    else {
        PyErr_Format(PyExc_TypeError, LE_MSG_POW3_TYPE_ERROR,
                     Py_TYPE(v)->tp_name, Py_TYPE(w)->tp_name,
                     Py_TYPE(z)->tp_name);
    }
    return NULL;
}

static PyObject *
cimpl_ternary_op(PyObject *v, PyObject *w, PyObject *z, const int op_slot,
                 const char *op_name)
{
    PyNumberMethods *mv = Py_TYPE(v)->tp_as_number;
    PyNumberMethods *mw = Py_TYPE(w)->tp_as_number;

    ternaryfunc slotv;
    if (mv != NULL) {
        slotv = NB_TERNOP(mv, op_slot);
    }
    else {
        slotv = NULL;
    }

    ternaryfunc slotw;
    if (Py_TYPE(w) != Py_TYPE(v) && mw != NULL) {
        slotw = NB_TERNOP(mw, op_slot);
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    else {
        slotw = NULL;
    }

    if (slotv) {
        PyObject *x;
        if (slotw && PyType_IsSubtype(Py_TYPE(w), Py_TYPE(v))) {
            x = slotw(v, w, z);
            if (x != Py_NotImplemented) {
                return x;
            }
            Py_DECREF(x); /* can't do it */
            slotw = NULL;
        }
        x = slotv(v, w, z);
        if (x != Py_NotImplemented) {
            return x;
        }
        Py_DECREF(x); /* can't do it */
    }
    if (slotw) {
        PyObject *x = slotw(v, w, z);
        if (x != Py_NotImplemented) {
            return x;
        }
        Py_DECREF(x); /* can't do it */
    }
    return cimpl_ternary_op_rest(v, w, z, op_slot, op_name,
                                 Py_TYPE(z)->tp_as_number, slotv, slotw);
}

/* PyNumber_Add once binary_op1 has declined: m is v's tp_as_sequence */
static PyObject *
LeNumber_Add(PyObject *v, PyObject *w, PySequenceMethods *m)
{
    if (m && m->sq_concat) {
        return (*m->sq_concat)(v, w);
    }
    return cimpl_binop_type_error(v, w, LE_MSG_OP_ADD);
}

static PyObject *
cimpl_sequence_repeat(ssizeargfunc repeatfunc, PyObject *seq, PyObject *n)
{
    Py_ssize_t count;
    if (Py_TYPE(n)->tp_as_number != NULL &&
        Py_TYPE(n)->tp_as_number->nb_index != NULL) {
        count = PyNumber_AsSsize_t(n, PyExc_OverflowError);
        if (count == -1 && PyErr_Occurred()) {
            return NULL;
        }
    }
    else {
        return cimpl_type_error(LE_MSG_SEQ_REPEAT_NON_INT, n);
    }
    return (*repeatfunc)(seq, count);
}

/* PyNumber_Multiply once binary_op1 has declined: mv and mw are v's and
   w's tp_as_sequence */
static PyObject *
LeNumber_Multiply(PyObject *v, PyObject *w, PySequenceMethods *mv,
                  PySequenceMethods *mw)
{
    if (mv && mv->sq_repeat) {
        return cimpl_sequence_repeat(mv->sq_repeat, v, w);
    }
    else if (mw && mw->sq_repeat) {
        return cimpl_sequence_repeat(mw->sq_repeat, w, v);
    }
    return cimpl_binop_type_error(v, w, LE_MSG_OP_MUL);
}

static PyObject *
cimpl_binary_iop1(PyObject *v, PyObject *w, const int iop_slot,
                  const int op_slot)
{
    PyNumberMethods *mv = Py_TYPE(v)->tp_as_number;
    if (mv != NULL) {
        binaryfunc slot = NB_BINOP(mv, iop_slot);
        if (slot) {
            PyObject *x = (slot)(v, w);
            if (x != Py_NotImplemented) {
                return x;
            }
            Py_DECREF(x);
        }
    }
    return cimpl_binary_op1(v, w, op_slot);
}

#if LE_HAS_TERNARY_IOP
static PyObject *
cimpl_ternary_iop(PyObject *v, PyObject *w, PyObject *z, const int iop_slot,
                  const int op_slot, const char *op_name)
{
    PyNumberMethods *mv = Py_TYPE(v)->tp_as_number;
    if (mv != NULL) {
        ternaryfunc slot = NB_TERNOP(mv, iop_slot);
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

/* PyNumber_InPlaceAdd once binary_iop1 has declined: m is v's
   tp_as_sequence */
static PyObject *
LeNumber_InPlaceAdd(PyObject *v, PyObject *w, PySequenceMethods *m)
{
    if (m != NULL) {
        binaryfunc func = m->sq_inplace_concat;
        if (func == NULL)
            func = m->sq_concat;
        if (func != NULL) {
            return func(v, w);
        }
    }
    return cimpl_binop_type_error(v, w, LE_MSG_OP_IADD);
}

/* PyNumber_InPlaceMultiply once binary_iop1 has declined: mv and mw are
   v's and w's tp_as_sequence */
static PyObject *
LeNumber_InPlaceMultiply(PyObject *v, PyObject *w, PySequenceMethods *mv,
                         PySequenceMethods *mw)
{
    if (mv != NULL) {
        ssizeargfunc f = mv->sq_inplace_repeat;
        if (f == NULL)
            f = mv->sq_repeat;
        if (f != NULL)
            return cimpl_sequence_repeat(f, v, w);
    }
    else if (mw != NULL) {
        /* Note that the right hand operand should not be
         * mutated in this case so sq_inplace_repeat is not
         * used. */
        if (mw->sq_repeat)
            return cimpl_sequence_repeat(mw->sq_repeat, w, v);
    }
    return cimpl_binop_type_error(v, w, LE_MSG_OP_IMUL);
}

/* PyNumber_InPlaceAdd, PyNumber_InPlaceMultiply or another in-place
   operation, after the in-place slot declined: binary_op1, then the
   sequence's slots for += and *=, then the error, with the types as they
   are now */
static PyObject *
cimpl_binary_iop_rest(PyObject *v, PyObject *w, const int iop_slot,
                      const int op_slot, const char *op_name)
{
    PyObject *x = cimpl_binary_op1(v, w, op_slot);
    if (x != Py_NotImplemented)
        return x;
    Py_DECREF(x);
    if (iop_slot == (int)NB_SLOT(nb_inplace_add))
        return LeNumber_InPlaceAdd(v, w, Py_TYPE(v)->tp_as_sequence);
    if (iop_slot == (int)NB_SLOT(nb_inplace_multiply))
        return LeNumber_InPlaceMultiply(v, w, Py_TYPE(v)->tp_as_sequence,
                                        Py_TYPE(w)->tp_as_sequence);
    return cimpl_binop_type_error(v, w, op_name);
}

/* PySequence_GetItem once a negative i has had the length added: m is s's
   tp_as_sequence */
static PyObject *
LeSequence_GetItem(PyObject *s, Py_ssize_t i, PySequenceMethods *m)
{
    return m->sq_item(s, i);
}

/* PySequence_GetSlice once the slice is made: mp is s's tp_as_mapping */
static PyObject *
LeSequence_GetSlice(PyObject *s, PyMappingMethods *mp, PyObject *slice)
{
    Py_INCREF(slice);
    PyObject *res = mp->mp_subscript(s, slice);
    Py_DECREF(slice);
    return res;
}

/* PySequence_SetItem once a negative i has had the length added */
static int
LeSequence_SetItem(PyObject *s, Py_ssize_t i, PyObject *o,
                   PySequenceMethods *m)
{
    return m->sq_ass_item(s, i, o);
}

/* PySequence_DelItem once a negative i has had the length added */
static int
LeSequence_DelItem(PyObject *s, Py_ssize_t i, PySequenceMethods *m)
{
    return m->sq_ass_item(s, i, (PyObject *)NULL);
}

/* PySequence_SetSlice once the slice is made */
static int
LeSequence_SetSlice(PyObject *s, PyObject *o, PyMappingMethods *mp,
                    PyObject *slice)
{
    Py_INCREF(slice);
    int res = mp->mp_ass_subscript(s, slice, o);
    Py_DECREF(slice);
    return res;
}

/* the metas translated from abstract.c */

static vinfo_t *
type_error(PsycoObject *po, const char *msg, vinfo_t *obj)
{
    PyTypeObject *tp_obj = Psyco_NeedType(po, obj);
    if (tp_obj == NULL)
        return NULL;
    PycException_SetTypeFormat(po, PyExc_TypeError, msg, NULL, tp_obj, NULL);
    return NULL;
}

DEFINEFN vinfo_t *
PsycoObject_Size(PsycoObject *po, vinfo_t *o)
{
    PyTypeObject *tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL)
        return NULL;

    PySequenceMethods *m = tp_o->tp_as_sequence;
    lenfunc sq_length = NULL;
    if (m != NULL) {
        switch (Psyco_TypeSlot(po, tp_o, &m->sq_length, (void **)&sq_length)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_Size,
                                          CfCommonPySSizeTResult, "v", o);
            default:
                return NULL;
        }
    }
    if (sq_length != NULL) {
        return Psyco_META1(po, sq_length, CfCommonPySSizeTResult, "v", o);
    }

    return PsycoMapping_Size(po, o);
}

DEFINEFN vinfo_t *
PsycoObject_GetItem(PsycoObject *po, vinfo_t *o, vinfo_t *key)
{
    PyTypeObject *tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL)
        return NULL;

    PyMappingMethods *m = tp_o->tp_as_mapping;
    binaryfunc mp_subscript = NULL;
    if (m != NULL) {
        switch (Psyco_TypeSlot(po, tp_o, &m->mp_subscript,
                               (void **)&mp_subscript)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_GetItem,
                                          CfCommonNewRefPyObject, "vv", o,
                                          key);
            default:
                return NULL;
        }
    }
    if (mp_subscript != NULL) {
        return Psyco_META2(po, mp_subscript, CfCommonNewRefPyObject, "vv", o,
                           key);
    }

    PySequenceMethods *ms = tp_o->tp_as_sequence;
    ssizeargfunc sq_item = NULL;
    if (ms != NULL) {
        switch (Psyco_TypeSlot(po, tp_o, &ms->sq_item, (void **)&sq_item)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_GetItem,
                                          CfCommonNewRefPyObject, "vv", o,
                                          key);
            default:
                return NULL;
        }
    }
    if (sq_item != NULL) {
        PyTypeObject *tp_key = Psyco_NeedType(po, key);
        if (tp_key == NULL)
            return NULL;
        unaryfunc nb_index = NULL;
        if (tp_key->tp_as_number != NULL) {
            switch (Psyco_TypeSlot(po, tp_key, &tp_key->tp_as_number->nb_index,
                                   (void **)&nb_index)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_GetItem,
                                              CfCommonNewRefPyObject, "vv", o,
                                              key);
                default:
                    return NULL;
            }
        }
        if (nb_index != NULL) {
            vinfo_t *key_value =
                PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
            if (key_value == NULL)
                return NULL;
            vinfo_t *item = PsycoSequence_GetItem(po, o, key_value);
            vinfo_decref(key_value, po);
            return item;
        }
        else {
            return type_error(po, LE_MSG_INDEX_GET_NOT_INT, key);
        }
    }

    if (tp_o->tp_flags & Py_TPFLAGS_TYPE_SUBCLASS) {
        /* a type's __class_getitem__ */
        return psyco_generic_call(po, PyObject_GetItem, CfCommonNewRefPyObject,
                                  "vv", o, key);
    }

    return type_error(po, LE_MSG_NOT_SUBSCRIPTABLE, o);
}

DEFINEFN bool
PsycoObject_SetItem(PsycoObject *po, vinfo_t *o, vinfo_t *key, vinfo_t *value)
{
    PyTypeObject *tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL)
        return false;

    PyMappingMethods *m = tp_o->tp_as_mapping;
    objobjargproc mp_ass_subscript = NULL;
    if (m != NULL) {
        switch (Psyco_TypeSlot(po, tp_o, &m->mp_ass_subscript,
                               (void **)&mp_ass_subscript)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_SetItem,
                                          CfCommonIntZeroOk, "vvv", o, key,
                                          value) != NULL;
            default:
                return false;
        }
    }
    if (mp_ass_subscript != NULL) {
        return Psyco_META3(po, mp_ass_subscript, CfCommonIntZeroOk, "vvv", o,
                           key, value) != NULL;
    }

    if (tp_o->tp_as_sequence) {
        PyTypeObject *tp_key = Psyco_NeedType(po, key);
        if (tp_key == NULL)
            return false;
        unaryfunc nb_index = NULL;
        if (tp_key->tp_as_number != NULL) {
            switch (Psyco_TypeSlot(po, tp_key, &tp_key->tp_as_number->nb_index,
                                   (void **)&nb_index)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_SetItem,
                                              CfCommonIntZeroOk, "vvv", o, key,
                                              value) != NULL;
                default:
                    return false;
            }
        }
        if (nb_index != NULL) {
            vinfo_t *key_value =
                PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
            if (key_value == NULL)
                return false;
            bool ok = PsycoSequence_SetItem(po, o, key_value, value);
            vinfo_decref(key_value, po);
            return ok;
        }
        else {
            ssizeobjargproc sq_ass_item;
            switch (Psyco_TypeSlot(po, tp_o,
                                   &tp_o->tp_as_sequence->sq_ass_item,
                                   (void **)&sq_ass_item)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_SetItem,
                                              CfCommonIntZeroOk, "vvv", o, key,
                                              value) != NULL;
                default:
                    return false;
            }
            if (sq_ass_item != NULL) {
                type_error(po, LE_MSG_INDEX_SET_NOT_INT, key);
                return false;
            }
        }
    }

    type_error(po, LE_MSG_NO_ITEM_ASSIGNMENT, o);
    return false;
}

DEFINEFN bool
PsycoObject_DelItem(PsycoObject *po, vinfo_t *o, vinfo_t *key)
{
    PyTypeObject *tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL)
        return false;

    PyMappingMethods *m = tp_o->tp_as_mapping;
    objobjargproc mp_ass_subscript = NULL;
    if (m != NULL) {
        switch (Psyco_TypeSlot(po, tp_o, &m->mp_ass_subscript,
                               (void **)&mp_ass_subscript)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyObject_DelItem,
                                          CfCommonIntZeroOk, "vv", o,
                                          key) != NULL;
            default:
                return false;
        }
    }
    if (mp_ass_subscript != NULL) {
        return Psyco_META3(po, mp_ass_subscript, CfCommonIntZeroOk, "vvl", o,
                           key, (long)NULL) != NULL;
    }

    if (tp_o->tp_as_sequence) {
        PyTypeObject *tp_key = Psyco_NeedType(po, key);
        if (tp_key == NULL)
            return false;
        unaryfunc nb_index = NULL;
        if (tp_key->tp_as_number != NULL) {
            switch (Psyco_TypeSlot(po, tp_key, &tp_key->tp_as_number->nb_index,
                                   (void **)&nb_index)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_DelItem,
                                              CfCommonIntZeroOk, "vv", o,
                                              key) != NULL;
                default:
                    return false;
            }
        }
        if (nb_index != NULL) {
            vinfo_t *key_value =
                PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
            if (key_value == NULL)
                return false;
            bool ok = PsycoSequence_DelItem(po, o, key_value);
            vinfo_decref(key_value, po);
            return ok;
        }
        else {
            ssizeobjargproc sq_ass_item;
            switch (Psyco_TypeSlot(po, tp_o,
                                   &tp_o->tp_as_sequence->sq_ass_item,
                                   (void **)&sq_ass_item)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PyObject_DelItem,
                                              CfCommonIntZeroOk, "vv", o,
                                              key) != NULL;
                default:
                    return false;
            }
            if (sq_ass_item != NULL) {
                type_error(po, LE_MSG_INDEX_DEL_NOT_INT, key);
                return false;
            }
        }
    }

    type_error(po, LE_MSG_NO_ITEM_DELETION, o);
    return false;
}

/* Binary operators */

static vinfo_t *
binary_op1(PsycoObject *po, vinfo_t *v, vinfo_t *w, const int op_slot)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    PyTypeObject *tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL)
        return NULL;

    binaryfunc slotv;
    if (tp_v->tp_as_number != NULL) {
        switch (Psyco_TypeSlot(po, tp_v,
                               &NB_BINOP(tp_v->tp_as_number, op_slot),
                               (void **)&slotv)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_binary_op1,
                                          CfCommonCheckNotImplemented, "vvl",
                                          v, w, (long)op_slot);
            default:
                return NULL;
        }
    }
    else {
        slotv = NULL;
    }

    binaryfunc slotw;
    if (tp_w != tp_v && tp_w->tp_as_number != NULL) {
        switch (Psyco_TypeSlot(po, tp_w,
                               &NB_BINOP(tp_w->tp_as_number, op_slot),
                               (void **)&slotw)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_binary_op1,
                                          CfCommonCheckNotImplemented, "vvl",
                                          v, w, (long)op_slot);
            default:
                return NULL;
        }
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    else {
        slotw = NULL;
    }

    if (slotv) {
        vinfo_t *x;
        if (slotw) {
            bool w_is_subtype;
            switch (Psyco_TypeIsSubtype(po, tp_w, tp_v, &w_is_subtype)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, cimpl_binary_op1,
                                              CfCommonCheckNotImplemented,
                                              "vvl", v, w, (long)op_slot);
                default:
                    return NULL;
            }
            if (w_is_subtype) {
                x = Psyco_META2(po, slotw, CfCommonCheckNotImplemented, "vv",
                                v, w);
                if (IS_IMPLEMENTED(x))
                    return x;
                vinfo_decref(x, po); /* can't do it */
                slotw = NULL;
            }
        }
        x = Psyco_META2(po, slotv, CfCommonCheckNotImplemented, "vv", v, w);
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po); /* can't do it */
    }
    if (slotw) {
        vinfo_t *x =
            Psyco_META2(po, slotw, CfCommonCheckNotImplemented, "vv", v, w);
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po); /* can't do it */
    }
    return psyco_vi_NotImplemented();
}

static vinfo_t *
binop_type_error(PsycoObject *po, vinfo_t *v, vinfo_t *w, const char *op_name)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    PyTypeObject *tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL)
        return NULL;
    if (Psyco_TypesStale(po, tp_v, tp_w))
        return psyco_generic_call(po, cimpl_binop_type_error,
                                  CfCommonNewRefPyObject, "vvl", v, w,
                                  (long)op_name);
    PycException_SetTypeFormat(po, PyExc_TypeError, LE_MSG_BINOP_TYPE_ERROR,
                               op_name, tp_v, tp_w);
    return NULL;
}

static vinfo_t *
binary_op(PsycoObject *po, vinfo_t *v, vinfo_t *w, const int op_slot,
          const char *op_name)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    vinfo_t *result = binary_op1(po, v, w, op_slot);
    if (!IS_IMPLEMENTED(result)) {
        vinfo_decref(result, po);
        if (op_slot == NB_SLOT(nb_rshift) && tp_v == &PyCFunction_Type) {
            /* the message depends on the function's name */
            return psyco_generic_call(po, cimpl_binary_op_error,
                                      CfCommonNewRefPyObject, "vvll", v, w,
                                      (long)op_slot, (long)op_name);
        }
        return binop_type_error(po, v, w, op_name);
    }
    return result;
}

static vinfo_t *
ternary_op(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t *z,
           const int op_slot, const char *op_name)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    PyTypeObject *tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL)
        return NULL;
    PyTypeObject *tp_z = Psyco_NeedType(po, z);
    if (tp_z == NULL)
        return NULL;

    PyNumberMethods *mv = tp_v->tp_as_number;
    PyNumberMethods *mw = tp_w->tp_as_number;

    ternaryfunc slotv;
    if (mv != NULL) {
        switch (Psyco_TypeSlot(po, tp_v, &NB_TERNOP(mv, op_slot),
                               (void **)&slotv)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_ternary_op,
                                          CfCommonNewRefPyObject, "vvvll", v,
                                          w, z, (long)op_slot, (long)op_name);
            default:
                return NULL;
        }
    }
    else {
        slotv = NULL;
    }

    ternaryfunc slotw;
    if (tp_w != tp_v && mw != NULL) {
        switch (Psyco_TypeSlot(po, tp_w, &NB_TERNOP(mw, op_slot),
                               (void **)&slotw)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_ternary_op,
                                          CfCommonNewRefPyObject, "vvvll", v,
                                          w, z, (long)op_slot, (long)op_name);
            default:
                return NULL;
        }
        if (slotw == slotv) {
            slotw = NULL;
        }
    }
    else {
        slotw = NULL;
    }

    if (slotv) {
        vinfo_t *x;
        if (slotw) {
            bool w_is_subtype;
            switch (Psyco_TypeIsSubtype(po, tp_w, tp_v, &w_is_subtype)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(
                        po, cimpl_ternary_op, CfCommonNewRefPyObject, "vvvll",
                        v, w, z, (long)op_slot, (long)op_name);
                default:
                    return NULL;
            }
            if (w_is_subtype) {
                x = Psyco_META3(po, slotw, CfCommonCheckNotImplemented, "vvv",
                                v, w, z);
                if (IS_IMPLEMENTED(x)) {
                    return x;
                }
                vinfo_decref(x, po); /* can't do it */
                slotw = NULL;
            }
        }
        x = Psyco_META3(po, slotv, CfCommonCheckNotImplemented, "vvv", v, w,
                        z);
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po); /* can't do it */
    }
    if (slotw) {
        vinfo_t *x = Psyco_META3(po, slotw, CfCommonCheckNotImplemented, "vvv",
                                 v, w, z);
        if (IS_IMPLEMENTED(x)) {
            return x;
        }
        vinfo_decref(x, po); /* can't do it */
    }

    PyNumberMethods *mz = tp_z->tp_as_number;
    if (mz != NULL) {
        ternaryfunc slotz;
        switch (Psyco_TypeSlot(po, tp_z, &NB_TERNOP(mz, op_slot),
                               (void **)&slotz)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_op_rest, CfCommonNewRefPyObject,
                    "vvvlllll", v, w, z, (long)op_slot, (long)op_name,
                    (long)mz, (long)slotv, (long)slotw);
            default:
                return NULL;
        }
        if (slotz == slotv || slotz == slotw) {
            slotz = NULL;
        }
        if (slotz) {
            vinfo_t *x = Psyco_META3(po, slotz, CfCommonCheckNotImplemented,
                                     "vvv", v, w, z);
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po); /* can't do it */
        }
    }

    /* the error, which depends on whether z is None */
    return psyco_generic_call(
        po, cimpl_ternary_op_rest, CfCommonNewRefPyObject, "vvvlllll", v, w, z,
        (long)op_slot, (long)op_name, (long)NULL, (long)NULL, (long)NULL);
}

#define BINARY_FUNC(func, op, op_name)                              \
    DEFINEFN vinfo_t *func(PsycoObject *po, vinfo_t *v, vinfo_t *w) \
    {                                                               \
        return binary_op(po, v, w, NB_SLOT(op), op_name);           \
    }

BINARY_FUNC(PsycoNumber_Or, nb_or, LE_MSG_OP_OR)
BINARY_FUNC(PsycoNumber_Xor, nb_xor, LE_MSG_OP_XOR)
BINARY_FUNC(PsycoNumber_And, nb_and, LE_MSG_OP_AND)
BINARY_FUNC(PsycoNumber_Lshift, nb_lshift, LE_MSG_OP_LSHIFT)
BINARY_FUNC(PsycoNumber_Rshift, nb_rshift, LE_MSG_OP_RSHIFT)
BINARY_FUNC(PsycoNumber_Subtract, nb_subtract, LE_MSG_OP_SUB)
BINARY_FUNC(PsycoNumber_Divmod, nb_divmod, LE_MSG_OP_DIVMOD)

DEFINEFN vinfo_t *
PsycoNumber_Add(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    vinfo_t *result = binary_op1(po, v, w, NB_SLOT(nb_add));
    if (IS_IMPLEMENTED(result)) {
        return result;
    }
    vinfo_decref(result, po);

    PySequenceMethods *m = tp_v->tp_as_sequence;
    binaryfunc sq_concat = NULL;
    if (m != NULL) {
        switch (Psyco_TypeSlot(po, tp_v, &m->sq_concat, (void **)&sq_concat)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, LeNumber_Add,
                                          CfCommonNewRefPyObject, "vvl", v, w,
                                          (long)m);
            default:
                return NULL;
        }
    }
    if (sq_concat != NULL) {
        return Psyco_META2(po, sq_concat, CfCommonNewRefPyObject, "vv", v, w);
    }

    return binop_type_error(po, v, w, LE_MSG_OP_ADD);
}

static vinfo_t *
sequence_repeat(PsycoObject *po, ssizeargfunc repeatfunc, vinfo_t *seq,
                vinfo_t *n)
{
    PyTypeObject *tp_n = Psyco_NeedType(po, n);
    if (tp_n == NULL)
        return NULL;
    unaryfunc nb_index = NULL;
    if (tp_n->tp_as_number != NULL) {
        switch (Psyco_TypeSlot(po, tp_n, &tp_n->tp_as_number->nb_index,
                               (void **)&nb_index)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_sequence_repeat,
                                          CfCommonNewRefPyObject, "lvv",
                                          (long)repeatfunc, seq, n);
            default:
                return NULL;
        }
    }
    vinfo_t *count;
    if (nb_index != NULL) {
        count = PsycoNumber_AsSsize_t(po, n, PyExc_OverflowError);
        if (count == NULL) {
            return NULL;
        }
    }
    else {
        return type_error(po, LE_MSG_SEQ_REPEAT_NON_INT, n);
    }
    vinfo_t *res =
        Psyco_META2(po, repeatfunc, CfCommonNewRefPyObject, "vv", seq, count);
    vinfo_decref(count, po);
    return res;
}

DEFINEFN vinfo_t *
PsycoNumber_Multiply(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    PyTypeObject *tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL)
        return NULL;
    vinfo_t *result = binary_op1(po, v, w, NB_SLOT(nb_multiply));
    if (!IS_IMPLEMENTED(result)) {
        PySequenceMethods *mv = tp_v->tp_as_sequence;
        PySequenceMethods *mw = tp_w->tp_as_sequence;
        vinfo_decref(result, po);
        ssizeargfunc sq_repeat_v = NULL;
        if (mv != NULL) {
            switch (Psyco_TypeSlot(po, tp_v, &mv->sq_repeat,
                                   (void **)&sq_repeat_v)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, LeNumber_Multiply,
                                              CfCommonNewRefPyObject, "vvll",
                                              v, w, (long)mv, (long)mw);
                default:
                    return NULL;
            }
        }
        if (sq_repeat_v != NULL) {
            return sequence_repeat(po, sq_repeat_v, v, w);
        }
        ssizeargfunc sq_repeat_w = NULL;
        if (mw != NULL) {
            switch (Psyco_TypeSlot(po, tp_w, &mw->sq_repeat,
                                   (void **)&sq_repeat_w)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, LeNumber_Multiply,
                                              CfCommonNewRefPyObject, "vvll",
                                              v, w, (long)mv, (long)mw);
                default:
                    return NULL;
            }
        }
        if (sq_repeat_w != NULL) {
            return sequence_repeat(po, sq_repeat_w, w, v);
        }
        result = binop_type_error(po, v, w, LE_MSG_OP_MUL);
    }
    return result;
}

BINARY_FUNC(PsycoNumber_MatrixMultiply, nb_matrix_multiply, LE_MSG_OP_MATMUL)
BINARY_FUNC(PsycoNumber_FloorDivide, nb_floor_divide, LE_MSG_OP_FLOORDIV)
BINARY_FUNC(PsycoNumber_TrueDivide, nb_true_divide, LE_MSG_OP_TRUEDIV)
BINARY_FUNC(PsycoNumber_Remainder, nb_remainder, LE_MSG_OP_MOD)

DEFINEFN
vinfo_t *
PsycoNumber_Power(PsycoObject *po, vinfo_t *v1, vinfo_t *v2, vinfo_t *v3)
{
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
    return psyco_generic_call(po, PyNumber_Power, CfCommonNewRefPyObject,
                              "vvv", v1, v2, v3);
}

/* Binary in-place operators */

static vinfo_t *
binary_iop1(PsycoObject *po, vinfo_t *v, vinfo_t *w, const int iop_slot,
            const int op_slot, const char *op_name)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    PyTypeObject *tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL)
        return NULL;

    PyNumberMethods *mv = tp_v->tp_as_number;
    if (mv != NULL) {
        binaryfunc slot;
        switch (Psyco_TypeSlot(po, tp_v, &NB_BINOP(mv, iop_slot),
                               (void **)&slot)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, cimpl_binary_iop1,
                                          CfCommonCheckNotImplemented, "vvll",
                                          v, w, (long)iop_slot, (long)op_slot);
            default:
                return NULL;
        }
        if (slot) {
            vinfo_t *x =
                Psyco_META2(po, slot, CfCommonCheckNotImplemented, "vv", v, w);
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
            if (Psyco_TypesStale(po, tp_v, tp_w))
                return psyco_generic_call(
                    po, cimpl_binary_iop_rest, CfCommonNewRefPyObject, "vvlll",
                    v, w, (long)iop_slot, (long)op_slot, (long)op_name);
        }
    }
    return binary_op1(po, v, w, op_slot);
}

static vinfo_t *
binary_iop(PsycoObject *po, vinfo_t *v, vinfo_t *w, const int iop_slot,
           const int op_slot, const char *op_name)
{
    vinfo_t *result = binary_iop1(po, v, w, iop_slot, op_slot, op_name);
    if (!IS_IMPLEMENTED(result)) {
        vinfo_decref(result, po);
        return binop_type_error(po, v, w, op_name);
    }
    return result;
}

#if LE_HAS_TERNARY_IOP
static vinfo_t *
ternary_iop(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t *z,
            const int iop_slot, const int op_slot, const char *op_name)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    PyTypeObject *tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL)
        return NULL;
    PyTypeObject *tp_z = Psyco_NeedType(po, z);
    if (tp_z == NULL)
        return NULL;

    PyNumberMethods *mv = tp_v->tp_as_number;
    if (mv != NULL) {
        ternaryfunc slot;
        switch (Psyco_TypeSlot(po, tp_v, &NB_TERNOP(mv, iop_slot),
                               (void **)&slot)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(
                    po, cimpl_ternary_iop, CfCommonNewRefPyObject, "vvvlll", v,
                    w, z, (long)iop_slot, (long)op_slot, (long)op_name);
            default:
                return NULL;
        }
        if (slot) {
            vinfo_t *x = Psyco_META3(po, slot, CfCommonCheckNotImplemented,
                                     "vvv", v, w, z);
            if (IS_IMPLEMENTED(x)) {
                return x;
            }
            vinfo_decref(x, po);
            if (Psyco_TypesStale(po, tp_v, tp_w) ||
                Psyco_TypesStale(po, tp_z, tp_z))
                return psyco_generic_call(po, cimpl_ternary_op,
                                          CfCommonNewRefPyObject, "vvvll", v,
                                          w, z, (long)op_slot, (long)op_name);
        }
    }
    return ternary_op(po, v, w, z, op_slot, op_name);
}
#endif

#define INPLACE_BINOP(func, iop, op, op_name)                            \
    DEFINEFN vinfo_t *func(PsycoObject *po, vinfo_t *v, vinfo_t *w)      \
    {                                                                    \
        return binary_iop(po, v, w, NB_SLOT(iop), NB_SLOT(op), op_name); \
    }

INPLACE_BINOP(PsycoNumber_InPlaceOr, nb_inplace_or, nb_or, LE_MSG_OP_IOR)
INPLACE_BINOP(PsycoNumber_InPlaceXor, nb_inplace_xor, nb_xor, LE_MSG_OP_IXOR)
INPLACE_BINOP(PsycoNumber_InPlaceAnd, nb_inplace_and, nb_and, LE_MSG_OP_IAND)
INPLACE_BINOP(PsycoNumber_InPlaceLshift, nb_inplace_lshift, nb_lshift,
              LE_MSG_OP_ILSHIFT)
INPLACE_BINOP(PsycoNumber_InPlaceRshift, nb_inplace_rshift, nb_rshift,
              LE_MSG_OP_IRSHIFT)
INPLACE_BINOP(PsycoNumber_InPlaceSubtract, nb_inplace_subtract, nb_subtract,
              LE_MSG_OP_ISUB)
INPLACE_BINOP(PsycoNumber_InPlaceMatrixMultiply, nb_inplace_matrix_multiply,
              nb_matrix_multiply, LE_MSG_OP_IMATMUL)
INPLACE_BINOP(PsycoNumber_InPlaceFloorDivide, nb_inplace_floor_divide,
              nb_floor_divide, LE_MSG_OP_IFLOORDIV)
INPLACE_BINOP(PsycoNumber_InPlaceTrueDivide, nb_inplace_true_divide,
              nb_true_divide, LE_MSG_OP_ITRUEDIV)
INPLACE_BINOP(PsycoNumber_InPlaceRemainder, nb_inplace_remainder, nb_remainder,
              LE_MSG_OP_IMOD)

DEFINEFN vinfo_t *
PsycoNumber_InPlaceAdd(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    vinfo_t *result = binary_iop1(po, v, w, NB_SLOT(nb_inplace_add),
                                  NB_SLOT(nb_add), LE_MSG_OP_IADD);
    if (!IS_IMPLEMENTED(result)) {
        PySequenceMethods *m = tp_v->tp_as_sequence;
        vinfo_decref(result, po);
        if (m != NULL) {
            binaryfunc func;
            switch (Psyco_TypeSlot(po, tp_v, &m->sq_inplace_concat,
                                   (void **)&func)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, LeNumber_InPlaceAdd,
                                              CfCommonNewRefPyObject, "vvl", v,
                                              w, (long)m);
                default:
                    return NULL;
            }
            if (func == NULL) {
                switch (
                    Psyco_TypeSlot(po, tp_v, &m->sq_concat, (void **)&func)) {
                    case 1:
                        break;
                    case 0:
                        return psyco_generic_call(po, LeNumber_InPlaceAdd,
                                                  CfCommonNewRefPyObject,
                                                  "vvl", v, w, (long)m);
                    default:
                        return NULL;
                }
            }
            if (func != NULL) {
                return Psyco_META2(po, func, CfCommonNewRefPyObject, "vv", v,
                                   w);
            }
        }
        result = binop_type_error(po, v, w, LE_MSG_OP_IADD);
    }
    return result;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlaceMultiply(PsycoObject *po, vinfo_t *v, vinfo_t *w)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    PyTypeObject *tp_w = Psyco_NeedType(po, w);
    if (tp_w == NULL)
        return NULL;
    vinfo_t *result = binary_iop1(po, v, w, NB_SLOT(nb_inplace_multiply),
                                  NB_SLOT(nb_multiply), LE_MSG_OP_IMUL);
    if (!IS_IMPLEMENTED(result)) {
        PySequenceMethods *mv = tp_v->tp_as_sequence;
        PySequenceMethods *mw = tp_w->tp_as_sequence;
        vinfo_decref(result, po);
        if (mv != NULL) {
            ssizeargfunc f;
            switch (Psyco_TypeSlot(po, tp_v, &mv->sq_inplace_repeat,
                                   (void **)&f)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, LeNumber_InPlaceMultiply,
                                              CfCommonNewRefPyObject, "vvll",
                                              v, w, (long)mv, (long)mw);
                default:
                    return NULL;
            }
            if (f == NULL) {
                switch (
                    Psyco_TypeSlot(po, tp_v, &mv->sq_repeat, (void **)&f)) {
                    case 1:
                        break;
                    case 0:
                        return psyco_generic_call(po, LeNumber_InPlaceMultiply,
                                                  CfCommonNewRefPyObject,
                                                  "vvll", v, w, (long)mv,
                                                  (long)mw);
                    default:
                        return NULL;
                }
            }
            if (f != NULL)
                return sequence_repeat(po, f, v, w);
        }
        else if (mw != NULL) {
            /* Note that the right hand operand should not be
             * mutated in this case so sq_inplace_repeat is not
             * used. */
            ssizeargfunc sq_repeat;
            switch (Psyco_TypeSlot(po, tp_w, &mw->sq_repeat,
                                   (void **)&sq_repeat)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, LeNumber_InPlaceMultiply,
                                              CfCommonNewRefPyObject, "vvll",
                                              v, w, (long)mv, (long)mw);
                default:
                    return NULL;
            }
            if (sq_repeat)
                return sequence_repeat(po, sq_repeat, w, v);
        }
        result = binop_type_error(po, v, w, LE_MSG_OP_IMUL);
    }
    return result;
}

DEFINEFN vinfo_t *
PsycoNumber_InPlacePower(PsycoObject *po, vinfo_t *v, vinfo_t *w, vinfo_t *z)
{
#if LE_HAS_TERNARY_IOP
    return ternary_iop(po, v, w, z, NB_SLOT(nb_inplace_power),
                       NB_SLOT(nb_power), LE_MSG_OP_IPOW);
#else
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;
    ternaryfunc nb_inplace_power = NULL;
    if (tp_v->tp_as_number != NULL) {
        switch (Psyco_TypeSlot(po, tp_v, &tp_v->tp_as_number->nb_inplace_power,
                               (void **)&nb_inplace_power)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyNumber_InPlacePower,
                                          CfCommonNewRefPyObject, "vvv", v, w,
                                          z);
            default:
                return NULL;
        }
    }
    if (nb_inplace_power != NULL) {
        return ternary_op(po, v, w, z, NB_SLOT(nb_inplace_power),
                          LE_MSG_OP_IPOW);
    }
    else {
        return ternary_op(po, v, w, z, NB_SLOT(nb_power), LE_MSG_OP_IPOW);
    }
#endif
}

/* Unary operators and functions */

#define UNARY_FUNC(func, op, cfunc, msg)                                  \
    DEFINEFN vinfo_t *func(PsycoObject *po, vinfo_t *o)                   \
    {                                                                     \
        PyTypeObject *tp_o = Psyco_NeedType(po, o);                       \
        if (tp_o == NULL)                                                 \
            return NULL;                                                  \
        PyNumberMethods *m = tp_o->tp_as_number;                          \
        unaryfunc slot = NULL;                                            \
        if (m != NULL) {                                                  \
            switch (Psyco_TypeSlot(po, tp_o, &m->op, (void **)&slot)) {   \
                case 1:                                                   \
                    break;                                                \
                case 0:                                                   \
                    return psyco_generic_call(                            \
                        po, cfunc, CfCommonNewRefPyObject, "v", o);       \
                default:                                                  \
                    return NULL;                                          \
            }                                                             \
        }                                                                 \
        if (slot != NULL) {                                               \
            return Psyco_META1(po, slot, CfCommonNewRefPyObject, "v", o); \
        }                                                                 \
        return type_error(po, msg, o);                                    \
    }

UNARY_FUNC(PsycoNumber_Negative, nb_negative, PyNumber_Negative,
           LE_MSG_BAD_NEGATIVE)
UNARY_FUNC(PsycoNumber_Positive, nb_positive, PyNumber_Positive,
           LE_MSG_BAD_POSITIVE)
UNARY_FUNC(PsycoNumber_Invert, nb_invert, PyNumber_Invert, LE_MSG_BAD_INVERT)
UNARY_FUNC(PsycoNumber_Absolute, nb_absolute, PyNumber_Absolute,
           LE_MSG_BAD_ABS)

/* Operations on sequences */

DEFINEFN vinfo_t *
PsycoSequence_GetItem(PsycoObject *po, vinfo_t *s, vinfo_t *i)
{
    PyTypeObject *tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL)
        return NULL;

    PySequenceMethods *m = tp_s->tp_as_sequence;
    ssizeargfunc sq_item = NULL;
    if (m != NULL) {
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_item, (void **)&sq_item)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_GetItem,
                                          CfCommonNewRefPyObject, "vv", s, i);
            default:
                return NULL;
        }
    }
    if (sq_item != NULL) {
        vinfo_t *own_i = NULL; /* i plus the length, held here */
        condition_code_t i_negative = integer_cmp_i(po, i, 0, Py_LT);
        if (i_negative == CC_ERROR)
            return NULL;
        if (runtime_condition_f(po, i_negative)) {
            lenfunc sq_length;
            switch (
                Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&sq_length)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_GetItem,
                                              CfCommonNewRefPyObject, "vv", s,
                                              i);
                default:
                    return NULL;
            }
            if (sq_length != NULL) {
                vinfo_t *l =
                    Psyco_META1(po, sq_length, CfCommonPySSizeTResult, "v", s);
                if (l == NULL)
                    return NULL;
                own_i = integer_add(po, i, l, false);
                vinfo_decref(l, po);
                if (own_i == NULL)
                    return NULL;
                i = own_i;
            }
        }
        else {
            assert_nonneg(i);
        }
        vinfo_t *res;
        switch (Psyco_TypeSlot(po, tp_s, &m->sq_item, (void **)&sq_item)) {
            case 1:
                res = Psyco_META2(po, sq_item, CfCommonNewRefPyObject, "vv", s,
                                  i);
                break;
            case 0:
                res = psyco_generic_call(po, LeSequence_GetItem,
                                         CfCommonNewRefPyObject, "vvl", s, i,
                                         (long)m);
                break;
            default:
                res = NULL;
        }
        vinfo_xdecref(own_i, po);
        return res;
    }

#if LE_HAS_NOT_A_SEQUENCE_ERROR
    binaryfunc mp_subscript = NULL;
    if (tp_s->tp_as_mapping != NULL) {
        switch (Psyco_TypeSlot(po, tp_s, &tp_s->tp_as_mapping->mp_subscript,
                               (void **)&mp_subscript)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_GetItem,
                                          CfCommonNewRefPyObject, "vv", s, i);
            default:
                return NULL;
        }
    }
    if (mp_subscript != NULL) {
        return type_error(po, LE_MSG_SEQ_GET_NOT_SEQUENCE, s);
    }
#endif
    return type_error(po, LE_MSG_SEQ_NO_GET, s);
}

DEFINEFN vinfo_t *
PsycoSequence_GetSlice(PsycoObject *po, vinfo_t *s, vinfo_t *i1, vinfo_t *i2)
{
    PyTypeObject *tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL)
        return NULL;

    PyMappingMethods *mp = tp_s->tp_as_mapping;
    binaryfunc mp_subscript = NULL;
    if (mp != NULL) {
        switch (Psyco_TypeSlot(po, tp_s, &mp->mp_subscript,
                               (void **)&mp_subscript)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_GetSlice,
                                          CfCommonNewRefPyObject, "vvv", s, i1,
                                          i2);
            default:
                return NULL;
        }
    }
    if (mp_subscript != NULL) {
        vinfo_t *slice = psyco_generic_call(
            po, _PySlice_FromIndices, CfCommonNewRefPyObject, "vv", i1, i2);
        if (slice == NULL)
            return NULL;
        vinfo_t *res;
        switch (Psyco_TypeSlot(po, tp_s, &mp->mp_subscript,
                               (void **)&mp_subscript)) {
            case 1:
                res = Psyco_META2(po, mp_subscript, CfCommonNewRefPyObject,
                                  "vv", s, slice);
                break;
            case 0:
                res = psyco_generic_call(po, LeSequence_GetSlice,
                                         CfCommonNewRefPyObject, "vlv", s,
                                         (long)mp, slice);
                break;
            default:
                res = NULL;
        }
        vinfo_decref(slice, po);
        return res;
    }

    return type_error(po, LE_MSG_SEQ_GET_SLICE, s);
}

DEFINEFN bool
PsycoSequence_SetItem(PsycoObject *po, vinfo_t *s, vinfo_t *i, vinfo_t *o)
{
    PyTypeObject *tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL)
        return false;

    PySequenceMethods *m = tp_s->tp_as_sequence;
    ssizeobjargproc sq_ass_item = NULL;
    if (m != NULL) {
        switch (
            Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&sq_ass_item)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_SetItem,
                                          CfCommonIntZeroOk, "vvv", s, i,
                                          o) != NULL;
            default:
                return false;
        }
    }
    if (sq_ass_item != NULL) {
        vinfo_t *own_i = NULL; /* i plus the length, held here */
        condition_code_t i_negative = integer_cmp_i(po, i, 0, Py_LT);
        if (i_negative == CC_ERROR)
            return false;
        if (runtime_condition_f(po, i_negative)) {
            lenfunc sq_length;
            switch (
                Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&sq_length)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_SetItem,
                                              CfCommonIntZeroOk, "vvv", s, i,
                                              o) != NULL;
                default:
                    return false;
            }
            if (sq_length != NULL) {
                vinfo_t *l =
                    Psyco_META1(po, sq_length, CfCommonPySSizeTResult, "v", s);
                if (l == NULL)
                    return false;
                own_i = integer_add(po, i, l, false);
                vinfo_decref(l, po);
                if (own_i == NULL)
                    return false;
                i = own_i;
            }
        }
        else {
            assert_nonneg(i);
        }
        vinfo_t *res;
        switch (
            Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&sq_ass_item)) {
            case 1:
                res = Psyco_META3(po, sq_ass_item, CfCommonIntZeroOk, "vvv", s,
                                  i, o);
                break;
            case 0:
                res = psyco_generic_call(po, LeSequence_SetItem,
                                         CfCommonIntZeroOk, "vvvl", s, i, o,
                                         (long)m);
                break;
            default:
                res = NULL;
        }
        vinfo_xdecref(own_i, po);
        return res != NULL;
    }

#if LE_HAS_NOT_A_SEQUENCE_ERROR
    objobjargproc mp_ass_subscript = NULL;
    if (tp_s->tp_as_mapping != NULL) {
        switch (Psyco_TypeSlot(po, tp_s,
                               &tp_s->tp_as_mapping->mp_ass_subscript,
                               (void **)&mp_ass_subscript)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_SetItem,
                                          CfCommonIntZeroOk, "vvv", s, i,
                                          o) != NULL;
            default:
                return false;
        }
    }
    if (mp_ass_subscript != NULL) {
        type_error(po, LE_MSG_SEQ_SET_NOT_SEQUENCE, s);
        return false;
    }
#endif
    type_error(po, LE_MSG_SEQ_NO_SET, s);
    return false;
}

DEFINEFN bool
PsycoSequence_DelItem(PsycoObject *po, vinfo_t *s, vinfo_t *i)
{
    PyTypeObject *tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL)
        return false;

    PySequenceMethods *m = tp_s->tp_as_sequence;
    ssizeobjargproc sq_ass_item = NULL;
    if (m != NULL) {
        switch (
            Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&sq_ass_item)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_DelItem,
                                          CfCommonIntZeroOk, "vv", s,
                                          i) != NULL;
            default:
                return false;
        }
    }
    if (sq_ass_item != NULL) {
        vinfo_t *own_i = NULL; /* i plus the length, held here */
        condition_code_t i_negative = integer_cmp_i(po, i, 0, Py_LT);
        if (i_negative == CC_ERROR)
            return false;
        if (runtime_condition_f(po, i_negative)) {
            lenfunc sq_length;
            switch (
                Psyco_TypeSlot(po, tp_s, &m->sq_length, (void **)&sq_length)) {
                case 1:
                    break;
                case 0:
                    return psyco_generic_call(po, PySequence_DelItem,
                                              CfCommonIntZeroOk, "vv", s,
                                              i) != NULL;
                default:
                    return false;
            }
            if (sq_length != NULL) {
                vinfo_t *l =
                    Psyco_META1(po, sq_length, CfCommonPySSizeTResult, "v", s);
                if (l == NULL)
                    return false;
                own_i = integer_add(po, i, l, false);
                vinfo_decref(l, po);
                if (own_i == NULL)
                    return false;
                i = own_i;
            }
        }
        else {
            assert_nonneg(i);
        }
        vinfo_t *res;
        switch (
            Psyco_TypeSlot(po, tp_s, &m->sq_ass_item, (void **)&sq_ass_item)) {
            case 1:
                res = Psyco_META3(po, sq_ass_item, CfCommonIntZeroOk, "vvl", s,
                                  i, (long)NULL);
                break;
            case 0:
                res = psyco_generic_call(po, LeSequence_DelItem,
                                         CfCommonIntZeroOk, "vvl", s, i,
                                         (long)m);
                break;
            default:
                res = NULL;
        }
        vinfo_xdecref(own_i, po);
        return res != NULL;
    }

#if LE_HAS_NOT_A_SEQUENCE_ERROR
    objobjargproc mp_ass_subscript = NULL;
    if (tp_s->tp_as_mapping != NULL) {
        switch (Psyco_TypeSlot(po, tp_s,
                               &tp_s->tp_as_mapping->mp_ass_subscript,
                               (void **)&mp_ass_subscript)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_DelItem,
                                          CfCommonIntZeroOk, "vv", s,
                                          i) != NULL;
            default:
                return false;
        }
    }
    if (mp_ass_subscript != NULL) {
        type_error(po, LE_MSG_SEQ_DEL_NOT_SEQUENCE, s);
        return false;
    }
#endif
    type_error(po, LE_MSG_SEQ_NO_DEL, s);
    return false;
}

DEFINEFN bool
PsycoSequence_SetSlice(PsycoObject *po, vinfo_t *s, vinfo_t *i1, vinfo_t *i2,
                       vinfo_t *o)
{
    PyTypeObject *tp_s = Psyco_NeedType(po, s);
    if (tp_s == NULL)
        return false;

    PyMappingMethods *mp = tp_s->tp_as_mapping;
    objobjargproc mp_ass_subscript = NULL;
    if (mp != NULL) {
        switch (Psyco_TypeSlot(po, tp_s, &mp->mp_ass_subscript,
                               (void **)&mp_ass_subscript)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_SetSlice,
                                          CfCommonIntZeroOk, "vvvv", s, i1, i2,
                                          o) != NULL;
            default:
                return false;
        }
    }
    if (mp_ass_subscript != NULL) {
        vinfo_t *slice = psyco_generic_call(
            po, _PySlice_FromIndices, CfCommonNewRefPyObject, "vv", i1, i2);
        if (slice == NULL)
            return false;
        vinfo_t *res;
        switch (Psyco_TypeSlot(po, tp_s, &mp->mp_ass_subscript,
                               (void **)&mp_ass_subscript)) {
            case 1:
                res = Psyco_META3(po, mp_ass_subscript, CfCommonIntZeroOk,
                                  "vvv", s, slice, o);
                break;
            case 0:
                res = psyco_generic_call(po, LeSequence_SetSlice,
                                         CfCommonIntZeroOk, "vvlv", s, o,
                                         (long)mp, slice);
                break;
            default:
                res = NULL;
        }
        vinfo_decref(slice, po);
        return res != NULL;
    }

    type_error(po, LE_MSG_SEQ_SET_SLICE, s);
    return false;
}

DEFINEFN vinfo_t *
PsycoSequence_Tuple(PsycoObject *po, vinfo_t *v)
{
    PyTypeObject *tp_v = Psyco_NeedType(po, v);
    if (tp_v == NULL)
        return NULL;

    /* Special-case the common tuple and list cases, for efficiency. */
    if (tp_v == &PyTuple_Type) {
        vinfo_incref(v);
        return v;
    }
    vinfo_t *result;
    if (tp_v == &PyList_Type) {
        result = psyco_generic_call(
            po, PyList_AsTuple,
            CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v", v);
    }
    else {
        result = psyco_generic_call(po, PySequence_Tuple,
                                    CfCommonNewRefPyObject, "v", v);
    }
    if (result != NULL)
        Psyco_AssertType(po, result, &PyTuple_Type);
    return result;
}

DEFINEFN vinfo_t *
PsycoSequence_List(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *result = psyco_generic_call(po, PySequence_List,
                                         CfCommonNewRefPyObject, "v", v);
    if (result != NULL)
        Psyco_AssertType(po, result, &PyList_Type);
    return result;
}

DEFINEFN vinfo_t *
PsycoSequence_Contains(PsycoObject *po, vinfo_t *seq, vinfo_t *ob)
{
    PyTypeObject *tp_seq = Psyco_NeedType(po, seq);
    if (tp_seq == NULL)
        return NULL;

    PySequenceMethods *sqm = tp_seq->tp_as_sequence;
    objobjproc sq_contains = NULL;
    if (sqm != NULL) {
        switch (Psyco_TypeSlot(po, tp_seq, &sqm->sq_contains,
                               (void **)&sq_contains)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PySequence_Contains,
                                          CfCommonIntBoolOrError, "vv", seq,
                                          ob);
            default:
                return NULL;
        }
    }
    if (sq_contains != NULL) {
        return Psyco_META2(po, sq_contains, CfCommonIntBoolOrError, "vv", seq,
                           ob);
    }
    return psyco_generic_call(po, PySequence_Contains, CfCommonIntBoolOrError,
                              "vv", seq, ob);
}

/* Operations on mappings */

DEFINEFN vinfo_t *
PsycoMapping_Size(PsycoObject *po, vinfo_t *o)
{
    PyTypeObject *tp_o = Psyco_NeedType(po, o);
    if (tp_o == NULL)
        return NULL;

    PyMappingMethods *m = tp_o->tp_as_mapping;
    lenfunc mp_length = NULL;
    if (m != NULL) {
        switch (Psyco_TypeSlot(po, tp_o, &m->mp_length, (void **)&mp_length)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyMapping_Size,
                                          CfCommonPySSizeTResult, "v", o);
            default:
                return NULL;
        }
    }
    if (mp_length != NULL) {
        return Psyco_META1(po, mp_length, CfCommonPySSizeTResult, "v", o);
    }

#if LE_HAS_NOT_A_SEQUENCE_ERROR
    lenfunc sq_length = NULL;
    if (tp_o->tp_as_sequence != NULL) {
        switch (Psyco_TypeSlot(po, tp_o, &tp_o->tp_as_sequence->sq_length,
                               (void **)&sq_length)) {
            case 1:
                break;
            case 0:
                return psyco_generic_call(po, PyMapping_Size,
                                          CfCommonPySSizeTResult, "v", o);
            default:
                return NULL;
        }
    }
    if (sq_length != NULL) {
        type_error(po, LE_MSG_MAPPING_SIZE_NOT_MAPPING, o);
        return NULL;
    }
#endif
    type_error(po, LE_MSG_MAPPING_NO_LEN, o);
    return NULL;
}

/* Iterators */

#if LE_HAS_ITER_CHECK_FUNCTION
DEFINEFN vinfo_t *
PsycoIter_Check(PsycoObject *po, vinfo_t *obj)
{
    PyTypeObject *tp = Psyco_NeedType(po, obj);
    if (tp == NULL)
        return NULL;
    iternextfunc tp_iternext;
    switch (Psyco_TypeSlot(po, tp, &tp->tp_iternext, (void **)&tp_iternext)) {
        case 1:
            break;
        case 0:
            return psyco_generic_call(
                po, PyIter_Check,
                CfCommonIntBoolOrError | CfNoPython | CfNoBlock, "v", obj);
        default:
            return NULL;
    }
    /* the test against _PyObject_NextNotImplemented is left to run time */
    if (tp_iternext != NULL) {
        return psyco_generic_call(
            po, PyIter_Check, CfCommonIntBoolOrError | CfNoPython | CfNoBlock,
            "v", obj);
    }
    return vinfo_new(CompileTime_New(0));
}
#endif

DEFINEFN
vinfo_t *
PsycoObject_GetIter(PsycoObject *po, vinfo_t *vi)
{
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
        return type_error_tp(po, LE_MSG_NOT_ITERABLE, t);
    }
    /* a heap type's __iter__ may return a non-iterator, which
       PyObject_GetIter checks */
    if (!Psyco_SlotsMutable(t))
        return Psyco_META1(po, f, CfCommonNewRefPyObject, "v", vi);
generic:
    return psyco_generic_call(po, PyObject_GetIter, CfCommonNewRefPyObject,
                              "v", vi);
}

/* ceval.c's FOR_ITER: GET_ITER made sure the slot was there, but a heap
   type's can have gone since (CPython leaves _PyObject_NextNotImplemented,
   which raises TypeError). The iterator of a frame that didn't make it
   (a generator expression's .0, replaced or given) may have none: that
   raises what next() and 3.14's FOR_ITER do. */
static PyObject *
cimpl_iternext(PyObject *iter)
{
    iternextfunc f = Py_TYPE(iter)->tp_iternext;
    if (f == NULL) {
        PyErr_Format(PyExc_TypeError, LE_MSG_NEXT_NOT_ITERATOR,
                     Py_TYPE(iter)->tp_name);
        return NULL;
    }
    return f(iter);
}

DEFINEFN
vinfo_t *
PsycoIter_Next(PsycoObject *po, vinfo_t *iter)
{
    iternextfunc f;
    PyTypeObject *tp = Psyco_NeedType(po, iter);
    if (tp == NULL)
        return NULL;
    switch (Psyco_TypeSlot(po, tp, &tp->tp_iternext, (void **)&f)) {
        case 1:
            break;
        case 0:
            return psyco_generic_call(po, cimpl_iternext, CfCommonIterNext,
                                      "v", iter);
        default:
            return NULL;
    }
    if (f == NULL) {
        PycException_SetTypeFormat(po, PyExc_TypeError,
                                   LE_MSG_NEXT_NOT_ITERATOR, NULL, tp, NULL);
        return NULL;
    }
    return Psyco_META1(po, f, CfCommonIterNext, "v", iter);
}

/* tuplesubscript, list_subscript and list_ass_subscript count a negative
   index from the object's own size and call their own item functions, not
   the type's slots, which a subclass's __len__ replaces */
static PyTypeObject *
subscript_base(PsycoObject *po, vinfo_t *o)
{
    PyTypeObject *tp = Psyco_NeedType(po, o);
    if (tp == NULL)
        return NULL;
    if (PyType_TypeCheck(tp, &PyList_Type))
        return &PyList_Type;
    extra_assert(PyType_TypeCheck(tp, &PyTuple_Type));
    return &PyTuple_Type;
}

/* the index plus the size if it is negative: a new reference */
static vinfo_t *
own_size_index(PsycoObject *po, PyTypeObject *base, vinfo_t *o, vinfo_t *i)
{
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
vinfo_t *
psyco_generic_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key)
{
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
    }
    else {
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
bool
psyco_generic_ass_subscript(PsycoObject *po, vinfo_t *o, vinfo_t *key,
                            vinfo_t *value)
{
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
            result =
                Psyco_META3(po, base->tp_as_sequence->sq_ass_item,
                            CfCommonIntZeroOk, "vvv", o, index, value) != NULL;
        else
            result = Psyco_META3(po, base->tp_as_sequence->sq_ass_item,
                                 CfCommonIntZeroOk, "vvl", o, index,
                                 (long)NULL) != NULL;
        vinfo_decref(index, po);
        return result;
    }
    else {
        char *vargs = (value != NULL) ? "vvv" : "vvl";
        PyTypeObject *tp = Psyco_NeedType(po, o);
        if (tp == NULL)
            return false;
        extra_assert(tp->tp_as_mapping != NULL);
        extra_assert(tp->tp_as_mapping->mp_ass_subscript != NULL);
        return psyco_generic_call(po, tp->tp_as_mapping->mp_ass_subscript,
                                  CfCommonIntZeroOk, vargs, o, key,
                                  value) != NULL;
    }
}
