/*
 * Virtual function objects created by MAKE_FUNCTION, and the function tp_call
 * meta. The callee is promoted to a constant (with a megamorphic cut-off) and
 * the call goes to psyfunc.c:psyco_call_pyfunc, which may inline it. A call to
 * a function that is still virtual reads its code, globals and defaults
 * directly and never materialises it. A generator called from here may stay
 * a pending generator (PsycoGenexpr_New) until a consumer fuses it or it
 * escapes. A promoted function's code and defaults are checked at run
 * time (compiled_code, compiled_defaults).
 * A nested function (a closure, a lambda, a def inside a function) only
 * known at run time is not promoted, only its code
 * (psyfunc.c:psyco_call_closure).
 */
#include "pfuncobject.h"
#include "../psyfunc.h"
#include "pclassobject.h"
#include "pabstract.h"
#include "pdictobject.h"
#include "ptupleobject.h"
#include "../segments.h"

/***************************************************************/
/***   Virtual functions                                     ***/

static source_virtual_t psyco_computed_function;

/* a field of a function MAKE_FUNCTION has just made, stored as ceval
   stores it, without the setters' checks and function watcher events:
   0 the closure, 1 the annotations, 2 the keyword defaults, 3 the defaults */
static void
cimpl_func_set(PyObject *f, long which, PyObject *value)
{
    PyFunctionObject *func = (PyFunctionObject *)f;
    Py_INCREF(value);
    switch (which) {
        case 0:
            Py_XSETREF(func->func_closure, value);
            break;
        case 1:
            Py_XSETREF(func->func_annotations, value);
            break;
        case 2:
            Py_XSETREF(func->func_kwdefaults, value);
            break;
        default:
            Py_XSETREF(func->func_defaults, value);
            break;
    }
}

#if LE_HAS_FUNCTION_ANNOTATE
/* f.__annotate__ set as ceval sets it: the annotate function's qualified
   name is then f's with ".__annotate__" */
static int
cimpl_func_set_annotate(PyObject *f, PyObject *annotate)
{
    PyFunctionObject *func = (PyFunctionObject *)f;
    PyObject *qualname;
    Py_INCREF(annotate);
    Py_XSETREF(func->func_annotate, annotate);
    if (!PyFunction_Check(annotate))
        return 0;
    qualname = PyUnicode_FromFormat("%U.__annotate__", func->func_qualname);
    if (qualname == NULL)
        return -1;
    Py_SETREF(((PyFunctionObject *)annotate)->func_qualname, qualname);
    return 0;
}
#endif

static bool
compute_function(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *newobj;
    vinfo_t *code;
    vinfo_t *globals;
    vinfo_t *qualname;
    vinfo_t *other;

    code = vinfo_getitem(v, iFUNC_CODE);
    if (code == NULL)
        return false;

    globals = vinfo_getitem(v, iFUNC_GLOBALS);
    if (globals == NULL)
        return false;

    qualname = vinfo_getitem(v, iFUNC_QUALNAME);
    if (qualname == NULL)
        return false;

    newobj = psyco_generic_call(po, PyFunction_NewWithQualName,
                                CfCommonNewRefPyObject, "vvv", code, globals,
                                qualname);
    if (newobj == NULL)
        return false;

    /* the optional fields, each a new reference cimpl_func_set takes */
#define SET_FIELD(def, which)                                             \
    do {                                                                  \
        other = vinfo_getitem(v, FIELD_INDEX(def));                       \
        if (other != NULL && !psyco_known_NULL(other)) {                  \
            if (psyco_generic_call(po, cimpl_func_set,                    \
                                   CfNoReturnValue | CfNoPython, "vlv",   \
                                   newobj, (long)(which), other) == NULL) \
                return false;                                             \
        }                                                                 \
    } while (0)

    SET_FIELD(FUNC_closure, 0);
    SET_FIELD(FUNC_annotations, 1);
    SET_FIELD(FUNC_kwdefaults, 2);
    SET_FIELD(FUNC_defaults, 3);
#undef SET_FIELD

    /* move the resulting non-virtual Python object back into 'v' */
    vinfo_move(po, v, newobj);
    return true;
}

DEFINEFN
vinfo_t *
PsycoFunction_NewWithQualName(PsycoObject *po, vinfo_t *codeobj,
                              vinfo_t *globals, vinfo_t *qualname)
{
    vinfo_t *r = vinfo_new(VirtualTime_New(&psyco_computed_function));
    r->array = array_new(FUNC_TOTAL);
    r->array->items[iOB_TYPE] =
        vinfo_new(CompileTime_New((long)(&PyFunction_Type)));

    vinfo_incref(codeobj);
    r->array->items[iFUNC_CODE] = codeobj;

    vinfo_incref(globals);
    r->array->items[iFUNC_GLOBALS] = globals;

    vinfo_incref(qualname);
    r->array->items[iFUNC_QUALNAME] = qualname;

    r->array->items[iFUNC_CLOSURE] = psyco_vi_Zero();
    r->array->items[iFUNC_ANNOTATIONS] = psyco_vi_Zero();
    r->array->items[iFUNC_KWDEFAULTS] = psyco_vi_Zero();
    r->array->items[iFUNC_DEFAULTS] = psyco_vi_Zero();
    return r;
}

DEFINEFN
bool
PsycoFunction_SetField(PsycoObject *po, vinfo_t *func, int which,
                       vinfo_t *value)
{
    vinfo_t **field;
#if LE_HAS_FUNCTION_ANNOTATE
    if (which == 4)
        return psyco_generic_call(po, cimpl_func_set_annotate,
                                  CfCommonIntZeroOk | CfNoPython, "vv", func,
                                  value) != NULL;
#endif
    if (!is_virtualtime(func->source) ||
        VirtualTime_Get(func->source) != &psyco_computed_function)
        return psyco_generic_call(po, cimpl_func_set,
                                  CfNoReturnValue | CfNoPython, "vlv", func,
                                  (long)which, value) != NULL;
    /* fit to cross calls, as the function's other fields are */
    if (!psyco_forking(po, value->array))
        return false;
    field = &func->array->items[which == 0   ? iFUNC_CLOSURE
                                : which == 1 ? iFUNC_ANNOTATIONS
                                : which == 2 ? iFUNC_KWDEFAULTS
                                             : iFUNC_DEFAULTS];
    vinfo_incref(value);
    vinfo_decref(*field, po);
    *field = value;
    return true;
}

static source_virtual_t psyco_pending_genexpr;

static bool
compute_genexpr(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *f, *a, *g;
    int i;
    f = vinfo_getitem(v, iGENEXPR_FUNC);
    a = vinfo_getitem(v, iGENEXPR_ARGS);
    if (f == NULL || a == NULL)
        return false;
    g = psyco_generic_call(po, le_function_tp_call(), CfCommonNewRefPyObject,
                           "vvl", f, a, 0L);
    if (g == NULL)
        return false;
    /* the call's function and arguments, which the generator's frame now
       holds, go as CPython's call drops them: kept, they would outlive a
       generator closed or cleared before its end */
    for (i = iGENEXPR_FUNC; i <= iGENEXPR_GLOBALS; i++) {
        vinfo_xdecref(v->array->items[i], po);
        v->array->items[i] = NULL;
    }
    vinfo_move(po, v, g);
    return true;
}

DEFINEFN
vinfo_t *
PsycoGenexpr_New(vinfo_t *vfunc, vinfo_t *arg_tuple, vinfo_t *vglobals,
                 PyCodeObject *co)
{
    vinfo_t *r = vinfo_new(VirtualTime_New(&psyco_pending_genexpr));
    r->array = array_new(iGENEXPR_CODE + 1);
    r->array->items[iOB_TYPE] = vinfo_new(CompileTime_New(
        (long)((co->co_flags & CO_COROUTINE) ? &PyCoro_Type : &PyGen_Type)));
    vinfo_incref(vfunc);
    r->array->items[iGENEXPR_FUNC] = vfunc;
    vinfo_incref(arg_tuple);
    r->array->items[iGENEXPR_ARGS] = arg_tuple;
    vinfo_incref(vglobals);
    r->array->items[iGENEXPR_GLOBALS] = vglobals;
    Py_INCREF(co);
    r->array->items[iGENEXPR_CODE] = vinfo_new(
        CompileTime_NewSk(sk_new((long)co, SkFlagPyObj | SkFlagFixed)));
    return r;
}

DEFINEFN
bool
PsycoGenexpr_Check(vinfo_t *v)
{
    return is_virtualtime(v->source) &&
           VirtualTime_Get(v->source) == &psyco_pending_genexpr;
}

/***************************************************************/
/*** function objects meta-implementation                    ***/

/* The defaults a call to 'f' is compiled with: those LeJIT first saw on f,
   recorded while code relies on them (psyco_record), never changed, so a
   respawn's replay decides as the original compile did and a guard
   comparing f's func_defaults with them can't be fooled by a new tuple at
   a reused address. Py_None stands for NULL. f is held by the code that
   promoted it, so its address isn't reused under the record either. */
static PyObject *
compiled_defaults(PyObject *f)
{
    static le_addrmap_t seen; /* function address -> defaults */
    PyObject **d = psyco_record(&seen, f);
    if (*d == NULL) {
        *d = PyFunction_GET_DEFAULTS(f);
        if (*d == NULL)
            *d = Py_None;
        Py_INCREF(*d);
    }
    return *d == Py_None ? NULL : *d;
}

/* The same for f's code: a call compiled for one code
   object checks that f still has it. */
static PyCodeObject *
compiled_code(PyObject *f)
{
    static le_addrmap_t seen; /* function address -> code */
    PyObject **c = psyco_record(&seen, f);
    if (*c == NULL) {
        *c = PyFunction_GET_CODE(f);
        Py_INCREF(*c);
    }
    return (PyCodeObject *)*c;
}

/* The keyword-only defaults a call to 'f' is compiled with, recorded as
   compiled_defaults records the defaults: Py_None where f had none, or
   where they can't be kept known (a key that isn't an exact str, or no
   tag to watch them by); else (dict, tag address, tag, copy). The dict is
   f's own, which can change in place, so a guard compares f's
   func_kwdefaults with it and the tag at the address with the one
   recorded; the copy, whose values the call binds, never changes. Where
   dicts have no tag, LeJIT's watcher counts the dict's changes. */
static PyObject *
compiled_kwdefaults(PyObject *f)
{
    static le_addrmap_t seen; /* function address -> record */
    PyObject **r = psyco_record(&seen, f);
    if (*r == NULL) {
        PyObject *d = PyFunction_GET_KW_DEFAULTS(f), *key, *value, *copy;
        Py_ssize_t pos = 0;
        uint64_t *tag;
        *r = Py_None;
        Py_INCREF(Py_None);
        if (d == NULL || !PyDict_CheckExact(d))
            return NULL;
        while (PyDict_Next(d, &pos, &key, &value))
            if (!PyUnicode_CheckExact(key))
                return NULL;
#if LE_HAS_DICT_VERSION_TAG
        tag = (uint64_t *)&((PyDictObject *)d)->ma_version_tag;
#else
        tag = le_dict_counter((PyDictObject *)d);
        if (tag == NULL)
            return NULL;
#endif
        copy = PyDict_Copy(d);
        if (copy == NULL) {
            PyErr_Clear();
            return NULL;
        }
        Py_SETREF(*r, Py_BuildValue("(ONKN)", d, PyLong_FromVoidPtr(tag),
                                    (unsigned long long)*tag, copy));
        if (*r == NULL) {
            PyErr_Clear();
            *r = Py_None;
            Py_INCREF(Py_None);
        }
    }
    return *r == Py_None ? NULL : *r;
}

/* the call names every keyword-only parameter, so their defaults aren't
   read */
static bool
names_every_kwonly(PyCodeObject *co, PyObject *kwnames)
{
    int i, j;
    for (i = co->co_argcount; i < co->co_argcount + co->co_kwonlyargcount;
         i++) {
        PyObject *name = le_code_local_name(co, i);
        bool found = false;
        for (j = 0; kwnames != NULL && j < PyTuple_GET_SIZE(kwnames) && !found;
             j++) {
            PyObject *k = PyTuple_GET_ITEM(kwnames, j);
            found = k == name || PyUnicode_Compare(k, name) == 0;
        }
        if (!found)
            return false;
    }
    return true;
}

/* emits 'the word at 'address' != expected' and says if it is true at run
   time */
static int
word_changed(PsycoObject *po, void *address, uint64_t expected)
{
    vinfo_t *va = vinfo_new(CompileTime_New((long)address));
    vinfo_t *vnow =
        psyco_get_field_offset(po, va, FMUT(DEF_ARRAY(long, 0)), 0);
    vinfo_t *vexp;
    condition_code_t cc;
    vinfo_decref(va, po);
    if (vnow == NULL)
        return -1;
    vexp = vinfo_new(CompileTime_New((long)expected));
    cc = integer_cmp(po, vnow, vexp, Py_NE);
    vinfo_decref(vexp, po);
    vinfo_decref(vnow, po);
    if (cc == CC_ERROR)
        return -1;
    return runtime_condition_f(po, cc);
}

/* emits 'f's field != expected' and says if it is true at run time */
static int
field_changed(PsycoObject *po, PyObject *f, defield_t df, PyObject *expected)
{
    vinfo_t *vf = vinfo_new(CompileTime_New((long)f));
    vinfo_t *vnow = psyco_get_field(po, vf, df);
    vinfo_t *vexp;
    condition_code_t cc;
    vinfo_decref(vf, po);
    if (vnow == NULL)
        return -1;
    vexp = vinfo_new(CompileTime_New((long)expected));
    cc = integer_cmp(po, vnow, vexp, Py_NE);
    vinfo_decref(vexp, po);
    vinfo_decref(vnow, po);
    if (cc == CC_ERROR)
        return -1;
    return runtime_condition_f(po, cc);
}

#define FUNC_code_now FMUT(FUNC_code)
#define FUNC_defaults_now FMUT(FUNC_defaults)
#define FUNC_kwdefaults_now FMUT(FUNC_kwdefaults)

#ifdef Py_TPFLAGS_HAVE_VECTORCALL
DEFINEVAR vectorcallfunc le_usual_vectorcall;
/* CPython calls a function through its vectorcall function, which C code
   may have replaced: emits 'func's is not the usual one' and says if it
   is true at run time */
#define FUNC_vectorcall_now                                          \
    STRUCT_FIELD_BUILD(PyFunctionObject, vectorcallfunc, vectorcall, \
                       NO_PREV_FIELD, FIELD_MUTABLE)
static int
vectorcall_replaced(PsycoObject *po, vinfo_t *func)
{
    vinfo_t *vnow = psyco_get_field(po, func, FUNC_vectorcall_now);
    vinfo_t *vusual;
    condition_code_t cc;
    if (vnow == NULL)
        return -1;
    vusual = vinfo_new(CompileTime_New((long)le_usual_vectorcall));
    cc = integer_cmp(po, vnow, vusual, Py_NE);
    vinfo_decref(vusual, po);
    vinfo_decref(vnow, po);
    if (cc == CC_ERROR)
        return -1;
    return runtime_condition_f(po, cc);
}
#endif

/* The real call of the Python function 'vfunc' with the arguments 'arg'
   and 'kw' (NULL, a known NULL, pending keywords or a dict), through
   vectorcall as ceval makes it where the arguments are known one by one,
   else through its tp_call. The function runs Python code, which tests
   eval_breaker itself: the call leaves no test after it to do
   (pycompiler.c:breaker_after_call), as tp_call doesn't. With
   'as_direct', it stands for a call CPython makes with no C stack, and is
   made by segments.c:le_call_as_direct. */
DEFINEFN
vinfo_t *
PsycoFunction_RealCall(PsycoObject *po, vinfo_t *vfunc, vinfo_t *arg,
                       vinfo_t *kw, bool as_direct)
{
    PyObject *kwnames = NULL;
    vinfo_t **kwvalues = NULL;
    vinfo_t *r = NULL;
    char blocking = po->blocking_call;
    int n = is_virtualtime(arg->source) ? PsycoTuple_Load(arg) : -1;
    bool keywords = kw != NULL && !psyco_known_NULL(kw);
#if !LE_STACK_SEGMENTS
    as_direct = false; /* CPython's calls take C stack too */
#endif
    if (keywords)
        kwnames = PsycoDict_PendingNames(kw, &kwvalues);
    if (n >= 0 && (!keywords || kwnames != NULL)) {
#if LE_STACK_SEGMENTS
        if (as_direct)
            r = psyco_vectorcall_as_direct(
                po, vfunc, &PsycoTuple_GET_ITEM(arg, 0), n, kwnames, kwvalues);
        else
#endif
            r = psyco_vectorcall(po, vfunc, NULL, &PsycoTuple_GET_ITEM(arg, 0),
                                 n, kwnames, kwvalues);
        if (r == NULL && PycException_Occurred(po))
            return NULL;
    }
#if LE_STACK_SEGMENTS
    if (r == NULL && as_direct && keywords)
        r = psyco_generic_call(po, le_call_as_direct, CfCommonNewRefPyObject,
                               "lvvv", (long)le_function_tp_call(), vfunc, arg,
                               kw);
    else if (r == NULL && as_direct)
        r = psyco_generic_call(po, le_call_as_direct, CfCommonNewRefPyObject,
                               "lvvl", (long)le_function_tp_call(), vfunc, arg,
                               (long)0);
#endif
    if (r == NULL && !as_direct && keywords)
        r = psyco_generic_call(po, le_function_tp_call(),
                               CfCommonNewRefPyObject, "vvv", vfunc, arg, kw);
    else if (r == NULL && !as_direct)
        r = psyco_generic_call(po, le_function_tp_call(),
                               CfCommonNewRefPyObject, "vvl", vfunc, arg,
                               (long)0);
    po->blocking_call = blocking;
    return r;
}

DEFINEFN
vinfo_t *
pfunction_simple_call(PsycoObject *po, vinfo_t *func, PyObject *f,
                      vinfo_t *arg, vinfo_t *kw, bool allow_inline)
{
    PyObject *glob;
    PyObject *defl;
    PyCodeObject *co;
    vinfo_t *fglobals;
    vinfo_t *fdefaults;
    vinfo_t *result, *vf;
    PyObject *kwdefs = NULL;
    int tuple_size;
    bool as_direct = true;

    /* the code and the defaults are those compiled while f still has
       them */
    co = compiled_code(f);
#ifdef Py_TPFLAGS_HAVE_VECTORCALL
    /* as vectorcall_replaced, read when compiling the call */
    if (*(vectorcallfunc *)((char *)f + Py_TYPE(f)->tp_vectorcall_offset) !=
        le_usual_vectorcall) {
        /* CPython calls it from C too */
        as_direct = false;
        goto fallback;
    }
#endif
    switch (field_changed(po, f, FUNC_code_now, (PyObject *)co)) {
        case 0:
            break;
        case 1:
            goto fallback;
        default:
            return NULL;
    }

    /* keyword-only defaults, unless the call names them all: those
       recorded, while f has that dict and it is unchanged */
    if (co->co_kwonlyargcount > 0) {
        vinfo_t **kwvalues;
        PyObject *kwnames =
            kw == NULL ? NULL : PsycoDict_PendingNames(kw, &kwvalues);
        PyObject *rec;
        if (!names_every_kwonly(co, kwnames) &&
            (rec = compiled_kwdefaults(f)) != NULL) {
            switch (field_changed(po, f, FUNC_kwdefaults_now,
                                  PyTuple_GET_ITEM(rec, 0))) {
                case 0:
                    break;
                case 1:
                    goto fallback;
                default:
                    return NULL;
            }
            switch (word_changed(
                po, PyLong_AsVoidPtr(PyTuple_GET_ITEM(rec, 1)),
                PyLong_AsUnsignedLongLong(PyTuple_GET_ITEM(rec, 2)))) {
                case 0:
                    break;
                case 1:
                    goto fallback;
                default:
                    return NULL;
            }
            kwdefs = PyTuple_GET_ITEM(rec, 3);
        }
    }

    /* a call that gives every positional argument by position doesn't
       use the defaults */
    defl = compiled_defaults(f);
    tuple_size = PsycoTuple_Load(arg);
    if (tuple_size < 0 || tuple_size < co->co_argcount)
        switch (field_changed(po, f, FUNC_defaults_now, defl)) {
            case 0:
                break;
            case 1:
                goto fallback;
            default:
                return NULL;
        }

    glob = PyFunction_GET_GLOBALS(f);
    Py_INCREF(glob);
    fglobals = vinfo_new(CompileTime_NewSk(sk_new((long)glob, SkFlagPyObj)));
    if (defl == NULL)
        fdefaults = psyco_vi_Zero();
    else {
        Py_INCREF(defl);
        fdefaults =
            vinfo_new(CompileTime_NewSk(sk_new((long)defl, SkFlagPyObj)));
    }

    result = psyco_call_pyfunc(po, func, co, fglobals, fdefaults, arg, kw,
                               kwdefs, po->pr.auto_recursion, allow_inline);
    vinfo_decref(fdefaults, po);
    vinfo_decref(fglobals, po);
    return result;

fallback:
    Py_INCREF(f);
    vf = vinfo_new(CompileTime_NewSk(sk_new((long)f, SkFlagPyObj)));
    result = PsycoFunction_RealCall(po, vf, arg, kw, as_direct);
    vinfo_decref(vf, po);
    return result;
}

DEFINEFN
vinfo_t *
pfunction_call(PsycoObject *po, vinfo_t *func, vinfo_t *arg, vinfo_t *kw)
{
    /* keyword arguments: only pending ones (pdictobject.c), which the
       binder takes; a call with them always has them */
    vinfo_t **kwvalues;
    bool keywords = !psyco_known_NULL(kw);
    if (keywords && PsycoDict_PendingNames(kw, &kwvalues) == NULL)
        goto fallback;

    if (!is_virtualtime(func->source)) {
        /* run-time or compile-time values: promote the
           function object as a whole into compile-time */
        PyObject *f;
        /* made run-time at a capped merge point: never promoted again */
        if (is_megamorphic(func->source))
            goto fallback;
        if (!is_compiletime(func->source)) {
            /* the code first: nested functions (CO_NESTED: closures,
               lambdas, defs inside a function) are new function objects on
               every run of the code that makes them, but their code is
               not. Promoting one would only add a specialisation per
               object, each keeping it alive, until the site went
               megamorphic: it is called with its cells read at run time
               instead, while it still has the code promoted. */
            PyObject *c;
            int r;
            vinfo_t *fcode = psyco_get_const(po, func, FUNC_code);
            if (fcode == NULL)
                return NULL;
            r = psyco_pyobj_atcompiletime_mega(po, fcode, &c);
            if (r < 0)
                return NULL;
            if (r == 0)
                goto fallback;
            if (((PyCodeObject *)c)->co_flags & CO_NESTED) {
                /* its defaults are its own: positional calls only */
                if (keywords)
                    goto fallback;
                condition_code_t cc;
                vinfo_t *now = psyco_get_field(po, func, FUNC_code_now);
                if (now == NULL)
                    return NULL;
                cc = integer_cmp(po, now, fcode, Py_NE);
                vinfo_decref(now, po);
                if (cc == CC_ERROR)
                    return NULL;
                if (runtime_condition_f(po, cc))
                    goto fallback;
#ifdef Py_TPFLAGS_HAVE_VECTORCALL
                switch (vectorcall_replaced(po, func)) {
                    case 0:
                        break;
                    case 1:
                        goto fallback;
                    default:
                        return NULL;
                }
#endif
                return psyco_call_closure(po, func, (PyCodeObject *)c, arg);
            }
        }
        switch (psyco_pyobj_atcompiletime_mega(po, func, &f)) {
            case 1: /* promotion ok */
                return pfunction_simple_call(po, func, f, arg, kw, true);

            case 0:            /* megamorphic site */
                goto fallback; /* XXX could do better */

            default:
                return NULL;
        }
    }
    else { /* virtual-time function objects: read the
          individual components */
        PyCodeObject *co;
        vinfo_t *fcode;
        vinfo_t *fglobals;
        vinfo_t *fdefaults;

        fcode = vinfo_getitem(func, iFUNC_CODE);
        if (fcode == NULL)
            return NULL;
        co = (PyCodeObject *)psyco_pyobj_atcompiletime(po, fcode);
        if (co == NULL)
            return NULL;

        fglobals = vinfo_getitem(func, iFUNC_GLOBALS);
        if (fglobals == NULL)
            return NULL;

        fdefaults = vinfo_getitem(func, iFUNC_DEFAULTS);
        if (fdefaults == NULL)
            return NULL;

        /* its keyword-only defaults aren't known: a call that needs them
           is the real one */
        return psyco_call_pyfunc(po, func, co, fglobals, fdefaults, arg, kw,
                                 NULL, po->pr.auto_recursion, true);
    }

fallback:
    return PsycoFunction_RealCall(po, func, arg, kw, true);
}

static vinfo_t *
pfunc_descr_get(PsycoObject *po, PyObject *func, vinfo_t *obj, PyObject *type)
{
    /* see comments of pmember_get() in pdescrobject.c. */

    /* XXX obj is never Py_None here in the current implementation,
       but could be if called by other routines than
       PsycoObject_GenericGetAttr(). */
    vinfo_t *vfunc, *result;
    Py_INCREF(func);
    vfunc = vinfo_new(CompileTime_NewSk(sk_new((long)func, SkFlagPyObj)));
    result = PsycoMethod_New(vfunc, obj);
    vinfo_decref(vfunc, po);
    return result;
}

static PyObject *
function_call_unshared(PyObject *f, PyObject *a, PyObject *k)
{
    return PyFunction_Type.tp_call(f, a, k);
}

/* the slot itself, or a function of its own where other types share it
   (from 3.9 a method's and a method descriptor's tp_call is the
   function's PyVectorcall_Call): psyfunc.c says a function's call cannot
   block, which a method descriptor's can */
DEFINEFN
ternaryfunc
le_function_tp_call(void)
{
    if (PyFunction_Type.tp_call == PyMethod_Type.tp_call ||
        PyFunction_Type.tp_call == PyMethodDescr_Type.tp_call)
        return function_call_unshared;
    return PyFunction_Type.tp_call;
}

INITIALIZATIONFN
void
psy_funcobject_init(void)
{
#ifdef Py_TPFLAGS_HAVE_VECTORCALL
    {
        /* not every CPython exports it by name */
        PyObject *glob = PyDict_New();
        PyCodeObject *co = PyCode_NewEmpty("", "", 0);
        PyObject *f = glob && co ? PyFunction_New((PyObject *)co, glob) : NULL;
        if (f == NULL)
            OUT_OF_MEMORY();
        le_usual_vectorcall = ((PyFunctionObject *)f)->vectorcall;
        Py_DECREF(f);
        Py_DECREF(co);
        Py_DECREF(glob);
    }
#endif
    Psyco_DefineCallMeta(&PyFunction_Type, pfunction_call);
    Psyco_DefineMeta(PyFunction_Type.tp_descr_get, pfunc_descr_get);

    /* function object are mutable;
       they must be forced out of virtual-time across function calls */
    INIT_SVIRTUAL_NOCALL(psyco_computed_function, compute_function, 1);
    /* a generator has an identity: made real before any call */
    INIT_SVIRTUAL_NOCALL(psyco_pending_genexpr, compute_genexpr, 1);
}
