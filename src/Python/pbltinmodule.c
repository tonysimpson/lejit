#include "pbltinmodule.h"
#include "../Objects/pboolobject.h"
#include "../Objects/pfloatobject.h"
#include "../Objects/plistobject.h"
#include "../Objects/plongobject.h"
#include "../Objects/pmethodobject.h"
#include "../Objects/prangeobject.h"
#include "../Objects/ptupleobject.h"
#include "../Objects/ptypeobject.h"
#include "../psyfunc.h"
#include "../mergepoints.h"
#include "../initialize.h"

/* the metas of Python/bltinmodule.c's abs, callable, delattr, divmod,
   getattr, hasattr, hash, id, isinstance, issubclass, len, pow and setattr;
   gen_bltinmodule_init registers them. The rest of this file is LeJIT's
   own, each with its reason. */
/* the C functions, for the fallbacks: set by the registration */
static PyCFunction cfunc_builtin_abs;
static PyCFunction cfunc_builtin_callable;
static PyCFunction cfunc_builtin_delattr;
static PyCFunction cfunc_builtin_divmod;
static PyCFunction cfunc_builtin_getattr;
static PyCFunction cfunc_builtin_hasattr;
static PyCFunction cfunc_builtin_hash;
static PyCFunction cfunc_builtin_id;
static PyCFunction cfunc_builtin_isinstance;
static PyCFunction cfunc_builtin_issubclass;
static PyCFunction cfunc_builtin_len;
static PyCFunction cfunc_builtin_next;
static PyCFunction cfunc_builtin_pow;
static PyCFunction cfunc_builtin_setattr;

/* CPython's code, copied for the metas to call at run time */
static PyObject *
cimpl_builtin_hasattr_impl(PyObject *module, PyObject *obj, PyObject *name)
{
    PyObject *v;
    if (!PyUnicode_Check(name)) {
        PyErr_SetString(PyExc_TypeError, LE_MSG_HASATTR_NAME_NOT_STR);
        return NULL;
    }
    if (le_lookup_attr(obj, name, &v) < 0) {
        return NULL;
    }
    if (v == NULL) {
        Py_RETURN_FALSE;
    }
    Py_DECREF(v);
    Py_RETURN_TRUE;
}

static vinfo_t *
builtin_abs(PsycoObject *po, vinfo_t *module, vinfo_t *x)
{
    return PsycoNumber_Absolute(po, x);
}

static vinfo_t *
builtin_callable(PsycoObject *po, vinfo_t *module, vinfo_t *obj)
{
    vinfo_t *res, *x;
    x = psyco_generic_call(po, PyCallable_Check,
                           CfReturnTypeInt | CfNoPython | CfNoBlock, "v", obj);
    res = PsycoBool_FromLong(po, x);
    vinfo_decref(x, po);
    return res;
}

/* the Argument Clinic wrappers' check of the number of arguments: the
   number, or -1 for the C function to raise its TypeError */
static int
nargs_between(vinfo_t *vargs, int min, int max)
{
    int nargs = PsycoTuple_Load(vargs);
    return nargs < min || nargs > max ? -1 : nargs;
}
#define ARG(i) PsycoTuple_GET_ITEM(vargs, i)

static vinfo_t *
builtin_delattr(PsycoObject *po, vinfo_t *module, vinfo_t *vargs)
{
    if (nargs_between(vargs, 2, 2) < 0) {
        return PsycoCFunction_CallFast(po, cfunc_builtin_delattr,
                                       METH_FASTCALL, module, vargs);
    }
    if (!PsycoObject_SetAttr(po, ARG(0), ARG(1), NULL)) {
        return NULL;
    }
    return psyco_vi_None();
}

static vinfo_t *
builtin_divmod(PsycoObject *po, vinfo_t *module, vinfo_t *vargs)
{
    if (nargs_between(vargs, 2, 2) < 0) {
        return PsycoCFunction_CallFast(po, cfunc_builtin_divmod, METH_FASTCALL,
                                       module, vargs);
    }
    return PsycoNumber_Divmod(po, ARG(0), ARG(1));
}

static vinfo_t *
builtin_getattr(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    PyTypeObject *tp_name;
    int nargs = nargs_between(vargs, 2, 3);
    if (nargs < 0) {
        return PsycoCFunction_CallFast(po, cfunc_builtin_getattr,
                                       METH_FASTCALL, self, vargs);
    }
    tp_name = Psyco_NeedType(po, ARG(1));
    if (tp_name == NULL) {
        return NULL;
    }
    if ((tp_name->tp_flags & Py_TPFLAGS_UNICODE_SUBCLASS) == 0) {
        PycException_SetString(po, PyExc_TypeError,
                               LE_MSG_GETATTR_NAME_NOT_STR);
        return NULL;
    }
    if (nargs == 2) {
        return PsycoObject_GetAttr(po, ARG(0), ARG(1));
    }
    /* with a default: CPython's */
    return PsycoCFunction_CallFast(po, cfunc_builtin_getattr, METH_FASTCALL,
                                   self, vargs);
}

static vinfo_t *
builtin_hasattr_impl(PsycoObject *po, vinfo_t *module, vinfo_t *obj,
                     vinfo_t *name)
{
    PyTypeObject *tp_name = Psyco_NeedType(po, name);
    if (tp_name == NULL) {
        return NULL;
    }
    if ((tp_name->tp_flags & Py_TPFLAGS_UNICODE_SUBCLASS) == 0) {
        PycException_SetString(po, PyExc_TypeError,
                               LE_MSG_HASATTR_NAME_NOT_STR);
        return NULL;
    }
    return psyco_generic_call(po, cimpl_builtin_hasattr_impl,
                              CfCommonNewRefPyObject, "vvv", module, obj,
                              name);
}

static vinfo_t *
builtin_hasattr(PsycoObject *po, vinfo_t *module, vinfo_t *vargs)
{
    if (nargs_between(vargs, 2, 2) < 0) {
        return PsycoCFunction_CallFast(po, cfunc_builtin_hasattr,
                                       METH_FASTCALL, module, vargs);
    }
    return builtin_hasattr_impl(po, module, ARG(0), ARG(1));
}

static vinfo_t *
builtin_hash(PsycoObject *po, vinfo_t *module, vinfo_t *obj)
{
    vinfo_t *res, *x = PsycoObject_Hash(po, obj);
    if (x == NULL) {
        return NULL;
    }
    res = PsycoLong_FromSsize_t(po, x);
    vinfo_decref(x, po);
    return res;
}

static vinfo_t *
builtin_id(PsycoObject *po, vinfo_t *self, vinfo_t *v)
{
    vinfo_t *id;
    id = psyco_generic_call(po, PyLong_FromVoidPtr,
                            CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                            "v", v);
#if LE_HAS_AUDIT_HOOKS
    if (id != NULL &&
        psyco_generic_call(
            po, PySys_Audit, CfNoReturnValue | CfReturnTypeInt | CfPyErrIfNeg,
            "llv", (long)"builtins.id", (long)"O", id) == NULL) {
        vinfo_decref(id, po);
        return NULL;
    }
#endif
    return id;
}

static vinfo_t *
builtin_isinstance(PsycoObject *po, vinfo_t *module, vinfo_t *vargs)
{
    vinfo_t *res, *retval;
    if (nargs_between(vargs, 2, 2) < 0) {
        return PsycoCFunction_CallFast(po, cfunc_builtin_isinstance,
                                       METH_FASTCALL, module, vargs);
    }
    retval = PsycoObject_IsInstance(po, ARG(0), ARG(1));
    if (retval == NULL) {
        return NULL;
    }
    res = PsycoBool_FromZeroOrOne(po, retval);
    vinfo_decref(retval, po);
    return res;
}

static vinfo_t *
builtin_issubclass(PsycoObject *po, vinfo_t *module, vinfo_t *vargs)
{
    vinfo_t *res, *retval;
    if (nargs_between(vargs, 2, 2) < 0) {
        return PsycoCFunction_CallFast(po, cfunc_builtin_issubclass,
                                       METH_FASTCALL, module, vargs);
    }
    retval = psyco_generic_call(po, PyObject_IsSubclass,
                                CfCommonIntBoolOrError, "vv", ARG(0), ARG(1));
    if (retval == NULL) {
        return NULL;
    }
    res = PsycoBool_FromZeroOrOne(po, retval);
    vinfo_decref(retval, po);
    return res;
}

static vinfo_t *
builtin_len(PsycoObject *po, vinfo_t *module, vinfo_t *obj)
{
    vinfo_t *x, *res = PsycoObject_Size(po, obj);
    if (res == NULL) {
        return NULL;
    }
    x = PsycoLong_FromSsize_t(po, res);
    vinfo_decref(res, po);
    return x;
}

/* next(g) of a generator is its step (psyfunc.c:psyco_gen_next); next()
   of anything else, or with a default, is CPython's */
static vinfo_t *
builtin_next(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    if (nargs_between(vargs, 1, 1) == 1) {
        PyTypeObject *tp = Psyco_NeedType(po, ARG(0));
        if (tp == NULL)
            return NULL;
        if (tp == &PyGen_Type)
            return psyco_gen_next(po, ARG(0));
    }
    return PsycoCFunction_CallFast(po, cfunc_builtin_next, METH_FASTCALL, self,
                                   vargs);
}

#if LE_HAS_POW_KEYWORDS
#define POW_FLAGS (METH_FASTCALL | METH_KEYWORDS)
#else
#define POW_FLAGS METH_FASTCALL
#endif

static vinfo_t *
builtin_pow(PsycoObject *po, vinfo_t *module, vinfo_t *vargs)
{
    vinfo_t *mod, *return_value;
    int nargs = nargs_between(vargs, 2, 3);
    if (nargs < 0) {
        return PsycoCFunction_CallFast(po, cfunc_builtin_pow, POW_FLAGS,
                                       module, vargs);
    }
    if (nargs == 3) {
        return PsycoNumber_Power(po, ARG(0), ARG(1), ARG(2));
    }
    mod = psyco_vi_None();
    return_value = PsycoNumber_Power(po, ARG(0), ARG(1), mod);
    vinfo_decref(mod, po);
    return return_value;
}

static vinfo_t *
builtin_setattr(PsycoObject *po, vinfo_t *module, vinfo_t *vargs)
{
    if (nargs_between(vargs, 3, 3) < 0) {
        return PsycoCFunction_CallFast(po, cfunc_builtin_setattr,
                                       METH_FASTCALL, module, vargs);
    }
    if (!PsycoObject_SetAttr(po, ARG(0), ARG(1), ARG(2))) {
        return NULL;
    }
    return psyco_vi_None();
}
#undef ARG

static void
gen_bltinmodule_init(PyObject *md)
{
    cfunc_builtin_abs = Psyco_DefineModuleFn(md, "abs", METH_O, &builtin_abs);
    cfunc_builtin_callable =
        Psyco_DefineModuleFn(md, "callable", METH_O, &builtin_callable);
    cfunc_builtin_delattr =
        Psyco_DefineModuleFn(md, "delattr", METH_FASTCALL, &builtin_delattr);
    cfunc_builtin_divmod =
        Psyco_DefineModuleFn(md, "divmod", METH_FASTCALL, &builtin_divmod);
    cfunc_builtin_getattr =
        Psyco_DefineModuleFn(md, "getattr", METH_FASTCALL, &builtin_getattr);
    cfunc_builtin_hasattr =
        Psyco_DefineModuleFn(md, "hasattr", METH_FASTCALL, &builtin_hasattr);
    cfunc_builtin_hash =
        Psyco_DefineModuleFn(md, "hash", METH_O, &builtin_hash);
    cfunc_builtin_id = Psyco_DefineModuleFn(md, "id", METH_O, &builtin_id);
    cfunc_builtin_isinstance = Psyco_DefineModuleFn(
        md, "isinstance", METH_FASTCALL, &builtin_isinstance);
    cfunc_builtin_issubclass = Psyco_DefineModuleFn(
        md, "issubclass", METH_FASTCALL, &builtin_issubclass);
    cfunc_builtin_len = Psyco_DefineModuleFn(md, "len", METH_O, &builtin_len);
    cfunc_builtin_next =
        Psyco_DefineModuleFn(md, "next", METH_FASTCALL, &builtin_next);
    cfunc_builtin_pow =
        Psyco_DefineModuleFn(md, "pow", POW_FLAGS, &builtin_pow);
    cfunc_builtin_setattr =
        Psyco_DefineModuleFn(md, "setattr", METH_FASTCALL, &builtin_setattr);
}

static PyCFunction cimpl_globals;
static PyCFunction cimpl_min;
static PyCFunction cimpl_max;
static PyCFunction cimpl_round;
static PyCFunction cimpl_sum;
/* the fastcall builtins' own ml_flags (Psyco_DefineModuleFast) */
static int round_flags, sum_flags;
/* min's and max's, METH_VARARGS | METH_KEYWORDS or a fastcall convention */
static int min_max_flags;
static PyCFunction cimpl_any;
static PyCFunction cimpl_all;

/* a pending generator (pfuncobject.h) consumed whole by a
   builtin: run its code fused with the consumer's step, so neither the
   generator nor its frame exists (psyfunc.c:psyco_call_genexpr). 'start'
   is sum()'s, or NULL. */
static vinfo_t *
genexpr_consume(PsycoObject *po, vinfo_t *vgen, int mode, PyTypeObject *tp,
                vinfo_t *start)
{
    vinfo_t *acc, *result, *t, *comp = NULL;
    if (mode == GENEXPR_LIST)
        acc = psyco_generic_call(po, PyList_New, CfCommonNewRefPyObject, "l",
                                 0L);
    else if (mode == GENEXPR_SET)
        acc =
            psyco_generic_call(po, PySet_New, CfCommonNewRefPyObject, "l", 0L);
    else if (mode == GENEXPR_SUM) {
#if LE_HAS_SUM_COMPENSATED
        /* the fused code goes on from the phase the start is in */
        if (!psyco_sum_start(po, start, &mode, &acc, &comp))
            return NULL;
#else
        if (start != NULL) {
            vinfo_incref(start);
            acc = start;
        }
        else
            acc = PsycoLong_FROM_LONG(po, psyco_vi_Zero());
#endif
    }
    else
        acc = psyco_vi_Zero();
    if (acc == NULL) {
        vinfo_xdecref(comp, po);
        return NULL;
    }
    result = psyco_call_genexpr(po, vgen, mode, acc, comp);
    vinfo_decref(acc, po);
    vinfo_xdecref(comp, po);
    if (result != NULL && tp == &PyTuple_Type) {
        t = psyco_generic_call(po, PyList_AsTuple, CfCommonNewRefPyObject, "v",
                               result);
        vinfo_decref(result, po);
        result = t;
    }
    if (result != NULL && tp != NULL)
        Psyco_AssertType(po, result, tp);
    return result;
}

static vinfo_t *
pbuiltin_any(PsycoObject *po, vinfo_t *vself, vinfo_t *vobj)
{
    switch (psyco_genexpr_fuse(po, vobj)) {
        case 1:
            return genexpr_consume(po, vobj, GENEXPR_ANY, &PyBool_Type, NULL);
        case -1:
            return NULL;
    }
    return psyco_generic_call(po, cimpl_any, CfCommonNewRefPyObject, "vv",
                              vself, vobj);
}

static vinfo_t *
pbuiltin_all(PsycoObject *po, vinfo_t *vself, vinfo_t *vobj)
{
    switch (psyco_genexpr_fuse(po, vobj)) {
        case 1:
            return genexpr_consume(po, vobj, GENEXPR_ALL, &PyBool_Type, NULL);
        case -1:
            return NULL;
    }
    return psyco_generic_call(po, cimpl_all, CfCommonNewRefPyObject, "vv",
                              vself, vobj);
}
static ternaryfunc type_call;
static PyObject *object_class_descr; /* object.__class__ */
static PyObject *str___class__;

/* globals() is the compiled function's own globals: the real builtin reads
   the nearest interpreted frame's */
static vinfo_t *
pbuiltin_globals(PsycoObject *po, vinfo_t *vself, vinfo_t *vnull)
{
    vinfo_incref(LOC_GLOBALS);
    return LOC_GLOBALS;
}

/* bltinmodule.c:min_max with two or more positional arguments: compare in
   order, keeping the first extreme item, as CPython does. Each comparison
   picks a path at run time (respawn), so a virtual result stays virtual.
   Every type is promoted before the first comparison: a promotion restarts
   the instruction, which would run a comparison already emitted again
   (a user __gt__ twice). A comparison after one that may have changed a
   class is CPython's (pobject.h:Psyco_TypesStale). One iterable argument,
   or key=/default=, goes to CPython: walking a sequence of run-time length
   can't be compiled (the NFS branch tried). vkw is NULL for a fastcall
   min and max, whose metas get no keywords. */
static vinfo_t *
pbuiltin_min_max(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs, vinfo_t *vkw,
                 int op, PyCFunction cimpl)
{
    int i, n = PsycoTuple_Load(vargs);
    vinfo_t *best, *item, *cmp;
    int fuse = 0;
    bool no_kw = vkw == NULL || psyco_knowntobe(vkw, (long)NULL);
    if (n == 1 && no_kw &&
        (fuse = psyco_genexpr_fuse(po, PsycoTuple_GET_ITEM(vargs, 0))) < 0)
        return NULL;
    if (fuse)
        return genexpr_consume(po, PsycoTuple_GET_ITEM(vargs, 0),
                               op == Py_LT ? GENEXPR_MIN : GENEXPR_MAX, NULL,
                               NULL);
    if (vkw == NULL && n < 2)
        return PsycoCFunction_CallFast(po, cimpl, min_max_flags, vself, vargs);
    if (n < 2 || !no_kw)
        return psyco_generic_call(po, cimpl, CfCommonNewRefPyObject, "lvv",
                                  NULL, vargs, vkw);
    for (i = 0; i < n; i++)
        if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(vargs, i)) == NULL)
            return NULL;
    best = PsycoTuple_GET_ITEM(vargs, 0);
    for (i = 1; i < n; i++) {
        item = PsycoTuple_GET_ITEM(vargs, i);
        cmp = PsycoObject_RichCompareBool(po, item, best, op);
        if (cmp == NULL)
            return NULL;
        switch (runtime_NON_NULL_f(po, cmp)) { /* consumes cmp */
            case true:
                best = item;
                break;
            case false:
                break;
            default:
                return NULL;
        }
    }
    vinfo_incref(best);
    return best;
}

static vinfo_t *
pbuiltin_min(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs, vinfo_t *vkw)
{
    return pbuiltin_min_max(po, vself, vargs, vkw, Py_LT, cimpl_min);
}

static vinfo_t *
pbuiltin_max(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs, vinfo_t *vkw)
{
    return pbuiltin_min_max(po, vself, vargs, vkw, Py_GT, cimpl_max);
}

static vinfo_t *
pbuiltin_min_fast(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs)
{
    return pbuiltin_min_max(po, vself, vargs, NULL, Py_LT, cimpl_min);
}

static vinfo_t *
pbuiltin_max_fast(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs)
{
    return pbuiltin_min_max(po, vself, vargs, NULL, Py_GT, cimpl_max);
}

static PyObject *str___instancecheck__;
static PyObject *type_instancecheck; /* type.__instancecheck__ */

/* builtin_isinstance: Objects/abstract.c:object_recursive_isinstance
   decided at compile time from the promoted type tp of obj and the class,
   or tuple of classes, 'vcls'. Sets *r to 1 or 0 if known, -1 if it must be
   asked at run time, and raises *levels to the number of
   Py_EnterRecursiveCall tests, each inside the last, that CPython makes on
   the way to the answer, which the caller tests at run time. Only type's
   own __instancecheck__ is folded: for a class whose metaclass isn't type,
   the metaclass's lookup of it is guarded. A heap type's subtype relation
   can change (__bases__ assignment) and so is never folded; a static
   type's can't. A false answer also needs obj.__class__ to be the
   ordinary object.__class__, which object_isinstance consults next.
   'depth' is the number of tuples 'vcls' is in. False on error or
   promotion. */
static bool
isinstance_known_v(PsycoObject *po, PyTypeObject *tp, vinfo_t *vcls, int depth,
                   int *r, int *levels);

/* the same for a class, or a tuple, known at compile time */
static bool
isinstance_known_c(PsycoObject *po, PyTypeObject *tp, vinfo_t *vcls,
                   PyObject *cls, int depth, int *r, int *levels)
{
    *r = -1;
    if ((PyObject *)tp == cls) { /* CPython's quick test, made first */
        *r = 1;
        return true;
    }
    if (PyTuple_Check(cls)) {
        Py_ssize_t i;
        if (depth > 8)
            return true;
        if (*levels < depth + 1)
            *levels = depth + 1;
        *r = 0;
        for (i = 0; i < PyTuple_GET_SIZE(cls) && *r == 0; i++) {
            PyObject *item = PyTuple_GET_ITEM(cls, i);
            vinfo_t *vitem;
            bool ok;
            Py_INCREF(item);
            vitem =
                vinfo_new(CompileTime_NewSk(sk_new((long)item, SkFlagPyObj)));
            ok = isinstance_known_v(po, tp, vitem, depth + 1, r, levels);
            vinfo_decref(vitem, po);
            if (!ok)
                return false;
        }
        return true; /* found, not found, or unknown: then CPython decides */
    }
    if (!PyType_Check(cls) || (tp->tp_flags & Py_TPFLAGS_HEAPTYPE))
        return true;
    if (!PyType_CheckExact(cls)) {
        /* _PyObject_LookupSpecial(cls, "__instancecheck__"), then a call
           of the bound method found */
        PyTypeObject *meta = Py_TYPE(cls);
        PyObject *descr;
        int m;
        if (Psyco_ConstantClassMutable(vcls) &&
            Psyco_ConstantType(po, vcls, &meta) < 0)
            return false;
        if (!(meta->tp_flags & Py_TPFLAGS_HEAPTYPE))
            return true;
        /* the key: an address of meta's that no slot's lookup uses */
        m = Psyco_TypeLookup(po, meta, (char *)meta + 1, str___instancecheck__,
                             &descr);
        if (m < 0)
            return false;
        if (m == 0)
            return psyco_generic_call(po, cimpl_type_lookup_changed,
                                      CfNoReturnValue | CfNoPython | CfNoBlock,
                                      "lll", (long)meta,
                                      (long)str___instancecheck__,
                                      (long)descr) != NULL;
        if (descr != type_instancecheck)
            return true;
        /* the checker's test, then the C call's of it */
        if (*levels < depth + 2)
            *levels = depth + 2;
    }
    if (PyType_IsSubtype(tp, (PyTypeObject *)cls))
        *r = 1;
    else if (tp->tp_getattro == PyObject_GenericGetAttr &&
             _PyType_Lookup(tp, str___class__) == object_class_descr)
        *r = 0;
    return true;
}

/* a virtual tuple (built here) is walked item by item; otherwise only a
   static type is promoted. Anything else at run time (a heap class, which
   a constant would keep alive, or a tuple made elsewhere, a new object
   each call) is left to CPython. */
static bool
isinstance_known_v(PsycoObject *po, PyTypeObject *tp, vinfo_t *vcls, int depth,
                   int *r, int *levels)
{
    PyObject *cls;
    int m;
    if (Psyco_KnownType(vcls) == &PyTuple_Type &&
        !is_compiletime(vcls->source) && PsycoTuple_Load(vcls) >= 0) {
        int i, n = PsycoTuple_Load(vcls);
        *r = -1;
        if (depth > 8)
            return true;
        if (*levels < depth + 1)
            *levels = depth + 1;
        *r = 0;
        for (i = 0; i < n && *r == 0; i++)
            if (!isinstance_known_v(po, tp, PsycoTuple_GET_ITEM(vcls, i),
                                    depth + 1, r, levels))
                return false;
        return true;
    }
    m = psyco_atcompiletime_static_type(po, vcls, &cls);
    if (m < 0)
        return false;
    if (m == 0) {
        *r = -1;
        return true;
    }
    return isinstance_known_c(po, tp, vcls, cls, depth, r, levels);
}

/* Objects/abstract.c:PyObject_IsInstance's meta, by hand: the answer at
   compile time when isinstance_known_v gives one (never for a heap type)
   and CPython's tests of the recursion limit would pass, else the real
   call.
   Hand-written because the shortcut is judgement: the C has no
   compile-time form of it. A 0 or 1, or NULL on error. */
DEFINEFN
vinfo_t *
PsycoObject_IsInstance(PsycoObject *po, vinfo_t *vobj, vinfo_t *vcls)
{
    PyTypeObject *tp;
    int r, levels = 0;
    /* isinstance_known_c never folds for a heap type, so only a static
       type is promoted: a constant heap type would keep the class alive, and a
       class made per call would compile a new path each time */
    r = Psyco_NeedStaticType(po, vobj, &tp);
    if (r < 0)
        return NULL;
    if (r == 0)
        r = -1;
    else if (!isinstance_known_v(po, tp, vcls, 0, &r, &levels))
        return NULL;
    if (r >= 0 && levels > 0) {
        /* near the limit, CPython decides whether a test fails */
        condition_code_t cc = psyco_recursion_levels_over(po, levels);
        if (cc == CC_ERROR)
            return NULL;
        if (runtime_condition_f(po, cc))
            r = -1;
    }
    if (r == 1)
        return psyco_vi_One();
    if (r == 0)
        return psyco_vi_Zero();
    return psyco_generic_call(po, PyObject_IsInstance, CfCommonIntBoolOrError,
                              "vv", vobj, vcls);
}

/* whether an int fits a C long, as PyLong_AsLongAndOverflow tells */
static int
cimpl_long_fits(PyObject *v)
{
    int overflow;
    PyLong_AsLongAndOverflow(v, &overflow);
    return !overflow;
}

/* the C long of 'v', an exact int or bool, as builtin_sum_impl's int
   loop reads its total and each item (PyLong_AsLongAndOverflow): 1 with a
   new reference in *pw; 0 if it doesn't fit; -1 on error */
static int
sum_int_word(PsycoObject *po, vinfo_t *v, vinfo_t **pw)
{
    vinfo_t *fits;
    int t;
    if (is_compiletime(v->source)) {
        int overflow;
        long w = PyLong_AsLongAndOverflow(
            (PyObject *)CompileTime_Get(v->source)->value, &overflow);
        if (overflow)
            return 0;
        *pw = vinfo_new(CompileTime_New(w));
        return 1;
    }
    t = PsycoLong_Word(po, v, pw);
    if (t != 0)
        return t;
    fits = psyco_generic_call(
        po, cimpl_long_fits, CfReturnTypeInt | CfNoPython | CfNoBlock, "v", v);
    if (fits == NULL || (t = runtime_NON_NULL_t(po, fits)) < 0)
        return -1;
    if (!t)
        return 0;
    *pw = psyco_generic_call(
        po, PyLong_AsLong, CfReturnTypeLong | CfNoPython | CfNoBlock, "v", v);
    return *pw != NULL ? 1 : -1;
}

/* builtin_sum_impl's int loop, which keeps the total as a C long, not an
   int object: the total 'acc' + 'item', both exact ints or bools, as a
   virtual int; NULL with no exception set where either doesn't fit a C
   long or the sum overflows one, which ends CPython's loop */
static vinfo_t *
sum_int_add(PsycoObject *po, vinfo_t *acc, vinfo_t *item)
{
    vinfo_t *a, *b, *w;
    int t = sum_int_word(po, acc, &a);
    if (t <= 0)
        return NULL;
    t = sum_int_word(po, item, &b);
    if (t <= 0) {
        vinfo_decref(a, po);
        return NULL;
    }
    w = integer_add(po, a, b, true);
    vinfo_decref(a, po);
    vinfo_decref(b, po);
    return w != NULL ? PsycoLong_FROM_WORD(w) : NULL;
}

#if !LE_HAS_SUM_COMPENSATED
/* one item of a sum() where float sums aren't compensated: acc + item as
   builtin_sum_impl's loops give it (a new reference, NULL on error). A
   virtual int total is in its int loop, which adds a C long. */
DEFINEFN
vinfo_t *
psyco_sum_add(PsycoObject *po, vinfo_t *acc, vinfo_t *item)
{
    if (PsycoLong_IsVirtual(acc)) {
        PyTypeObject *tp = Psyco_NeedType(po, item);
        if (tp == NULL)
            return NULL;
        if (tp == &PyLong_Type || tp == &PyBool_Type) {
            vinfo_t *x = sum_int_add(po, acc, item);
            if (x != NULL || PycException_Occurred(po))
                return x;
        }
    }
    if (Psyco_KnownType(acc) != NULL)
        return PsycoNumber_Add(po, acc, item);
    /* e.g. from a user __add__: a temporary, so its type can't be
       promoted; CPython's generic loop */
    return psyco_generic_call(po, PyNumber_Add, CfCommonNewRefPyObject, "vv",
                              acc, item);
}
#endif

#if LE_HAS_SUM_COMPENSATED
/* builtin_sum_impl (Python/bltinmodule.c) where it compensates float sums
   runs in phases, one loop each, and never goes back to an earlier one:
   its int loop while the result is an exact int that fits a C long
   (GENEXPR_SUM: 'acc' the int), its float loop while it is an exact float
   (GENEXPR_SUM_FLOAT: 'acc' the float, 'comp' the compensation, a double),
   where SUM_COMPLEX_COMPENSATED its complex loop while it is an exact
   complex (GENEXPR_SUM_COMPLEX: 'acc' cimpl_sum_complex_*'s state), then
   plain + (GENEXPR_SUM_GENERIC). Compiled code follows the phase at
   compile time, and tests at run time only what CPython's loops test then:
   whether an int fits, and the result's type on leaving a loop. Types are
   promoted only before an addition: a promotion restarts the instruction,
   which would add again. */

/* the compensated sum (bltinmodule.c's float loop, or its cs_add and
   cs_to_double) */
static double
cimpl_cs_hi(double hi, double x)
{
    return psyco_addsd(hi, x);
}

static double
cimpl_cs_lo(double hi, double lo, double x)
{
    double t = hi + x;
    if (fabs(hi) >= fabs(x))
        lo += (hi - t) + x;
    else
        lo += (x - t) + hi;
    return lo;
}

/* the sum's double: the compensation is added only where it can't lose a
   negative zero's sign or turn an infinite or overflowed sum into a NaN */
static double
cimpl_cs_to_double(double hi, double lo)
{
    if (lo && isfinite(lo))
        return hi + lo;
    return hi;
}

#if !LE_HAS_SUM_INT_AS_DOUBLE
/* the float loop's int that fits a C long, as it converts it */
static double
cimpl_long_as_double(PyObject *v)
{
    return (double)PyLong_AsLong(v);
}
#endif

#if LE_HAS_SUM_COMPLEX_COMPENSATED
/* the complex loop's state: a bytearray of the real and the imaginary
   parts' compensated sums, (sum, compensation) each */
static PyObject *
cimpl_sum_complex_begin(PyObject *z)
{
    PyObject *box = PyByteArray_FromStringAndSize(NULL, 4 * sizeof(double));
    double *d;
    if (box == NULL)
        return NULL;
    d = (double *)PyByteArray_AS_STRING(box);
    d[0] = PyComplex_RealAsDouble(z);
    d[1] = 0.0;
    d[2] = PyComplex_ImagAsDouble(z);
    d[3] = 0.0;
    return box;
}

static void
complex_box_add(double *d, double x)
{
    double hi = d[0];
    d[0] = cimpl_cs_hi(hi, x);
    d[1] = cimpl_cs_lo(hi, d[1], x);
}

static PyObject *
complex_box_value(PyObject *box)
{
    double *d = (double *)PyByteArray_AS_STRING(box);
    return PyComplex_FromDoubles(cimpl_cs_to_double(d[0], d[1]),
                                 cimpl_cs_to_double(d[2], d[3]));
}

/* one item of the complex loop: the state itself (a new reference) while
   the loop goes on, or the result of leaving it, the sum + item; NULL on
   error */
static PyObject *
cimpl_sum_complex_step(PyObject *box, PyObject *item)
{
    double *d = (double *)PyByteArray_AS_STRING(box);
    PyObject *result, *x;
    if (PyComplex_CheckExact(item)) {
        Py_complex z = PyComplex_AsCComplex(item);
        complex_box_add(d, z.real);
        d += 2;
        complex_box_add(d, z.imag);
    }
#if LE_HAS_SUM_EXACT_NUMBERS
    else if (PyLong_CheckExact(item)) {
#else
    else if (PyLong_Check(item)) {
#endif
        double value = PyLong_AsDouble(item);
        if (value == -1.0 && PyErr_Occurred())
            return NULL;
        complex_box_add(d, value);
    }
#if LE_HAS_SUM_EXACT_NUMBERS
    else if (PyFloat_CheckExact(item))
#else
    else if (PyFloat_Check(item))
#endif
        complex_box_add(d, PyFloat_AS_DOUBLE(item));
    else {
        result = complex_box_value(box);
        if (result == NULL)
            return NULL;
        x = PyNumber_Add(result, item);
        Py_DECREF(result);
        return x;
    }
    Py_INCREF(box);
    return box;
}

static PyObject *
cimpl_sum_complex_end(PyObject *box)
{
    return complex_box_value(box);
}
#endif

/* 1 if x's type is tp at run time, known from then on; 0 if not; -1 on
   error. A run-time test, not a promotion. */
static int
sum_type_is(PsycoObject *po, vinfo_t *x, PyTypeObject *tp)
{
    PyTypeObject *known = Psyco_KnownType(x);
    vinfo_t *vtp;
    condition_code_t cc;
    bool t;
    if (known != NULL)
        return known == tp;
    vtp = psyco_get_field(po, x, OB_type);
    if (vtp == NULL)
        return -1;
    cc = integer_cmp_i(po, vtp, (long)tp, Py_EQ);
    if (cc == CC_ERROR) {
        vinfo_decref(vtp, po);
        return -1;
    }
    t = runtime_condition_t(po, cc);
    vinfo_decref(vtp, po);
    if (!t)
        return 0;
    Psyco_AssertType(po, x, tp);
    return 1;
}

/* a value whose type is known to be tp, for its later uses */
static void
sum_know_type(PsycoObject *po, vinfo_t *v, PyTypeObject *tp)
{
    if (!is_compiletime(v->source) && Psyco_KnownType(v) == NULL)
        Psyco_AssertType(po, v, tp);
}

static int
sum_rank(int phase)
{
    switch (phase) {
        case 0:
            return 0;
        case GENEXPR_SUM:
            return 1;
        case GENEXPR_SUM_FLOAT:
            return 2;
        case GENEXPR_SUM_COMPLEX:
            return 3;
        default:
            return 4;
    }
}

/* the phase that 'x' (consumed), the result after leaving phase 'after'
   (0 at the start), goes on in: the first later loop whose test it passes */
static bool
sum_enter(PsycoObject *po, int after, vinfo_t *x, int *phase, vinfo_t **acc,
          vinfo_t **comp)
{
    int r = sum_rank(after), t;
    *comp = psyco_vi_Zero();
    if (r < 1) {
        if ((t = sum_type_is(po, x, &PyLong_Type)) < 0)
            goto fail;
        if (t) {
            if (!PsycoLong_IsVirtual(x) && !is_compiletime(x->source)) {
                vinfo_t *fits = psyco_generic_call(
                    po, cimpl_long_fits,
                    CfReturnTypeInt | CfNoPython | CfNoBlock, "v", x);
                if (fits == NULL || (t = runtime_NON_NULL_t(po, fits)) < 0)
                    goto fail;
            }
            else if (is_compiletime(x->source))
                t = cimpl_long_fits(
                    (PyObject *)CompileTime_Get(x->source)->value);
            if (t) {
                *phase = GENEXPR_SUM;
                *acc = x;
                return true;
            }
        }
    }
    if (r < 2) {
        if ((t = sum_type_is(po, x, &PyFloat_Type)) < 0)
            goto fail;
        if (t) {
            *phase = GENEXPR_SUM_FLOAT;
            *acc = x;
            return true;
        }
    }
#if LE_HAS_SUM_COMPLEX_COMPENSATED
    if (r < 3) {
        if ((t = sum_type_is(po, x, &PyComplex_Type)) < 0)
            goto fail;
        if (t) {
            vinfo_t *box = psyco_generic_call(po, cimpl_sum_complex_begin,
                                              CfCommonNewRefPyObject, "v", x);
            vinfo_decref(x, po);
            if (box == NULL) {
                vinfo_decref(*comp, po);
                return false;
            }
            *phase = GENEXPR_SUM_COMPLEX;
            *acc = box;
            return true;
        }
    }
#endif
    *phase = GENEXPR_SUM_GENERIC;
    *acc = x;
    return true;
fail:
    vinfo_decref(*comp, po);
    vinfo_decref(x, po);
    return false;
}

/* the start of a sum(): 'start' is its start argument, or NULL for 0 */
DEFINEFN
bool
psyco_sum_start(PsycoObject *po, vinfo_t *start, int *phase, vinfo_t **acc,
                vinfo_t **comp)
{
    vinfo_t *x;
    if (start != NULL) {
        vinfo_incref(start);
        x = start;
    }
    else
        x = PsycoLong_FROM_LONG(po, psyco_vi_Zero());
    if (x == NULL)
        return false;
    return sum_enter(po, 0, x, phase, acc, comp);
}

/* the float loop's step for an exact float or int's double 'xd' */
static bool
sum_float_add(PsycoObject *po, vinfo_t **acc, vinfo_t **comp, vinfo_t *xd,
              bool compensated)
{
    vinfo_t *hi = PsycoFloat_AS_DOUBLE(po, *acc), *nhi, *nlo;
    if (hi == NULL)
        return false;
    nhi = psyco_generic_call(
        po, cimpl_cs_hi, CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock,
        "dd", hi, xd);
    if (nhi == NULL)
        return false;
    if (compensated) {
        nlo = psyco_generic_call(
            po, cimpl_cs_lo,
            CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "ddd", hi,
            *comp, xd);
        if (nlo == NULL) {
            vinfo_decref(nhi, po);
            return false;
        }
        vinfo_decref(*comp, po);
        *comp = nlo;
    }
    vinfo_decref(*acc, po);
    *acc = PsycoFloat_FROM_DOUBLE(nhi);
    return true;
}

/* the float loop's double: a new reference */
static vinfo_t *
sum_float_value(PsycoObject *po, vinfo_t *acc, vinfo_t *comp)
{
    vinfo_t *hi = PsycoFloat_AS_DOUBLE(po, acc), *d;
    if (hi == NULL)
        return NULL;
    d = psyco_generic_call(
        po, cimpl_cs_to_double,
        CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "dd", hi, comp);
    return d != NULL ? PsycoFloat_FROM_DOUBLE(d) : NULL;
}

/* one item of a sum() in its phase: *acc and *comp (and *phase) replaced
   by the next ones; false on error, leaving them */
DEFINEFN
bool
psyco_sum_step(PsycoObject *po, int *phase, vinfo_t **acc, vinfo_t **comp,
               vinfo_t *item)
{
    PyTypeObject *tp;
    vinfo_t *x, *res, *xd;
    vinfo_t *nacc = *acc, *ncomp = *comp;
    int nphase = *phase;
    if (*phase == GENEXPR_SUM_GENERIC) {
        if (Psyco_KnownType(*acc) != NULL)
            x = PsycoNumber_Add(po, *acc, item);
        else
            x = psyco_generic_call(po, PyNumber_Add, CfCommonNewRefPyObject,
                                   "vv", *acc, item);
        if (x == NULL)
            return false;
        vinfo_decref(*acc, po);
        *acc = x;
        return true;
    }
#if LE_HAS_SUM_COMPLEX_COMPENSATED
    if (*phase == GENEXPR_SUM_COMPLEX) {
        condition_code_t cc;
        x = psyco_generic_call(po, cimpl_sum_complex_step,
                               CfCommonNewRefPyObject, "vv", *acc, item);
        if (x == NULL)
            return false;
        cc = integer_cmp(po, x, *acc, Py_EQ);
        if (cc == CC_ERROR) {
            vinfo_decref(x, po);
            return false;
        }
        if (!runtime_condition_t(po, cc))
            *phase = GENEXPR_SUM_GENERIC;
        vinfo_decref(*acc, po);
        *acc = x;
        return true;
    }
#endif
    /* every promotion first, before anything adds */
    tp = Psyco_NeedType(po, item);
    if (tp == NULL)
        return false;
    if (*phase == GENEXPR_SUM) {
        bool is_int = tp == &PyLong_Type || tp == &PyBool_Type;
        if (is_int) {
            x = sum_int_add(po, *acc, item);
            if (x != NULL) {
                vinfo_decref(*acc, po);
                *acc = x;
                return true;
            }
            if (PycException_Occurred(po))
                return false;
        }
        /* the int loop ends: its total + item */
        sum_know_type(po, *acc, &PyLong_Type);
        x = PsycoNumber_Add(po, *acc, item);
        if (x == NULL)
            return false;
        if (is_int) {
            /* an int, which goes on with plain + */
            sum_know_type(po, x, &PyLong_Type);
            vinfo_decref(*acc, po);
            *acc = x;
            *phase = GENEXPR_SUM_GENERIC;
            return true;
        }
        if (!sum_enter(po, GENEXPR_SUM, x, &nphase, &nacc, &ncomp))
            return false;
        vinfo_decref(*acc, po);
        vinfo_decref(*comp, po);
        *phase = nphase;
        *acc = nacc;
        *comp = ncomp;
        return true;
    }
    /* the float loop */
    sum_know_type(po, *acc, &PyFloat_Type);
    if (tp == &PyFloat_Type) {
        xd = PsycoFloat_AS_DOUBLE(po, item);
        return xd != NULL && sum_float_add(po, acc, comp, xd, true);
    }
#if LE_HAS_SUM_EXACT_NUMBERS
    if (tp == &PyLong_Type) {
#else
    if (PyType_IsSubtype(tp, &PyLong_Type)) {
#endif
#if LE_HAS_SUM_INT_AS_DOUBLE
        bool ok;
        xd = PsycoLong_AsDouble(po, item);
        if (xd == NULL)
            return false;
        ok = sum_float_add(po, acc, comp, xd, true);
        vinfo_decref(xd, po);
        return ok;
#else
        /* added as a C long, with plain +; one that doesn't fit ends the
           loop */
        bool ok;
        vinfo_t *fits;
        int t;
        if (PsycoLong_IsVirtual(item)) {
            xd = psyco_generic_call(
                po, cimpl_fp_from_long,
                CfPure | CfReturnTypeDouble | CfNoPython | CfNoBlock, "v",
                item->array->items[iLONG_VALUE]);
            if (xd == NULL)
                return false;
        }
        else {
            if (is_compiletime(item->source))
                t = cimpl_long_fits(
                    (PyObject *)CompileTime_Get(item->source)->value);
            else {
                fits = psyco_generic_call(
                    po, cimpl_long_fits,
                    CfReturnTypeInt | CfNoPython | CfNoBlock, "v", item);
                if (fits == NULL || (t = runtime_NON_NULL_t(po, fits)) < 0)
                    return false;
            }
            xd = !t ? NULL
                    : psyco_generic_call(
                          po, cimpl_long_as_double,
                          CfReturnTypeDouble | CfNoPython | CfNoBlock, "v",
                          item);
            if (t && xd == NULL)
                return false;
        }
        if (xd != NULL) {
            ok = sum_float_add(po, acc, comp, xd, false);
            vinfo_decref(xd, po);
            return ok;
        }
#endif
    }
    /* the float loop ends: its result + item */
    res = sum_float_value(po, *acc, *comp);
    if (res == NULL)
        return false;
    x = PsycoNumber_Add(po, res, item);
    vinfo_decref(res, po);
    if (x == NULL)
        return false;
    if (!sum_enter(po, GENEXPR_SUM_FLOAT, x, &nphase, &nacc, &ncomp))
        return false;
    vinfo_decref(*acc, po);
    vinfo_decref(*comp, po);
    *phase = nphase;
    *acc = nacc;
    *comp = ncomp;
    return true;
}

/* the sum's result, a new reference */
DEFINEFN
vinfo_t *
psyco_sum_result(PsycoObject *po, int phase, vinfo_t *acc, vinfo_t *comp)
{
    if (phase == GENEXPR_SUM_FLOAT)
        return sum_float_value(po, acc, comp);
#if LE_HAS_SUM_COMPLEX_COMPENSATED
    if (phase == GENEXPR_SUM_COMPLEX)
        return psyco_generic_call(po, cimpl_sum_complex_end,
                                  CfCommonNewRefPyObject, "v", acc);
#endif
    vinfo_incref(acc);
    return acc;
}
#endif /* LE_HAS_SUM_COMPENSATED */

/* builtin_sum_impl (Python/bltinmodule.c) is a left fold of PyNumber_Add
   from start (default 0); its C fast path for exact ints gives the same
   results, and so does its one for floats except where SUM_COMPENSATED,
   where every sum() is CPython's. Compiled only when the length is known
   now: a tuple built here or a constant tuple. Every type is promoted before
   the first addition: a user __add__ must not run twice. A str, bytes or
   bytearray start raises, and anything else (lists, iterators: items that are
   not frame values can't be promoted) is CPython's, as before. */
static vinfo_t *
pbuiltin_sum(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs)
{
    int n = PsycoTuple_Load(vargs);
    int i, k;

    vinfo_t *vit, *result, *x;
    PyTypeObject *tp;
    if (n != 1 && n != 2)
        goto fallback;
    vit = PsycoTuple_GET_ITEM(vargs, 0);
    if (psyco_genexpr_fusable(po, vit)) {
        if (n == 2) {
            tp = Psyco_NeedType(po, PsycoTuple_GET_ITEM(vargs, 1));
            if (tp == NULL)
                return NULL;
            if (PyType_IsSubtype(tp, &PyUnicode_Type) ||
                PyType_IsSubtype(tp, &PyBytes_Type) ||
                PyType_IsSubtype(tp, &PyByteArray_Type))
                goto fallback; /* sum() raises its own TypeError */
        }
        switch (psyco_genexpr_fuse(po, vit)) {
            case 0:
                goto fallback;
            case -1:
                return NULL;
        }
        return genexpr_consume(po, vit, GENEXPR_SUM, NULL,
                               n == 2 ? PsycoTuple_GET_ITEM(vargs, 1) : NULL);
    }
    if (is_compiletime(vit->source)) {
        if (!PyTuple_CheckExact(
                (PyObject *)CompileTime_Get(vit->source)->value))
            goto fallback;
    }
    else if (Psyco_KnownType(vit) != &PyTuple_Type)
        goto fallback;
    k = PsycoTuple_Load(vit);
    if (k < 0)
        goto fallback;
    if (n == 2) {
        tp = Psyco_NeedType(po, PsycoTuple_GET_ITEM(vargs, 1));
        if (tp == NULL)
            return NULL;
        if (PyType_IsSubtype(tp, &PyUnicode_Type) ||
            PyType_IsSubtype(tp, &PyBytes_Type) ||
            PyType_IsSubtype(tp, &PyByteArray_Type))
            goto fallback;
    }
    for (i = 0; i < k; i++)
        if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(vit, i)) == NULL)
            return NULL;
#if LE_HAS_SUM_COMPENSATED
    {
        int phase;
        vinfo_t *comp;
        if (!psyco_sum_start(po, n == 2 ? PsycoTuple_GET_ITEM(vargs, 1) : NULL,
                             &phase, &result, &comp))
            return NULL;
        for (i = 0; i < k; i++)
            if (!psyco_sum_step(po, &phase, &result, &comp,
                                PsycoTuple_GET_ITEM(vit, i)))
                break;
        x = i < k ? NULL : psyco_sum_result(po, phase, result, comp);
        vinfo_decref(result, po);
        vinfo_decref(comp, po);
        return x;
    }
#else
    if (n == 2) {
        result = PsycoTuple_GET_ITEM(vargs, 1);
        vinfo_incref(result);
    }
    else
        result = PsycoLong_FROM_LONG(po, psyco_vi_Zero());
    for (i = 0; i < k && result != NULL; i++) {
        x = psyco_sum_add(po, result, PsycoTuple_GET_ITEM(vit, i));
        vinfo_decref(result, po);
        result = x;
    }
    return result;
#endif
fallback:
    return PsycoCFunction_CallFast(po, cimpl_sum, sum_flags, vself, vargs);
}

/* An exact float's whole part (trunc or round-half-even) as an int, or
   NULL in *out (with true returned) if it doesn't fit a long: then the
   caller asks CPython, which raises for NaN and infinities or builds a
   big int. */
static bool
float_whole_to_int(PsycoObject *po, vinfo_t *vf, bool round_even,
                   vinfo_t **out)
{
    vinfo_t *vd, *vw;
    int r;
    *out = NULL;
    vd = PsycoFloat_AS_DOUBLE(po, vf); /* borrowed */
    if (vd == NULL)
        return false;
    vw = round_even ? PsycoFloat_RoundEven(po, vd) : PsycoFloat_Trunc(po, vd);
    if (vw == NULL)
        return false;
    r = PsycoFloat_WholeToLong(po, vw, out);
    vinfo_decref(vw, po);
    return r >= 0;
}

/* builtin_round (Python/bltinmodule.c) without ndigits calls
   type(number).__round__(): for an exact int that is long_round ->
   long_long, the same object; for an exact float float___round___impl,
   round half to even then PyLong_FromDouble. */
static vinfo_t *
pbuiltin_round(PsycoObject *po, vinfo_t *vself, vinfo_t *vargs)
{
    vinfo_t *v, *x;
    PyTypeObject *tp;
    if (PsycoTuple_Load(vargs) == 1) {
        v = PsycoTuple_GET_ITEM(vargs, 0);
        tp = Psyco_NeedType(po, v);
        if (tp == NULL)
            return NULL;
        if (tp == &PyLong_Type) {
            vinfo_incref(v);
            return v;
        }
        if (tp == &PyFloat_Type) {
            if (!float_whole_to_int(po, v, true, &x))
                return NULL;
            if (x != NULL)
                return x;
        }
    }
    return PsycoCFunction_CallFast(po, cimpl_round, round_flags, vself, vargs);
}

/* super() with no arguments: typeobject.c:super_init's frame lookup, with
   the frame being the compiled one, which a direct call (psyfunc.c) never
   makes real. 'argc' is the code's co_argcount, 'obj' its first local and
   'objcell' the cell holding the first argument instead, if any. */
static PyObject *
cimpl_super_noargs(long argc, PyObject *obj, PyObject *objcell,
                   PyObject *classcell)
{
    PyObject *type;
    if (argc == 0) {
        PyErr_SetString(PyExc_RuntimeError, LE_MSG_SUPER_NO_ARGS);
        return NULL;
    }
    if (obj == NULL && objcell != NULL)
        obj = PyCell_GET(objcell);
    if (obj == NULL) {
        PyErr_SetString(PyExc_RuntimeError, LE_MSG_SUPER_ARG0_DELETED);
        return NULL;
    }
    if (classcell == NULL) {
        PyErr_SetString(PyExc_RuntimeError, LE_MSG_SUPER_NO_CLASS_CELL);
        return NULL;
    }
    if (!PyCell_Check(classcell)) {
        PyErr_SetString(PyExc_RuntimeError, "super(): bad __class__ cell");
        return NULL;
    }
    type = PyCell_GET(classcell);
    if (type == NULL) {
        PyErr_SetString(PyExc_RuntimeError, LE_MSG_SUPER_EMPTY_CLASS_CELL);
        return NULL;
    }
    if (!PyType_Check(type)) {
        PyErr_Format(PyExc_RuntimeError, LE_MSG_SUPER_CLASS_NOT_TYPE,
                     Py_TYPE(type)->tp_name);
        return NULL;
    }
    return PyObject_CallFunctionObjArgs((PyObject *)&PySuper_Type, type, obj,
                                        NULL);
}

static vinfo_t *
psuper_noargs(PsycoObject *po)
{
    PyCodeObject *co = po->pr.co;
    Py_ssize_t i, ncells = le_code_ncells(co);
    int fv = psyco_class_cell(co);
    vinfo_t *obj, *objcell = NULL, *classcell = NULL, *zero, *r;
    zero = psyco_vi_Zero();
    /* the first argument, or the cell it went into */
    obj = co->co_argcount > 0 && !le_code_local_is_cell(co, 0)
              ? LOC_LOCALS_PLUS[0]
              : zero;
    if (obj == NULL)
        obj = zero;
    for (i = 0; i < ncells; i++)
        if (le_code_cell_arg(co, (int)i) == 0)
            objcell = LOC_LOCALS_PLUS[le_code_cell_slot(co, (int)i)];
    if (fv >= 0)
        classcell = LOC_LOCALS_PLUS[le_code_free_slot(co, fv)];
    r = psyco_generic_call(po, cimpl_super_noargs, CfCommonNewRefPyObject,
                           "lvvv", (long)co->co_argcount, obj,
                           objcell ? objcell : zero,
                           classcell ? classcell : zero);
    vinfo_decref(zero, po);
    return r;
}

/* type_call (Objects/typeobject.c) for type(x), int(x) and float(x) with
   one positional argument of an exact type, and for a heap class already
   known (ptypeobject.c:psyco_heap_type_call); everything else is
   PyObject_Call, which calls a type through its tp_vectorcall where it has
   one, as ceval's calls do, and type_call otherwise.
   type(x): the "ugly exception", the type itself. int(x): long_new_impl ->
   PyNumber_Long, which returns an exact int unchanged and uses nb_int
   (float___trunc___impl) for a float. float(x): float_new_impl ->
   PyNumber_Float, which returns an exact float unchanged and uses nb_float
   (long_float -> PyLong_AsDouble) for an int. Only a static type
   callee is promoted: a promoted class would be kept alive by the compiled
   code (freeing.namedtuple_code_dies). */
static vinfo_t *
ptype_call(PsycoObject *po, vinfo_t *vtype, vinfo_t *varg, vinfo_t *vkw)
{
    PyObject *t;
    PyTypeObject *tp;
    vinfo_t *v, *x;
    int fuse;
    int m = psyco_atcompiletime_static_type(po, vtype, &t);
    if (m < 0)
        return NULL;
    /* super(): also when the callee isn't known, if this code has the
       __class__ cell super() would look for */
    if (psyco_knowntobe(vkw, 0) && PsycoTuple_Load(varg) == 0) {
        if (m != 0 && t == (PyObject *)&PySuper_Type)
            return psuper_noargs(po);
        if (m == 0 && psyco_class_cell(po->pr.co) >= 0) {
            condition_code_t cc =
                integer_cmp_i(po, vtype, (long)&PySuper_Type, Py_EQ);
            if (cc == CC_ERROR)
                return NULL;
            if (runtime_condition_f(po, cc))
                return psuper_noargs(po);
        }
    }
    if (m != 0 && t == (PyObject *)&PyRange_Type && psyco_knowntobe(vkw, 0)) {
        int n = PsycoTuple_Load(varg);
        if (n >= 1 && n <= 3) {
            int r = PsycoRange_New(po, &PsycoTuple_GET_ITEM(varg, 0), n, &x);
            if (r < 0)
                return NULL;
            if (r > 0)
                return x;
        }
        goto fallback;
    }
    if (m != 0 && psyco_knowntobe(vkw, 0) && PsycoTuple_Load(varg) == 1 &&
        (t == (PyObject *)&PyList_Type || t == (PyObject *)&PyTuple_Type ||
         t == (PyObject *)&PySet_Type) &&
        (fuse = psyco_genexpr_fuse(po, PsycoTuple_GET_ITEM(varg, 0))) != 0) {
        if (fuse < 0)
            return NULL;
        return genexpr_consume(
            po, PsycoTuple_GET_ITEM(varg, 0),
            t == (PyObject *)&PySet_Type ? GENEXPR_SET : GENEXPR_LIST,
            (PyTypeObject *)t, NULL);
    }
    /* a class known now (a cached global, a class attribute): its code
       already holds it */
    if (m != 0 && PyType_Check(t) &&
        (((PyTypeObject *)t)->tp_flags & Py_TPFLAGS_HEAPTYPE))
        return psyco_heap_type_call(po, (PyTypeObject *)t, vtype, varg, vkw);
    if (m == 0 || !psyco_knowntobe(vkw, 0) || PsycoTuple_Load(varg) != 1 ||
        (t != (PyObject *)&PyType_Type && t != (PyObject *)&PyLong_Type &&
         t != (PyObject *)&PyFloat_Type))
        goto fallback;
    v = PsycoTuple_GET_ITEM(varg, 0);
    if (t == (PyObject *)&PyType_Type) {
        vinfo_t *vt =
            is_runtime(v->source) ? vinfo_getitem(v, iOB_TYPE) : NULL;
        /* a run-time type already loaded (a megamorphic site's) is not
           forgotten after a call that may assign __class__: read it again */
        if (vt != NULL && is_runtime(vt->source))
            return psyco_get_field(po, v, FMUT(OB_type));
        return psyco_get_field(po, v, OB_type);
    }
    tp = Psyco_NeedType(po, v);
    if (tp == NULL)
        return NULL;
    if ((PyObject *)tp == t) {
        vinfo_incref(v);
        return v;
    }
    if (t == (PyObject *)&PyLong_Type && tp == &PyFloat_Type) {
        if (!float_whole_to_int(po, v, false, &x))
            return NULL;
        if (x != NULL)
            return x;
    }
    if (t == (PyObject *)&PyFloat_Type && tp == &PyLong_Type) {
        if ((x = PsycoLong_AsDouble(po, v)) == NULL)
            return NULL;
        return PsycoFloat_FROM_DOUBLE(x);
    }
fallback:
    return psyco_residual_call(po, PyObject_Call, vtype, varg, vkw);
}

/***************************************************************/

INITIALIZATIONFN
void
psyco_bltinmodule_init(void)
{
    /* a meta is registered only if the builtin's ml_flags match the meta's
       (either fastcall convention for DEFFAST's), otherwise
       lejit._core._unregistered_metas() lists it. A METH_FASTCALL meta takes a
       tuple (pmethodobject.h). No meta for iter(): PsycoObject_GetIter gives
       lists a virtual seq-iterator, right for a for loop but the wrong type
       for iter() to hand out. */
    PyObject *md = Psyco_DefineMetaModule("builtins");
    PyObject *min;

    gen_bltinmodule_init(md);
#define DEFMETA(name, flags) \
    cimpl_##name = Psyco_DefineModuleFn(md, #name, flags, &pbuiltin_##name)
    DEFMETA(globals, METH_NOARGS);
#define DEFFAST(name) \
    cimpl_##name =    \
        Psyco_DefineModuleFast(md, #name, &name##_flags, &pbuiltin_##name)
    min_max_flags = METH_VARARGS | METH_KEYWORDS;
    min = Psyco_GetModuleObject(md, "min", &PyCFunction_Type);
    if (min != NULL && (PyCFunction_GET_FLAGS(min) & METH_FASTCALL)) {
        cimpl_min = Psyco_DefineModuleFast(md, "min", &min_max_flags,
                                           &pbuiltin_min_fast);
        cimpl_max =
            Psyco_DefineModuleFn(md, "max", min_max_flags, &pbuiltin_max_fast);
    }
    else {
        DEFMETA(min, METH_VARARGS | METH_KEYWORDS);
        DEFMETA(max, METH_VARARGS | METH_KEYWORDS);
    }
    Py_XDECREF(min);
    DEFFAST(round);
    DEFFAST(sum);
    DEFMETA(any, METH_O);
    DEFMETA(all, METH_O);
    type_call = PyType_Type.tp_call;
    Psyco_DefineMeta(type_call, ptype_call);
    str___class__ = PyUnicode_InternFromString("__class__");
    object_class_descr = _PyType_Lookup(&PyBaseObject_Type, str___class__);
    str___instancecheck__ = PyUnicode_InternFromString("__instancecheck__");
    type_instancecheck = _PyType_Lookup(&PyType_Type, str___instancecheck__);
#undef DEFMETA
#undef DEFFAST
    Py_XDECREF(md);
}
