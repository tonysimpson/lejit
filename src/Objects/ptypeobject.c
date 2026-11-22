/*
 * typeobject.c's slot functions for special methods defined in Python
 * (slot_nb_add and the rest of SLOT1BINFULL, the SLOT0 unary ones,
 * slot_tp_richcompare, slot_mp_subscript, slot_mp_ass_subscript): a user's
 * __add__ or __getitem__ called from compiled code is a
 * direct call of the compiled function, as CALL_FUNCTION's, instead of the
 * residual slot function entering it from C through the eval hook.
 *
 * The slot functions are static in typeobject.c: their addresses are read
 * from a class made at start-up (probe_slots), as Psyco did for
 * slot_tp_init. Each meta follows its slot function with the types known:
 * the method comes from Psyco_TypeLookup (recorded, behind a version-tag
 * guard), and anything else CPython would do (a method that isn't a plain
 * function, the reflected operand's subclass case with method_is_overloaded,
 * a missing method that raises) is the slot function itself, called before
 * any method has been, so nothing runs twice. Every guard is emitted
 * before the first call.
 *
 * The known types are right only until something runs Python code
 * (po->pr.class_unchecked: a __class__ assignment), and the instruction
 * forgets them only when it ends. So a meta reached after an earlier call
 * of the same instruction (a second slot of binary_op1 or
 * try_rich_compare) is the slot function, and the reflected call after
 * self's method declined is binary_rest, CPython's end of SLOT1BINFULL
 * with the types as they are then.
 *
 * The calls are never inlined: an inlined body returns by restarting the
 * instruction (pycompiler.c, EReturn), which would run a first call's
 * NotImplemented check and the guards again before a second call, and
 * hand the first call's result to the second.
 */
#include "ptypeobject.h"
#include "pfuncobject.h"
#include "ptupleobject.h"

/* the slot functions, from probe_slots; NULL if not found */
typedef struct {
    const char *op, *rop; /* rop NULL: a unary SLOT0 */
    size_t offset;        /* in PyNumberMethods */
    void *cfunc;
    PyObject *op_name, *rop_name;
} number_slot_t;

#define NB(slot) offsetof(PyNumberMethods, slot)
static number_slot_t number_slots[] = {
    {"__add__", "__radd__", NB(nb_add)},
    {"__sub__", "__rsub__", NB(nb_subtract)},
    {"__mul__", "__rmul__", NB(nb_multiply)},
    {"__mod__", "__rmod__", NB(nb_remainder)},
    {"__divmod__", "__rdivmod__", NB(nb_divmod)},
    {"__lshift__", "__rlshift__", NB(nb_lshift)},
    {"__rshift__", "__rrshift__", NB(nb_rshift)},
    {"__and__", "__rand__", NB(nb_and)},
    {"__xor__", "__rxor__", NB(nb_xor)},
    {"__or__", "__ror__", NB(nb_or)},
    {"__floordiv__", "__rfloordiv__", NB(nb_floor_divide)},
    {"__truediv__", "__rtruediv__", NB(nb_true_divide)},
    {"__matmul__", "__rmatmul__", NB(nb_matrix_multiply)},
    {"__neg__", NULL, NB(nb_negative)},
    {"__pos__", NULL, NB(nb_positive)},
    {"__abs__", NULL, NB(nb_absolute)},
    {"__invert__", NULL, NB(nb_invert)},
};
#undef NB
#define N_NUMBER_SLOTS (sizeof(number_slots) / sizeof(number_slots[0]))

/* object.c's order of rich comparison operators */
static const char *const richcmp_names[] = {"__lt__", "__le__", "__eq__",
                                            "__ne__", "__gt__", "__ge__"};
static PyObject *richcmp_name[6], *init_name;
static richcmpfunc slot_tp_richcompare;

/* The special method 'name' of tp (a heap type, through whose slot the
   call came), at key 'key': 1 with *f the function (NULL: none), 0 if
   the method isn't a plain function (the caller calls the slot function),
   2 on the path where the type changed (the repair is emitted: the caller
   calls the slot function), -1 on error. */
static int
special_method(PsycoObject *po, PyTypeObject *tp, void *key, PyObject *name,
               PyObject **f)
{
    PyObject *descr;
    int r = Psyco_TypeLookup(po, tp, key, name, &descr);
    if (r < 0)
        return -1;
    if (r == 0) {
        if (psyco_generic_call(po, cimpl_type_lookup_changed,
                               CfNoReturnValue | CfNoPython | CfNoBlock, "lll",
                               (long)tp, (long)name, (long)descr) == NULL)
            return -1;
        return 2;
    }
    if (descr != NULL && !PyFunction_Check(descr))
        return 0;
    *f = descr;
    return 1;
}

/* call_unbound(1, f, self, {args}): a direct call of f with self and
   'n' more arguments, compiled separately (see the header) */
static vinfo_t *
call_special_n(PsycoObject *po, PyObject *f, vinfo_t *self, vinfo_t **arg,
               int n)
{
    vinfo_t *vf, *args, *r;
    int i;
    args = PsycoTuple_New(n + 1, NULL);
    vinfo_incref(self);
    PsycoTuple_GET_ITEM(args, 0) = self;
    for (i = 0; i < n; i++) {
        vinfo_incref(arg[i]);
        PsycoTuple_GET_ITEM(args, i + 1) = arg[i];
    }
    Py_INCREF(f);
    vf = vinfo_new(CompileTime_NewSk(sk_new((long)f, SkFlagPyObj)));
    r = pfunction_simple_call(po, vf, f, args, false);
    vinfo_decref(vf, po);
    vinfo_decref(args, po);
    return r;
}

static vinfo_t *
call_special(PsycoObject *po, PyObject *f, vinfo_t *self, vinfo_t *arg)
{
    return call_special_n(po, f, self, &arg, arg != NULL);
}

/* r, or exactly psyco_vi_NotImplemented() on the path where it is
   NotImplemented at run time, as CfPyErrNotImplemented gives */
static vinfo_t *
not_implemented(PsycoObject *po, vinfo_t *r)
{
    condition_code_t cc;
    if (r == NULL)
        return NULL;
    cc = integer_cmp_i(po, r, (long)Py_NotImplemented, Py_EQ);
    if (cc == CC_ERROR) {
        vinfo_decref(r, po);
        return NULL;
    }
    if (runtime_condition_f(po, cc)) {
        vinfo_decref(r, po);
        return psyco_vi_NotImplemented();
    }
    return r;
}

/* 1 with *f the slot, 0 on the path where it changed, -1 */
static int
number_slot(PsycoObject *po, PyTypeObject *tp, size_t offset, void **f)
{
    *f = NULL;
    if (tp->tp_as_number == NULL)
        return 1;
    return Psyco_TypeSlot(po, tp, (char *)tp->tp_as_number + offset, f);
}

/* SLOT1BINFULL's end after self's method returned NotImplemented, when
   other's slot was the same slot function: the method may have changed
   either class */
static PyObject *
binary_rest(PyObject *self, PyObject *other, PyObject *rop)
{
    PyObject *f, *r;
    descrgetfunc get;
    if (Py_TYPE(other) == Py_TYPE(self))
        Py_RETURN_NOTIMPLEMENTED;
    /* call_maybe(other, rop, {self}) */
    f = _PyType_Lookup(Py_TYPE(other), rop);
    if (f == NULL)
        Py_RETURN_NOTIMPLEMENTED;
    Py_INCREF(f);
    if (PyFunction_Check(f))
        r = PyObject_CallFunctionObjArgs(f, other, self, NULL);
    else {
        get = Py_TYPE(f)->tp_descr_get;
        if (get != NULL) {
            PyObject *bound = get(f, other, (PyObject *)Py_TYPE(other));
            Py_DECREF(f);
            if (bound == NULL)
                return NULL;
            f = bound;
        }
        r = PyObject_CallFunctionObjArgs(f, self, NULL);
    }
    Py_DECREF(f);
    return r;
}

/* SLOT1BINFULL(FUNCNAME, FUNCNAME, ...) */
static vinfo_t *
slot_binary(PsycoObject *po, const number_slot_t *s, vinfo_t *self,
            vinfo_t *other)
{
    PyTypeObject *ts = Psyco_KnownType(self), *to = Psyco_KnownType(other);
    void *fs, *fo = NULL;
    PyObject *ms = NULL, *mo = NULL;
    bool do_other = false, self_has, sub;
    vinfo_t *r;

    /* the slot was read from known types, unchecked only after an earlier
       call of this instruction */
    if (ts == NULL || to == NULL || po->pr.class_unchecked)
        goto generic;
    if (ts != to) {
        switch (number_slot(po, to, s->offset, &fo)) {
            case 1:
                break;
            case 0:
                goto generic;
            default:
                return NULL;
        }
        do_other = fo == s->cfunc;
    }
    switch (number_slot(po, ts, s->offset, &fs)) {
        case 1:
            break;
        case 0:
            goto generic;
        default:
            return NULL;
    }
    self_has = fs == s->cfunc;
    if (self_has && do_other) {
        switch (Psyco_TypeIsSubtype(po, to, ts, &sub)) {
            case 1:
                break;
            case 0:
                goto generic;
            default:
                return NULL;
        }
        /* method_is_overloaded reads both classes' attributes through their
           metaclass and compares them with PyObject_RichCompareBool */
        if (sub)
            goto generic;
    }
    if (self_has)
        switch (special_method(po, ts, (char *)ts->tp_as_number + s->offset,
                               s->op_name, &ms)) {
            case 1:
                break;
            case -1:
                return NULL;
            default:
                goto generic;
        }
    /* after self's method, binary_rest finds other's */
    if (do_other && !(self_has && ms != NULL))
        switch (special_method(po, to,
                               (char *)to->tp_as_number + s->offset + 1,
                               s->rop_name, &mo)) {
            case 1:
                break;
            case -1:
                return NULL;
            default:
                goto generic;
        }

    if (self_has) {
        r = ms != NULL ? not_implemented(po, call_special(po, ms, self, other))
                       : psyco_vi_NotImplemented();
        if (IS_IMPLEMENTED(r) || to == ts)
            return r;
        vinfo_decref(r, po);
        if (do_other && ms != NULL)
            return psyco_generic_call(po, binary_rest,
                                      CfCommonCheckNotImplemented, "vvl", self,
                                      other, (long)s->rop_name);
    }
    if (do_other)
        return mo != NULL
                   ? not_implemented(po, call_special(po, mo, other, self))
                   : psyco_vi_NotImplemented();
    return psyco_vi_NotImplemented();

generic:
    return psyco_generic_call(po, s->cfunc, CfCommonCheckNotImplemented, "vv",
                              self, other);
}

/* SLOT0: call_method(self, op), whose lookup_method raises
   AttributeError for a missing method */
static vinfo_t *
slot_unary(PsycoObject *po, const number_slot_t *s, vinfo_t *self)
{
    PyTypeObject *ts = Psyco_KnownType(self);
    PyObject *m = NULL;
    if (ts == NULL || ts->tp_as_number == NULL || po->pr.class_unchecked)
        goto generic;
    switch (special_method(po, ts, (char *)ts->tp_as_number + s->offset,
                           s->op_name, &m)) {
        case 1:
            break;
        case -1:
            return NULL;
        default:
            goto generic;
    }
    if (m != NULL)
        return call_special(po, m, self, NULL);
generic:
    return psyco_generic_call(po, s->cfunc, CfCommonNewRefPyObject, "v", self);
}

/* slot_tp_richcompare: call_unbound with a method found, else
   NotImplemented */
static vinfo_t *
pslot_tp_richcompare(PsycoObject *po, vinfo_t *self, vinfo_t *other, long op)
{
    PyTypeObject *ts = Psyco_KnownType(self);
    PyObject *m = NULL;
    if (ts == NULL || op < 0 || op > 5 || po->pr.class_unchecked)
        goto generic;
    switch (special_method(po, ts, (char *)&ts->tp_richcompare + op,
                           richcmp_name[op], &m)) {
        case 1:
            break;
        case -1:
            return NULL;
        default:
            goto generic;
    }
    if (m == NULL)
        return psyco_vi_NotImplemented();
    return not_implemented(po, call_special(po, m, self, other));
generic:
    return psyco_generic_call(po, slot_tp_richcompare,
                              CfCommonCheckNotImplemented, "vvl", self, other,
                              op);
}

/* SLOT1(slot_mp_subscript, "__getitem__") and slot_mp_ass_subscript,
   whose vectorcall_method raises AttributeError for a missing method: the
   slot function then. A store's value is NULL for a delete, which
   PyObject_DelItem passes as a constant; __delitem__'s lookup is keyed
   one byte past the slot __setitem__'s shares. */
static binaryfunc slot_mp_subscript;
static objobjargproc slot_mp_ass_subscript;
static PyObject *getitem_name, *setitem_name, *delitem_name;

static vinfo_t *
pslot_mp_subscript(PsycoObject *po, vinfo_t *self, vinfo_t *key)
{
    PyTypeObject *ts = Psyco_KnownType(self);
    PyObject *m = NULL;
    if (ts == NULL || ts->tp_as_mapping == NULL || po->pr.class_unchecked)
        goto generic;
    switch (special_method(po, ts, &ts->tp_as_mapping->mp_subscript,
                           getitem_name, &m)) {
        case 1:
            break;
        case -1:
            return NULL;
        default:
            goto generic;
    }
    if (m != NULL)
        return call_special(po, m, self, key);
generic:
    return psyco_generic_call(po, slot_mp_subscript, CfCommonNewRefPyObject,
                              "vv", self, key);
}

static vinfo_t *
pslot_mp_ass_subscript(PsycoObject *po, vinfo_t *self, vinfo_t *key,
                       vinfo_t *value)
{
    PyTypeObject *ts = Psyco_KnownType(self);
    PyObject *m = NULL;
    vinfo_t *args[2], *r;
    if (ts == NULL || ts->tp_as_mapping == NULL || po->pr.class_unchecked)
        goto generic;
    switch (special_method(
        po, ts, (char *)&ts->tp_as_mapping->mp_ass_subscript + (value == NULL),
        value == NULL ? delitem_name : setitem_name, &m)) {
        case 1:
            break;
        case -1:
            return NULL;
        default:
            goto generic;
    }
    if (m == NULL)
        goto generic;
    args[0] = key;
    args[1] = value;
    r = call_special_n(po, m, self, args, value != NULL ? 2 : 1);
    if (r == NULL)
        return NULL;
    vinfo_decref(r, po);
    return VINFO_OK;
generic:
    if (value == NULL)
        return psyco_generic_call(po, slot_mp_ass_subscript, CfCommonIntZeroOk,
                                  "vvl", self, key, (long)NULL);
    return psyco_generic_call(po, slot_mp_ass_subscript, CfCommonIntZeroOk,
                              "vvv", self, key, value);
}

/* type_call(tp, args, NULL) for a heap class whose tp_new is object_new
   and whose tp_init is slot_tp_init with a plain function as __init__,
   or object_init with no arguments; anything else is PyObject_Call. The
   instance is new, so its type is tp and type_call's subtype test
   passes. */
static initproc slot_tp_init;

/* object_new once its excess_args test has passed at compile time: the
   allocation, or object_new itself for its error, and where classes have
   managed dicts for the instance's values (_PyObject_InitializeDict),
   which only it can give */
static PyObject *
object_new_checked(PyTypeObject *tp)
{
    PyObject *r, *args;
    if (!LE_HAS_MANAGED_DICT && !(tp->tp_flags & Py_TPFLAGS_IS_ABSTRACT))
        return tp->tp_alloc(tp, 0);
    args = PyTuple_New(0);
    if (args == NULL)
        return NULL;
    r = PyBaseObject_Type.tp_new(tp, args, NULL);
    Py_DECREF(args);
    return r;
}

/* slot_tp_init's error for a result that isn't None */
static PyObject *
init_result_error(PyObject *res)
{
    PyErr_Format(PyExc_TypeError, LE_MSG_INIT_RETURNED, Py_TYPE(res)->tp_name);
    return NULL;
}

DEFINEFN
vinfo_t *
psyco_heap_type_call(PsycoObject *po, PyTypeObject *tp, vinfo_t *vtype,
                     vinfo_t *varg, vinfo_t *vkw)
{
    newfunc new;
    initproc init;
    PyObject *m = NULL;
    vinfo_t *vobj, *args, *vf, *r;
    condition_code_t cc;
    int i, n = PsycoTuple_Load(varg);

    if (n < 0 || slot_tp_init == NULL || !(tp->tp_flags & Py_TPFLAGS_HEAPTYPE))
        goto fallback;
    switch (Psyco_TypeSlot(po, tp, &tp->tp_new, (void **)&new)) {
        case 1:
            break;
        case 0:
            goto fallback;
        default:
            return NULL;
    }
    if (new != PyBaseObject_Type.tp_new)
        goto fallback;
    switch (Psyco_TypeSlot(po, tp, &tp->tp_init, (void **)&init)) {
        case 1:
            break;
        case 0:
            goto fallback;
        default:
            return NULL;
    }
    if (init == PyBaseObject_Type.tp_init ? n > 0 : init != slot_tp_init)
        goto fallback;
    if (init == slot_tp_init) {
        switch (special_method(po, tp, &tp->tp_init, init_name, &m)) {
            case 1:
                break;
            case -1:
                return NULL;
            default:
                goto fallback;
        }
        /* none: lookup_method's AttributeError */
        if (m == NULL)
            goto fallback;
    }

    vobj = psyco_generic_call(po, object_new_checked, CfCommonNewRefPyObject,
                              "l", (long)tp);
    if (vobj == NULL || m == NULL)
        return vobj;
    /* slot_tp_init: _PyObject_Call_Prepend(__init__, self, args, NULL) */
    args = PsycoTuple_New(n + 1, NULL);
    vinfo_incref(vobj);
    PsycoTuple_GET_ITEM(args, 0) = vobj;
    for (i = 0; i < n; i++) {
        vinfo_t *v = PsycoTuple_GET_ITEM(varg, i);
        vinfo_incref(v);
        PsycoTuple_GET_ITEM(args, i + 1) = v;
    }
    Py_INCREF(m);
    vf = vinfo_new(CompileTime_NewSk(sk_new((long)m, SkFlagPyObj)));
    r = pfunction_simple_call(po, vf, m, args, false);
    vinfo_decref(vf, po);
    vinfo_decref(args, po);
    if (r == NULL)
        goto error;
    cc = integer_cmp_i(po, r, (long)Py_None, Py_NE);
    if (cc == CC_ERROR)
        goto error_r;
    if (runtime_condition_f(po, cc)) {
        psyco_generic_call(po, init_result_error,
                           CfReturnTypePtr | CfPyErrAlways, "v", r);
        goto error_r;
    }
    vinfo_decref(r, po);
    return vobj;

error_r:
    vinfo_decref(r, po);
error:
    vinfo_decref(vobj, po);
    return NULL;
fallback:
    return psyco_generic_call(po, PyObject_Call, CfCommonNewRefPyObject, "vvv",
                              vtype, varg, vkw);
}

#define BINARY(i)                                                             \
    static vinfo_t *pslot_binary_##i(PsycoObject *po, vinfo_t *a, vinfo_t *b) \
    {                                                                         \
        return slot_binary(po, &number_slots[i], a, b);                       \
    }
#define UNARY(i)                                                 \
    static vinfo_t *pslot_unary_##i(PsycoObject *po, vinfo_t *a) \
    {                                                            \
        return slot_unary(po, &number_slots[i], a);              \
    }
/* clang-format off */
BINARY(0) BINARY(1) BINARY(2) BINARY(3) BINARY(4) BINARY(5) BINARY(6)
BINARY(7) BINARY(8) BINARY(9) BINARY(10) BINARY(11) BINARY(12)
UNARY(13) UNARY(14) UNARY(15) UNARY(16)
#undef BINARY
#undef UNARY
static void *const number_metas[N_NUMBER_SLOTS] = {
    pslot_binary_0,  pslot_binary_1,  pslot_binary_2,  pslot_binary_3,
    pslot_binary_4,  pslot_binary_5,  pslot_binary_6,  pslot_binary_7,
    pslot_binary_8,  pslot_binary_9,  pslot_binary_10, pslot_binary_11,
    pslot_binary_12, pslot_unary_13,  pslot_unary_14,  pslot_unary_15,
    pslot_unary_16,
};
/* clang-format on */

static PyObject *
slot_name(const char *s)
{
    PyObject *o = PyUnicode_InternFromString(s);
    if (o == NULL)
        OUT_OF_MEMORY();
    return o;
}

/* a class defining every special method above: type_new gives it the
   slot functions (update_one_slot's generic case, as the values are
   neither wrappers nor None) */
static PyTypeObject *
probe_slots(void)
{
    PyObject *d = PyDict_New(), *t = NULL;
    size_t i;
    if (d == NULL)
        return NULL;
    for (i = 0; i < N_NUMBER_SLOTS; i++)
        if (PyDict_SetItemString(d, number_slots[i].op, Py_True) < 0 ||
            (number_slots[i].rop != NULL &&
             PyDict_SetItemString(d, number_slots[i].rop, Py_True) < 0))
            goto done;
    for (i = 0; i < 6; i++)
        if (PyDict_SetItemString(d, richcmp_names[i], Py_True) < 0)
            goto done;
    if (PyDict_SetItemString(d, "__init__", Py_True) < 0 ||
        PyDict_SetItemString(d, "__getitem__", Py_True) < 0 ||
        PyDict_SetItemString(d, "__setitem__", Py_True) < 0)
        goto done;
    t = PyObject_CallFunction((PyObject *)&PyType_Type, "s(O)O", "le_slots",
                              (PyObject *)&PyBaseObject_Type, d);
    if (t != NULL && !PyType_Check(t))
        Py_CLEAR(t);
done:
    Py_DECREF(d);
    return (PyTypeObject *)t;
}

INITIALIZATIONFN
void
psy_typeobject_init(void)
{
    PyTypeObject *t = probe_slots();
    size_t i;
    if (t == NULL) {
        PyErr_Clear();
        return;
    }
    for (i = 0; i < N_NUMBER_SLOTS; i++) {
        number_slot_t *s = &number_slots[i];
        s->cfunc = *(void **)((char *)t->tp_as_number + s->offset);
        s->op_name = slot_name(s->op);
        s->rop_name = s->rop != NULL ? slot_name(s->rop) : NULL;
        if (s->cfunc != NULL)
            Psyco_DefineMeta(s->cfunc, number_metas[i]);
    }
    for (i = 0; i < 6; i++) richcmp_name[i] = slot_name(richcmp_names[i]);
    init_name = slot_name("__init__");
    slot_tp_init = t->tp_init;
    slot_tp_richcompare = t->tp_richcompare;
    if (slot_tp_richcompare != NULL)
        Psyco_DefineMeta(slot_tp_richcompare, pslot_tp_richcompare);
    getitem_name = slot_name("__getitem__");
    setitem_name = slot_name("__setitem__");
    delitem_name = slot_name("__delitem__");
    slot_mp_subscript = t->tp_as_mapping->mp_subscript;
    slot_mp_ass_subscript = t->tp_as_mapping->mp_ass_subscript;
    if (slot_mp_subscript != NULL)
        Psyco_DefineMeta(slot_mp_subscript, pslot_mp_subscript);
    if (slot_mp_ass_subscript != NULL)
        Psyco_DefineMeta(slot_mp_ass_subscript, pslot_mp_ass_subscript);
    Py_DECREF(t);
}
