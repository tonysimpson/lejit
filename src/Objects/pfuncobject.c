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
#include "ptupleobject.h"

/***************************************************************/
/***   Virtual functions                                     ***/

static source_virtual_t psyco_computed_function;

/* 0 or -1, as a long so that the whole register is meaningful */
static long
cimpl_func_set(PyObject *f, long which, PyObject *value)
{
    switch (which) {
        case 0:
            return PyFunction_SetClosure(f, value);
        case 1:
            /* stored as ceval's MAKE_FUNCTION stores it: 3.10's compiler gives
               a tuple of names and values, which the setter refuses */
            Py_INCREF(value);
            Py_XSETREF(((PyFunctionObject *)f)->func_annotations, value);
            return 0;
        case 2:
            return PyFunction_SetKwDefaults(f, value);
        default:
            return PyFunction_SetDefaults(f, value);
    }
}

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

    /* the optional fields go through CPython's setters, which take their
       own references */
#define SET_FIELD(def, which)                                                 \
    do {                                                                      \
        other = vinfo_getitem(v, FIELD_INDEX(def));                           \
        if (other != NULL && !psyco_known_NULL(other)) {                      \
            if (psyco_generic_call(po, cimpl_func_set,                        \
                                   CfNoReturnValue | CfPyErrIfNonNull, "vlv", \
                                   newobj, (long)(which), other) == NULL)     \
                return false;                                                 \
        }                                                                     \
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

static source_virtual_t psyco_pending_genexpr;

static bool
compute_genexpr(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *f, *a, *g;
    f = vinfo_getitem(v, iGENEXPR_FUNC);
    a = vinfo_getitem(v, iGENEXPR_ARGS);
    if (f == NULL || a == NULL)
        return false;
    g = psyco_generic_call(po, le_function_tp_call(), CfCommonNewRefPyObject,
                           "vvl", f, a, 0L);
    if (g == NULL)
        return false;
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
    r->array->items[iOB_TYPE] =
        vinfo_new(CompileTime_New((long)(&PyGen_Type)));
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

DEFINEFN
vinfo_t *
pfunction_simple_call(PsycoObject *po, vinfo_t *func, PyObject *f,
                      vinfo_t *arg, bool allow_inline)
{
    PyObject *glob;
    PyObject *defl;
    PyCodeObject *co;
    vinfo_t *fglobals;
    vinfo_t *fdefaults;
    vinfo_t *result;
    int tuple_size;

    /* the code and the defaults are those compiled while f still has
       them */
    co = compiled_code(f);
    /* psyco_call_pyfunc's own fallback rebuilds the function from code,
       globals and defaults, losing keyword-only defaults: call the real
       function instead */
    if (co->co_kwonlyargcount > 0)
        goto fallback;
    switch (field_changed(po, f, FUNC_code_now, (PyObject *)co)) {
        case 0:
            break;
        case 1:
            goto fallback;
        default:
            return NULL;
    }

    /* a call that gives every positional argument doesn't use the
       defaults (keyword-only ones went to the fallback) */
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

    result = psyco_call_pyfunc(po, func, co, fglobals, fdefaults, arg,
                               po->pr.auto_recursion, allow_inline);
    vinfo_decref(fdefaults, po);
    vinfo_decref(fglobals, po);
    return result;

fallback:
    return psyco_generic_call(po, le_function_tp_call(),
                              CfCommonNewRefPyObject, "lvl", (long)f, arg,
                              (long)0);
}

DEFINEFN
vinfo_t *
pfunction_call(PsycoObject *po, vinfo_t *func, vinfo_t *arg, vinfo_t *kw)
{
    if (!psyco_known_NULL(kw))
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
            if (r == 0 || ((PyCodeObject *)c)->co_kwonlyargcount > 0)
                goto fallback;
            if (((PyCodeObject *)c)->co_flags & CO_NESTED) {
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
                return psyco_call_closure(po, func, (PyCodeObject *)c, arg);
            }
        }
        switch (psyco_pyobj_atcompiletime_mega(po, func, &f)) {
            case 1: /* promotion ok */
                return pfunction_simple_call(po, func, f, arg, true);

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
        if (co->co_kwonlyargcount > 0)
            goto fallback; /* as in pfunction_simple_call */

        fglobals = vinfo_getitem(func, iFUNC_GLOBALS);
        if (fglobals == NULL)
            return NULL;

        fdefaults = vinfo_getitem(func, iFUNC_DEFAULTS);
        if (fdefaults == NULL)
            return NULL;

        return psyco_call_pyfunc(po, func, co, fglobals, fdefaults, arg,
                                 po->pr.auto_recursion, true);
    }

fallback:

    return psyco_generic_call(po, le_function_tp_call(),
                              CfCommonNewRefPyObject, "vvv", func, arg, kw);
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
    Psyco_DefineCallMeta(&PyFunction_Type, pfunction_call);
    Psyco_DefineMeta(PyFunction_Type.tp_descr_get, pfunc_descr_get);

    /* function object are mutable;
       they must be forced out of virtual-time across function calls */
    INIT_SVIRTUAL_NOCALL(psyco_computed_function, compute_function, 1);
    /* a generator has an identity: made real before any call */
    INIT_SVIRTUAL_NOCALL(psyco_pending_genexpr, compute_genexpr, 1);
}
