/*
 * Virtual function objects created by MAKE_FUNCTION, and the function tp_call
 * meta. The callee is promoted to a constant (with a megamorphic cut-off) and
 * the call goes to psyfunc.c:psyco_call_pyfunc, which may inline it. A call to
 * a function that is still virtual reads its code, globals and defaults
 * directly and never materialises it.
 * Suspected bugs (from reading):
 *  - compute_function's SET_FIELD tests 'other != NULL ||
 *    !psyco_known_NULL(other)', which should be &&;
 *  - it stores without FMUT/FPYREF, so the new function holds no references
 *    to its closure or defaults;
 *  - the virtual-call path ignores closures and keyword-only defaults.
 */
#include "pfuncobject.h"
#include "../psyfunc.h"
#include "pclassobject.h"

/***************************************************************/
/***   Virtual functions                                     ***/

static source_virtual_t psyco_computed_function;

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

#define SET_FIELD(def)                                   \
    do {                                                 \
        other = vinfo_getitem(v, FIELD_INDEX(def));      \
        if (other != NULL || !psyco_known_NULL(other)) { \
            psyco_put_field(po, newobj, def, other);     \
        }                                                \
    } while (0)

    SET_FIELD(FUNC_closure);
    SET_FIELD(FUNC_annotations);
    SET_FIELD(FUNC_kwdefaults);
    SET_FIELD(FUNC_defaults);
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

/***************************************************************/
/*** function objects meta-implementation                    ***/

DEFINEFN
vinfo_t *
pfunction_simple_call(PsycoObject *po, PyObject *f, vinfo_t *arg,
                      bool allow_inline)
{
    PyObject *glob;
    PyObject *defl;
    PyCodeObject *co;
    vinfo_t *fglobals;
    vinfo_t *fdefaults;
    vinfo_t *result;
    int saved_inlining;

    /* XXX we capture the code object, so this breaks if someone
       changes the .func_code attribute of the function later */
    co = (PyCodeObject *)PyFunction_GET_CODE(f);
    /* psyco_call_pyfunc's own fallback rebuilds the function from code,
       globals and defaults, losing keyword-only defaults: call the real
       function instead */
    if (PyCode_GetNumFree(co) > 0 || co->co_kwonlyargcount > 0)
        goto fallback;

    glob = PyFunction_GET_GLOBALS(f);
    defl = PyFunction_GET_DEFAULTS(f);
    Py_INCREF(glob);
    fglobals = vinfo_new(CompileTime_NewSk(sk_new((long)glob, SkFlagPyObj)));
    if (defl == NULL)
        fdefaults = psyco_vi_Zero();
    else {
        Py_INCREF(defl);
        fdefaults =
            vinfo_new(CompileTime_NewSk(sk_new((long)defl, SkFlagPyObj)));
    }

    saved_inlining = po->pr.is_inlining;
    if (!allow_inline)
        po->pr.is_inlining = true;
    result = psyco_call_pyfunc(po, co, fglobals, fdefaults, arg,
                               po->pr.auto_recursion);
    po->pr.is_inlining = saved_inlining;
    vinfo_decref(fdefaults, po);
    vinfo_decref(fglobals, po);
    return result;

fallback:
    return psyco_generic_call(po, PyFunction_Type.tp_call,
                              CfCommonNewRefPyObject, "lvl", (long)f, arg, 0);
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
        switch (psyco_pyobj_atcompiletime_mega(po, func, &f)) {
            case 1: /* promotion ok */
                return pfunction_simple_call(po, f, arg, true);

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

        return psyco_call_pyfunc(po, co, fglobals, fdefaults, arg,
                                 po->pr.auto_recursion);
    }

fallback:

    return psyco_generic_call(po, PyFunction_Type.tp_call,
                              CfCommonNewRefPyObject, "vvv", func, arg, kw);
}

static vinfo_t *
pfunc_descr_get(PsycoObject *po, PyObject *func, vinfo_t *obj, PyObject *type)
{
    /* see comments of pmember_get() in pdescrobject.c. */

    /* XXX obj is never Py_None here in the current implementation,
       but could be if called by other routines than
       PsycoObject_GenericGetAttr(). */
    return PsycoMethod_New(func, obj);
}

INITIALIZATIONFN
void
psy_funcobject_init(void)
{
    Psyco_DefineMeta(PyFunction_Type.tp_call, pfunction_call);
    Psyco_DefineMeta(PyFunction_Type.tp_descr_get, pfunc_descr_get);

    /* function object are mutable;
       they must be forced out of virtual-time across function calls */
    INIT_SVIRTUAL_NOCALL(psyco_computed_function, compute_function, 1);
}
