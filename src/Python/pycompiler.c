/*
 * The bytecode compiler: a compile-time copy of ceval.c's main loop. Each
 * opcode case works on a stack of vinfo_t and calls the compile-time object
 * protocol (Objects/p*.c). What differs from ceval, and where cases come from:
 *  - Restart. If an opcode needs a run-time value as a constant it raises the
 *    promotion pseudo-exception: compilation stops, a run-time switch is
 *    emitted, and the *same opcode is re-run from the start* once the value is
 *    known. So an opcode must do nothing irreversible before the last point
 *    where it can fail or promote (use TOP(), and POP() only at the end).
 *  - LOAD_GLOBAL is a constant behind a dict-version guard whose failure
 *    respawns (load_global_cached); the block is split after each such
 *    site so that a respawn replay never re-decides one.
 *  - Most of the switch's cases are translated from CPython's ceval.c
 *    (the stack, locals, cells, jumps, operators, attributes and the
 *    BUILD_ cases); the others are written for LeJIT. In front of the
 *    translated BINARY_ADD and INPLACE_ADD, unicode_concatenate does
 *    ceval's in-place append of a str stored back into its variable.
 *  - After the switch come the merge-point checks, then the handling of
 *    pseudo-exceptions (promotion, EInline, EReturn/EBreak/EContinue, Python
 *    exceptions) with ceval-style unwinding of the block stack.
 *  - After a call that may have started tracing, a frame can leave compiled
 *    code for ceval (resume_if_tracing), with its state at that instruction.
 * The block opcodes are those of the source's opcode.h; the others are
 * values no instruction has (pycinternal.h), so both models are here:
 * 3.7's SETUP_LOOP/EXCEPT/FINALLY with BREAK_LOOP and CONTINUE_LOOP as
 * pseudo-exceptions, and 3.8's SETUP_FINALLY with BEGIN_FINALLY,
 * CALL_FINALLY and POP_FINALLY, where return, break and continue never
 * unwind a block.
 * Exceptions: catching one follows ceval (an EXCEPT_HANDLER block over six
 * stack items, cimpl_push_exc_info), and a finally block entered by an
 * exception gets the same. On 3.7 a finally block entered by a
 * pseudo-exception (EReturn, EBreak, EContinue) differs from ceval: it
 * gets a single 3-tuple, which END_FINALLY tells apart from an exception
 * type. The one word psyco_emit_header reserves for a function with
 * handlers is its catcher word: the traceback frame of the exception it
 * last caught, held while something else may read that frame's locals
 * (catcher_flush, frames.c psyco_frame_catch).
 */
#include "pycompiler.h"
#include "../codemanager.h"
#include "../dispatcher.h"
#include "../mergepoints.h"
#include "../processor.h"
#include "../psyfunc.h"
#include "../bytecode.h"
#include "frames.h"
/* le_frame_current, le_frame_set_current */
#include "../mechanisms/frames.h"
/* le_recursion_suspend and le_recursion_resume */
#include "../mechanisms/recursion.h"
#include "../pycodegen.h"

#include "../Objects/pabstract.h"
#include "../Objects/pdictobject.h"
#include "../Objects/pfuncobject.h"
#include "../Objects/plistobject.h"
#include "../Objects/plongobject.h"
#include "../Objects/pboolobject.h"
#include "../Objects/ptupleobject.h"
#include "../Objects/psliceobject.h"
#include "../Objects/punicodeobject.h"

#include "pycinternal.h"
#include <ipyencoding.h>

/* C API calls that some CPython versions don't export: the function where
   libpython exports it, else a copy of CPython's or a public call that
   does the same */
#if LE_HAS_DICT_MERGE_EX_LINKABLE
/* some versions declare it only in an internal header */
PyAPI_FUNC(int) _PyDict_MergeEx(PyObject *mp, PyObject *other, int override);
#define le_dict_merge_unique(a, b) _PyDict_MergeEx(a, b, 2)
#else
PyAPI_FUNC(int)
    _PyDict_MergeUniq(PyObject *mp, PyObject *other, PyObject **dupkey);
/* _PyDict_MergeEx(a, b, 2): a key of b already in a raises KeyError(key) */
static int
le_dict_merge_unique(PyObject *a, PyObject *b)
{
    PyObject *dupkey = NULL, *args;
    int r = _PyDict_MergeUniq(a, b, &dupkey);
    if (r == -2) {
        args = PyTuple_Pack(1, dupkey);
        if (args != NULL) {
            PyErr_SetObject(PyExc_KeyError, args);
            Py_DECREF(args);
        }
        Py_DECREF(dupkey);
        r = -1;
    }
    return r;
}
#endif

#if LE_HAS_LOOKUP_SPECIAL_ID
#define le_lookup_special_id _PyObject_LookupSpecial
#else
/* typeobject.c:_PyObject_LookupSpecialId and _PyObject_LookupSpecial */
static PyObject *
le_lookup_special_id(PyObject *self, _Py_Identifier *attrid)
{
    PyObject *attr = _PyUnicode_FromId(attrid), *res; /* borrowed */
    descrgetfunc f;
    if (attr == NULL)
        return NULL;
    res = _PyType_Lookup(Py_TYPE(self), attr);
    if (res != NULL) {
        if ((f = Py_TYPE(res)->tp_descr_get) == NULL)
            Py_INCREF(res);
        else
            res = f(res, self, (PyObject *)(Py_TYPE(self)));
    }
    return res;
}
#endif

#if LE_HAS_IMPORT_FROM_SPEC
#if LE_HAS_MODULE_SPEC_IS_INITIALIZING_LINKABLE
#define le_module_spec_is_initializing _PyModuleSpec_IsInitializing
#else
/* moduleobject.c:_PyModuleSpec_IsInitializing, which can fail */
static int
le_module_spec_is_initializing(PyObject *spec)
{
    _Py_IDENTIFIER(_initializing);
    PyObject *name, *value;
    int rc;
    if (spec == NULL)
        return 0;
    name = _PyUnicode_FromId(&PyId__initializing); /* borrowed */
    if (name == NULL)
        return -1;
    rc = le_lookup_attr(spec, name, &value);
    if (rc > 0) {
        rc = PyObject_IsTrue(value);
        Py_DECREF(value);
    }
    return rc;
}
#endif
#endif

#if LE_CODESTATS
#include <dlfcn.h>
#include <execinfo.h>
/* Promotion sites (pycompiler.h). Each site is a list
   [code, instr, opcode, pflags, stub bytes, (backtrace offsets in
   lejit._core), entry kind, [values], parent, parent's value] and each value
   [description, ns, bytes, counter]; the parent is the site whose value was
   being compiled when this site was emitted, or -1, and the parent's value
   the index of that value in its [values], or -1,
   the counter being a long the promoted block increments each time it runs.
   Never reset: a site's id indexes this list for as long as its code lives. */
#define CS_PROMO_BT 8
static void *cs_promo_bt[CS_PROMO_BT];
static int cs_promo_nbt = 0;
static PyObject *cs_promo_sites = NULL;
static long *cs_promo_misses = NULL;
static PyObject *cs_promo_pending = NULL; /* the value being compiled */
static long cs_promo_t0, cs_promo_b0;
static long *cs_promo_last_counter = NULL;
DEFINEVAR int psyco_cs_promo_cur = -1;
static int cs_promo_vidx = -1; /* index of the value being compiled in its
                                  site's [values], for the copies */

DEFINEFN int
psyco_cs_promo_site(PsycoObject *po, int pflags, long stub_bytes)
{
    PyObject *bt, *rec;
    Py_ssize_t n;
    int i;
    Dl_info info;
    if (cs_promo_sites == NULL && (cs_promo_sites = PyList_New(0)) == NULL)
        OUT_OF_MEMORY();
    bt = PyTuple_New(cs_promo_nbt);
    for (i = 0; i < cs_promo_nbt; i++) {
        long off = -1;
        if (dladdr(cs_promo_bt[i], &info) && info.dli_fbase != NULL)
            off = (char *)cs_promo_bt[i] - (char *)info.dli_fbase;
        PyTuple_SET_ITEM(bt, i, PyLong_FromLong(off));
    }
    rec = Py_BuildValue("[OiiilNi[]ii]", po->pr.co, po->pr.next_instr,
                        le_op(le_code(po->pr.co), po->pr.next_instr), pflags,
                        stub_bytes, bt, psyco_stat_compile_kind,
                        psyco_cs_promo_cur, cs_promo_vidx);
    if (rec == NULL || PyList_Append(cs_promo_sites, rec) < 0)
        OUT_OF_MEMORY();
    Py_DECREF(rec);
    n = PyList_GET_SIZE(cs_promo_sites);
    cs_promo_misses = PyMem_Realloc(cs_promo_misses, n * sizeof(long));
    cs_promo_misses[n - 1] = 0;
    cs_promo_nbt = 0;
    return (int)(n - 1);
}

/* kind: 1 a PyObject*, 0 a raw word, -1 the megamorphic version */
DEFINEFN void
psyco_cs_promo_begin(int site, long key, int kind)
{
    PyObject *et, *ev, *tb, *d;
    PyErr_Fetch(&et, &ev, &tb);
    if (kind < 0)
        d = Py_BuildValue("(ss)", "MEGA", "");
    else if (kind == 0)
        d = Py_BuildValue("(sl)", "word", key);
    else {
        PyObject *o = (PyObject *)key;
        const char *detail = "";
        char buf[64];
        if (PyType_Check(o))
            detail = ((PyTypeObject *)o)->tp_name;
        else if (PyFunction_Check(o)) {
            PyObject *q = ((PyFunctionObject *)o)->func_qualname;
            detail = q && PyUnicode_Check(q) ? PyUnicode_AsUTF8(q) : "?";
        }
        else if (PyCode_Check(o))
            detail = PyUnicode_AsUTF8(((PyCodeObject *)o)->co_name);
        else if (PyCFunction_Check(o))
            detail = ((PyCFunctionObject *)o)->m_ml->ml_name;
        else if (PyBool_Check(o))
            detail = o == Py_True ? "True" : "False";
        else if (PyLong_CheckExact(o)) {
            snprintf(buf, sizeof(buf), "%ld", PyLong_AsLong(o));
            detail = buf;
        }
        else if (PyUnicode_CheckExact(o) && PyUnicode_GET_LENGTH(o) < 30)
            detail = PyUnicode_AsUTF8(o);
        else if (PyModule_Check(o))
            detail = PyModule_GetName(o);
        if (detail == NULL)
            detail = "?";
        d = Py_BuildValue("(ss)", Py_TYPE(o)->tp_name, detail);
    }
    PyErr_Clear();
    PyErr_Restore(et, ev, tb);
    Py_XDECREF(cs_promo_pending);
    cs_promo_pending = Py_BuildValue("[Nll]", d, (long)site, 0L);
    cs_promo_last_counter = NULL;
    cs_promo_t0 = le_now_ns();
    cs_promo_b0 = psyco_stat_code_bytes;
    psyco_cs_promo_cur = site;
    cs_promo_vidx = (int)PyList_GET_SIZE(
        PyList_GET_ITEM(PyList_GET_ITEM(cs_promo_sites, site), 7));
}

DEFINEFN void
psyco_cs_promo_end(void)
{
    long site;
    PyObject *v = cs_promo_pending, *rec;
    psyco_cs_promo_cur = -1;
    cs_promo_vidx = -1;
    if (v == NULL)
        return;
    cs_promo_pending = NULL;
    site = PyLong_AsLong(PyList_GET_ITEM(v, 1));
    PyList_SetItem(v, 1, PyLong_FromLong(le_now_ns() - cs_promo_t0));
    PyList_SetItem(v, 2, PyLong_FromLong(psyco_stat_code_bytes - cs_promo_b0));
    PyList_Append(v, PyLong_FromVoidPtr(cs_promo_last_counter));
    Py_DECREF(PyList_GET_ITEM(v, 3)); /* PyList_Append took its own */
    rec = PyList_GET_ITEM(cs_promo_sites, site);
    PyList_Append(PyList_GET_ITEM(rec, 7), v);
    Py_DECREF(v);
}

/* 'mov r11, &counter; inc qword [r11]' (13 bytes). Flags are dead here:
   ipromotion_finish spilled them before its compare, and R11 is never
   allocated. */
DEFINEFN code_t *
psyco_cs_promo_counter(code_t *code)
{
    long *c = PyMem_Malloc(sizeof(long));
    *c = 0;
    cs_promo_last_counter = c;
    code[0] = 0x49;
    code[1] = 0xBB;
    *(long *)(code + 2) = (long)c;
    code[10] = 0x49;
    code[11] = 0xFF;
    code[12] = 0x03;
    return code + 13;
}

DEFINEFN void
psyco_cs_promo_miss(int site)
{
    cs_promo_misses[site]++;
}

/* lejit._core._promostats(): the sites, each with its misses and its values'
   run counts filled in (copies; the counters keep counting) */
DEFINEFN PyObject *
psyco_cs_promo_stats(void)
{
    Py_ssize_t i, j;
    PyObject *r = PyList_New(0);
    if (cs_promo_sites == NULL)
        return r;
    for (i = 0; i < PyList_GET_SIZE(cs_promo_sites); i++) {
        PyObject *s = PyList_GET_ITEM(cs_promo_sites, i);
        PyObject *vals = PyList_GET_ITEM(s, 7);
        PyObject *nv = PyList_New(0);
        PyObject *ns;
        for (j = 0; j < PyList_GET_SIZE(vals); j++) {
            PyObject *v = PyList_GET_ITEM(vals, j);
            long *c = PyLong_AsVoidPtr(PyList_GET_ITEM(v, 3));
            PyObject *t = Py_BuildValue("(OOOl)", PyList_GET_ITEM(v, 0),
                                        PyList_GET_ITEM(v, 1),
                                        PyList_GET_ITEM(v, 2), c ? *c : -1L);
            PyList_Append(nv, t);
            Py_DECREF(t);
        }
        ns = Py_BuildValue("(OOOOOOONlOO)", PyList_GET_ITEM(s, 0),
                           PyList_GET_ITEM(s, 1), PyList_GET_ITEM(s, 2),
                           PyList_GET_ITEM(s, 3), PyList_GET_ITEM(s, 4),
                           PyList_GET_ITEM(s, 5), PyList_GET_ITEM(s, 6), nv,
                           cs_promo_misses[i], PyList_GET_ITEM(s, 8),
                           PyList_GET_ITEM(s, 9));
        PyList_Append(r, ns);
        Py_DECREF(ns);
    }
    return r;
}
#endif

#define KNOWN_VAR(type, varname, loc) \
    type varname = (type)(CompileTime_Get(loc->source)->value)

/*****************************************************************/
/***   Meta functions                                          ***/

DEFINEVAR PyObject *Psyco_Meta_Dict = NULL;

DEFINEFN
void
Psyco_DefineMeta(void *c_function, void *psyco_function)
{
    PyObject *key;
    PyObject *value;
    if (Psyco_Meta_Dict == NULL) {
        Psyco_Meta_Dict = PyDict_New();
        if (Psyco_Meta_Dict == NULL)
            return;
    }
    if (c_function == NULL) {
        /* should not occur */
        return;
    }
    key = PyLong_FromLong((long)c_function);
    if (key != NULL) {
        value = PyLong_FromLong((long)psyco_function);
        if (value != NULL) {
            PyDict_SetItem(Psyco_Meta_Dict, key, value);
            Py_DECREF(value);
        }
        Py_DECREF(key);
    }
}

DEFINEFN
PyObject *
Psyco_DefineMetaModule(char *modulename)
{
    PyObject *module = PyImport_ImportModule(modulename);
    if (module == NULL) {
        PyErr_Clear();
    }
    return module;
}

DEFINEFN
PyObject *
Psyco_GetModuleObject(PyObject *module, char *name,
                      PyTypeObject *expected_type)
{
    PyObject *fobj;
    if (module == NULL)
        return NULL;

    fobj = PyObject_GetAttrString(module, name);
    if (fobj == NULL) {
        PyErr_Clear();
        return NULL;
    }
    if (expected_type != NULL && !PyObject_TypeCheck(fobj, expected_type)) {
        Py_DECREF(fobj);
        fobj = NULL;
    }
    return fobj;
}

DEFINEVAR PyObject *Psyco_Unregistered_Metas = NULL;

static void
meta_not_registered_as(const char *owner, char *meth_name, const char *why)
{
    PyObject *s = PyUnicode_FromFormat("%s.%s: %s", owner, meth_name, why);
    if (s == NULL) {
        PyErr_Clear();
        return;
    }
    if (Psyco_Unregistered_Metas == NULL)
        Psyco_Unregistered_Metas = PyList_New(0);
    if (Psyco_Unregistered_Metas == NULL ||
        PyList_Append(Psyco_Unregistered_Metas, s) < 0)
        PyErr_Clear();
#if ALL_CHECKS
    fprintf(stderr, "lejit: no meta for %s\n", PyUnicode_AsUTF8(s));
#endif
    Py_DECREF(s);
}

static void
meta_not_registered(PyObject *module, char *meth_name, const char *why)
{
    const char *name = module ? PyModule_GetName(module) : "?";
    if (name == NULL) {
        PyErr_Clear();
        name = "?";
    }
    meta_not_registered_as(name, meth_name, why);
}

DEFINEFN
PyCFunction
Psyco_DefineMethodFn(PyTypeObject *tp, char *meth_name, int meth_flags,
                     void *meta_fn)
{
    PyMethodDef *ml;
    PyObject *d =
        tp->tp_dict ? PyDict_GetItemString(tp->tp_dict, meth_name) : NULL;
    if (d == NULL || Py_TYPE(d) != &PyMethodDescr_Type) {
        meta_not_registered_as(tp->tp_name, meth_name,
                               "not a method descriptor");
        return NULL;
    }
    ml = ((PyMethodDescrObject *)d)->d_method;
    if (ml->ml_flags != meth_flags) {
        char why[64];
        snprintf(why, sizeof(why), "ml_flags are 0x%x, the meta's are 0x%x",
                 ml->ml_flags, meth_flags);
        meta_not_registered_as(tp->tp_name, meth_name, why);
        return NULL;
    }
    Psyco_DefineMeta(ml->ml_meth, meta_fn);
    return ml->ml_meth;
}

DEFINEFN
PyCFunction
Psyco_DefineModuleFn(PyObject *module, char *meth_name, int meth_flags,
                     void *meta_fn)
{
    PyCFunction f;
    PyObject *fobj =
        Psyco_GetModuleObject(module, meth_name, &PyCFunction_Type);
    if (fobj == NULL) {
        meta_not_registered(module, meth_name, "not a builtin function");
        return NULL;
    }

    if (PyCFunction_GET_FLAGS(fobj) != meth_flags) {
        /* the meta's arguments and its fallback follow meth_flags' convention,
           so it can't stand in for a function with another one */
        char why[64];
        snprintf(why, sizeof(why), "ml_flags are 0x%x, the meta's are 0x%x",
                 PyCFunction_GET_FLAGS(fobj), meth_flags);
        meta_not_registered(module, meth_name, why);
        f = NULL;
    }
    else {
        f = PyCFunction_GET_FUNCTION(fobj);
        Psyco_DefineMeta(f, meta_fn);
    }
    Py_DECREF(fobj);
    return f;
}

DEFINEFN
PyCFunction
Psyco_DefineModuleFast(PyObject *module, char *meth_name, int *meth_flags,
                       void *meta_fn)
{
    PyObject *fobj =
        Psyco_GetModuleObject(module, meth_name, &PyCFunction_Type);
    if (fobj != NULL &&
        PyCFunction_GET_FLAGS(fobj) == (METH_FASTCALL | METH_KEYWORDS))
        *meth_flags = METH_FASTCALL | METH_KEYWORDS;
    else
        *meth_flags = METH_FASTCALL;
    Py_XDECREF(fobj);
    return Psyco_DefineModuleFn(module, meth_name, *meth_flags, meta_fn);
}

DEFINEFN
PyCFunction
Psyco_DefineModuleC(PyObject *module, char *meth_name, int meth_flags,
                    void *meta_fn, void *meta_type_new)
{
    PyObject *o = Psyco_GetModuleObject(module, meth_name, NULL);
    if (o == NULL)
        return NULL;
    if (PyType_Check(o) && ((PyTypeObject *)o)->tp_new != NULL) {
        /* maps a callable type */
        Psyco_DefineMeta(((PyTypeObject *)o)->tp_new, meta_type_new);
        return NULL;
    }
    else
        return Psyco_DefineModuleFn(module, meth_name, meth_flags, meta_fn);
}

DEFINEFN
vinfo_t *
Psyco_Meta1x(PsycoObject *po, void *c_function, int flags,
             const char *arguments, long a1)
{
    void *psyco_fn = Psyco_Lookup(c_function);
    if (psyco_fn == NULL) {
        return psyco_generic_call(po, c_function, flags, arguments, a1);
    }
    else
        return ((vinfo_t * (*)(PsycoObject *, long))(psyco_fn))(po, a1);
}

DEFINEFN
vinfo_t *
Psyco_Meta2x(PsycoObject *po, void *c_function, int flags,
             const char *arguments, long a1, long a2)
{
    void *psyco_fn = Psyco_Lookup(c_function);
    if (psyco_fn == NULL)
        return psyco_generic_call(po, c_function, flags, arguments, a1, a2);
    else
        return ((vinfo_t * (*)(PsycoObject *, long, long))(psyco_fn))(po, a1,
                                                                      a2);
}

DEFINEFN
vinfo_t *
Psyco_Meta3x(PsycoObject *po, void *c_function, int flags,
             const char *arguments, long a1, long a2, long a3)
{
    void *psyco_fn = Psyco_Lookup(c_function);
    if (psyco_fn == NULL)
        return psyco_generic_call(po, c_function, flags, arguments, a1, a2,
                                  a3);
    else
        return ((vinfo_t * (*)(PsycoObject *, long, long, long))(psyco_fn))(
            po, a1, a2, a3);
}

DEFINEFN
int
psyco_pyobj_atcompiletime_mega(PsycoObject *po, vinfo_t *vi, PyObject **out)
{
    if (!compute_vinfo(vi, po))
        return -1;
    if (is_runtime(vi->source)) {
        if (vi->source & RunTime_Megamorphic)
            return 0;
        PycException_Promote(po, vi, &psyco_nonfixed_pyobj_promotion_mega);
        return -1;
    }
    else {
        source_known_t *sk = CompileTime_Get(vi->source);
        sk_mark_used(po, sk);
        *out = (PyObject *)sk->value;
        return 1;
    }
}

DEFINEFN
int
psyco_atcompiletime_static_type(PsycoObject *po, vinfo_t *vi, PyObject **out)
{
    if (!compute_vinfo(vi, po))
        return -1;
    if (is_runtime(vi->source)) {
        if (vi->source & RunTime_Megamorphic)
            return 0;
        PycException_Promote(po, vi, &psyco_static_type_promotion);
        return -1;
    }
    else {
        source_known_t *sk = CompileTime_Get(vi->source);
        sk_mark_used(po, sk);
        *out = (PyObject *)sk->value;
        return 1;
    }
}

/***************************************************************/
/***   pyc_data_t                                            ***/

DEFINEFN
void
pyc_data_build(PsycoObject *po, PyObject *merge_points)
{
    /* rebuild the data in the pyc_data_t */
    int i;
    PyCodeObject *co = po->pr.co;
    int stack_base = po->vlocals.count - co->co_stacksize;
    for (i = stack_base; i < po->vlocals.count; i++)
        if (po->vlocals.items[i] == NULL)
            break;
    po->pr.stack_base = stack_base;
    po->pr.stack_level = i - stack_base;
    po->pr.merge_points = merge_points;
}

static void
block_setup(PsycoObject *po, int type, int handler, int level)
{
    le_block_t *b;
    extra_assert(!po->pr.is_inlining);
    if (po->pr.iblock >= CO_MAXBLOCKS)
        Py_FatalError("block stack overflow");
    b = &po->pr.blockstack[(int)(po->pr.iblock++)];
    b->b_type = type;
    b->b_level = level;
    b->b_handler = handler;
}

static le_block_t *
block_pop(PsycoObject *po)
{
    extra_assert(!po->pr.is_inlining);
    if (po->pr.iblock <= 0)
        Py_FatalError("block stack underflow");
    return &po->pr.blockstack[(int)(--po->pr.iblock)];
}

/*****************************************************************/
/***   Compile-time Pseudo exceptions                          ***/

DEFINEVAR source_virtual_t ERtPython; /* Exception raised by Python */
DEFINEVAR source_virtual_t EReturn;   /* 'return' statement */
DEFINEVAR source_virtual_t EBreak;    /* 'break' statement */
DEFINEVAR source_virtual_t EContinue; /* 'continue' statement */
DEFINEVAR source_virtual_t EInline;   /* request to inline a function */

DEFINEFN
void
PycException_SetString(PsycoObject *po, PyObject *e, const char *text)
{
    PyObject *s = PyUnicode_FromString(text);
    if (s == NULL)
        OUT_OF_MEMORY();
    PycException_SetObject(po, e, s);
}

DEFINEFN
void
PycException_SetObject(PsycoObject *po, PyObject *e, PyObject *v)
{
    PycException_Raise(
        po, vinfo_new(CompileTime_New((long)e)),
        vinfo_new(CompileTime_NewSk(sk_new((long)v, SkFlagPyObj))));
}

DEFINEFN
void
PycException_SetVInfo(PsycoObject *po, PyObject *e, vinfo_t *v)
{
    PycException_Raise(po, vinfo_new(CompileTime_New((long)e)), v);
}

DEFINEFN
void
PycException_Promote(PsycoObject *po, vinfo_t *vi, c_promotion_t *promotion)
{
#if LE_CODESTATS
    cs_promo_nbt = backtrace(cs_promo_bt, CS_PROMO_BT);
#endif
    vinfo_incref(vi);
    PycException_Raise(po, vinfo_new(VirtualTime_New(&promotion->header)), vi);
}

DEFINEFN
void
PycException_SetFormat(PsycoObject *po, PyObject *e, const char *fmt, ...)
{
    PyObject *s;
    va_list vargs;

#ifdef HAVE_STDARG_PROTOTYPES
    va_start(vargs, fmt);
#else
    va_start(vargs);
#endif
    s = PyUnicode_FromFormatV(fmt, vargs);
    va_end(vargs);

    if (s == NULL)
        OUT_OF_MEMORY();
    PycException_SetObject(po, e, s);
}

/* PycException_SetTypeFormat's message, from the names the types have now */
static PyObject *
cimpl_type_message(const char *fmt, const char *s, PyTypeObject *t1,
                   PyTypeObject *t2)
{
    const char *n2 = t2 != NULL ? t2->tp_name : NULL;
    if (s != NULL)
        return PyUnicode_FromFormat(fmt, s, t1->tp_name, n2);
    return PyUnicode_FromFormat(fmt, t1->tp_name, n2);
}

/* A message naming a class that can be renamed (a mutable type) is made
   now, as for any other error, behind a guard on each such class
   (pobject.c:Psyco_TypeNameChanged): CPython makes it at each raise, so a
   class renamed after compiling must not keep its old name. The exception
   stays virtual either way, so an except clause still matches at compile
   time; only on the path where a class changed is its message made at run
   time. Raising it there instead would set the thread state, match,
   normalise and build a traceback entry on every raise. */
DEFINEFN
void
PycException_SetTypeFormat(PsycoObject *po, PyObject *e, const char *fmt,
                           const char *s, PyTypeObject *t1, PyTypeObject *t2)
{
    PyTypeObject *t[2] = {t1, t2};
    const char *n2 = t2 != NULL ? t2->tp_name : NULL;
    vinfo_t *v;
    int i;
    for (i = 0; i < 2; i++) {
        if (t[i] == NULL || !Psyco_TypeMutable(t[i]))
            continue;
        if (runtime_condition_f(po, Psyco_TypeNameChanged(po, t[i]))) {
            v = psyco_generic_call(po, cimpl_type_message,
                                   CfCommonNewRefPyObject | CfNoPython, "llll",
                                   (long)fmt, (long)s, (long)t1, (long)t2);
            if (v != NULL)
                PycException_SetVInfo(po, e, v);
            return;
        }
    }
    if (s != NULL)
        PycException_SetFormat(po, e, fmt, s, t1->tp_name, n2);
    else
        PycException_SetFormat(po, e, fmt, t1->tp_name, n2);
}

DEFINEFN
vinfo_t *
PycException_Matches(PsycoObject *po, PyObject *e)
{
    vinfo_t *result;
    if (PycException_Is(po, &ERtPython)) {
        /* Exception raised by Python, emit a call to
           PyErr_ExceptionMatches() */
        result = psyco_generic_call(po, PyErr_ExceptionMatches,
                                    CfReturnTypeInt, "l", (long)e);
    }
    else if (PycException_IsPython(po)) {
        /* Exception virtually set, that is, present in the PsycoObject
           but not actually set at run-time by Python's PyErr_SetXxx */
        result = psyco_generic_call(po, PyErr_GivenExceptionMatches,
                                    CfPure | CfReturnTypeInt, "vl", po->pr.exc,
                                    (long)e);
    }
    else { /* pseudo exceptions don't match real Python ones */
        result = psyco_vi_Zero();
    }
    return result;
}

PSY_INLINE void
clear_pseudo_exception(PsycoObject *po)
{
    extra_assert(PycException_Occurred(po));
    if (po->pr.tb != NULL) {
        vinfo_decref(po->pr.tb, po);
        po->pr.tb = NULL;
    }
    if (po->pr.val != NULL) {
        vinfo_decref(po->pr.val, po);
        po->pr.val = NULL;
    }
    vinfo_decref(po->pr.exc, po);
    po->pr.exc = NULL;
}

DEFINEFN
void
PycException_Clear(PsycoObject *po)
{
    if (PycException_Is(po, &ERtPython)) {
        /* Clear the Python exception set at run-time */
        psyco_generic_call(po, PyErr_Clear, CfNoReturnValue, "");
    }
    clear_pseudo_exception(po);
}

DEFINEFN
void
psyco_virtualize_exception(PsycoObject *po)
{
    /* fetch a Python exception set at compile-time (that is, now)
       and turn into a pseudo-exception (typically to be re-raised
       at run-time). */
    PyObject *exc, *val, *tb;
    vinfo_t *vexc, *vval, *vtb;
    PyErr_Fetch(&exc, &val, &tb);
    extra_assert(exc != NULL);

    vexc = vinfo_new(CompileTime_NewSk(sk_new((long)exc, SkFlagPyObj)));
    /* val is NULL for PyErr_SetNone, e.g. MemoryError */
    vval = val == NULL
               ? NULL
               : vinfo_new(CompileTime_NewSk(sk_new((long)val, SkFlagPyObj)));
    vtb = tb == NULL
              ? NULL
              : vinfo_new(CompileTime_NewSk(sk_new((long)tb, SkFlagPyObj)));
    PycException_Restore(po, vexc, vval, vtb);
}

static void
cimpl_pyerr_fetch(PyObject *target[])
{
    extra_assert(PyErr_Occurred());
    PyErr_Fetch(target + 0, target + 1, target + 2);
    if (target[0] == NULL) {
        target[0] = Py_None;
        Py_INCREF(Py_None);
    }
    if (target[1] == NULL) {
        target[1] = Py_None;
        Py_INCREF(Py_None);
    }
    if (target[2] == NULL) {
        target[2] = Py_None;
        Py_INCREF(Py_None);
    }
}

/* Catching an exception, as 3.7 ceval.c's exception_unwind does for
   SETUP_EXCEPT: the handled exception that was current (tstate->exc_info) is
   saved, then the pending error is fetched, normalised and made current.
   target[] gets the six objects ceval pushes, bottom first: old tb, value,
   type, then new tb, value, type (new references). ceval may push NULL for
   the old tb/value; LeJIT's stack can't hold NULL, and None means the same
   to exc_info readers. 'word' is the catching frame's catcher word, which
   takes the frame of the entry made for it (frames.c:psyco_frame_catch),
   and 'own' its slot. */
static void
cimpl_push_exc_info(PyObject *target[], PyObject **word,
                    stack_frame_info_t **own)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *otype, *ovalue, *otb;
    PyObject *exc, *val, *tb;
    extra_assert(PyErr_Occurred());
    le_exc_info_get(tstate, &otype, &ovalue, &otb);
    target[0] = otb ? otb : Py_None;
    target[1] = ovalue ? ovalue : Py_None;
    target[2] = otype ? otype : Py_None;
    /* the old references move to target[]; borrowed Nones need one */
    if (otb == NULL)
        Py_INCREF(Py_None);
    if (ovalue == NULL)
        Py_INCREF(Py_None);
    if (otype == NULL)
        Py_INCREF(Py_None);
    PyErr_Fetch(&exc, &val, &tb);
    PyErr_NormalizeException(&exc, &val, &tb);
    PyException_SetTraceback(val, tb != NULL ? tb : Py_None);
    Py_INCREF(exc);
    Py_INCREF(val);
    le_exc_info_put(tstate, exc, val, tb);
    if (tb == NULL)
        tb = Py_None;
    Py_INCREF(tb);
    target[3] = tb;
    target[4] = val;
    target[5] = exc;
    if (word != NULL && PyTraceBack_Check(tb))
        psyco_frame_catch(((PyTracebackObject *)tb)->tb_frame, word, own);
}

/* The restoring half (ceval's UNWIND_EXCEPT_HANDLER), for POP_EXCEPT and for
   unwinding through an EXCEPT_HANDLER block. Doesn't consume its arguments
   (psyco_generic_call can't express that); the caller decrefs them. The
   handled exception goes, and with it, as a rule, the last hold on the
   frame in the catcher word 'word', which is then let go. */
static void
cimpl_pop_exc_info(PyObject *type, PyObject *value, PyObject *tb,
                   PyObject **word)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *otype, *ovalue, *otb;
    le_exc_info_get(tstate, &otype, &ovalue, &otb);
    Py_INCREF(type);
    Py_INCREF(value);
    Py_INCREF(tb);
    le_exc_info_put(tstate, type, value, tb);
    Py_XDECREF(otype);
    Py_XDECREF(ovalue);
    Py_XDECREF(otb);
    if (word != NULL)
        psyco_frame_catcher_drop(word);
}

DEFINEFN
void
PycException_Fetch(PsycoObject *po)
{
    if (PycException_Is(po, &ERtPython)) {
        /* 		vinfo_t* exc = vinfo_new(SOURCE_DUMMY_WITH_REF); */
        /* 		vinfo_t* val = vinfo_new(SOURCE_DUMMY_WITH_REF); */
        /* 		vinfo_t* tb  = vinfo_new(SOURCE_DUMMY_WITH_REF); */
        /* 		psyco_generic_call(po, PyErr_Fetch, CfNoReturnValue, */
        /* 				   "rrr", exc, val, tb); */
        /* 		vinfo_decref(tb, po); */
        vinfo_array_t *array = array_new(3);
        psyco_generic_call(po, cimpl_pyerr_fetch, CfNoReturnValue, "A", array);
        clear_pseudo_exception(po);
        po->pr.exc = array->items[0];
        po->pr.val = array->items[1]; /* po->pr.val!=NULL after this */
        po->pr.tb = array->items[2];  /* po->pr.tb!=NULL after this */
        array_release(array);
    }
}

/* A frame's place for a traceback entry: where f_lineno is the line
   whenever it isn't 0, it is left 0, so that the line comes from f_lasti
   and doesn't stay stale while compiled code goes on in the frame */
static void
set_frame_place(PyFrameObject *f, int lasti, int lineno)
{
    le_frame_set_lasti(le_frame_of(f), le_lasti_of(lasti));
    f->f_lineno = LE_HAS_LINENO_WHEN_SET ? 0 : lineno;
}

static PyFrameObject *
cimpl_new_frame(PyThreadState *tstate, PyCodeObject *code, PyObject *globals,
                int lasti, int lineno, PyObject **word)
{
    /* The real frame if the code has one (run by LeCode_Run), the one asked
       for, or the one that stood for the call when it caught an exception
       (its catcher word's, 'word'), so that the call's entries share one
       frame, as CPython's do. Otherwise a minimalistic frame object,
       working around the specific expectations of PyFrame_New(), which is
       the only reasonable way to create frame objects (because of free
       lists etc). Its f_back is the entry's over it
       (psyco_frame_link_callee), or for the frame that catches the
       exception its caller, when read (frames.c:catcher_caller). */
    PyFrameObject *result = psyco_running_real_frame(code);
    le_frame_t *back = le_frame_current(tstate);
    if (result == NULL)
        result = psyco_innermost_activation(code); /* one asked for */
    if (result == NULL && word != NULL)
        result = (PyFrameObject *)*word;
    if (result != NULL) {
        Py_INCREF(result);
        set_frame_place(result, lasti, lineno);
        return result;
    }
    /* frame objects are not created in stack order with Psyco, so it's
       probably better not to create plain wrong chained lists */
    le_frame_set_current(tstate, NULL);
    result = PyFrame_New(tstate, code, globals, NULL);
    le_frame_set_current(tstate, back);
    if (result != NULL) {
        /* a frame reused from the code's free one may hold a note */
        if (code->co_stacksize >= 3)
            le_pyframe_stack(result)[0] = NULL;
        set_frame_place(result, lasti, lineno);
    }
    return result;
}

static void
cimpl_rt_traceback(PyCodeObject *code, PyObject *globals, int lasti,
                   int lineno, PyObject **word)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyFrameObject *f =
        cimpl_new_frame(tstate, code, globals, lasti, lineno, word);
    /* an out-of-memory error just replaces the current exception */
    if (f != NULL) {
        /* the exception came out of the frame of the last entry: if that is an
           emulated one (a compiled callee), this frame is its f_back; an
           entry is recorded in every frame the exception passes through */
        PyObject *last = le_exc_traceback(tstate);
        if (last != NULL && PyTraceBack_Check(last)) {
            psyco_frame_link_callee(((PyTracebackObject *)last)->tb_frame, f);
        }
        PyTraceBack_Here(f);
        Py_DECREF(f);
    }
}

/* five of a compiled frame's locals, from 'start', into the frame of its
   traceback entry: 'tb', or the raised exception's when NULL */
static void
cimpl_tb_locals(PyObject *tb, long start, PyObject *v0, PyObject *v1,
                PyObject *v2, PyObject *v3, PyObject *v4)
{
    PyObject *values[5] = {v0, v1, v2, v3, v4};
    if (tb == NULL)
        tb = le_exc_traceback(PyThreadState_GET());
    if (tb != NULL && PyTraceBack_Check(tb))
        psyco_frame_write_locals(((PyTracebackObject *)tb)->tb_frame,
                                 (int)start, values, 5);
}

static PyObject *
cimpl_vt_traceback(PyCodeObject *code, PyObject *globals, int lasti,
                   int lineno, PyObject **word)
{
    /* PyTraceBack_Here() is the only API that makes a traceback object, so
       the entry is made on the current exception's and taken off it */
    PyObject *oldtb;
    PyObject *newtb;
    PyThreadState *tstate = PyThreadState_GET();
    PyFrameObject *f =
        cimpl_new_frame(tstate, code, globals, lasti, lineno, word);
    if (f == NULL)
        return NULL;
    if (LE_FAIL(LE_FAIL_TRACEBACK)) {
        Py_DECREF(f);
        return PyErr_NoMemory();
    }
    oldtb = le_exc_traceback(tstate); /* might be NULL */
    Py_XINCREF(oldtb);
    if (oldtb != NULL && PyTraceBack_Check(oldtb))
        psyco_frame_link_callee(((PyTracebackObject *)oldtb)->tb_frame, f);
    if (PyTraceBack_Here(f)) {
        Py_XDECREF(oldtb);
        Py_DECREF(f);
        return NULL;
    }
    /* the new traceback is now the current exception's */
    newtb = le_exc_traceback(tstate);    /* extracts the reference */
    le_exc_put_traceback(tstate, oldtb); /* consumes the reference */
    Py_DECREF(f);
    return newtb;
}

/* The pending exception, set aside while a traceback's locals are made
   real (traceback_locals): the code that makes a virtual local real may
   test for an error, and would take the pending one for its own. NULL if
   out of memory: the exception then stays pending. */
static PyObject **
cimpl_exc_hold(void)
{
    PyObject **held = PyMem_Malloc(3 * sizeof(PyObject *));
    if (held != NULL)
        PyErr_Fetch(&held[0], &held[1], &held[2]);
    return held;
}

static void
cimpl_exc_restore(PyObject **held)
{
    if (held != NULL) {
        PyErr_Restore(held[0], held[1], held[2]);
        PyMem_Free(held);
    }
}

/* as cimpl_tb_locals, for the exception set aside by cimpl_exc_hold */
static void
cimpl_held_tb_locals(PyObject **held, long start, PyObject *v0, PyObject *v1,
                     PyObject *v2, PyObject *v3, PyObject *v4)
{
    cimpl_tb_locals(held != NULL ? held[2] : NULL, start, v0, v1, v2, v3, v4);
}

/* the frame's locals, five to a call of fn(target, start, ...), 'target'
   passed by 'tag'. An unbound one is cleared there, so a del after the
   raise, seen at a re-raise from END_FINALLY, releases what the frame
   held; a cell or free variable's slot is left as it is. False if a
   call couldn't be compiled. */
static bool
locals_calls(PsycoObject *po, void *fn, char tag, long target)
{
    PyCodeObject *co = po->pr.co;
    int n = le_code_nlocalsplus(co);
    int i, j;
    for (i = 0; i < n; i += 5) {
        char tags[8] = "?lvvvvv";
        long args[5];
        tags[0] = tag;
        for (j = 0; j < 5; j++) {
            vinfo_t *v = i + j < n ? LOC_LOCALS_PLUS[i + j] : NULL;
            if (v == NULL || v->source == SOURCE_NOT_IMPORTANT) {
                tags[2 + j] = 'l';
                args[j] = v == NULL && i + j < le_code_nlocals(co)
                              ? 0
                              : (long)LE_LOCAL_UNKNOWN;
            }
            else
                args[j] = (long)v;
        }
        if (psyco_generic_call(po, fn, CfNoReturnValue, tags, target, (long)i,
                               args[0], args[1], args[2], args[3],
                               args[4]) == NULL)
            return false;
    }
    return true;
}

/* The frame's locals go into its traceback entry's frame, as a frame
   holds them: they show in tb_frame.f_locals and live as long as the
   traceback. 'tb' is the entry, or NULL for the raised exception's, which
   is still pending, and is set aside meanwhile if a local is virtual: the
   code that makes it real may test for an error. */
static bool
has_virtual_local(PsycoObject *po)
{
    PyCodeObject *co = po->pr.co;
    int i, n = le_code_nlocalsplus(co);
    for (i = 0; i < n; i++) {
        vinfo_t *v = LOC_LOCALS_PLUS[i];
        if (v != NULL && v->source != SOURCE_NOT_IMPORTANT &&
            is_virtualtime(v->source))
            return true;
    }
    return false;
}

static void
traceback_locals(PsycoObject *po, vinfo_t *tb)
{
    vinfo_t *held;
    if (tb == NULL && has_virtual_local(po)) {
        held = psyco_generic_call(po, cimpl_exc_hold,
                                  CfReturnTypePtr | CfNoPython, "");
        if (held == NULL)
            return;
        if (locals_calls(po, (void *)cimpl_held_tb_locals, 'v', (long)held))
            psyco_generic_call(po, cimpl_exc_restore,
                               CfNoReturnValue | CfNoPython, "v", held);
        vinfo_decref(held, po);
    }
    else if (tb == NULL) {
        /* NULL: cimpl_tb_locals takes the pending one's */
        locals_calls(po, (void *)cimpl_tb_locals, 'l', 0L);
    }
    else {
        locals_calls(po, (void *)cimpl_tb_locals, 'v', (long)tb);
    }
}

/* A frame with handlers keeps a catcher word in its stack, the first
   word its header reserves (psyco_emit_header), NULL at entry. When the
   frame catches an exception, the word takes a reference to the frame of
   the entry made for it (frames.c:psyco_frame_catch): a frame reads its
   locals from while the call runs, which stands for the call in its later
   entries too (cimpl_new_frame). So the locals are written when Python
   reads them, as they are then, not as the exception passes. Where reads
   can't find them, they are written before the call goes into an inlined
   frame (psyco_catcher_flush), and once more as it ends, while something
   besides the word holds the frame (catcher_flush). NULL in an inlined
   frame, which has no handlers. */
static vinfo_t *
catcher_word(PsycoObject *po)
{
    if (po->pr.is_inlining || LOC_CONTINUATION->array->count == 0)
        return NULL;
    return LOC_CONTINUATION->array->items[0];
}

/* A frame with handlers may hold the frame of an exception it caught,
   whose locals Python reads from this frame's maps while it runs
   (frames.c:frame_sync_locals). A virtual value no map can give (a list,
   which is mutable) is made real as it becomes a local, so that a read
   finds it, as CPython's frame holds the object. */
static bool
catcher_local_readable(PsycoObject *po, vinfo_t *v)
{
    if (catcher_word(po) == NULL || psyco_locals_map_reads(v, 0))
        return true;
    return compute_vinfo(v, po);
}

/* the catching frame's own slot, as a stack position for 'r' */
static vinfo_t *
own_slot(void)
{
    return vinfo_new(RunTime_NewStack(INITIAL_STACK_DEPTH, false, false));
}

/* an exception raised now is caught in this frame, which has a word */
static bool
caught_here(PsycoObject *po)
{
    int i;
    if (catcher_word(po) == NULL)
        return false;
    for (i = po->pr.iblock; i-- > 0;) {
        int type = po->pr.blockstack[i].b_type;
        if (type == SETUP_LOOP || type == EXCEPT_HANDLER)
            continue; /* unwound on the way to the next block */
        return type == SETUP_EXCEPT || type == SETUP_FINALLY;
    }
    return false;
}

/* five of the frame's locals, from 'start', into the frame its catcher
   word holds, if any */
static void
cimpl_catcher_locals(PyObject **word, long start, PyObject *v0, PyObject *v1,
                     PyObject *v2, PyObject *v3, PyObject *v4)
{
    PyObject *values[5] = {v0, v1, v2, v3, v4};
    if (*word != NULL)
        psyco_frame_write_locals((PyFrameObject *)*word, (int)start, values,
                                 5);
}

/* If the catcher word holds a frame something else holds too, it gets
   the locals now, and with 'end' is let go after; one nothing else holds
   is let go. The tests are compiled out of line when first taken. */
static bool
catcher_flush(PsycoObject *po, bool end)
{
    vinfo_t *word = catcher_word(po), *now, *shared, *held = NULL;
    int on;
    if (word == NULL)
        return true;
    /* a word of its own: the word's value changes under calls */
    now = vinfo_new(RunTime_NewStack(getstack(word->source), false, false));
    on = runtime_NON_NULL_f(po, now);
    if (on <= 0)
        return on == 0;
    shared = psyco_generic_call(po, psyco_frame_catcher_shared,
                                CfReturnTypeLong, "r", word);
    if (shared == NULL)
        return false;
    on = runtime_NON_NULL_t(po, shared);
    if (on <= 0)
        return on == 0;
    if (PycException_Occurred(po) && PycException_Is(po, &ERtPython) &&
        has_virtual_local(po)) {
        /* the code that makes a local real may test for an error */
        held = psyco_generic_call(po, cimpl_exc_hold,
                                  CfReturnTypePtr | CfNoPython, "");
        if (held == NULL)
            return false;
    }
    on = locals_calls(po, (void *)cimpl_catcher_locals, 'r', (long)word);
    if (held != NULL) {
        if (on)
            psyco_generic_call(po, cimpl_exc_restore,
                               CfNoReturnValue | CfNoPython, "v", held);
        vinfo_decref(held, po);
    }
    if (on && end)
        on = psyco_generic_call(po, psyco_frame_catcher_end, CfNoReturnValue,
                                "r", word) != NULL;
    return on;
}

DEFINEFN
bool
psyco_catcher_flush(PsycoObject *po)
{
    return catcher_flush(po, false);
}

/* Whether the handler at 'handler' drops the exception unseen
   ('except E: pass' or 'except: pass', with E a name): then nothing can
   reach the traceback's frame. If E doesn't match, END_FINALLY (RERAISE
   from 3.9) re-raises it and puts the locals in then. The test of E is
   COMPARE_OP's exception match and a jump, or from 3.9 one
   JUMP_IF_NOT_EXC_MATCH. */
static bool
handler_drops_exception(PyCodeObject *co, int handler)
{
    le_code_t c = le_code(co);
    int p = handler, q, i;
#define OP_AT(pos) ((pos) < c.size ? le_op(c, (pos)) : -1)
    if (OP_AT(p) == DUP_TOP) {
        q = le_after(c, p);
        i = OP_AT(q);
        if (i == LOAD_GLOBAL || i == LOAD_NAME || i == LOAD_FAST ||
            i == LOAD_DEREF) {
            q = le_after(c, q);
            if (OP_AT(q) == COMPARE_OP && le_arg(c, q) == PyCmp_EXC_MATCH &&
                OP_AT(le_after(c, q)) == POP_JUMP_IF_FALSE)
                p = le_after(c, le_after(c, q));
            else if (OP_AT(q) == JUMP_IF_NOT_EXC_MATCH)
                p = le_after(c, q);
        }
    }
    for (i = 0; i < 3; i++, p = le_after(c, p))
        if (OP_AT(p) != POP_TOP)
            return false;
    return OP_AT(p) == POP_EXCEPT;
#undef OP_AT
}

/* Whether an exception raised now puts the frame's locals in its entry's
   frame now: not if this frame catches it (catcher_word), nor for an
   inlined frame whose caller drops it unseen. A frame it leaves has them
   as they are when it leaves. */
static bool
locals_wanted_now(PsycoObject *po)
{
    int i;
    PyCodeObject *co;
    if (!po->pr.is_inlining)
        return !caught_here(po);
    /* an inlined frame's blocks in 'pr' are its caller's, with their
       handlers in the caller's code */
    co = (PyCodeObject *)CompileTime_Get(
             LOC_INLINING->array->items[LOC_INLINE_CO]->source)
             ->value;
    for (i = po->pr.iblock; i-- > 0;) {
        le_block_t *b = &po->pr.blockstack[i];
        if (b->b_type == SETUP_LOOP)
            continue;
        return !((b->b_type == SETUP_EXCEPT || b->b_type == SETUP_FINALLY) &&
                 handler_drops_exception(co, b->b_handler));
    }
    return true;
}

/* ceval's _PyErr_Restore of three stack items: they become the
   exception, raised again with no traceback entry (exception_unwind), its
   locals put in the traceback's frame as END_FINALLY's are */
DEFINEFN
void
PycException_Reraise(PsycoObject *po, vinfo_t *exc, vinfo_t *val, vinfo_t *tb)
{
    extra_assert(!PycException_Occurred(po));
    vinfo_incref(exc);
    vinfo_incref(val);
    vinfo_incref(tb);
    po->pr.exc = exc;
    po->pr.val = val;
    po->pr.tb = tb;
    if (locals_wanted_now(po))
        traceback_locals(po, tb);
}

/* record a traceback entry for the current virtual Python exception */
PSY_INLINE void
PsycoTraceBack_Here(PsycoObject *po, int lasti)
{
    int lineno = PyCode_Addr2Line(po->pr.co, lasti);
    vinfo_t *word = catcher_word(po);
    const char *tags = word != NULL ? "lvllr" : "lvlll";

    if (PycException_Is(po, &ERtPython)) {
        /* Python exception is actually set at run-time */
        if (psyco_generic_call(po, cimpl_rt_traceback, CfNoReturnValue, tags,
                               (long)po->pr.co, LOC_GLOBALS, (long)lasti,
                               (long)lineno, (long)word) != NULL &&
            locals_wanted_now(po))
            traceback_locals(po, NULL);
    }
    else {
        /* We only have a virtual-time exception (not set in Python),
           so we build po->pr.tb without actually setting it either. Out of
           memory, the MemoryError replaces it. */
        vinfo_t *tb =
            psyco_generic_call(po, cimpl_vt_traceback, CfCommonNewRefPyObject,
                               tags, (long)po->pr.co, LOC_GLOBALS, (long)lasti,
                               (long)lineno, (long)word);
        vinfo_xdecref(po->pr.tb, po);
        po->pr.tb = tb;
        if (tb != NULL && locals_wanted_now(po))
            traceback_locals(po, tb);
    }
}

/***************************************************************/
/***                      Initialization                       ***/
/***************************************************************/

DEFINEVAR source_known_t psyco_skZero;     /* known value 0 */
DEFINEVAR source_known_t psyco_skOne;      /* known value 1 */
DEFINEVAR source_known_t psyco_skNone;     /* known value 'Py_None' */
DEFINEVAR source_known_t psyco_skPy_False; /* known value 'Py_False' */
DEFINEVAR source_known_t psyco_skPy_True;  /* known value 'Py_True' */
DEFINEVAR source_known_t psyco_skNotImplemented;

static PyObject *s_builtin_object; /* intern string '__builtins__' */
struct varying_global_s;
static PyObject *
cimpl_load_global_miss(struct varying_global_s *vg);
static code_t *load_global_miss_glue;

INITIALIZATIONFN
void
psyco_pycompiler_init(void)
{
    s_builtin_object = PyUnicode_InternFromString("__builtins__");
    load_global_miss_glue = psyco_aligned_call_glue(cimpl_load_global_miss);

    psyco_skZero.refcount1_flags = SkFlagFixed;
    psyco_skZero.value = (long)0;
    psyco_skOne.refcount1_flags = SkFlagFixed;
    psyco_skOne.value = (long)1;
    psyco_skNone.refcount1_flags = SkFlagFixed;
    psyco_skNone.value = (long)Py_None;
    psyco_skPy_False.refcount1_flags = SkFlagFixed;
    psyco_skPy_False.value = (long)Py_False;
    psyco_skPy_True.refcount1_flags = SkFlagFixed;
    psyco_skPy_True.value = (long)Py_True;
    psyco_skNotImplemented.refcount1_flags = SkFlagFixed;
    psyco_skNotImplemented.value = (long)Py_NotImplemented;

    ERtPython = psyco_vsource_not_important;
    EReturn = psyco_vsource_not_important;
    EContinue = psyco_vsource_not_important;
    EBreak = psyco_vsource_not_important;
    EInline = psyco_vsource_not_important;
}

/***************************************************************/
/***                          Compiler                         ***/
/***************************************************************/

#define CHKSTACK(n)                                             \
    extra_assert(0 <= po->pr.stack_level + (n) &&               \
                 po->pr.stack_base + po->pr.stack_level + (n) < \
                     po->vlocals.count)

#define PUSH(v) (CHKSTACK(0), stack_a[po->pr.stack_level++] = v)
#define POP(targ)                             \
    do {                                      \
        CHKSTACK(-1);                         \
        targ = stack_a[--po->pr.stack_level]; \
        stack_a[po->pr.stack_level] = NULL;   \
    } while (0)
#define NTOP(n) (CHKSTACK(-(n)), stack_a[po->pr.stack_level - (n)])
#define TOP() NTOP(1)
#define POP_DECREF()          \
    do {                      \
        vinfo_t *v1;          \
        POP(v1);              \
        vinfo_decref(v1, po); \
    } while (0)

/*#define GETCODEOBJ()    ((PyCodeObject*)(KNOWN_SOURCE(LOC_CODE)->value))*/
#define GETCONST(i) (PyTuple_GET_ITEM(co->co_consts, i))
#define GETNAMEV(i) (PyTuple_GET_ITEM(co->co_names, i))

#define GETLOCAL(i) (LOC_LOCALS_PLUS[i])
#define SETLOCAL(i, v) psyco_rebind_local(po, &GETLOCAL(i), (v))

#define STACK_POINTER() (stack_a + po->pr.stack_level)
#define INSTR_OFFSET() (next_instr)
#define STACK_LEVEL() (po->pr.stack_level)
#define JUMPBY(offset) (next_instr += (offset))
#define JUMPTO(target) (next_instr = (target))
/* the target of the jump instruction being compiled, in bytes: its
   argument counts from the start or from the next instruction, forwards or
   backwards, as LE_IS_JABS and LE_IS_JBACK say */
#define JUMP_DEST()                                                     \
    (LE_IS_JABS(opcode) ? LE_JUMP_BYTES(oparg)                          \
                        : next_instr + (LE_IS_JBACK(opcode) ? -1 : 1) * \
                                           LE_JUMP_BYTES(oparg))

/* CALL_FINALLY's return offset: an int object, one per offset, so that two
   states pushing it compare equal (a fixed constant must match exactly) */
static vinfo_t *
finally_return(int offset)
{
    static PyObject *ints = NULL;
    PyObject *o, *x;
    if (ints == NULL && (ints = PyDict_New()) == NULL)
        OUT_OF_MEMORY();
    x = PyLong_FromLong(offset);
    if (x == NULL || (o = PyDict_SetDefault(ints, x, x)) == NULL)
        OUT_OF_MEMORY();
    Py_INCREF(o);
    Py_DECREF(x);
    return vinfo_new(
        CompileTime_NewSk(sk_new((long)o, SkFlagFixed | SkFlagPyObj)));
}

static bool
is_finally_return(vinfo_t *v)
{
    return is_compiletime(v->source) &&
           (CompileTime_Get(v->source)->refcount1_flags & SkFlagPyObj) &&
           PyLong_CheckExact((PyObject *)CompileTime_Get(v->source)->value);
}

/* ceval's UNWIND_EXCEPT_HANDLER: drop what the handler left above its six
   items, then restore the saved exc_info from the bottom three */
static void
unwind_except_handler(PsycoObject *po, le_block_t *b, bool unwind)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *type, *value, *tb, *word = catcher_word(po);
    extra_assert(STACK_LEVEL() >= b->b_level + 3);
    while (unwind && STACK_LEVEL() > b->b_level + 3) POP_DECREF();
    POP(type);
    POP(value);
    POP(tb);
    psyco_generic_call(po, cimpl_pop_exc_info, CfNoReturnValue,
                       word != NULL ? "vvvr" : "vvvl", type, value, tb,
                       (long)word);
    vinfo_decref(tb, po);
    vinfo_decref(value, po);
    vinfo_decref(type, po);
}

/* the innermost block, which must be an except handler, popped and
   unwound; false with ceval's SystemError if it is another */
static bool
pop_except_handler(PsycoObject *po, bool unwind)
{
    le_block_t *b = block_pop(po);
    if (b->b_type != EXCEPT_HANDLER) {
        PycException_SetString(po, PyExc_SystemError,
                               "popped block is not an except handler");
        return false;
    }
    unwind_except_handler(po, b, unwind);
    return true;
}

#define SAVE_NEXT_INSTR(nextinstr1) (po->pr.next_instr = (nextinstr1))

/* Jumps, for the cases translated from ceval.c. A case that
   jumped ends with JUMPED(): the next merge point is the target's.
   JUMP_IF_CC(cc, JUMP, npop, forget) jumps where cc holds. A run-time cc
   is a branch to the target's merge point, compiled later from there
   (psyco_compile_cond); the top npop (0 or 1) values are popped on that
   path at the target (NEXT_INSTR_POP), because they must still be on the
   stack when the branch saves its state. forget is the policy's
   ('forget', 'before_branch'). A cc known now jumps or falls through
   without code. Where cc doesn't hold, the case goes on. */
#define JUMPED()                                                      \
    do {                                                              \
        mp = psyco_next_merge_point(po->pr.merge_points, next_instr); \
        goto fine;                                                    \
    } while (0)
#define JUMP_IF_CC(cc, JUMP, npop, forget)                                    \
    do {                                                                      \
        extra_assert((npop) == 0 || (npop) == 1);                             \
        if ((int)(cc) < CC_TOTAL) {                                           \
            int current_instr = next_instr;                                   \
            JUMP;                                                             \
            if (npop)                                                         \
                next_instr |= NEXT_INSTR_POP;                                 \
            SAVE_NEXT_INSTR(next_instr);                                      \
            if (forget)                                                       \
                Psyco_ForgetClasses(po);                                      \
            psyco_compile_cond(                                               \
                po, psyco_exact_merge_point(po->pr.merge_points, next_instr), \
                (cc));                                                        \
            next_instr = current_instr;                                       \
        }                                                                     \
        else if ((cc) == CC_ALWAYS_TRUE) {                                    \
            JUMP;                                                             \
            if (npop)                                                         \
                POP_DECREF();                                                 \
            JUMPED();                                                         \
        }                                                                     \
    } while (0)

/***************************************************************/
/***   LOAD_GLOBAL tricks                                    ***/

/* copy-on-write: snapshots and forked branches share the dict */
static void
mark_varying(PsycoObject *po, PyObject *key)
{
    PyObject *old = po->pr.changing_globals;
    PyObject *d;
    if (old != NULL && PyDict_GetItem(old, key) != NULL)
        return;
    d = old == NULL ? PyDict_New() : PyDict_Copy(old);
    if (d == NULL || PyDict_SetItem(d, key, Py_True))
        OUT_OF_MEMORY();
    Py_XDECREF(old);
    po->pr.changing_globals = d;
}

DEFINEFN
PyObject *
psy_get_builtins(PyObject *globals)
{
    static PyObject *minimal_builtins = NULL;
    PyObject *builtins;
    /* code copied from frameobject.c */
    builtins = PyDict_GetItem((PyObject *)globals, s_builtin_object);
    if (builtins) {
        if (PyDict_Check(builtins))
            goto done;
        if (PyModule_Check(builtins)) {
            builtins = PyModule_GetDict(builtins);
            if (builtins) {
                psyco_assert(PyDict_Check(builtins));
                goto done;
            }
        }
    }
    /* No builtins!  Make up a minimal one
       Give them 'None', at least. */
    if (minimal_builtins == NULL) {
        minimal_builtins = PyDict_New();
        if (minimal_builtins == NULL ||
            PyDict_SetItemString(minimal_builtins, "None", Py_None) < 0)
            OUT_OF_MEMORY();
    }
    builtins = minimal_builtins;
done:
    return builtins;
}

/* Cached LOAD_GLOBAL. The name is looked up now, as
   cimpl_load_global does at run time, and the value becomes a
   compile-time constant behind a guard (x64/ipyencoding.c:
   psyco_global_guard). A name found in globals is guarded as Psyco did:
   the keys of globals still have the size, and hold the same name and
   value at the entry, written into the code, so writes to other names
   don't fail it. A name found in builtins relies on its absence from
   globals too: the guard walks the probe of globals as it was, slot by
   slot (psyco_global_absent), then compares the version tag of builtins
   (PEP 509: every change to a dict gives it a new tag, unique in the
   process) with one written into the code. Where the probe can't be
   shown so (a long one, a hash too like the name's), and for a name not
   found, which is loaded at run time behind the guard, it is the version
   tags of both dicts, kept in a per-site record.
   A failed guard jumps to do_changed_global, which redoes the lookup: the
   same object re-arms the guard (the entry moved, another name of the
   module was written, or the probe an absent guard walks changed, which
   is shown again for the keys now: psyco_global_absent_rearm) and
   resumes; anything else respawns into a version
   where the name is varying, read from its entry with C only on a miss
   (load_global_varying), and the guard's jump goes straight there. An
   entry guard also fails when only its entry moved (the keys resized), so
   its varying version starts with a check of the name's entry, where the
   guard was moved to at the respawn: a mismatch calls do_stale_global,
   which moves guard, check and load to where the name is now, then resumes
   after the guard if the name holds its value again, as do_changed_global
   does, or goes on in the varying version. The code's owner holds name
   and value, so the entry compared can't hold another object at the same
   address, and holds builtins, whose tag the guard reads; globals stays
   borrowed: a new dict at its address is looked up again. The records
   live as long as the owner. */
typedef union {
    global_patch_t entry;
    uint64_t tags[2]; /* of globals and builtins */
    global_absent_patch_t absent;
} global_guard_t;

/* all borrowed; the owner holds name, value and builtins */
typedef struct {
    PyObject *name;
    PyObject *value; /* the value compiled in, or NULL */
    PyObject *globals;
    PyObject *builtins; /* the frame's (LOC_BUILTINS), which a new
                           globals['__builtins__'] doesn't change */
    global_guard_t *guard;
    code_t *resume; /* after the guard's jump */
    char form;
} changed_global_t;
enum { GLOBAL_TAGS, GLOBAL_ENTRY, GLOBAL_ABSENT }; /* the guard's forms */

/* Where the entry holding name and value is in the keys of a dict:
   false if there is none (a key equal to the name but another object,
   values kept apart from the keys, or keys too large for the code's
   32-bit fields). */
static bool
global_entry_find(PyObject *globals, PyObject *name, PyObject *value,
                  long *size, long *offset)
{
    PyDictObject *mp = (PyDictObject *)globals;
    le_dictkeys_t *keys = (le_dictkeys_t *)mp->ma_keys;
    le_dictentry_t *ep = le_dict_entries(keys);
    Py_ssize_t i;
    if (mp->ma_values != NULL)
        return false;
    for (i = 0; i < keys->dk_nentries; i++)
        if (ep[i].me_key == name && ep[i].me_value == value) {
            *size = keys->dk_size;
            *offset = (char *)&ep[i] - (char *)keys;
            return *offset + sizeof(le_dictentry_t) <= INT32_MAX;
        }
    return false;
}

static void
global_tags_set(global_guard_t *guard, PyObject *globals, PyObject *builtins)
{
    guard->tags[0] = ((PyDictObject *)globals)->ma_version_tag;
    guard->tags[1] = ((PyDictObject *)builtins)->ma_version_tag;
}

/* the entry guard being respawned, whose version load_global_cached
   compiles */
static changed_global_t *respawning_global;

#if ALL_CHECKS
DEFINEVAR long psyco_stat_global_fails = 0;
#endif

static code_t *
do_changed_global(changed_global_t *cg)
{
    long size, offset;
    code_t *code;
    changed_global_t *prev = respawning_global;
    PyObject *x = PyDict_GetItem(cg->globals, cg->name);
    LE_STAT(psyco_stat_global_fails++);
    if (x == NULL && cg->form != GLOBAL_ENTRY)
        x = PyDict_GetItem(cg->builtins, cg->name);
    if (x != cg->value) {
        /* the respawn compiles a version that never comes back here */
        if (cg->form == GLOBAL_ENTRY && x != NULL &&
            global_entry_find(cg->globals, cg->name, x, &size, &offset))
            psyco_global_guard_move(&cg->guard->entry, size, offset);
        respawning_global = cg->form == GLOBAL_ENTRY ? cg : NULL;
        code = psyco_do_respawn(cg, sizeof(*cg));
        respawning_global = prev;
        return code;
    }
    if (cg->form == GLOBAL_TAGS)
        global_tags_set(cg->guard, cg->globals, cg->builtins);
    else if (cg->form == GLOBAL_ABSENT)
        psyco_global_absent_rearm(&cg->guard->absent, cg->globals,
                                  cg->builtins, cg->name);
    else if (global_entry_find(cg->globals, cg->name, x, &size, &offset))
        psyco_global_guard_move(&cg->guard->entry, size, offset);
    else
        return psyco_do_respawn(cg, sizeof(*cg));
    return psyco_dont_respawn(cg, sizeof(*cg));
}

/* A varying global's load (psyco_global_load) and its miss, which moves
   the code's entry to where globals now hold the name. All borrowed; the
   owner holds name and builtins. */
typedef struct varying_global_s {
    global_patch_t entry;
    PyObject *globals, *builtins, *name;
} varying_global_t;

static PyObject *
cimpl_load_global(PyObject *globals, PyObject *builtins, PyObject *w);

static PyObject *
cimpl_load_global_miss(varying_global_t *vg)
{
    long size, offset;
    PyObject *x = cimpl_load_global(vg->globals, vg->builtins, vg->name);
    if (x != NULL &&
        global_entry_find(vg->globals, vg->name, x, &size, &offset))
        psyco_global_guard_move(&vg->entry, size, offset);
    return x;
}

/* Emitted whether or not globals hold the name now (a size of -1 never
   matches), so that a replay changes the compiler's state the same way. */
static bool
load_global_varying(PsycoObject *po, PyObject *globals, PyObject *builtins,
                    PyObject *name, varying_global_t **vgp, vinfo_t **result)
{
    static varying_global_t respawn_vg; /* written for trashed code only */
    varying_global_t *vg;
    PyObject *x = PyDict_GetItem(globals, name);
    long size = -1, offset = 0;
    vinfo_t *v;
    if (x != NULL && !global_entry_find(globals, name, x, &size, &offset))
        size = -1;
    if (is_respawning(po))
        vg = &respawn_vg;
    else {
        vg = psyco_owner_alloc(sizeof(varying_global_t));
        vg->globals = globals;
        vg->builtins = builtins;
        psyco_owner_pin(builtins);
        vg->name = name;
        psyco_owner_pin(name);
        if (vgp != NULL)
            *vgp = vg;
    }
    v = psyco_global_load(po, globals, name, &vg->entry, size, offset,
                          load_global_miss_glue, vg);
    if (runtime_condition_f(po, CC_E)) {
        vinfo_decref(v, po);
        PycException_Raise(po, vinfo_new(VirtualTime_New(&ERtPython)), NULL);
        return false;
    }
    v->source = add_rtref(v->source);
    *result = v;
    return true;
}

/* The check at the start of an entry guard's varying version
   (psyco_global_stale) and its mismatch. All borrowed: the owner holds
   name, value and the guard. */
typedef struct {
    global_patch_t check;
    global_patch_t *guard;
    varying_global_t *vg;
    PyObject *globals, *name, *value;
    code_t *resume;  /* after the guard's jump */
    code_t *varying; /* after the check */
} stale_global_t;

static code_t *
do_stale_global(stale_global_t *sg)
{
    long size, offset;
    PyObject *x = PyDict_GetItem(sg->globals, sg->name);
    if (x == NULL ||
        !global_entry_find(sg->globals, sg->name, x, &size, &offset))
        return sg->varying; /* its load calls C */
    psyco_global_guard_move(sg->guard, size, offset);
    psyco_global_guard_move(&sg->check, size, offset);
    psyco_global_guard_move(&sg->vg->entry, size, offset);
    return x == sg->value ? sg->resume : sg->varying;
}

/* the varying version of cg's guard, just respawned: the code is entered
   from the guard's jump, with the state the guard left */
static bool
load_global_rearmed(PsycoObject *po, changed_global_t *cg, vinfo_t **result)
{
    long size = -1, offset = 0;
    code_t *stub;
    stale_global_t *sg;
    PyObject *x = PyDict_GetItem(cg->globals, cg->name);
    if (x != NULL &&
        !global_entry_find(cg->globals, cg->name, x, &size, &offset))
        size = -1;
    sg = psyco_new_stub(&do_stale_global, sizeof(stale_global_t), &stub);
    sg->guard = &cg->guard->entry;
    sg->globals = cg->globals;
    sg->name = cg->name;
    sg->value = cg->value;
    sg->resume = cg->resume;
    psyco_global_stale(po, cg->globals, cg->name, &sg->check, size, offset,
                       stub);
    sg->varying = po->code;
    return load_global_varying(po, cg->globals, cg->builtins, cg->name,
                               &sg->vg, result);
}

/* Returns true with *result a new vinfo for the global, or with *result
   NULL if it isn't cached here (the caller loads it at run time); false
   on error. */
static bool
load_global_cached(PsycoObject *po, PyObject *name, vinfo_t **result)
{
    static global_guard_t respawn_guard; /* written for trashed code only */
    PyObject *globals, *builtins, *value;
    global_guard_t *guard, tags;
    condition_code_t cc;
    changed_global_t *cg;
    global_absent_t absent;
    long size = 0, offset = 0;
    char form = GLOBAL_TAGS;

    *result = NULL;
    if (!is_compiletime(LOC_GLOBALS->source) ||
        !is_compiletime(LOC_BUILTINS->source))
        return true;
    globals = (PyObject *)CompileTime_Get(LOC_GLOBALS->source)->value;
    builtins = (PyObject *)CompileTime_Get(LOC_BUILTINS->source)->value;
    if (!PyDict_CheckExact(globals) || !PyDict_CheckExact(builtins))
        return true;
    if (po->pr.changing_globals != NULL &&
        PyDict_GetItem(po->pr.changing_globals, name) != NULL)
        return load_global_varying(po, globals, builtins, name, NULL, result);

    value = PyDict_GetItem(globals, name);
    if (value != NULL) {
        if (global_entry_find(globals, name, value, &size, &offset))
            form = GLOBAL_ENTRY;
    }
    else {
        value = PyDict_GetItem(builtins, name);
        if (value != NULL && psyco_global_absent(globals, name, &absent))
            form = GLOBAL_ABSENT;
    }

    /* A respawn replays compilation from the block's snapshot with the
       dicts as they are then, so a cached site must not be re-decided:
       - both forms of the guard change the compiler's state in the same
         way, and a replay's guard is trashed, so the replay may take
         either;
       - the block ends right after the site (split_after_global), so no
         later respawn point replays through it: later blocks start from a
         snapshot holding the constant. */
    if (is_respawning(po))
        guard = &respawn_guard;
    else {
        guard = psyco_owner_alloc(sizeof(global_guard_t));
    }
    global_tags_set(&tags, globals, builtins);
    if (form == GLOBAL_TAGS)
        *guard = tags;
    cc = psyco_global_guard(po, globals, builtins, name, value,
                            form == GLOBAL_ENTRY ? &guard->entry : NULL, size,
                            offset, form == GLOBAL_ABSENT ? &absent : NULL,
                            &guard->absent,
                            form == GLOBAL_TAGS ? guard->tags : tags.tags);
    if (cc == CC_ERROR)
        return false;

    if (detect_respawn(po)) {
        /* the guard failed and the value really changed: from here on this
           name is loaded at run time */
        changed_global_t *rg = respawning_global;
        mark_varying(po, name);
        if (rg != NULL && !is_respawning(po) && rg->name == name &&
            rg->globals == globals && rg->builtins == builtins)
            return load_global_rearmed(po, rg, result);
        return load_global_varying(po, globals, builtins, name, NULL, result);
    }
    cg = (changed_global_t *)psyco_prepare_respawn_ex(
        po, cc, &do_changed_global, sizeof(changed_global_t));
    po->pr.split_after_global = 1;
    if (cg != NULL) {
        cg->resume = po->code;
        cg->name = name;
        psyco_owner_pin(name);
        cg->value = value;
        psyco_owner_pin(value);
        cg->globals = globals;
        cg->builtins = builtins;
        psyco_owner_pin(builtins);
        cg->guard = guard;
        cg->form = form;
    }
    /* not found: a run-time load raises NameError; the guard catches a
       later definition */
    if (value != NULL) {
        Py_INCREF(value);
        *result =
            vinfo_new(CompileTime_NewSk(sk_new((long)value, SkFlagPyObj)));
    }
    return true;
}

/* CALL_FUNCTION (oparg positional arguments) and CALL_FUNCTION_KW (oparg
   arguments, the last ones named by the constant tuple on top), as 3.7's
   call_function(). The stack, top down: [names tuple], arguments, callable.
   *stack_to_pop counts everything including the callable. Keyword
   arguments go in a fresh dict: duplicates are impossible here (the
   bytecode compiler rejects them) and the callee checks the rest. */
static void
end_call_site(PsycoObject *po)
{
    po->pr.call_site = 0;
}

static vinfo_t *
psyco_ext_do_calls(PsycoObject *po, int opcode, int oparg, vinfo_t **stack_top,
                   int *stack_to_pop)
{
    int na = oparg, nk = 0, i;
    PyObject *kwnames = NULL;
    vinfo_t **args;
    vinfo_t *vargs = NULL;
    vinfo_t *wdict = NULL;
    vinfo_t *result = NULL;

    if (opcode == CALL_FUNCTION_KW) {
        vinfo_t *names = stack_top[-1];
        if (!is_compiletime(names->source) ||
            !PyTuple_CheckExact(
                kwnames = (PyObject *)CompileTime_Get(names->source)->value) ||
            PyTuple_GET_SIZE(kwnames) > oparg) {
            PycException_SetString(po, PyExc_SystemError,
                                   "bad CALL_FUNCTION_KW keyword names");
            return NULL;
        }
        nk = PyTuple_GET_SIZE(kwnames);
        na = oparg - nk;
        stack_top--;
    }
    args = stack_top - oparg;
    *stack_to_pop = oparg + 1 + (opcode == CALL_FUNCTION_KW);

    if (nk == 0)
        wdict = psyco_vi_Zero(); /* no keyword arguments */
    else {
        wdict = _PsycoDict_NewPresized(po, nk);
        if (wdict == NULL)
            goto fail;
        for (i = 0; i < nk; i++)
            if (!PsycoDict_SetItemC(po, wdict, PyTuple_GET_ITEM(kwnames, i),
                                    args[na + i]))
                goto fail;
    }
    vargs = PsycoTuple_New(na, args); /* virtual tuple */
    result = PsycoObject_Call(po, args[-1], vargs, wdict);
fail:
    vinfo_xdecref(vargs, po);
    vinfo_xdecref(wdict, po);
    return result;
}

/***************************************************************/
/***   Run-time implementation of various opcodes            ***/

/* the code of the following functions is "copy-pasted" from ceval.c */

/* a frame's own cells, which CPython made before the frame reached LeJIT
   (PsycoObject_FromCode maps them as borrowed words); a cell can change
   under any call (nonlocal in a callee), so its content is read each time.
   The translated *_DEREF cases read and store it through this. */
#define CELL_ob_ref FMUT(CPY_FIELD(PyCellObject, ob_ref, OB_type))
#define OB_refcnt FMUT(CPY_FIELD(PyObject, ob_refcnt, NO_PREV_FIELD))

static PyObject *
cimpl_unicode_append(PyObject *v, PyObject *w)
{
    PyUnicode_Append(&v, w);
    return v;
}

/* a run-time test that 'v' holds 'n' references */
static condition_code_t
refcnt_is(PsycoObject *po, vinfo_t *v, long n)
{
    vinfo_t *rc = psyco_get_field_offset(po, v, OB_refcnt, 0);
    condition_code_t cc;
    if (rc == NULL)
        return CC_ERROR;
    cc = integer_cmp_i(po, rc, n, Py_EQ);
    vinfo_decref(rc, po);
    return cc;
}

/* run-time words 'a' and 'b' are the same object */
static condition_code_t
same_object(PsycoObject *po, vinfo_t *a, vinfo_t *b)
{
    return integer_cmp(po, a, b, Py_EQ);
}

/* ceval.c:unicode_concatenate, for BINARY_ADD and INPLACE_ADD of two
   exact str followed by STORE_FAST or STORE_DEREF. CPython clears the
   variable when it holds the stack's object and the two are its only
   references, so that PyUnicode_Append, given the last reference, resizes
   the string in place. LeJIT's references are not CPython's: one vinfo_t
   shared by the variable and the stack owns one reference where CPython's
   frame holds two, and a borrowed word (an argument) or a constant is no
   reference at all. So the test is made only where LeJIT's owned references
   stand for exactly the frame's: the shared vinfo_t alone (CPython's 2 is
   1), or the stack's and the variable's each alone (2 is 2, and the two
   words must be equal). Elsewhere CPython's count is more than 2 or LeJIT
   cannot tell, and the translated case concatenates as PyUnicode_Append
   does then. On the in-place path the variable is unbound before the call,
   as CPython's is, so an error leaves it unbound. Returns 1 with the sum
   pushed, 0 for the translated case, -1 on an error. */
static int
unicode_concatenate(PsycoObject *po, int next_instr)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    PyCodeObject *co = po->pr.co;
    le_code_t c = le_code(co);
    /* the next code unit alone, as ceval's unicode_concatenate reads it */
    int opcode = le_op(c, next_instr), oparg = le_arg(c, next_instr);
    vinfo_t *left = NTOP(2), *right = NTOP(1), *var = NULL, *sum;
    condition_code_t cc;

    if (!has_rtref(left->source) || Psyco_KnownType(left) != &PyUnicode_Type ||
        Psyco_KnownType(right) != &PyUnicode_Type)
        return 0;
    if (opcode == STORE_FAST) {
        var = GETLOCAL(oparg);
        if (var == left) {
            if (left->refcount != 2)
                return 0;
            cc = refcnt_is(po, left, 1);
            if (cc == CC_ERROR)
                return -1;
            if (!runtime_condition_t(po, cc))
                return 0;
        }
        else {
            if (var == NULL || left->refcount != 1 || var->refcount != 1 ||
                !has_rtref(var->source))
                return 0;
            cc = refcnt_is(po, left, 2);
            if (cc == CC_ERROR)
                return -1;
            if (!runtime_condition_t(po, cc))
                return 0;
            cc = same_object(po, var, left);
            if (cc == CC_ERROR)
                return -1;
            if (!runtime_condition_t(po, cc))
                return 0;
        }
        SETLOCAL(oparg, psyco_vi_Zero());
    }
    else if (opcode == STORE_DEREF) {
        vinfo_t *cell = GETLOCAL(le_code_nlocals(co) + oparg), *zero;
        bool ok;
        if (left->refcount != 1)
            return 0;
        cc = refcnt_is(po, left, 2);
        if (cc == CC_ERROR)
            return -1;
        if (!runtime_condition_t(po, cc))
            return 0;
        var = psyco_get_field(po, cell, CELL_ob_ref);
        if (var == NULL)
            return -1;
        cc = same_object(po, var, left);
        vinfo_decref(var, po);
        if (cc == CC_ERROR)
            return -1;
        if (!runtime_condition_t(po, cc))
            return 0;
        zero = psyco_vi_Zero();
        ok = psyco_put_field(po, cell, CELL_ob_ref, zero);
        vinfo_decref(zero, po);
        if (!ok)
            return -1;
        psyco_decref_v(po, left); /* the cell's */
    }
    else
        return 0;
    /* the stack's reference is the call's */
    left->source = remove_rtref(left->source);
    sum = psyco_generic_call(po, cimpl_unicode_append,
                             CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                             "vv", left, right);
    if (sum == NULL)
        return -1;
    POP(right);
    POP(left);
    vinfo_decref(right, po);
    vinfo_decref(left, po);
    PUSH(sum);
    return 1;
}

/* whether the item on top of the stack at next_instr is a part of an
   f-string's BUILD_STRING with only loads and other FORMAT_VALUEs
   between, read from the bytecode */
static bool
feeds_build_string(PyCodeObject *co, int next_instr)
{
    le_code_t c = le_code(co);
    int depth = 1;
    for (; next_instr < c.size; next_instr = le_after(c, next_instr)) {
        int op = le_op(c, next_instr), arg = le_arg(c, next_instr);
        if (op == LOAD_CONST || op == LOAD_FAST || op == LOAD_DEREF)
            depth++;
        else if (op == FORMAT_VALUE)
            depth -= (arg & 0x4) != 0;
        else if (op == BUILD_STRING && arg >= depth)
            return true;
        else if (op == BUILD_STRING)
            depth -= arg - 1;
        else
            return false;
    }
    return false;
}

/* f-strings: FORMAT_VALUE of an exact int with no conversion and no spec,
   for a BUILD_STRING, is its str() not made yet, and a BUILD_STRING of
   such parts writes them into its one result (punicodeobject.c). Returns
   1 with the result on the stack, 0 for the translated case, -1 on an
   error. */
static int
fstring_part(PsycoObject *po, int opcode, int oparg, int next_instr)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *r, *v;
    int done;
    if (opcode == FORMAT_VALUE) {
        v = TOP();
        if (oparg != 0 || Psyco_KnownType(v) != &PyLong_Type ||
            !feeds_build_string(po->pr.co, next_instr))
            return 0;
        done = PsycoUnicode_DecimalPart(po, v, &r);
        if (done <= 0)
            return done;
        POP_DECREF();
        PUSH(r);
        return 1;
    }
    done = PsycoUnicode_BuildParts(po, STACK_POINTER() - oparg, oparg, &r);
    if (done <= 0)
        return done;
    while (oparg--) POP_DECREF();
    PUSH(r);
    return 1;
}

/* the run-time copies of ceval.c helpers' arms that translated cases call
   (cmp_outcome's PyCmp_EXC_MATCH), and of the whole cases that LeJIT's own
   cases leave to CPython (cimpl_call_function_ex) */
/* the translation keeps CPython's unused variables, labels and values */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-label"
#pragma GCC diagnostic ignored "-Wunused-value"
#if LE_HAS_CMP_OUTCOME
static PyObject *
cimpl_cmp_outcome(int op, PyObject *v, PyObject *w)
{
    int res = 0;
    switch (op) {
        case PyCmp_IS:
            res = (v == w);
            break;
        case PyCmp_IS_NOT:
            res = (v != w);
            break;
        case PyCmp_IN:
            res = PySequence_Contains(w, v);
            if (res < 0)
                return NULL;
            break;
        case PyCmp_NOT_IN:
            res = PySequence_Contains(w, v);
            if (res < 0)
                return NULL;
            res = !res;
            break;
        case PyCmp_EXC_MATCH:
            if (PyTuple_Check(w)) {
                Py_ssize_t i, length;
                length = PyTuple_Size(w);
                for (i = 0; i < length; i += 1) {
                    PyObject *exc = PyTuple_GET_ITEM(w, i);
                    if (!PyExceptionClass_Check(exc)) {
                        PyErr_SetString(PyExc_TypeError,
                                        LE_MSG_CATCHING_NON_EXCEPTION);
                        return NULL;
                    }
                }
            }
            else {
                if (!PyExceptionClass_Check(w)) {
                    PyErr_SetString(PyExc_TypeError,
                                    LE_MSG_CATCHING_NON_EXCEPTION);
                    return NULL;
                }
            }
            res = PyErr_GivenExceptionMatches(v, w);
            break;
        default:
            return PyObject_RichCompare(v, w, op);
    }
    v = res ? Py_True : Py_False;
    Py_INCREF(v);
    return v;
}
#endif

#if LE_HAS_CEVAL_TRACE_INFO
static void
cimpl_PyLineTable_InitAddressRange(const char *linetable, Py_ssize_t length,
                                   int firstlineno, PyCodeAddressRange *range)
{
    range->opaque.lo_next = linetable;
    range->opaque.limit = range->opaque.lo_next + length;
    range->ar_start = -1;
    range->ar_end = 0;
    range->opaque.computed_line = firstlineno;
    range->ar_line = -1;
}
#endif

#if LE_HAS_CEVAL_TRACE_INFO
static int
cimpl__PyCode_InitAddressRange(PyCodeObject *co, PyCodeAddressRange *bounds)
{
    const char *linetable = PyBytes_AS_STRING(co->co_linetable);
    Py_ssize_t length = PyBytes_GET_SIZE(co->co_linetable);
    cimpl_PyLineTable_InitAddressRange(linetable, length, co->co_firstlineno,
                                       bounds);
    return bounds->ar_line;
}
#endif

#if LE_HAS_FASTCALL_DICT_TSTATE
extern PyObject *
_PyObject_FastCallDictTstate(PyThreadState *tstate, PyObject *callable,
                             PyObject *const *args, size_t nargsf,
                             PyObject *kwargs);
#define le_fastcall_dict_tstate _PyObject_FastCallDictTstate
#else
#define le_fastcall_dict_tstate(tstate, callable, args, nargsf, kwargs) \
    _PyObject_FastCallDict(callable, args, nargsf, kwargs)
#endif

#if !LE_HAS_CFRAME
static int
cimpl_call_trace(Py_tracefunc func, PyObject *obj, PyFrameObject *frame,
                 int what, PyObject *arg)
{
    PyThreadState *tstate = PyThreadState_GET();

    int result;
    if (tstate->tracing)
        return 0;
    tstate->tracing++;
    tstate->use_tracing = 0;
    result = func(obj, frame, what, arg);
    tstate->use_tracing =
        ((tstate->c_tracefunc != NULL) || (tstate->c_profilefunc != NULL));
    tstate->tracing--;
    return result;
}
#endif

#if !LE_HAS_CFRAME
static int
cimpl_call_trace_protected(Py_tracefunc func, PyObject *obj,
                           PyFrameObject *frame, int what, PyObject *arg)
{
    PyObject *type, *value, *traceback;
    int err;
    PyErr_Fetch(&type, &value, &traceback);
    err = cimpl_call_trace(func, obj, frame, what, arg);
    if (err == 0) {
        PyErr_Restore(type, value, traceback);
        return 0;
    }
    else {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
        return -1;
    }
}
#endif

#if !LE_HAS_CFRAME
static PyObject *
cimpl_do_call_core(PyObject *func, PyObject *callargs, PyObject *kwdict)
#if LE_HAS_CALL_TRACE_METHOD_DESCR
{
    PyThreadState *tstate = PyThreadState_GET();

    PyObject *result;
#if LE_HAS_FASTCALL_DICT_TSTATE
    if (PyCFunction_CheckExact(func) || PyCMethod_CheckExact(func)) {
#else
    if (PyCFunction_Check(func)) {
#endif
        if (tstate->use_tracing && tstate->c_profilefunc) {
            if (cimpl_call_trace(tstate->c_profilefunc, tstate->c_profileobj,
                                 tstate->frame, PyTrace_C_CALL, func)) {
                result = NULL;
            }
            else {
#if LE_HAS_FASTCALL_DICT_TSTATE
                result = PyObject_Call(func, callargs, kwdict);
#else
                result = PyCFunction_Call(func, callargs, kwdict);
#endif
                if (tstate->c_profilefunc != NULL) {
                    if (result == NULL) {
                        cimpl_call_trace_protected(
                            tstate->c_profilefunc, tstate->c_profileobj,
                            tstate->frame, PyTrace_C_EXCEPTION, func);
                    }
                    else {
                        if (cimpl_call_trace(
                                tstate->c_profilefunc, tstate->c_profileobj,
                                tstate->frame, PyTrace_C_RETURN, func)) {
                            Py_DECREF(result);
                            result = NULL;
                        }
                    }
                }
            }
        }
        else {
#if LE_HAS_FASTCALL_DICT_TSTATE
            result = PyObject_Call(func, callargs, kwdict);
#else
            result = PyCFunction_Call(func, callargs, kwdict);
#endif
        }
        return result;
    }
    else if (Py_TYPE(func) == &PyMethodDescr_Type) {
        Py_ssize_t nargs = PyTuple_GET_SIZE(callargs);
        if (nargs > 0 && tstate->use_tracing) {
            PyObject *self = PyTuple_GET_ITEM(callargs, 0);
            func = Py_TYPE(func)->tp_descr_get(func, self,
                                               (PyObject *)Py_TYPE(self));
            if (func == NULL) {
                return NULL;
            }
            if (tstate->use_tracing && tstate->c_profilefunc) {
                if (cimpl_call_trace(tstate->c_profilefunc,
                                     tstate->c_profileobj, tstate->frame,
                                     PyTrace_C_CALL, func)) {
                    result = NULL;
                }
                else {
                    result = le_fastcall_dict_tstate(
                        tstate, func, &(_PyTuple_CAST(callargs)->ob_item)[1],
                        nargs - 1, kwdict);
                    if (tstate->c_profilefunc != NULL) {
                        if (result == NULL) {
                            cimpl_call_trace_protected(
                                tstate->c_profilefunc, tstate->c_profileobj,
                                tstate->frame, PyTrace_C_EXCEPTION, func);
                        }
                        else {
                            if (cimpl_call_trace(tstate->c_profilefunc,
                                                 tstate->c_profileobj,
                                                 tstate->frame,
                                                 PyTrace_C_RETURN, func)) {
                                Py_DECREF(result);
                                result = NULL;
                            }
                        }
                    }
                }
            }
            else {
                result = le_fastcall_dict_tstate(
                    tstate, func, &(_PyTuple_CAST(callargs)->ob_item)[1],
                    nargs - 1, kwdict);
            }
            Py_DECREF(func);
            return result;
        }
    }
    return PyObject_Call(func, callargs, kwdict);
}
#else
{
    if (PyCFunction_Check(func)) {
        PyObject *result;
        PyThreadState *tstate = PyThreadState_GET();
        if (tstate->use_tracing && tstate->c_profilefunc) {
            if (cimpl_call_trace(tstate->c_profilefunc, tstate->c_profileobj,
                                 tstate->frame, PyTrace_C_CALL, func)) {
                result = NULL;
            }
            else {
                result = PyCFunction_Call(func, callargs, kwdict);
                if (tstate->c_profilefunc != NULL) {
                    if (result == NULL) {
                        cimpl_call_trace_protected(
                            tstate->c_profilefunc, tstate->c_profileobj,
                            tstate->frame, PyTrace_C_EXCEPTION, func);
                    }
                    else {
                        if (cimpl_call_trace(
                                tstate->c_profilefunc, tstate->c_profileobj,
                                tstate->frame, PyTrace_C_RETURN, func)) {
                            Py_DECREF(result);
                            result = NULL;
                        }
                    }
                }
            }
        }
        else {
            result = PyCFunction_Call(func, callargs, kwdict);
        }
        return result;
    }
    else {
        return PyObject_Call(func, callargs, kwdict);
    }
}
#endif
#endif

#if !LE_HAS_IMPORT_FROM_SPEC
static PyObject *
cimpl_import_from(PyObject *v, PyObject *name)
{
    PyObject *x;
    _Py_IDENTIFIER(__name__);
    PyObject *fullmodname, *pkgname, *pkgpath, *pkgname_or_unknown, *errmsg;
    if (le_lookup_attr(v, name, &x) != 0) {
        return x;
    }
    pkgname = _PyObject_GetAttrId(v, &PyId___name__);
    if (pkgname == NULL) {
        goto error;
    }
    if (!PyUnicode_Check(pkgname)) {
        Py_CLEAR(pkgname);
        goto error;
    }
    fullmodname = PyUnicode_FromFormat("%U.%U", pkgname, name);
    if (fullmodname == NULL) {
        Py_DECREF(pkgname);
        return NULL;
    }
    x = PyImport_GetModule(fullmodname);
    Py_DECREF(fullmodname);
    if (x == NULL && !PyErr_Occurred()) {
        goto error;
    }
    Py_DECREF(pkgname);
    return x;
error:
    pkgpath = PyModule_GetFilenameObject(v);
    if (pkgname == NULL) {
        pkgname_or_unknown = PyUnicode_FromString(LE_MSG_UNKNOWN_MODULE_NAME);
        if (pkgname_or_unknown == NULL) {
            Py_XDECREF(pkgpath);
            return NULL;
        }
    }
    else {
        pkgname_or_unknown = pkgname;
    }
    if (pkgpath == NULL || !PyUnicode_Check(pkgpath)) {
        PyErr_Clear();
        errmsg = PyUnicode_FromFormat(LE_MSG_IMPORT_NAME_UNKNOWN_LOCATION,
                                      name, pkgname_or_unknown);
        PyErr_SetImportError(errmsg, pkgname, NULL);
    }
    else {
        errmsg = PyUnicode_FromFormat(LE_MSG_IMPORT_NAME_FROM_FILE, name,
                                      pkgname_or_unknown, pkgpath);
        PyErr_SetImportError(errmsg, pkgname, pkgpath);
    }
    Py_XDECREF(errmsg);
    Py_XDECREF(pkgname_or_unknown);
    Py_XDECREF(pkgpath);
    return NULL;
}
#endif

#if LE_HAS_CEVAL_TRACE_INFO
typedef struct {
    PyCodeObject *code;
    PyCodeAddressRange bounds;
    CFrame cframe;
} PyTraceInfo;
#endif

#if LE_HAS_OPCODE_MATCH_CLASS
static PyObject *
cimpl_match_class_attr(PyObject *subject, PyObject *type, PyObject *name,
                       PyObject *seen)
{
    assert(PyUnicode_CheckExact(name));
    assert(PySet_CheckExact(seen));
    if (PySet_Contains(seen, name) || PySet_Add(seen, name)) {
        if (!PyErr_Occurred()) {
            PyErr_Format(PyExc_TypeError, LE_MSG_MATCH_DUPLICATE_ATTRIBUTE,
                         ((PyTypeObject *)type)->tp_name, name);
        }
        return NULL;
    }
    PyObject *attr = PyObject_GetAttr(subject, name);
    if (attr == NULL && PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
    }
    return attr;
}
#endif

#if LE_HAS_OPCODE_MATCH_CLASS
static PyObject *
cimpl_match_class(PyObject *subject, PyObject *type, Py_ssize_t nargs,
                  PyObject *kwargs)
{
    if (!PyType_Check(type)) {
        const char *e = LE_MSG_MATCH_NOT_A_TYPE;
        PyErr_Format(PyExc_TypeError, e);
        return NULL;
    }
    assert(PyTuple_CheckExact(kwargs));
    if (PyObject_IsInstance(subject, type) <= 0) {
        return NULL;
    }
    PyObject *seen = PySet_New(NULL);
    if (seen == NULL) {
        return NULL;
    }
    PyObject *attrs = PyList_New(0);
    if (attrs == NULL) {
        Py_DECREF(seen);
        return NULL;
    }
    PyObject *match_args = NULL;
    if (nargs) {
        int match_self = 0;
        /* An interned name, not PyObject_GetAttrString's new string each
           time: 3.10's type attribute cache is keyed by the name's address
           and keeps the name alive, so each new string could take another
           slot and hold on to its memory until the cache is full. */
        _Py_IDENTIFIER(__match_args__);
        match_args = _PyObject_GetAttrId(type, &PyId___match_args__);
        if (match_args) {
            if (!PyTuple_CheckExact(match_args)) {
                const char *e = LE_MSG_MATCH_ARGS_NOT_TUPLE;
                PyErr_Format(PyExc_TypeError, e,
                             ((PyTypeObject *)type)->tp_name,
                             Py_TYPE(match_args)->tp_name);
                goto fail;
            }
        }
        else if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Clear();
            match_args = PyTuple_New(0);
            match_self = PyType_HasFeature((PyTypeObject *)type,
                                           _Py_TPFLAGS_MATCH_SELF);
        }
        else {
            goto fail;
        }
        assert(PyTuple_CheckExact(match_args));
        Py_ssize_t allowed = match_self ? 1 : PyTuple_GET_SIZE(match_args);
        if (allowed < nargs) {
            const char *plural = (allowed == 1) ? "" : "s";
            PyErr_Format(PyExc_TypeError, LE_MSG_MATCH_TOO_MANY_POSITIONAL,
                         ((PyTypeObject *)type)->tp_name, allowed, plural,
                         nargs);
            goto fail;
        }
        if (match_self) {
            PyList_Append(attrs, subject);
        }
        else {
            for (Py_ssize_t i = 0; i < nargs; i++) {
                PyObject *name = PyTuple_GET_ITEM(match_args, i);
                if (!PyUnicode_CheckExact(name)) {
                    PyErr_Format(PyExc_TypeError, LE_MSG_MATCH_ARGS_NOT_STR,
                                 Py_TYPE(name)->tp_name);
                    goto fail;
                }
                PyObject *attr =
                    cimpl_match_class_attr(subject, type, name, seen);
                if (attr == NULL) {
                    goto fail;
                }
                PyList_Append(attrs, attr);
                Py_DECREF(attr);
            }
        }
        Py_CLEAR(match_args);
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(kwargs); i++) {
        PyObject *name = PyTuple_GET_ITEM(kwargs, i);
        PyObject *attr = cimpl_match_class_attr(subject, type, name, seen);
        if (attr == NULL) {
            goto fail;
        }
        PyList_Append(attrs, attr);
        Py_DECREF(attr);
    }
    Py_SETREF(attrs, PyList_AsTuple(attrs));
    Py_DECREF(seen);
    return attrs;
fail:
    Py_XDECREF(match_args);
    Py_DECREF(seen);
    Py_DECREF(attrs);
    return NULL;
}
#endif

#if LE_HAS_OPCODE_MATCH_KEYS
static PyObject *
cimpl_match_keys(PyObject *map, PyObject *keys)
{
    assert(PyTuple_CheckExact(keys));
    Py_ssize_t nkeys = PyTuple_GET_SIZE(keys);
    if (!nkeys) {
        return PyTuple_New(0);
    }
    PyObject *seen = NULL;
    PyObject *dummy = NULL;
    PyObject *values = NULL;
    _Py_IDENTIFIER(get);
    PyObject *get = _PyObject_GetAttrId(map, &PyId_get);
    if (get == NULL) {
        goto fail;
    }
    seen = PySet_New(NULL);
    if (seen == NULL) {
        goto fail;
    }
    dummy = PyObject_CallObject((PyObject *)&PyBaseObject_Type, NULL);
    if (dummy == NULL) {
        goto fail;
    }
    values = PyList_New(0);
    if (values == NULL) {
        goto fail;
    }
    for (Py_ssize_t i = 0; i < nkeys; i++) {
        PyObject *key = PyTuple_GET_ITEM(keys, i);
        if (PySet_Contains(seen, key) || PySet_Add(seen, key)) {
            if (!PyErr_Occurred()) {
                PyErr_Format(PyExc_ValueError, LE_MSG_MATCH_DUPLICATE_KEY,
                             key);
            }
            goto fail;
        }
        PyObject *value = PyObject_CallFunctionObjArgs(get, key, dummy, NULL);
        if (value == NULL) {
            goto fail;
        }
        if (value == dummy) {
            Py_DECREF(value);
            Py_DECREF(values);
            Py_INCREF(Py_None);
            values = Py_None;
            goto done;
        }
        PyList_Append(values, value);
        Py_DECREF(value);
    }
    Py_SETREF(values, PyList_AsTuple(values));
done:
    Py_DECREF(get);
    Py_DECREF(seen);
    Py_DECREF(dummy);
    return values;
fail:
    Py_XDECREF(get);
    Py_XDECREF(seen);
    Py_XDECREF(dummy);
    Py_XDECREF(values);
    return NULL;
}
#endif

#if !LE_HAS_INLINE_CALL_TSTATE_GET
static int
cimpl_print_expr_r0(PyObject *_in0, long _oparg, PyObject *value,
                    PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 1, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(70, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    Py_INCREF(value);
    {
        {
            PyObject *hook = PySys_GetObject("displayhook");
            PyObject *res;
            if (hook == NULL) {
                PyErr_SetString(PyExc_RuntimeError, LE_MSG_NO_DISPLAYHOOK);
                Py_DECREF(value);
                goto _error;
            }
#if LE_HAS_CALL_ONE_ARG
            res = PyObject_CallOneArg(hook, value);
#else
            res = PyObject_CallFunctionObjArgs(hook, value, NULL);
#endif
            Py_DECREF(value);
            if (res == NULL)
                goto _error;
            Py_DECREF(res);
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}
#endif

static int
cimpl_get_aiter_r0(PyObject *_in0, long _oparg, PyObject *iter, PyObject *obj,
                   PyTypeObject *type, PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 1, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(50, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    unaryfunc getter;
    *stack_pointer = _in0;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    {
        {
            {
                getter = type->tp_as_async->am_aiter;
            }
            if (getter != NULL) {
                iter = (*getter)(obj);
                Py_DECREF(obj);
                if (iter == NULL) {
                    stack_pointer[-1] = NULL;
                    goto _error;
                }
            }
            else {
                stack_pointer[-1] = NULL;
                PyErr_Format(PyExc_TypeError, LE_MSG_ASYNC_FOR_NO_AITER,
                             type->tp_name);
                Py_DECREF(obj);
                goto _error;
            }
            if ((Py_TYPE(iter)->tp_as_async == NULL) ||
                (Py_TYPE(iter)->tp_as_async->am_anext == NULL)) {
                stack_pointer[-1] = NULL;
                PyErr_Format(PyExc_TypeError, LE_MSG_ASYNC_FOR_AITER_NO_ANEXT,
                             Py_TYPE(iter)->tp_name);
                Py_DECREF(iter);
                goto _error;
            }
            stack_pointer[-1] = iter;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}

static int
cimpl_get_aiter_r1(PyObject *_in0, long _oparg, PyObject *iter,
                   PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 1, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(50, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    *stack_pointer = _in0;
    stack_pointer++;
    Py_INCREF(iter);
    {
        {
            if ((Py_TYPE(iter)->tp_as_async == NULL) ||
                (Py_TYPE(iter)->tp_as_async->am_anext == NULL)) {
                stack_pointer[-1] = NULL;
                PyErr_Format(PyExc_TypeError, LE_MSG_ASYNC_FOR_AITER_NO_ANEXT,
                             Py_TYPE(iter)->tp_name);
                Py_DECREF(iter);
                goto _error;
            }
            stack_pointer[-1] = iter;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}

#if LE_HAS_CORO_GET_AWAITABLE_ITER_LINKABLE
static PyObject *
cimpl_get_anext_0(PyObject *next_iter)
{
    PyObject *_r = _PyCoro_GetAwaitableIter(next_iter);
    if (_r == NULL) {
        _PyErr_FormatFromCause(PyExc_TypeError, LE_MSG_ASYNC_FOR_BAD_ANEXT,
                               Py_TYPE(next_iter)->tp_name);
    }
    return _r;
}
#endif

#if LE_HAS_CORO_GET_AWAITABLE_ITER_LINKABLE
static int
cimpl_get_anext_r0(PyObject *_in0, long _oparg, PyObject *aiter,
                   PyObject *next_iter, PyTypeObject *type, PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 1, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(51, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    unaryfunc getter;
    PyObject *awaitable;
    *stack_pointer = _in0;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    {
        {
            {
                {
                    getter = type->tp_as_async->am_anext;
                }
                if (getter != NULL) {
                    next_iter = (*getter)(aiter);
                    if (next_iter == NULL) {
                        goto _error;
                    }
                }
                else {
                    PyErr_Format(PyExc_TypeError, LE_MSG_ASYNC_FOR_NO_ANEXT,
                                 type->tp_name);
                    goto _error;
                }
                awaitable = cimpl_get_anext_0(next_iter);
                if (awaitable == NULL) {
                    Py_DECREF(next_iter);
                    goto _error;
                }
                else {
                    Py_DECREF(next_iter);
                }
            }
            *stack_pointer++ = awaitable;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}
#endif

#if LE_HAS_OPCODE_LOAD_ASSERTION_ERROR
static int
cimpl_load_assertion_error_r0(long _oparg, PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 0, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(74, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    {
        {
            PyObject *value = PyExc_AssertionError;
            Py_INCREF(value);
            *stack_pointer++ = value;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}
#endif

_Py_IDENTIFIER(__build_class__);

static PyObject *
cimpl_load_build_class_1(PyObject *build_class_str, PyObject *_in0)
{
    PyObject *_r = PyObject_GetItem(_in0, build_class_str);
    if (_r == NULL) {
        if (PyErr_ExceptionMatches(PyExc_KeyError))
            PyErr_SetString(PyExc_NameError, LE_MSG_NO_BUILD_CLASS);
    }
    return _r;
}

static int
cimpl_load_build_class_r0(long _oparg, PyObject *_f_f_builtins,
                          PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 0, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(71, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    PyObject *bc;
    {
        {
            {
                PyObject *build_class_str =
                    _PyUnicode_FromId(&PyId___build_class__);
                if (build_class_str == NULL)
                    goto _error;
                bc = cimpl_load_build_class_1(build_class_str, _f_f_builtins);
                if (bc == NULL) {
                    goto _error;
                }
            }
            *stack_pointer++ = bc;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}

static void
cimpl_format_exc_check_arg(PyObject *exc, const char *format_str,
                           PyObject *obj)
#if LE_HAS_NAME_ERROR_NAME
{
    const char *obj_str;
    if (!obj)
        return;
    obj_str = PyUnicode_AsUTF8(obj);
    if (!obj_str)
        return;
    PyErr_Format(exc, format_str, obj_str);
    if (exc == PyExc_NameError) {
        PyObject *type, *value, *traceback;
        PyErr_Fetch(&type, &value, &traceback);
        PyErr_NormalizeException(&type, &value, &traceback);
        if (PyErr_GivenExceptionMatches(value, PyExc_NameError)) {
            PyNameErrorObject *exc = (PyNameErrorObject *)value;
            if (exc->name == NULL) {
                (void)PyObject_SetAttrString(value, "name", obj);
            }
        }
        PyErr_Restore(type, value, traceback);
    }
}
#else
{
    const char *obj_str;
    if (!obj)
        return;
    obj_str = PyUnicode_AsUTF8(obj);
    if (!obj_str)
        return;
    PyErr_Format(exc, format_str, obj_str);
}
#endif

/* raised when it runs, by CPython's code, which sets a NameError's name */
static void
ct_format_exc_check_arg(PsycoObject *po, PyObject *exc, const char *format_str,
                        PyObject *obj)
{
    psyco_generic_call(po, cimpl_format_exc_check_arg, CfPyErrAlways, "lll",
                       (long)exc, (long)format_str, (long)obj);
}

static int
cimpl_delete_global_0(PyObject *name, PyObject *_in0)
#if LE_HAS_DELETE_GLOBAL_KEYERROR_ONLY
{
    int _r = PyDict_DelItem(_in0, name);
    if (_r != 0) {
        if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            cimpl_format_exc_check_arg(PyExc_NameError,
                                       LE_MSG_NAME_NOT_DEFINED, name);
        }
    }
    return _r;
}
#else
{
    int _r = PyDict_DelItem(_in0, name);
    if (_r != 0) {
        cimpl_format_exc_check_arg(PyExc_NameError, LE_MSG_NAME_NOT_DEFINED,
                                   name);
    }
    return _r;
}
#endif

static void
ct_format_exc_unbound(PsycoObject *po, PyCodeObject *co, int oparg)
{
    PyObject *name;
    if (PycException_Occurred(po))
        return;
    if (oparg < le_code_ncells(co)) {
        name = le_code_cell_name(co, oparg);
        ct_format_exc_check_arg(po, PyExc_UnboundLocalError,
                                LE_MSG_LOCAL_UNBOUND, name);
    }
    else {
        name = le_code_free_name(co, oparg - le_code_ncells(co));
        ct_format_exc_check_arg(po, PyExc_NameError, LE_MSG_FREE_UNBOUND,
                                name);
    }
}

static PyObject *
cimpl_build_string_array(long _count, PyObject *_a0, PyObject *_a1,
                         PyObject *_a2, PyObject *_a3)
{
    PyObject *_words[4] = {_a0, _a1, _a2, _a3};
    PyObject **_items = _words;
    Py_ssize_t _n = _count;
    if (_count < 0) { /* a tuple of the items */
        _items = &PyTuple_GET_ITEM(_a0, 0);
        _n = PyTuple_GET_SIZE(_a0);
    }
    PyObject *_p0 = PyUnicode_New(0, 0);
    if (_p0 == NULL)
        return NULL;
    PyObject *_r = _PyUnicode_JoinArray(_p0, _items, _n);
    Py_DECREF(_p0);
    return _r;
}

#if LE_HAS_OPCODE_LIST_EXTEND
static PyObject *
cimpl_list_extend_0(PyObject *list, PyObject *iterable)
{
    PyObject *_r = _PyList_Extend((PyListObject *)list, iterable);
    if (_r == NULL) {
        if (PyErr_ExceptionMatches(PyExc_TypeError) &&
            ((Py_TYPE(iterable)->tp_iter == NULL) &&
             (!PySequence_Check(iterable)))) {
            PyErr_Clear();
            PyErr_Format(PyExc_TypeError, LE_MSG_LIST_STAR_NOT_ITERABLE,
                         Py_TYPE(iterable)->tp_name);
        }
    }
    return _r;
}
#endif

#if LE_HAS_OPCODE_DICT_UPDATE
static int
cimpl_dict_update_0(PyObject *dict, PyObject *update)
{
    int _r = PyDict_Update(dict, update);
    if (_r < 0) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Format(PyExc_TypeError, LE_MSG_DICT_STAR_NOT_MAPPING,
                         Py_TYPE(update)->tp_name);
        }
    }
    return _r;
}
#endif

#if LE_HAS_FUNCTION_STR
static void
cimpl_format_kwargs_error(PyObject *func, PyObject *kwargs)
{
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Clear();
        PyObject *funcstr = _PyObject_FunctionStr(func);
        if (funcstr != NULL) {
            PyErr_Format(PyExc_TypeError,
                         LE_MSG_KWARGS_NOT_MAPPING_FUNCTION_STR, funcstr,
                         Py_TYPE(kwargs)->tp_name);
            Py_DECREF(funcstr);
        }
    }
    else if (PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyObject *exc, *val, *tb;
        PyErr_Fetch(&exc, &val, &tb);
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
            PyErr_Clear();
            PyObject *funcstr = _PyObject_FunctionStr(func);
            if (funcstr != NULL) {
                PyObject *key = PyTuple_GET_ITEM(val, 0);
                PyErr_Format(PyExc_TypeError,
                             LE_MSG_KWARGS_DUPLICATE_FUNCTION_STR, funcstr,
                             key);
                Py_DECREF(funcstr);
            }
            Py_XDECREF(exc);
            Py_XDECREF(val);
            Py_XDECREF(tb);
        }
        else {
            PyErr_Restore(exc, val, tb);
        }
    }
}
#endif

#if LE_HAS_OPCODE_DICT_MERGE
static int
cimpl_dict_merge_0(PyObject *dict, PyObject *update, PyObject *_in0)
{
    int _r = le_dict_merge_unique(dict, update);
    if (_r < 0) {
        cimpl_format_kwargs_error(_in0, update);
    }
    return _r;
}
#endif

#if LE_HAS_OPCODE_JUMP_IF_NOT_EXC_MATCH
static int
cimpl_jump_if_not_exc_match_0(PyObject *right)
{
    if (PyTuple_Check(right)) {
        Py_ssize_t i;
        Py_ssize_t length;
        length = PyTuple_GET_SIZE(right);
        for (i = 0; i < length; i++) {
            PyObject *exc = PyTuple_GET_ITEM(right, i);
            if (!PyExceptionClass_Check(exc)) {
                PyErr_SetString(PyExc_TypeError,
                                LE_MSG_CATCHING_NON_EXCEPTION);
                return -1;
            }
        }
    }
    else {
        if (!PyExceptionClass_Check(right)) {
            PyErr_SetString(PyExc_TypeError, LE_MSG_CATCHING_NON_EXCEPTION);
            return -1;
        }
    }
    return 0;
}
#endif

#if LE_HAS_OPCODE_MATCH_MAPPING
static int
cimpl_match_mapping_r0(PyObject *_in0, long _oparg, PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 1, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(31, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    *stack_pointer = _in0;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    {
        {
            int match = Py_TYPE(_in0)->tp_flags & Py_TPFLAGS_MAPPING;
            PyObject *res = (match) ? Py_True : Py_False;
            Py_INCREF(res);
            *stack_pointer++ = res;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}
#endif

#if LE_HAS_OPCODE_MATCH_SEQUENCE
static int
cimpl_match_sequence_r0(PyObject *_in0, long _oparg, PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 1, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(32, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    *stack_pointer = _in0;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    {
        {
            int match = Py_TYPE(_in0)->tp_flags & Py_TPFLAGS_SEQUENCE;
            PyObject *res = (match) ? Py_True : Py_False;
            Py_INCREF(res);
            *stack_pointer++ = res;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}
#endif

#if LE_HAS_OPCODE_MATCH_KEYS
static int
cimpl_match_keys_r0(PyObject *_in0, PyObject *_in1, long _oparg,
                    PyObject *values_or_none, PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 2, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(33, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    *stack_pointer = _in0;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = _in1;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = values_or_none;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    {
        {
            if (values_or_none == Py_None) {
                Py_INCREF(Py_False);
                *stack_pointer++ = Py_False;
                goto _done;
            }
            Py_INCREF(Py_True);
            *stack_pointer++ = Py_True;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}
#endif

#if LE_HAS_OPCODE_COPY_DICT_WITHOUT_KEYS
static int
cimpl_copy_dict_without_keys_r0(PyObject *_in0, PyObject *_in1, long _oparg,
                                PyObject *keys, PyObject *rest,
                                PyObject *subject, PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 2, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(34, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    *stack_pointer = _in0;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = _in1;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    Py_INCREF(rest);
    {
        {
            if ((rest == NULL) || PyDict_Update(rest, subject)) {
                Py_XDECREF(rest);
                goto _error;
            }
            for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(keys); i++) {
                if (PyDict_DelItem(rest, PyTuple_GET_ITEM(keys, i))) {
                    Py_DECREF(rest);
                    goto _error;
                }
            }
            Py_DECREF(keys);
            stack_pointer[-1] = rest;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}
#endif

#if LE_HAS_OPCODE_COPY_DICT_WITHOUT_KEYS
static int
cimpl_copy_dict_without_keys_r1(PyObject *_in0, PyObject *_in1, long _oparg,
                                PyObject *keys, PyObject *rest,
                                PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 2, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(34, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    *stack_pointer = _in0;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = _in1;
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    Py_INCREF(rest);
    {
        {
            for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(keys); i++) {
                if (PyDict_DelItem(rest, PyTuple_GET_ITEM(keys, i))) {
                    Py_DECREF(rest);
                    goto _error;
                }
            }
            Py_DECREF(keys);
            stack_pointer[-1] = rest;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}
#endif

#if LE_HAS_CEVAL_TRACE_INFO
static void
cimpl_initialize_trace_info(PyTraceInfo *trace_info, PyFrameObject *frame)
{
    if (trace_info->code != frame->f_code) {
        trace_info->code = frame->f_code;
        cimpl__PyCode_InitAddressRange(frame->f_code, &trace_info->bounds);
    }
}
#endif

#if LE_HAS_CEVAL_TRACE_INFO
static int
cimpl_call_trace(Py_tracefunc func, PyObject *obj, PyFrameObject *frame,
                 PyTraceInfo *trace_info, int what, PyObject *arg)
{
    PyThreadState *tstate = PyThreadState_GET();

    int result;
    if (tstate->tracing)
        return 0;
    tstate->tracing++;
    tstate->cframe->use_tracing = 0;
    if (frame->f_lasti < 0) {
        frame->f_lineno = frame->f_code->co_firstlineno;
    }
    else {
        cimpl_initialize_trace_info(trace_info, frame);
        frame->f_lineno = _PyCode_CheckLineNumber(
            frame->f_lasti * sizeof(_Py_CODEUNIT), &trace_info->bounds);
    }
    result = func(obj, frame, what, arg);
    frame->f_lineno = 0;
    tstate->cframe->use_tracing =
        ((tstate->c_tracefunc != NULL) || (tstate->c_profilefunc != NULL));
    tstate->tracing--;
    return result;
}
#endif

_Py_IDENTIFIER(__aenter__);
_Py_IDENTIFIER(__aexit__);

static PyObject *
cimpl_special_lookup(PyObject *o, _Py_Identifier *id)
{
    PyObject *res;
    res = le_lookup_special_id(o, id);
    if (res == NULL && !PyErr_Occurred()) {
        PyErr_SetObject(PyExc_AttributeError, _PyUnicode_FromId(id));
        return NULL;
    }
    return res;
}

#if LE_HAS_OPCODE_WITH_EXCEPT_START
static int
cimpl_with_except_start_r0(PyObject *_items, long _oparg, PyObject *exc,
                           PyObject *exit_func, PyObject *tb, PyObject *val,
                           PyObject **_out)
{
    int oparg = (int)_oparg;
    Py_ssize_t _t = 7, _i;
    PyObject *_stack[_t + PyCompile_OpcodeStackEffect(49, oparg) + 0 + 1];
    PyObject **stack_pointer = _stack;
    PyObject *res;
    for (_i = 0; _i < _t - 7; _i++) {
        *stack_pointer = PyTuple_GET_ITEM(_items, _i);
        Py_INCREF(*stack_pointer);
        stack_pointer++;
    }
    *stack_pointer = PyTuple_GET_ITEM(_items, _t - 7);
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = PyTuple_GET_ITEM(_items, _t - 6);
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = PyTuple_GET_ITEM(_items, _t - 5);
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = PyTuple_GET_ITEM(_items, _t - 4);
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = PyTuple_GET_ITEM(_items, _t - 3);
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = PyTuple_GET_ITEM(_items, _t - 2);
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    *stack_pointer = PyTuple_GET_ITEM(_items, _t - 1);
    Py_INCREF(*stack_pointer);
    stack_pointer++;
    {
        {
            PyObject *stack[4] = {NULL, exc, val, tb};
            res = PyObject_Vectorcall(
                exit_func, stack + 1,
                3 | (((size_t)1) << ((8 * (sizeof(size_t))) - 1)), NULL);
            if (res == NULL)
                goto _error;
            *stack_pointer++ = res;
            goto _done;
        }
    }
_done:
    for (_i = 0; _stack + _i < stack_pointer; _i++) _out[_i] = _stack[_i];
    return 0;
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return -1;
}
#endif

static int
cimpl_check_args_iterable(PyObject *func, PyObject *args)
{
#if LE_HAS_FUNCTION_STR
    if (Py_TYPE(args)->tp_iter == NULL && !PySequence_Check(args)) {
        PyErr_Clear();
        PyObject *funcstr = _PyObject_FunctionStr(func);
        if (funcstr != NULL) {
            PyErr_Format(PyExc_TypeError,
                         LE_MSG_STAR_NOT_ITERABLE_FUNCTION_STR, funcstr,
                         Py_TYPE(args)->tp_name);
            Py_DECREF(funcstr);
        }
        return -1;
    }
#else
    if (args->ob_type->tp_iter == NULL && !PySequence_Check(args)) {
        PyErr_Format(PyExc_TypeError, LE_MSG_STAR_NOT_ITERABLE_FUNC_NAME,
                     PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                     args->ob_type->tp_name);
        return -1;
    }
#endif
    return 0;
}

#if LE_HAS_OPCODE_BUILD_TUPLE_UNPACK_WITH_CALL
static PyObject *
cimpl_build_tuple_unpack_with_call_0(PyObject *sum, PyObject *_in0,
                                     PyObject *_in1)
{
    PyObject *_r = _PyList_Extend((PyListObject *)sum, _in0);
    if (_r == NULL) {
        if (PyErr_ExceptionMatches(PyExc_TypeError)) {
            cimpl_check_args_iterable(_in1, _in0);
        }
    }
    return _r;
}
#endif

#if LE_HAS_OPCODE_BUILD_MAP_UNPACK
static int
cimpl_build_map_unpack_0(PyObject *sum, PyObject *arg)
{
    int _r = PyDict_Update(sum, arg);
    if (_r < 0) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Format(PyExc_TypeError, LE_MSG_DICT_STAR_NOT_MAPPING,
                         arg->ob_type->tp_name);
        }
    }
    return _r;
}
#endif

#if !LE_HAS_KWARGS_MERGE_EX
static void
cimpl_format_kwargs_mapping_error(PyObject *func, PyObject *kwargs)
{
    PyErr_Format(PyExc_TypeError, LE_MSG_KWARGS_NOT_MAPPING_FUNC_NAME,
                 PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                 kwargs->ob_type->tp_name);
}
#endif

#if LE_HAS_KWARGS_MERGE_EX && !LE_HAS_FUNCTION_STR
static void
cimpl_format_kwargs_error(PyObject *func, PyObject *kwargs)
{
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Format(PyExc_TypeError, LE_MSG_KWARGS_NOT_MAPPING_FUNC_NAME,
                     PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                     kwargs->ob_type->tp_name);
    }
    else if (PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyObject *exc, *val, *tb;
        PyErr_Fetch(&exc, &val, &tb);
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
            PyObject *key = PyTuple_GET_ITEM(val, 0);
            if (!PyUnicode_Check(key)) {
                PyErr_Format(
                    PyExc_TypeError, LE_MSG_KWARGS_NOT_STRINGS_FUNC_NAME,
                    PyEval_GetFuncName(func), PyEval_GetFuncDesc(func));
            }
            else {
                PyErr_Format(
                    PyExc_TypeError, LE_MSG_KWARGS_DUPLICATE_FUNC_NAME,
                    PyEval_GetFuncName(func), PyEval_GetFuncDesc(func), key);
            }
            Py_XDECREF(exc);
            Py_XDECREF(val);
            Py_XDECREF(tb);
        }
        else {
            PyErr_Restore(exc, val, tb);
        }
    }
}
#endif

#if LE_HAS_OPCODE_BUILD_MAP_UNPACK_WITH_CALL
static int
cimpl_build_map_unpack_with_call_0(PyObject *sum, PyObject *arg,
                                   PyObject *_in0)
{
    int _r = le_dict_merge_unique(sum, arg);
    if (_r < 0) {
#if LE_HAS_KWARGS_MERGE_EX
        Py_DECREF(sum);
        cimpl_format_kwargs_error(_in0, arg);
#else
        PyObject *func = _in0;
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            cimpl_format_kwargs_mapping_error(func, arg);
        }
        else if (PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyObject *exc;
            PyObject *val;
            PyObject *tb;
            PyErr_Fetch(&exc, &val, &tb);
            if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
                PyObject *key = PyTuple_GET_ITEM(val, 0);
                if (!PyUnicode_Check(key)) {
                    PyErr_Format(
                        PyExc_TypeError, LE_MSG_KWARGS_NOT_STRINGS_FUNC_NAME,
                        PyEval_GetFuncName(func), PyEval_GetFuncDesc(func));
                }
                else {
                    PyErr_Format(PyExc_TypeError,
                                 LE_MSG_KWARGS_DUPLICATE_FUNC_NAME,
                                 PyEval_GetFuncName(func),
                                 PyEval_GetFuncDesc(func), key);
                }
                Py_XDECREF(exc);
                Py_XDECREF(val);
                Py_XDECREF(tb);
            }
            else {
                PyErr_Restore(exc, val, tb);
            }
        }
#endif
    }
    return _r;
}
#endif

#if LE_HAS_IMPORT_FROM_SPEC
static PyObject *
cimpl_import_from_whole(PyObject *v, PyObject *name)
{
    PyObject *x;
    _Py_IDENTIFIER(__name__);
    PyObject *fullmodname, *pkgname, *pkgpath, *pkgname_or_unknown, *errmsg;
    if (le_lookup_attr(v, name, &x) != 0) {
        return x;
    }
    pkgname = _PyObject_GetAttrId(v, &PyId___name__);
    if (pkgname == NULL) {
        goto error;
    }
    if (!PyUnicode_Check(pkgname)) {
        Py_CLEAR(pkgname);
        goto error;
    }
    fullmodname = PyUnicode_FromFormat("%U.%U", pkgname, name);
    if (fullmodname == NULL) {
        Py_DECREF(pkgname);
        return NULL;
    }
    x = PyImport_GetModule(fullmodname);
    Py_DECREF(fullmodname);
    if (x == NULL && !PyErr_Occurred()) {
        goto error;
    }
    Py_DECREF(pkgname);
    return x;
error:
    pkgpath = PyModule_GetFilenameObject(v);
    if (pkgname == NULL) {
        pkgname_or_unknown = PyUnicode_FromString(LE_MSG_UNKNOWN_MODULE_NAME);
        if (pkgname_or_unknown == NULL) {
            Py_XDECREF(pkgpath);
            return NULL;
        }
    }
    else {
        pkgname_or_unknown = pkgname;
    }
    if (pkgpath == NULL || !PyUnicode_Check(pkgpath)) {
        PyErr_Clear();
        errmsg = PyUnicode_FromFormat(LE_MSG_IMPORT_NAME_UNKNOWN_LOCATION,
                                      name, pkgname_or_unknown);
        PyErr_SetImportError(errmsg, pkgname, NULL);
    }
    else {
        _Py_IDENTIFIER(__spec__);
        PyObject *spec = _PyObject_GetAttrId(v, &PyId___spec__);
        const char *fmt = le_module_spec_is_initializing(spec)
                              ? LE_MSG_IMPORT_NAME_CIRCULAR
                              : LE_MSG_IMPORT_NAME_FROM_FILE;
        Py_XDECREF(spec);
        errmsg = PyUnicode_FromFormat(fmt, name, pkgname_or_unknown, pkgpath);
        PyErr_SetImportError(errmsg, pkgname, pkgpath);
    }
    Py_XDECREF(errmsg);
    Py_XDECREF(pkgname_or_unknown);
    Py_XDECREF(pkgpath);
    return NULL;
}
#endif

#if LE_HAS_CEVAL_TRACE_INFO
static int
cimpl_call_trace_protected(Py_tracefunc func, PyObject *obj,
                           PyFrameObject *frame, PyTraceInfo *trace_info,
                           int what, PyObject *arg)
{
    PyObject *type, *value, *traceback;
    int err;
    PyErr_Fetch(&type, &value, &traceback);
    err = cimpl_call_trace(func, obj, frame, trace_info, what, arg);
    if (err == 0) {
        PyErr_Restore(type, value, traceback);
        return 0;
    }
    else {
        Py_XDECREF(type);
        Py_XDECREF(value);
        Py_XDECREF(traceback);
        return -1;
    }
}
#endif

#if LE_HAS_CEVAL_TRACE_INFO
static PyObject *
cimpl_do_call_core(PyTraceInfo *trace_info, PyObject *func, PyObject *callargs,
                   PyObject *kwdict)
{
    PyThreadState *tstate = PyThreadState_GET();

    PyObject *result;
    if (PyCFunction_CheckExact(func) || PyCMethod_CheckExact(func)) {
        if (trace_info->cframe.use_tracing && tstate->c_profilefunc) {
            if (cimpl_call_trace(tstate->c_profilefunc, tstate->c_profileobj,
                                 tstate->frame, trace_info, PyTrace_C_CALL,
                                 func)) {
                result = NULL;
            }
            else {
                result = PyObject_Call(func, callargs, kwdict);
                if (tstate->c_profilefunc != NULL) {
                    if (result == NULL) {
                        cimpl_call_trace_protected(tstate->c_profilefunc,
                                                   tstate->c_profileobj,
                                                   tstate->frame, trace_info,
                                                   PyTrace_C_EXCEPTION, func);
                    }
                    else {
                        if (cimpl_call_trace(tstate->c_profilefunc,
                                             tstate->c_profileobj,
                                             tstate->frame, trace_info,
                                             PyTrace_C_RETURN, func)) {
                            Py_DECREF(result);
                            result = NULL;
                        }
                    }
                }
            }
        }
        else {
            result = PyObject_Call(func, callargs, kwdict);
        }
        return result;
    }
    else if (Py_IS_TYPE(func, &PyMethodDescr_Type)) {
        Py_ssize_t nargs = PyTuple_GET_SIZE(callargs);
        if (nargs > 0 && trace_info->cframe.use_tracing) {
            PyObject *self = PyTuple_GET_ITEM(callargs, 0);
            func = Py_TYPE(func)->tp_descr_get(func, self,
                                               (PyObject *)Py_TYPE(self));
            if (func == NULL) {
                return NULL;
            }
            if (trace_info->cframe.use_tracing && tstate->c_profilefunc) {
                if (cimpl_call_trace(tstate->c_profilefunc,
                                     tstate->c_profileobj, tstate->frame,
                                     trace_info, PyTrace_C_CALL, func)) {
                    result = NULL;
                }
                else {
                    result = le_fastcall_dict_tstate(
                        tstate, func, &(_PyTuple_CAST(callargs)->ob_item)[1],
                        nargs - 1, kwdict);
                    if (tstate->c_profilefunc != NULL) {
                        if (result == NULL) {
                            cimpl_call_trace_protected(
                                tstate->c_profilefunc, tstate->c_profileobj,
                                tstate->frame, trace_info, PyTrace_C_EXCEPTION,
                                func);
                        }
                        else {
                            if (cimpl_call_trace(tstate->c_profilefunc,
                                                 tstate->c_profileobj,
                                                 tstate->frame, trace_info,
                                                 PyTrace_C_RETURN, func)) {
                                Py_DECREF(result);
                                result = NULL;
                            }
                        }
                    }
                }
            }
            else {
                result = le_fastcall_dict_tstate(
                    tstate, func, &(_PyTuple_CAST(callargs)->ob_item)[1],
                    nargs - 1, kwdict);
            }
            Py_DECREF(func);
            return result;
        }
    }
    return PyObject_Call(func, callargs, kwdict);
}
#endif

#if LE_HAS_CFRAME && !LE_HAS_CEVAL_TRACE_INFO
/* ceval's do_call_core where it keeps its trace info in the thread state,
   without the profiler's c_call events around a C function: compiled code
   makes no such call on this CPython yet (mergepoints.c refuses every
   code object) */
static PyObject *
cimpl_do_call_core(PyObject *func, PyObject *callargs, PyObject *kwdict)
{
    return PyObject_Call(func, callargs, kwdict);
}
#endif

static PyObject *
cimpl_call_function_ex(PyObject *_in0, PyObject *_in1, PyObject *_in2)
#if LE_HAS_CEVAL_TRACE_INFO
{
    PyThreadState *tstate = PyThreadState_GET();
    PyTraceInfo trace_info = {.code = NULL, .cframe = *tstate->cframe};
    int oparg = _in2 != NULL ? 1 : 0;
    PyObject *_stack[3];
    PyObject **stack_pointer = _stack;
    *stack_pointer++ = _in0;
    Py_INCREF(_in0);
    *stack_pointer++ = _in1;
    Py_INCREF(_in1);
    if (_in2 != NULL) {
        *stack_pointer++ = _in2;
        Py_INCREF(_in2);
    }
    {
        {
            PyObject *func;
            PyObject *callargs;
            PyObject *kwargs = NULL;
            PyObject *result;
            if (oparg & 0x01) {
                kwargs = *--stack_pointer;
                if (!PyDict_CheckExact(kwargs)) {
                    PyObject *d = PyDict_New();
                    if (d == NULL)
                        goto _error;
                    if (le_dict_merge_unique(d, kwargs) < 0) {
                        Py_DECREF(d);
                        cimpl_format_kwargs_error(stack_pointer[-2], kwargs);
                        Py_DECREF(kwargs);
                        goto _error;
                    }
                    Py_DECREF(kwargs);
                    kwargs = d;
                }
            }
            callargs = *--stack_pointer;
            func = stack_pointer[-1];
            if (!PyTuple_CheckExact(callargs)) {
                if (cimpl_check_args_iterable(func, callargs) < 0) {
                    Py_DECREF(callargs);
                    goto _error;
                }
                Py_SETREF(callargs, PySequence_Tuple(callargs));
                if (callargs == NULL) {
                    goto _error;
                }
            }
            result = cimpl_do_call_core(&trace_info, func, callargs, kwargs);
            Py_DECREF(func);
            Py_DECREF(callargs);
            Py_XDECREF(kwargs);
            stack_pointer[-1] = result;
            if (result == NULL) {
                goto _error;
            }
            goto _done;
        }
    }
_done:
    return _stack[0];
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return NULL;
}
#else
{
    int oparg = _in2 != NULL ? 1 : 0;
    PyObject *_stack[3];
    PyObject **stack_pointer = _stack;
    *stack_pointer++ = _in0;
    Py_INCREF(_in0);
    *stack_pointer++ = _in1;
    Py_INCREF(_in1);
    if (_in2 != NULL) {
        *stack_pointer++ = _in2;
        Py_INCREF(_in2);
    }
    {
        {
            PyObject *func;
            PyObject *callargs;
            PyObject *kwargs = NULL;
            PyObject *result;
            if (oparg & 0x01) {
                kwargs = *--stack_pointer;
                if (!PyDict_CheckExact(kwargs)) {
                    PyObject *d = PyDict_New();
                    if (d == NULL)
                        goto _error;
#if LE_HAS_KWARGS_MERGE_EX
                    if (le_dict_merge_unique(d, kwargs) < 0) {
                        Py_DECREF(d);
                        cimpl_format_kwargs_error(stack_pointer[-2], kwargs);
                        Py_DECREF(kwargs);
                        goto _error;
                    }
#else
                    if (PyDict_Update(d, kwargs) != 0) {
                        Py_DECREF(d);
                        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                            cimpl_format_kwargs_mapping_error(
                                stack_pointer[-2], kwargs);
                        }
                        Py_DECREF(kwargs);
                        goto _error;
                    }
#endif
                    Py_DECREF(kwargs);
                    kwargs = d;
                }
            }
            callargs = *--stack_pointer;
            func = stack_pointer[-1];
            if (!PyTuple_CheckExact(callargs)) {
                if (cimpl_check_args_iterable(func, callargs) < 0) {
                    Py_DECREF(callargs);
                    goto _error;
                }
                Py_SETREF(callargs, PySequence_Tuple(callargs));
                if (callargs == NULL) {
                    goto _error;
                }
            }
            result = cimpl_do_call_core(func, callargs, kwargs);
            Py_DECREF(func);
            Py_DECREF(callargs);
            Py_XDECREF(kwargs);
            stack_pointer[-1] = result;
            if (result == NULL) {
                goto _error;
            }
            goto _done;
        }
    }
_done:
    return _stack[0];
_error:
    while (stack_pointer > _stack) {
        PyObject *_x = *--stack_pointer;
        Py_XDECREF(_x);
    }
    return NULL;
}
#endif

#if LE_HAS_CMP_OUTCOME
static vinfo_t *
cmp_outcome(PsycoObject *po, int op, vinfo_t *v, vinfo_t *w)
{
    condition_code_t _t1;
    vinfo_t *_t10;
    int _t11;
    int _t12;
    int _t13;
    condition_code_t _t15;
    vinfo_t *_t16;
    vinfo_t *_t17;
    vinfo_t *_t2;
    condition_code_t _t3;
    vinfo_t *_t4;
    condition_code_t _t6;
    vinfo_t *_t7;
    condition_code_t _t9;
    vinfo_t *exc;
    Py_ssize_t i;
    PyTypeObject *k_exc;
    PyTypeObject *k_w;
    Py_ssize_t length;
    vinfo_t *res;
    PyTypeObject *tp_exc;
    PyTypeObject *tp_w;
    switch (op) {
        case PyCmp_IS:
            _t1 = integer_cmp(po, v, w, Py_EQ);
            if (_t1 == CC_ERROR) {
                return NULL;
            }
            _t2 = PsycoBool_FromCondition(po, _t1);
            v = _t2;
            return v;
        case PyCmp_IS_NOT:
            _t3 = integer_cmp(po, v, w, Py_NE);
            if (_t3 == CC_ERROR) {
                return NULL;
            }
            _t4 = PsycoBool_FromCondition(po, _t3);
            v = _t4;
            return v;
        case PyCmp_IN:
            res = PsycoSequence_Contains(po, w, v);
            if (res == NULL) {
                return NULL;
            }
            _t6 = integer_non_null(po, res);
            if (_t6 == CC_ERROR) {
                vinfo_decref(res, po);
                return NULL;
            }
            _t7 = PsycoBool_FromCondition(po, _t6);
            v = _t7;
            vinfo_decref(res, po);
            return v;
        case PyCmp_NOT_IN:
            res = PsycoSequence_Contains(po, w, v);
            if (res == NULL) {
                return NULL;
            }
            _t9 = integer_non_null(po, res);
            if (_t9 == CC_ERROR) {
                vinfo_decref(res, po);
                return NULL;
            }
            _t9 = INVERT_CC(_t9);
            vinfo_decref(res, po);
            _t10 = PsycoBool_FromCondition(po, _t9);
            v = _t10;
            return v;
        case PyCmp_EXC_MATCH:
            tp_w = Psyco_KnownType(w);
            if (tp_w == NULL) {
                return psyco_generic_call(po, cimpl_cmp_outcome,
                                          CfCommonNewRefPyObject, "lvv",
                                          (long)(op), v, w);
            }
            if ((tp_w->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) != 0) {
                _t11 = PsycoTuple_Load(w);
                if (_t11 < 0 || _t11 > 16) {
                    return psyco_generic_call(po, cimpl_cmp_outcome,
                                              CfCommonNewRefPyObject, "lvv",
                                              (long)(op), v, w);
                }
                length = _t11;
                i = 0;
                for (;;) {
                    if (!(i < length))
                        break;
                    exc = PsycoTuple_GET_ITEM(w, i);
                    tp_exc = Psyco_KnownType(exc);
                    if (tp_exc == NULL) {
                        return psyco_generic_call(po, cimpl_cmp_outcome,
                                                  CfCommonNewRefPyObject,
                                                  "lvv", (long)(op), v, w);
                    }
                    _t12 = (tp_exc->tp_flags & Py_TPFLAGS_TYPE_SUBCLASS) != 0;
                    if (_t12) {
                        if (!is_compiletime(exc->source)) {
                            return psyco_generic_call(po, cimpl_cmp_outcome,
                                                      CfCommonNewRefPyObject,
                                                      "lvv", (long)(op), v, w);
                        }
                        k_exc = (PyTypeObject *)CompileTime_Get(exc->source)
                                    ->value;
                        _t12 = (k_exc->tp_flags &
                                Py_TPFLAGS_BASE_EXC_SUBCLASS) != 0;
                    }
                    if (!(_t12)) {
                        PycException_SetString(po, PyExc_TypeError,
                                               LE_MSG_CATCHING_NON_EXCEPTION);
                        return NULL;
                    }
                    i += 1;
                }
            }
            else {
                _t13 = (tp_w->tp_flags & Py_TPFLAGS_TYPE_SUBCLASS) != 0;
                if (_t13) {
                    if (!is_compiletime(w->source)) {
                        return psyco_generic_call(po, cimpl_cmp_outcome,
                                                  CfCommonNewRefPyObject,
                                                  "lvv", (long)(op), v, w);
                    }
                    k_w = (PyTypeObject *)CompileTime_Get(w->source)->value;
                    _t13 = (k_w->tp_flags & Py_TPFLAGS_BASE_EXC_SUBCLASS) != 0;
                }
                if (!(_t13)) {
                    PycException_SetString(po, PyExc_TypeError,
                                           LE_MSG_CATCHING_NON_EXCEPTION);
                    return NULL;
                }
            }
            res = psyco_generic_call(po, PyErr_GivenExceptionMatches,
                                     CfReturnTypeInt | CfNoPython | CfNoBlock,
                                     "vv", v, w);
            _t15 = integer_non_null(po, res);
            if (_t15 == CC_ERROR) {
                vinfo_decref(res, po);
                return NULL;
            }
            _t16 = PsycoBool_FromCondition(po, _t15);
            v = _t16;
            vinfo_decref(res, po);
            return v;
        default:
            _t17 = PsycoObject_RichCompare(po, v, w, op);
            return _t17;
    }
}
#endif

#if LE_HAS_OPCODE_MATCH_CLASS
static vinfo_t *
match_class(PsycoObject *po, vinfo_t *subject, vinfo_t *type, Py_ssize_t nargs,
            vinfo_t *kwargs)
{
    const char *e;
    PyTypeObject *tp_type;
    tp_type = Psyco_NeedType(po, type);
    if (tp_type == NULL) {
        return NULL;
    }
    if ((tp_type->tp_flags & Py_TPFLAGS_TYPE_SUBCLASS) == 0) {
        e = LE_MSG_MATCH_NOT_A_TYPE;
        PycException_SetFormat(po, PyExc_TypeError, e);
        return NULL;
    }
    return psyco_generic_call(po, cimpl_match_class,
                              CfReturnTypePtr | CfNewRef | CfPyErrCheckNull,
                              "vvlv", subject, type, (long)(nargs), kwargs);
}
#endif

#if LE_HAS_OPCODE_MATCH_KEYS
static vinfo_t *
match_keys(PsycoObject *po, vinfo_t *map, vinfo_t *keys)
{
    int _t1;
    Py_ssize_t _t2;
    vinfo_t *_t3;
    Py_ssize_t nkeys;
    _t1 = PsycoTuple_Load(keys);
    if (_t1 < 0 || _t1 > 16) {
        return psyco_generic_call(po, cimpl_match_keys, CfCommonNewRefPyObject,
                                  "vv", map, keys);
    }
    nkeys = _t1;
    if (!(nkeys != 0)) {
        _t2 = 0;
        _t3 = PsycoTuple_New(_t2, NULL);
        return _t3;
    }
    return psyco_generic_call(po, cimpl_match_keys, CfCommonNewRefPyObject,
                              "vv", map, keys);
}
#endif
#pragma GCC diagnostic pop

/* a global's NameError: ceval's NAME_ERROR_MSG through its own function */
static void
cimpl_name_error(PyObject *w)
{
    cimpl_format_exc_check_arg(PyExc_NameError, LE_MSG_NAME_NOT_DEFINED, w);
}

/* CALL_FUNCTION_EX: f(*args) or f(*args, **kwargs). The stack, top down:
   [kwargs], args, callable. f(*tuple) goes straight to PsycoObject_Call,
   which can specialise the call; anything else runs CPython's whole case
   at run time (its copy cimpl_call_function_ex). */
static vinfo_t *
psyco_ext_do_call_ex(PsycoObject *po, int oparg, vinfo_t **stack_top,
                     int *stack_to_pop)
{
    vinfo_t *kwargs = (oparg & 1) ? stack_top[-1] : NULL;
    vinfo_t *callargs = stack_top[-1 - (oparg & 1)];
    vinfo_t *func = stack_top[-2 - (oparg & 1)];
    vinfo_t *zero = NULL, *result;
    PyTypeObject *t;
    *stack_to_pop = 2 + (oparg & 1);

    t = Psyco_NeedType(po, callargs);
    if (t == NULL)
        return NULL;
    if (kwargs == NULL)
        kwargs = zero = psyco_vi_Zero();
    if (t == &PyTuple_Type && zero != NULL) {
        /* f(*args): the call can be specialised. With **kwargs it goes to the
           run-time helper, which holds a reference to the dict as ceval does
         */
        result = PsycoObject_Call(po, func, callargs, kwargs);
        goto done;
    }
    result =
        psyco_generic_call(po, cimpl_call_function_ex, CfCommonNewRefPyObject,
                           "vvv", func, callargs, kwargs);
done:
    vinfo_xdecref(zero, po);
    return result;
}

/* ceval.c:special_lookup */
static PyObject *
special_lookup(PyObject *o, _Py_Identifier *id)
{
    PyObject *res = le_lookup_special_id(o, id);
    if (res == NULL && !PyErr_Occurred()) {
        PyErr_SetObject(PyExc_AttributeError, _PyUnicode_FromId(id));
        return NULL;
    }
    return res;
}

/* ceval.c SETUP_WITH up to the block setup: out[0] gets the bound
   __exit__, out[1] __enter__'s result. Unlike ceval, mgr stays on the stack
   (one reference more) while __enter__ runs. */
static int
cimpl_setup_with(PyObject *mgr, PyObject *out[])
{
    _Py_IDENTIFIER(__exit__);
    _Py_IDENTIFIER(__enter__);
    PyObject *enter = special_lookup(mgr, &PyId___enter__), *exit, *res;
    if (enter == NULL)
        return -1;
    exit = special_lookup(mgr, &PyId___exit__);
    if (exit == NULL) {
        Py_DECREF(enter);
        return -1;
    }
    res = PyObject_CallObject(enter, NULL);
    Py_DECREF(enter);
    if (res == NULL) {
        Py_DECREF(exit);
        return -1;
    }
    out[0] = exit;
    out[1] = res;
    return 0;
}

/* WITH_CLEANUP_START's call */
static PyObject *
cimpl_call_exit(PyObject *exit_func, PyObject *exc, PyObject *val,
                PyObject *tb)
{
    PyObject *stack[3];
    stack[0] = exc;
    stack[1] = val;
    stack[2] = tb;
    return _PyObject_FastCall(exit_func, stack, 3);
}

static PyObject *
cimpl_load_global(PyObject *globals, PyObject *builtins, PyObject *w)
{
    PyObject *x;
    le_recursion_suspend();
    x = PyDict_GetItem(globals, w);
    if (x == NULL) {
        x = PyDict_GetItem(builtins, w);
        if (x == NULL) {
            cimpl_name_error(w);
            le_recursion_resume();
            return NULL;
        }
    }
    le_recursion_resume();
    Py_INCREF(x);
    return x;
}

/* ceval.c:unpack_iterable (argcntafter -1: no starred target), returning
   0 or -1 and filling sp[] upwards: the first item in sp[0], which the
   caller pushes last, so that it is on top as in ceval */
static int
unpack_iterable(PyObject *v, int argcnt, int argcntafter, PyObject **sp)
{
    int i = 0, j = 0;
    Py_ssize_t ll = 0;
    PyObject *it; /* iter(v) */
    PyObject *w;
    PyObject *l = NULL; /* variable list */

    extra_assert(v != NULL);

    it = PyObject_GetIter(v);
    if (it == NULL) {
        if (PyErr_ExceptionMatches(PyExc_TypeError) &&
            v->ob_type->tp_iter == NULL && !PySequence_Check(v))
            PyErr_Format(PyExc_TypeError, LE_MSG_UNPACK_NON_ITERABLE,
                         v->ob_type->tp_name);
        return -1;
    }

    for (; i < argcnt; i++) {
        w = PyIter_Next(it);
        if (w == NULL) {
            /* Iterator done, via error or exhaustion. */
            if (!PyErr_Occurred()) {
                if (argcntafter == -1)
                    PyErr_Format(PyExc_ValueError, LE_MSG_UNPACK_NOT_ENOUGH,
                                 argcnt, i);
                else
                    PyErr_Format(PyExc_ValueError,
                                 LE_MSG_UNPACK_NOT_ENOUGH_STAR,
                                 argcnt + argcntafter, i);
            }
            goto Error;
        }
        *sp++ = w;
    }

    if (argcntafter == -1) {
        /* We better have exhausted the iterator now. */
        w = PyIter_Next(it);
        if (w == NULL) {
            if (PyErr_Occurred())
                goto Error;
            Py_DECREF(it);
            return 0;
        }
        Py_DECREF(w);
        PyErr_Format(PyExc_ValueError, LE_MSG_UNPACK_TOO_MANY, argcnt);
        goto Error;
    }

    l = PySequence_List(it);
    if (l == NULL)
        goto Error;
    *sp++ = l;
    i++;

    ll = PyList_GET_SIZE(l);
    if (ll < argcntafter) {
        PyErr_Format(PyExc_ValueError, LE_MSG_UNPACK_NOT_ENOUGH_AFTER_STAR,
                     argcnt + argcntafter, argcnt + ll);
        goto Error;
    }

    /* Pop the "after-variable" args off the list. */
    for (j = argcntafter; j > 0; j--, i++) *sp++ = PyList_GET_ITEM(l, ll - j);
    /* Resize the list. */
    ((PyVarObject *)l)->ob_size = ll - argcntafter;
    Py_DECREF(it);
    return 0;

Error:
    for (; i > 0; i--) Py_DECREF(*--sp);
    Py_XDECREF(it);
    return -1;
}

static int
cimpl_unpack_iterable(PyObject *v, long argcnt, PyObject **sp)
{
    return unpack_iterable(v, (int)argcnt, -1, sp);
}

/* UNPACK_EX's oparg: the targets before the star, and after it << 8 */
static int
cimpl_unpack_ex(PyObject *v, long oparg, PyObject **sp)
{
    return unpack_iterable(v, (int)(oparg & 0xFF), (int)(oparg >> 8), sp);
}

static int
cimpl_unpack_list(PyObject *listobject, long argcnt, PyObject **sp)
{
    int i;
    extra_assert(PyList_CheckExact(listobject));

    if (PyList_GET_SIZE(listobject) != argcnt)
        return cimpl_unpack_iterable(listobject, argcnt, sp); /* the error */
    for (i = argcnt; i--;) {
        PyObject *v = sp[i] = PyList_GET_ITEM(listobject, i);
        Py_INCREF(v);
    }
    return 0;
}

/* Logic for the raise statement.
   XXX a meta-implementation would be nice (e.g. to know at compile-time
   which exception class we got, so that we can know at compile-time which
   except: statements match) */
/* 3.7 ceval.c do_raise's reraise, with ceval's traceback rule: a reraised
   exception gets no entry for this frame (ceval goes to exception_unwind),
   so the unwind loop records none, and the RuntimeError
   for no active exception gets its entry here instead (ceval's error:) */
static void
cimpl_reraise(PyCodeObject *code, PyObject *globals, int lasti, int lineno)
{
    PyObject *type, *value, *tb;
    le_exc_info_topmost(PyThreadState_GET(), &type, &value, &tb);
    if (type == Py_None || type == NULL) {
        PyErr_SetString(PyExc_RuntimeError, LE_MSG_NO_ACTIVE_EXCEPTION);
        cimpl_rt_traceback(code, globals, lasti, lineno, NULL);
        return;
    }
    Py_XINCREF(type);
    Py_XINCREF(value);
    Py_XINCREF(tb);
    PyErr_Restore(type, value, tb);
}

/* 3.7 ceval.c do_raise with an exception (the reraise is cimpl_reraise),
   except that it doesn't consume references to its arguments
   (psyco_generic_call can't express that), so it takes its own first */
static void
cimpl_do_raise(PyObject *exc, PyObject *cause)
{
    PyObject *type = NULL, *value = NULL;

    Py_INCREF(exc);
    Py_XINCREF(cause);

    if (PyExceptionClass_Check(exc)) {
        type = exc;
        value = PyObject_CallObject(exc, NULL);
        if (value == NULL)
            goto raise_error;
        if (!PyExceptionInstance_Check(value)) {
            PyErr_Format(PyExc_TypeError, LE_MSG_RAISE_NOT_INSTANCE, type,
                         Py_TYPE(value));
            goto raise_error;
        }
    }
    else if (PyExceptionInstance_Check(exc)) {
        value = exc;
        type = PyExceptionInstance_Class(exc);
        Py_INCREF(type);
    }
    else {
        Py_DECREF(exc);
        PyErr_SetString(PyExc_TypeError, LE_MSG_RAISE_NOT_EXCEPTION);
        goto raise_error;
    }

    if (cause) {
        PyObject *fixed_cause;
        if (PyExceptionClass_Check(cause)) {
            fixed_cause = PyObject_CallObject(cause, NULL);
            if (fixed_cause == NULL)
                goto raise_error;
            Py_DECREF(cause);
        }
        else if (PyExceptionInstance_Check(cause)) {
            fixed_cause = cause;
        }
        else if (cause == Py_None) {
            Py_DECREF(cause);
            fixed_cause = NULL;
        }
        else {
            PyErr_SetString(PyExc_TypeError, LE_MSG_RAISE_CAUSE_NOT_EXCEPTION);
            goto raise_error;
        }
        PyException_SetCause(value, fixed_cause);
    }

    PyErr_SetObject(type, value);
    Py_DECREF(value);
    Py_DECREF(type);
    return;

raise_error:
    Py_XDECREF(value);
    Py_XDECREF(type);
    Py_XDECREF(cause);
}

static PyObject *
cimpl_import_name(PyObject *globals, PyObject *builtins, PyObject *name,
                  PyObject *fromlist, PyObject *fitharg)
{
    PyObject *w;
    PyObject *x = PyDict_GetItemString(builtins, "__import__");
    if (x == NULL) {
        PyErr_SetString(PyExc_ImportError, LE_MSG_NO_IMPORT);
        return NULL;
    }
    if (PyLong_AsLong(fitharg) != -1 || PyErr_Occurred())
        w = PyTuple_Pack(5, name, globals, Py_None, fromlist, fitharg);
    else
        w = Py_BuildValue("(OOOO)", name, globals, Py_None, fromlist);
    if (w == NULL)
        return NULL;
    x = PyEval_CallObject(x, w);
    Py_DECREF(w);
    return x;
}

static int
cimpl_import_all_from(PyObject *locals, PyObject *v)
{
    PyObject *all = PyObject_GetAttrString(v, "__all__");
    PyObject *dict, *name, *value;
    int skip_leading_underscores = 0;
    int pos, err;

    if (all == NULL) {
        if (!PyErr_ExceptionMatches(PyExc_AttributeError))
            return -1; /* Unexpected error */
        PyErr_Clear();
        dict = PyObject_GetAttrString(v, "__dict__");
        if (dict == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_AttributeError))
                return -1;
            PyErr_SetString(PyExc_ImportError, LE_MSG_IMPORT_STAR_NO_DICT);
            return -1;
        }
        all = PyMapping_Keys(dict);
        Py_DECREF(dict);
        if (all == NULL)
            return -1;
        skip_leading_underscores = 1;
    }

    for (pos = 0, err = 0;; pos++) {
        name = PySequence_GetItem(all, pos);
        if (name == NULL) {
            if (!PyErr_ExceptionMatches(PyExc_IndexError))
                err = -1;
            else
                PyErr_Clear();
            break;
        }
        if (skip_leading_underscores && PyUnicode_Check(name) &&
            PyUnicode_AsUTF8(name)[0] == '_') {
            Py_DECREF(name);
            continue;
        }
        value = PyObject_GetAttr(v, name);
        if (value == NULL)
            err = -1;
        else
            err = PyDict_SetItem(locals, name, value);
        Py_DECREF(name);
        Py_XDECREF(value);
        if (err != 0)
            break;
    }
    Py_DECREF(all);
    return err;
}

/***************************************************************/
/***                         Main loop                         ***/
/***************************************************************/

static bool
compute_and_raise_exception(PsycoObject *po)
{
    /* &ERtPython is the case where the code that raised
       the Python exception is already written, e.g. when
       we called a function in the Python interpreter which
       raised an exception. */
    if (!PycException_Is(po, &ERtPython)) {
        /* In the other cases (virtual exception),
           compute and raise the exception now. */
        char args[4];
        args[3] = 0;
        if (po->pr.tb == NULL)
            args[2] = 'l';
        else {
            args[2] = 'v';
            consume_reference(po, po->pr.tb);
        }
        if (po->pr.val == NULL)
            args[1] = 'l';
        else {
            args[1] = 'v';
            consume_reference(po, po->pr.val);
        }
        args[0] = 'v';
        consume_reference(po, po->pr.exc);
        return (psyco_generic_call(po, PyErr_Restore, CfNoReturnValue, args,
                                   po->pr.exc, po->pr.val, po->pr.tb) != NULL);
    }
    else
        return true;
}

/* min_max (bltinmodule.c) without key or default, one item at a time */
static PyObject *
cimpl_minmax_step(PyObject *best, PyObject *item, long op)
{
    if (best != NULL) {
        int c = PyObject_RichCompareBool(item, best, (int)op);
        if (c < 0)
            return NULL;
        if (c == 0) {
            Py_INCREF(best);
            return best;
        }
    }
    Py_INCREF(item);
    return item;
}

static PyObject *
cimpl_minmax_result(PyObject *best, long is_min)
{
    if (best == NULL) {
        PyErr_Format(PyExc_ValueError, LE_MSG_MINMAX_EMPTY,
                     is_min ? LE_MSG_MINMAX_NAME_MIN : LE_MSG_MINMAX_NAME_MAX);
        return NULL;
    }
    Py_INCREF(best);
    return best;
}

static void
cimpl_generator_stop(void)
{
    PyObject *exc, *val, *tb;
    if (!PyErr_ExceptionMatches(PyExc_StopIteration))
        return;
    PyErr_Fetch(&exc, &val, &tb);
    PyErr_NormalizeException(&exc, &val, &tb);
    if (tb != NULL)
        PyException_SetTraceback(val, tb);
    Py_DECREF(exc);
    Py_XDECREF(tb);
    PyErr_SetString(PyExc_RuntimeError, LE_MSG_GENERATOR_STOP_ITERATION);
    /* as _PyErr_ChainExceptions: the new exception's cause and context */
    {
        PyObject *e2, *v2, *t2;
        PyErr_Fetch(&e2, &v2, &t2);
        PyErr_NormalizeException(&e2, &v2, &t2);
        Py_INCREF(val);
        PyException_SetCause(v2, val);
        PyException_SetContext(v2, val);
        PyErr_Restore(e2, v2, t2);
    }
}

static code_t *
exit_function(PsycoObject *po)
{
    vinfo_t **locals_plus;
    vinfo_t **pp;
    Source retsource;
    extra_assert(!po->pr.is_inlining);

    /* the frame that stood for the call keeps its locals as they end */
    if (!catcher_flush(po, true))
        return NULL;
    /* clear the locals, first to last as frame_dealloc does (finalisers run
       in that order), then the stack. On a return CPython has popped the
       frame first. */
    locals_plus = LOC_LOCALS_PLUS;
    for (pp = locals_plus; PycException_Is(po, &EReturn) &&
                           pp < po->vlocals.items + po->vlocals.count;
         pp++)
        if (*pp != NULL && !is_compiletime((*pp)->source)) {
            BEGIN_CODE
            ABOUT_TO_RETURN();
            END_CODE
            break;
        }
    for (pp = locals_plus; pp < po->vlocals.items + po->vlocals.count; pp++)
        if (*pp != NULL) {
            vinfo_decref(*pp, po);
            *pp = NULL;
        }

    if (PycException_Is(po, &EReturn)) {
        /* load the return value */
        vinfo_t *retval = po->pr.val;
        extra_assert(retval != NULL);
        if (!compute_vinfo(retval, po))
            return NULL;
        /* return a new reference */
        consume_reference(po, retval);
        if (retval->array->count > 0) {
            array_delete(retval->array, po);
            retval->array = NullArray;
        }
        retsource = retval->source;
    }
    else {
        if (!compute_and_raise_exception(po))
            return NULL;
        /* PEP 479, as gen_send_ex: StopIteration leaving a generator (a fused
           generator expression) becomes RuntimeError */
        if ((po->pr.co->co_flags & CO_GENERATOR) &&
            !psyco_generic_call(po, cimpl_generator_stop, CfNoReturnValue, ""))
            return NULL;
        clear_pseudo_exception(po);
        sk_incref(&psyco_skZero);
        retsource = CompileTime_NewSk(&psyco_skZero); /*to return NULL */
    }

    return psyco_finish_return(po, retsource);
}

/***************************************************************/
/***   the main loop of the interpreter/compiler.            ***/

#if LE_CODESTATS
/* Code size per instruction (lejit._core._codestats()): for each
   (code object, instruction offset), [opcode, times compiled, bytes, the
   copies as (address, bytes, how the compiler
   was entered: codemanager.h LE_ENTER_*, the promotion site or -1, the
   index of that site's value or -1)]. The
   bytes are those emitted from the start of one instruction to the start of
   the next, so an instruction's error paths and the end of a block count
   with it; replays and code written into other buffers in between
   (respawn and promotion stubs) don't. */
DEFINEVAR PyObject *psyco_codestats = NULL;
DEFINEVAR long psyco_codestats_lost = 0; /* bytes in a jump to a new buffer */

/* The counters run while compiling, where the compiler and the metas read
   the thread's error indicator (a compile-time call that failed): they
   leave it as they found it, and a counter that fails is lost. */
static void
codestats_flush(PsycoObject *po)
{
    if (po->cs_mark != NULL) {
        long n = (char *)po->code - (char *)po->cs_mark;
        PyObject *et, *ev, *etb, *key, *e;
        PyErr_Fetch(&et, &ev, &etb);
        key = Py_BuildValue("(Oi)", po->cs_co, po->cs_instr);
        e = key ? PyDict_GetItem(psyco_codestats, key) : NULL;
        Py_XDECREF(key);
        if (e != NULL && n >= 0 && n < 65536) {
            PyObject *b =
                PyLong_FromLong(PyLong_AsLong(PyList_GET_ITEM(e, 2)) + n);
            PyObject *copy = Py_BuildValue(
                "(kNiii)", (unsigned long)po->cs_mark,
                PyBytes_FromStringAndSize((char *)po->cs_mark, n),
                psyco_stat_compile_kind, psyco_cs_promo_cur, cs_promo_vidx);
            if (b != NULL)
                PyList_SetItem(e, 2, b);
            if (copy != NULL) {
                PyList_Append(PyList_GET_ITEM(e, 3),
                              copy); /* (address, bytes) */
                Py_DECREF(copy);
            }
            else
                psyco_codestats_lost++;
        }
        else
            psyco_codestats_lost++;
        PyErr_Clear();
        PyErr_Restore(et, ev, etb);
        po->cs_mark = NULL;
    }
}

/* the PsycoObject the innermost main loop compiles: every way out of the
   loop frees it (psyco_finish_return, psyco_compile...), so its last
   instruction is flushed as it is freed, not read after */
static PsycoObject *cs_owner = NULL;

DEFINEFN void
psyco_codestats_delete(PsycoObject *po)
{
    if (po == cs_owner) {
        codestats_flush(po);
        cs_owner = NULL;
    }
}

static void
codestats_begin(PsycoObject *po, int instr, int opcode)
{
    PyObject *key, *e, *et, *ev, *etb;
    codestats_flush(po);
    if (is_respawning(po))
        return;
    PyErr_Fetch(&et, &ev, &etb);
    if (psyco_codestats == NULL && (psyco_codestats = PyDict_New()) == NULL)
        OUT_OF_MEMORY();
    key = Py_BuildValue("(Oi)", po->pr.co, instr);
    if (key == NULL)
        OUT_OF_MEMORY();
    e = PyDict_GetItem(psyco_codestats, key);
    if (e == NULL) {
        e = Py_BuildValue("[iii[]]", opcode, 0, 0);
        if (e == NULL || PyDict_SetItem(psyco_codestats, key, e) < 0)
            OUT_OF_MEMORY();
        Py_DECREF(e);
    }
    Py_DECREF(key);
    PyList_SetItem(e, 1,
                   PyLong_FromLong(PyLong_AsLong(PyList_GET_ITEM(e, 1)) + 1));
    PyErr_Clear();
    PyErr_Restore(et, ev, etb);
    po->cs_mark = po->code;
    po->cs_co = po->pr.co;
    po->cs_instr = instr;
}
#endif

/* The eval_breaker test after a call, once its
   result is pushed: only if the call emitted a C call that isn't known to
   be brief and non-blocking (po->blocking_call, set by psyco_generic_call
   unless psyfunc.c:psyco_cannot_block accepts the function). A call that
   was inlined, folded, made virtual or made directly into compiled code
   emits none: that code tests at its own loop heads and after its own
   calls. The test is part of the CALL_*, not of the next instruction: a
   merge point there is matched before the next instruction is compiled,
   and code compiled after a call that needed no test would then serve one
   that did. None when the next instruction tests anyway (a loop head), or
   is one ceval skips, after which mergepoints.c marks a test that is
   always made. False with an exception set. */
static bool
breaker_after_call(PsycoObject *po, int next_instr)
{
    le_code_t c;
    if (!po->blocking_call)
        return true;
    c = le_code(po->pr.co);
    if (next_instr >= c.size || BREAKER_SKIPS(le_op(c, next_instr)) ||
        psyco_breaker_check_at(po->pr.merge_points, next_instr))
        return true;
    return psyco_check_eval_breaker(po);
}

/* an operator that may stand between a fused get and its store */
static bool
fusion_operator(int op)
{
    /* clang-format off */
    static const short ops[] = {
        BINARY_ADD,          BINARY_SUBTRACT,      BINARY_MULTIPLY,
        BINARY_TRUE_DIVIDE,  BINARY_FLOOR_DIVIDE,  BINARY_MODULO,
        BINARY_POWER,        BINARY_MATRIX_MULTIPLY, BINARY_LSHIFT,
        BINARY_RSHIFT,       BINARY_AND,           BINARY_OR,
        BINARY_XOR,          INPLACE_ADD,          INPLACE_SUBTRACT,
        INPLACE_MULTIPLY,    INPLACE_TRUE_DIVIDE,  INPLACE_FLOOR_DIVIDE,
        INPLACE_MODULO,      INPLACE_POWER,        INPLACE_MATRIX_MULTIPLY,
        INPLACE_LSHIFT,      INPLACE_RSHIFT,       INPLACE_AND,
        INPLACE_OR,          INPLACE_XOR};
    /* clang-format on */
    size_t i;
    for (i = 0; i < sizeof(ops) / sizeof(ops[0]); i++)
        if (op == ops[i])
            return true;
    return false;
}

/* The STORE_SUBSCR fused with the get at byte offset 'i', or -1. The
   forms, d and k local variables, y a constant or a local:
     d[k] = d.get(k[, x]) OP y   LOAD_FAST d, LOAD_METHOD get, LOAD_FAST k,
                                 [LOAD_CONST/LOAD_FAST x,] CALL_METHOD (i),
                                 LOAD y, OP, LOAD_FAST d, LOAD_FAST k,
                                 STORE_SUBSCR
     d[k] = d[k] OP y            LOAD_FAST d, LOAD_FAST k, BINARY_SUBSCR (i),
                                 LOAD y, OP, LOAD_FAST d, LOAD_FAST k,
                                 STORE_SUBSCR
     d[k] OP= y                  DUP_TOP_TWO, BINARY_SUBSCR (i), LOAD y, OP,
                                 ROT_THREE, STORE_SUBSCR
   with no merge point from the instruction after the first to the store,
   so the store is reached only through the get. Only loads stand between
   the two besides OP, so d and k are the get's objects. */
static int
fused_store(PsycoObject *po, int i)
{
    PyCodeObject *co = po->pr.co;
    le_code_t c = le_code(co);
    int n = c.size, first, s, d, k;
    mergepoint_t *mp;
    /* the unit 'u' units after i (before it, if negative) */
#define OP(u) le_op(c, i + (u) * LE_UNIT)
#define ARG(u) le_arg(c, i + (u) * LE_UNIT)
#define FUSION_LOAD(u) (OP(u) == LOAD_CONST || OP(u) == LOAD_FAST)
    if (i < LE_UNIT || i + 4 * LE_UNIT >= n || !FUSION_LOAD(1) ||
        !fusion_operator(OP(2)))
        return -1;
    if (OP(0) == BINARY_SUBSCR && OP(-1) == DUP_TOP_TWO) {
        if (OP(3) != ROT_THREE || OP(4) != STORE_SUBSCR)
            return -1;
        first = i;
        s = i + 4 * LE_UNIT;
    }
    else {
        int kpos, dpos; /* in units from i */
        if (OP(0) == BINARY_SUBSCR) {
            kpos = -1;
        }
        else if (OP(0) == CALL_METHOD && (ARG(0) == 1 || ARG(0) == 2)) {
            kpos = -ARG(0);
            if ((ARG(0) == 2 && !FUSION_LOAD(-1)) ||
                i + (kpos - 1) * LE_UNIT < 0 || OP(kpos - 1) != LOAD_METHOD ||
                PyUnicode_CompareWithASCIIString(
                    PyTuple_GET_ITEM(co->co_names, ARG(kpos - 1)), "get") != 0)
                return -1;
        }
        else
            return -1;
        first = i + (OP(0) == BINARY_SUBSCR ? kpos : kpos - 1) * LE_UNIT;
        dpos = (first - i) / LE_UNIT - 1;
        if (i + dpos * LE_UNIT < 0 || OP(dpos) != LOAD_FAST ||
            OP(kpos) != LOAD_FAST)
            return -1;
        d = ARG(dpos);
        k = ARG(kpos);
        if (i + 5 * LE_UNIT >= n || OP(3) != LOAD_FAST || ARG(3) != d ||
            OP(4) != LOAD_FAST || ARG(4) != k || OP(5) != STORE_SUBSCR)
            return -1;
        s = i + 5 * LE_UNIT;
    }
#undef FUSION_LOAD
#undef ARG
#undef OP
    mp = psyco_next_merge_point(po->pr.merge_points, first);
    if (mp->bytecode_position >= 0 && mp->bytecode_position <= s)
        return -1;
    return s;
}

/* The get then store of one str key in one dict (pdictobject.c's probe):
   the get records the slot it finds and the store reuses it if the dict
   is unchanged (its keys and version tag), checked at run time, else
   probes again. Called for BINARY_SUBSCR, CALL_METHOD and STORE_SUBSCR at
   'next_instr' less one instruction; 1 when done, 0 for the translated
   case, -1 on an error. A get of the forms above that is not this probe
   clears the record, so that the store never meets an older one. */
static int
dict_str_fusion(PsycoObject *po, int next_instr)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int i = next_instr - LE_UNIT;
    le_code_t c = le_code(po->pr.co);
    int op = le_op(c, i), argc = le_arg(c, i), j;
    vinfo_t *d, *k, *res;
    if (op == STORE_SUBSCR) {
        if (!(i >= 5 * LE_UNIT && fused_store(po, i - 5 * LE_UNIT) == i) &&
            !(i >= 4 * LE_UNIT && fused_store(po, i - 4 * LE_UNIT) == i))
            return 0;
        d = NTOP(2);
        k = NTOP(1);
        if (!PsycoDict_StrKey(d, k))
            return 0;
        if (!PsycoDict_StrSetItem(po, d, k, NTOP(3), true))
            return -1;
        for (j = 3; j--;) POP_DECREF();
        return 1;
    }
    if (fused_store(po, i) < 0)
        return 0;
    if (op == BINARY_SUBSCR) {
        d = NTOP(2);
        k = NTOP(1);
        if (Psyco_NeedType(po, d) == NULL)
            return -1;
    }
    else {
        /* the LOAD_FAST d's */
        d = GETLOCAL(le_arg(c, i - (argc + 2) * LE_UNIT));
        k = NTOP(argc);
    }
    if (!PsycoDict_StrKey(d, k))
        return PsycoDict_StrForget(po) ? 0 : -1;
    if (op == BINARY_SUBSCR) {
        res = PsycoDict_StrGetItem(po, d, k, true);
        if (res == NULL)
            return -1;
        POP_DECREF();
        POP_DECREF();
        PUSH(res);
        return 1;
    }
    po->pr.call_site = i + 1;
    res = PsycoDict_StrGet(po, d, k, argc == 2 ? NTOP(1) : NULL);
    end_call_site(po);
    if (res == NULL)
        return -1;
    for (j = argc + 2; j--;) /* args, the method and the NULL */
        POP_DECREF();
    PUSH(res);
    return breaker_after_call(po, next_instr) ? 1 : -1;
}

/* the 3-tuple a finally block entered by EReturn, EBreak or EContinue has
   on the stack where ceval has other items */
static bool
is_pseudo_exception_tuple(vinfo_t *v)
{
    vinfo_t *e;
    if (v == NULL || !is_virtualtime(v->source) ||
        Psyco_KnownType(v) != &PyTuple_Type || PsycoTuple_Load(v) != 3)
        return false;
    e = PsycoTuple_GET_ITEM(v, 0);
    return e != NULL && is_virtualtime(e->source) &&
           (VirtualTime_Get(e->source) == &EReturn ||
            VirtualTime_Get(e->source) == &EBreak ||
            VirtualTime_Get(e->source) == &EContinue);
}

/* After an instruction that made a call that may run Python code: if that
   started tracing or profiling, the frame goes on interpreted from the
   next instruction, as a frame ceval was running would get its events
   from there. The rare path, compiled when first taken,
   copies the frame's state into a frame object for ceval
   (frames.c:psyco_resume_run), then ends this frame with ceval's result:
   ceval has run its handlers and recorded its traceback entry, so the
   blocks are dropped and '*unwound' is set. Not inside a
   fused generator expression, nor while a finally block entered by a
   pseudo-exception holds its 3-tuple. 1 to go on, 0 with a
   pseudo-exception set. */
static int
resume_in_ceval(PsycoObject *po, int next_instr, int *unwound);

static int
resume_if_tracing(PsycoObject *po, int next_instr, int *unwound)
{
    int on;
    if (!psyco_can_go_interpreted(po))
        return 1;
    on = psyco_tracing_on(po);
    if (on <= 0)
        return on == 0;
    return resume_in_ceval(po, next_instr, unwound);
}

DEFINEFN
bool
psyco_can_go_interpreted(PsycoObject *po)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int i, nstack = STACK_LEVEL();
    if (po->pr.co->co_flags & (CO_GENERATOR | CO_COROUTINE |
                               CO_ITERABLE_COROUTINE | CO_ASYNC_GENERATOR))
        return false;
    for (i = 0; i < nstack; i++)
        if (is_pseudo_exception_tuple(stack_a[i]))
            return false;
    return true;
}

/* The rare path itself: ceval runs the frame from 'next_instr' and this
   frame ends with its result. 0, with EReturn or an exception set. */
static int
resume_in_ceval(PsycoObject *po, int next_instr, int *unwound)
{
    PyCodeObject *co = po->pr.co;
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int nlocals = le_code_nlocalsplus(co);
    int nstack = STACK_LEVEL();
    /* an inlined frame has no blocks of its own: those in 'pr' are its
       caller's, which goes on compiled once the call returns */
    int nblocks = po->pr.is_inlining ? 0 : po->pr.iblock;
    int i;
    vinfo_t *st, *result, *word = catcher_word(po);

    st = psyco_generic_call(
        po, psyco_resume_begin, CfReturnTypePtr | CfPyErrIfNull,
        word != NULL ? "lvvlr" : "lvvll", co, LOC_GLOBALS, LOC_BUILTINS,
        (long)(nlocals + nstack), (long)word);
    if (st == NULL)
        return 0;
    for (i = 0; i < nlocals + nstack; i++) {
        vinfo_t *v = i < nlocals ? LOC_LOCALS_PLUS[i] : stack_a[i - nlocals];
        vinfo_t *ok =
            v == NULL
                ? psyco_generic_call(po, psyco_resume_value, CfNoReturnValue,
                                     "vll", st, (long)i, 0L)
                : psyco_generic_call(po, psyco_resume_value, CfNoReturnValue,
                                     "vlv", st, (long)i, v);
        if (ok == NULL) {
            vinfo_decref(st, po);
            return 0;
        }
    }
#if LE_HAS_EXCEPTION_TABLE
    (void)nblocks; /* ceval has no block stack */
#else
    for (i = 0; i < nblocks; i++) {
        le_block_t *b = &po->pr.blockstack[i];
        if (psyco_generic_call(po, psyco_resume_block, CfNoReturnValue, "vlll",
                               st, (long)b->b_type, (long)b->b_handler,
                               (long)b->b_level) == NULL) {
            vinfo_decref(st, po);
            return 0;
        }
    }
#endif
    result = psyco_generic_call(
        po, psyco_resume_run, CfCommonNewRefPyObject, "vll", st,
        (long)(next_instr - sizeof(_Py_CODEUNIT)), (long)nstack);
    vinfo_decref(st, po);
    if (result == NULL && !PycException_Is(po, &ERtPython))
        return 0;
    while (STACK_LEVEL() > 0) POP_DECREF();
    if (!po->pr.is_inlining)
        po->pr.iblock = 0;
    *unwound = 1;
    if (result != NULL)
        PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), result);
    return 0;
}

/* psyco_pycompiler_mainloop() compiles the function,
   writing function entry and exit points if needed, and destroys 'po' when
   it is done */
DEFINEFN
code_t *
psyco_pycompiler_mainloop(PsycoObject *po)
{
    /* 'stack_a' is the Python stack base pointer */
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int opcode = 0; /* Current opcode */
    int oparg = 0;  /* Current opcode argument, if any */
    code_t *code1;
    int saved_rec_limit;
    int unwound = 0;  /* the frame went on interpreted (resume_if_tracing) */
    int reraised = 0; /* exception_unwind: an exception raised again */
    int resumed = 0;
#if LE_CODESTATS
    PsycoObject *cs_saved_owner = cs_owner;
    cs_owner = po;
#endif

    /* save and restore the current Python exception throughout compilation */
    PyObject *old_py_exc, *old_py_val, *old_py_tb;
    PyErr_Fetch(&old_py_exc, &old_py_val, &old_py_tb);

    /* hack: protection against obscure failures during compilation caused by
       the CPython recursion depth being very close to the limit */
    saved_rec_limit = Py_GetRecursionLimit();
    Py_SetRecursionLimit(saved_rec_limit + 50);

    /* a frame with handlers has its catcher word (catcher_word) */
    if ((psyco_mp_flags(po->pr.merge_points) &
         (MP_FLAGS_HAS_EXCEPT | MP_FLAGS_HAS_FINALLY)) != 0 &&
        (LOC_CONTINUATION->array == NullArray))
        psyco_emit_header(po, 1);

    int at_jump_target = 0;
    if (po->pr.next_instr < -1) {
        /* there is a pending POP operation to be done: this is the target of a
           POP_JUMP_IF_xxx. With the pop done the state is the target's, so its
           merge point (often a loop head, for a jump back) is checked before
           the first instruction; otherwise the path would be compiled afresh
           up to the next merge point, every back-edge a new copy of the loop
         */
        POP_DECREF();
        po->pr.next_instr &= ~NEXT_INSTR_POP;
        at_jump_target = !is_respawning(po);
    }

    while (1) {
        /* 'co' is the code object we are interpreting/compiling */
        PyCodeObject *co = po->pr.co;
        le_code_t bytecode = le_code(co);
        vinfo_t *u, *v, /* temporary objects    */
            *w, *x;     /* popped off the stack */
        condition_code_t cc;
        /* 'next_instr' is the position in the byte-code of the next instr */
        int next_instr = po->pr.next_instr;
        mergepoint_t *mp =
            psyco_next_merge_point(po->pr.merge_points, next_instr + 1);

        if (at_jump_target) {
            at_jump_target = 0;
            mp = psyco_next_merge_point(po->pr.merge_points, next_instr);
            goto fine; /* the merge point check, before any instruction */
        }

        /* main loop */
        while (1) {
            extra_assert(!PycException_Occurred(po));
            psyco_assert_coherent(po); /* this test is expensive */

            /* are we potentially running out of stack space? */
            CHECK_STACK_SPACE();

            /* save 'next_instr' and 'respawn_cnt' */
            SAVE_NEXT_INSTR(next_instr); /* could be optimized, not needed in
                                            the case of an opcode that cannot
                                            set run-time conditions */

            /* a type-promotion site's megamorphic version: ceval runs the
               frame from this instruction, which the promotion restarted
               (Psyco_NeedType). A replay from a snapshot taken here does the
               same, as the flag is part of the snapshot. */
            if (po->pr.go_interpreted) {
                po->pr.go_interpreted = 0;
                if (!resume_in_ceval(po, next_instr, &unwound))
                    break;
            }

#if LE_CODESTATS
            codestats_begin(po, next_instr, le_op(bytecode, next_instr));
#endif
            /* no Python error is pending between instructions: a meta that
               tests for one would decide otherwise in a respawn's replay */
            extra_assert(!PyErr_Occurred());
            opcode = le_op(bytecode, next_instr);
            oparg = le_arg(bytecode, next_instr);
            next_instr += LE_UNIT;
            /* at loop heads and after calls */
            if (psyco_breaker_check_at(po->pr.merge_points,
                                       po->pr.next_instr) &&
                !psyco_check_eval_breaker(po))
                break;
            po->blocking_call = 0;
            po->pr.python_call = 0;
        dispatch_opcode:
            next_instr += LE_CACHE_BYTES(opcode);

            /* Main switch on opcode */

            /* !!IMPORTANT!!
               No operation with side-effects must be performed before we are
               sure the compilation of the current instruction succeeded!
               Indeed, if the compilation is interrupted by a C++ exception,
               it will be restarted by re-running psyco_pycompiler_mainloop()
               and this will restart the compilation of the instruction from
               the beginning. In particular, use POP() with care. Better use
               TOP() to get the arguments off the stack and call POP() at the
               end when you are sure everything when fine. */

            /* All opcodes found here must also be listed in mergepoints.c.
               Python code objects using a bytecode instruction not listed
               in mergepoints.c are never Psyco-ified. */

            if (opcode == BINARY_SUBSCR || opcode == CALL_METHOD ||
                opcode == STORE_SUBSCR) {
                int done = dict_str_fusion(po, next_instr);
                if (done < 0)
                    break;
                if (done > 0)
                    goto fine;
            }

            /* the translated BINARY_ADD and INPLACE_ADD keep ceval's generic
               arm only */
            if (opcode == BINARY_ADD || opcode == INPLACE_ADD) {
                int done = unicode_concatenate(po, next_instr);
                if (done < 0)
                    break; /* as a case's error */
                if (done > 0)
                    goto fine;
            }

            if (opcode == FORMAT_VALUE || opcode == BUILD_STRING) {
                int done = fstring_part(po, opcode, oparg, next_instr);
                if (done < 0)
                    break;
                if (done > 0)
                    goto fine;
            }

            switch (opcode) {
                /* case STOP_CODE: this is an error! */

                /* the opcodes translated from ceval.c */
                /* the translation keeps CPython's unused variables */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
                case NOP: {
                    goto fine;
                }

                case LOAD_FAST: {
                    vinfo_t *value;
                    value = GETLOCAL(oparg);
                    if (psyco_knowntobe(value, 0)) {
                        ct_format_exc_check_arg(po, PyExc_UnboundLocalError,
                                                LE_MSG_LOCAL_UNBOUND,
                                                le_code_local_name(co, oparg));
                        break;
                    }
                    vinfo_incref(value);
                    PUSH(value);
                    goto fine;
                }

                case LOAD_CONST: {
                    vinfo_t *value;
                    value = vinfo_new(CompileTime_New((long)GETCONST(oparg)));
                    PUSH(value);
                    goto fine;
                }

                case STORE_FAST: {
                    vinfo_t *value;
                    value = NTOP(1);
                    if (!catcher_local_readable(po, value))
                        break;
                    SETLOCAL(oparg, value);
                    POP(value);
                    goto fine;
                }

                case POP_TOP: {
                    vinfo_t *value;
                    value = NTOP(1);
                    POP(value);
                    vinfo_decref(value, po);
                    goto fine;
                }

                case ROT_TWO: {
                    vinfo_t *second;
                    vinfo_t *top;
                    top = NTOP(1);
                    second = NTOP(2);
                    POP(top);
                    POP(second);
                    PUSH(top);
                    PUSH(second);
                    goto fine;
                }

                case ROT_THREE: {
                    vinfo_t *second;
                    vinfo_t *third;
                    vinfo_t *top;
                    top = NTOP(1);
                    second = NTOP(2);
                    third = NTOP(3);
                    POP(top);
                    POP(second);
                    POP(third);
                    PUSH(top);
                    PUSH(third);
                    PUSH(second);
                    goto fine;
                }

#if LE_HAS_OPCODE_ROT_FOUR
                case ROT_FOUR: {
                    vinfo_t *fourth;
                    vinfo_t *second;
                    vinfo_t *third;
                    vinfo_t *top;
                    top = NTOP(1);
                    second = NTOP(2);
                    third = NTOP(3);
                    fourth = NTOP(4);
                    POP(top);
                    POP(second);
                    POP(third);
                    POP(fourth);
                    PUSH(top);
                    PUSH(fourth);
                    PUSH(third);
                    PUSH(second);
                    goto fine;
                }
#endif

                case DUP_TOP: {
                    vinfo_t *top;
                    top = NTOP(1);
                    vinfo_incref(top);
                    PUSH(top);
                    goto fine;
                }

                case DUP_TOP_TWO: {
                    vinfo_t *second;
                    vinfo_t *top;
                    top = NTOP(1);
                    second = NTOP(2);
                    vinfo_incref(top);
                    vinfo_incref(second);
                    PUSH(second);
                    PUSH(top);
                    goto fine;
                }

                /* the frame's start (LeCode_Run tested eval_breaker), or a
                   generator's resumption, which isn't compiled */
                case RESUME:
                    goto fine;

                case COPY: {
                    vinfo_t *bottom = NTOP(oparg);
                    vinfo_incref(bottom);
                    PUSH(bottom);
                    goto fine;
                }

                case SWAP: {
                    vinfo_t *top = NTOP(1);
                    stack_a[po->pr.stack_level - 1] = NTOP(oparg);
                    stack_a[po->pr.stack_level - oparg] = top;
                    goto fine;
                }

                case PUSH_NULL:
                    PUSH(psyco_vi_Zero());
                    goto fine;

#if LE_HAS_OPCODE_BINARY_OP
                /* an operator by its NB_ number: compiled as the opcode
                   that had it before (absent here, so its number is
                   free) */
                case BINARY_OP: {
                    static const short ops[] = {
                        [NB_ADD] = BINARY_ADD,
                        [NB_AND] = BINARY_AND,
                        [NB_FLOOR_DIVIDE] = BINARY_FLOOR_DIVIDE,
                        [NB_LSHIFT] = BINARY_LSHIFT,
                        [NB_MATRIX_MULTIPLY] = BINARY_MATRIX_MULTIPLY,
                        [NB_MULTIPLY] = BINARY_MULTIPLY,
                        [NB_REMAINDER] = BINARY_MODULO,
                        [NB_OR] = BINARY_OR,
                        [NB_POWER] = BINARY_POWER,
                        [NB_RSHIFT] = BINARY_RSHIFT,
                        [NB_SUBTRACT] = BINARY_SUBTRACT,
                        [NB_TRUE_DIVIDE] = BINARY_TRUE_DIVIDE,
                        [NB_XOR] = BINARY_XOR,
                        [NB_INPLACE_ADD] = INPLACE_ADD,
                        [NB_INPLACE_AND] = INPLACE_AND,
                        [NB_INPLACE_FLOOR_DIVIDE] = INPLACE_FLOOR_DIVIDE,
                        [NB_INPLACE_LSHIFT] = INPLACE_LSHIFT,
                        [NB_INPLACE_MATRIX_MULTIPLY] = INPLACE_MATRIX_MULTIPLY,
                        [NB_INPLACE_MULTIPLY] = INPLACE_MULTIPLY,
                        [NB_INPLACE_REMAINDER] = INPLACE_MODULO,
                        [NB_INPLACE_OR] = INPLACE_OR,
                        [NB_INPLACE_POWER] = INPLACE_POWER,
                        [NB_INPLACE_RSHIFT] = INPLACE_RSHIFT,
                        [NB_INPLACE_SUBTRACT] = INPLACE_SUBTRACT,
                        [NB_INPLACE_TRUE_DIVIDE] = INPLACE_TRUE_DIVIDE,
                        [NB_INPLACE_XOR] = INPLACE_XOR};
                    extra_assert(oparg >= 0 &&
                                 oparg < (int)(sizeof(ops) / sizeof(ops[0])));
                    opcode = ops[oparg];
                    goto dispatch_opcode;
                }
#endif

                case UNARY_POSITIVE: {
                    vinfo_t *res;
                    vinfo_t *value;
                    value = NTOP(1);
                    res = PsycoNumber_Positive(po, value);
                    if (res == NULL) {
                        break;
                    }
                    POP(value);
                    vinfo_decref(value, po);
                    PUSH(res);
                    goto fine;
                }

                case UNARY_NEGATIVE: {
                    vinfo_t *res;
                    vinfo_t *value;
                    value = NTOP(1);
                    res = PsycoNumber_Negative(po, value);
                    if (res == NULL) {
                        break;
                    }
                    POP(value);
                    vinfo_decref(value, po);
                    PUSH(res);
                    goto fine;
                }

                case UNARY_NOT: {
                    condition_code_t _t2;
                    vinfo_t *_t3;
                    vinfo_t *err;
                    vinfo_t *value;
                    value = NTOP(1);
                    err = PsycoObject_IsTrue(po, value);
                    if (err == NULL) {
                        break;
                    }
                    _t2 = integer_non_null(po, err);
                    if (_t2 == CC_ERROR) {
                        vinfo_decref(err, po);
                        break;
                    }
                    vinfo_decref(err, po);
                    _t3 = PsycoBool_FromCondition(po, INVERT_CC(_t2));
                    POP(value);
                    vinfo_decref(value, po);
                    PUSH(_t3);
                    goto fine;
                }

                case UNARY_INVERT: {
                    vinfo_t *res;
                    vinfo_t *value;
                    value = NTOP(1);
                    res = PsycoNumber_Invert(po, value);
                    if (res == NULL) {
                        break;
                    }
                    POP(value);
                    vinfo_decref(value, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_POWER: {
                    vinfo_t *_t1;
                    vinfo_t *base;
                    vinfo_t *exp;
                    vinfo_t *res;
                    exp = NTOP(1);
                    base = NTOP(2);
                    _t1 = psyco_vi_None();
                    res = PsycoNumber_Power(po, base, exp, _t1);
                    vinfo_decref(_t1, po);
                    if (res == NULL) {
                        break;
                    }
                    POP(exp);
                    POP(base);
                    vinfo_decref(exp, po);
                    vinfo_decref(base, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_MULTIPLY: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_Multiply(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_MATRIX_MULTIPLY: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_MatrixMultiply(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_TRUE_DIVIDE: {
                    vinfo_t *dividend;
                    vinfo_t *divisor;
                    vinfo_t *quotient;
                    divisor = NTOP(1);
                    dividend = NTOP(2);
                    quotient = PsycoNumber_TrueDivide(po, dividend, divisor);
                    if (quotient == NULL) {
                        break;
                    }
                    POP(divisor);
                    POP(dividend);
                    vinfo_decref(divisor, po);
                    vinfo_decref(dividend, po);
                    PUSH(quotient);
                    goto fine;
                }

                case BINARY_FLOOR_DIVIDE: {
                    vinfo_t *dividend;
                    vinfo_t *divisor;
                    vinfo_t *quotient;
                    divisor = NTOP(1);
                    dividend = NTOP(2);
                    quotient = PsycoNumber_FloorDivide(po, dividend, divisor);
                    if (quotient == NULL) {
                        break;
                    }
                    POP(divisor);
                    POP(dividend);
                    vinfo_decref(divisor, po);
                    vinfo_decref(dividend, po);
                    PUSH(quotient);
                    goto fine;
                }

                case BINARY_MODULO: {
                    vinfo_t *dividend;
                    vinfo_t *divisor;
                    vinfo_t *res;
                    divisor = NTOP(1);
                    dividend = NTOP(2);
                    res = PsycoNumber_Remainder(po, dividend, divisor);
                    if (res == NULL) {
                        break;
                    }
                    POP(divisor);
                    POP(dividend);
                    vinfo_decref(divisor, po);
                    vinfo_decref(dividend, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_ADD: {
                    vinfo_t *left;
                    vinfo_t *right;
                    vinfo_t *sum;
                    right = NTOP(1);
                    left = NTOP(2);
                    sum = PsycoNumber_Add(po, left, right);
                    if (sum == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(sum);
                    goto fine;
                }

                case BINARY_SUBTRACT: {
                    vinfo_t *diff;
                    vinfo_t *left;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    diff = PsycoNumber_Subtract(po, left, right);
                    if (diff == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(diff);
                    goto fine;
                }

                case BINARY_SUBSCR: {
                    vinfo_t *container;
                    vinfo_t *res;
                    vinfo_t *sub;
                    sub = NTOP(1);
                    container = NTOP(2);
                    res = PsycoObject_GetItem(po, container, sub);
                    if (res == NULL) {
                        break;
                    }
                    POP(sub);
                    POP(container);
                    vinfo_decref(sub, po);
                    vinfo_decref(container, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_LSHIFT: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_Lshift(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_RSHIFT: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_Rshift(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_AND: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_And(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_XOR: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_Xor(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case BINARY_OR: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_Or(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case LIST_APPEND: {
                    bool err;
                    vinfo_t *list;
                    vinfo_t *v;
                    v = NTOP(1);
                    list = NTOP(oparg + 1);
                    err = PsycoList_Append(po, list, v);
                    if (!(err)) {
                        break;
                    }
                    POP(v);
                    vinfo_decref(v, po);
                    goto fine;
                }

                case SET_ADD: {
                    vinfo_t *err;
                    vinfo_t *set;
                    vinfo_t *v;
                    v = NTOP(1);
                    set = NTOP(oparg + 1);
                    err = psyco_generic_call(po, PySet_Add, CfCommonIntZeroOk,
                                             "vv", set, v);
                    if (err == NULL) {
                        break;
                    }
                    POP(v);
                    vinfo_decref(v, po);
                    goto fine;
                }

                case INPLACE_POWER: {
                    vinfo_t *_t1;
                    vinfo_t *base;
                    vinfo_t *exp;
                    vinfo_t *res;
                    exp = NTOP(1);
                    base = NTOP(2);
                    _t1 = psyco_vi_None();
                    res = PsycoNumber_InPlacePower(po, base, exp, _t1);
                    vinfo_decref(_t1, po);
                    if (res == NULL) {
                        break;
                    }
                    POP(exp);
                    POP(base);
                    vinfo_decref(exp, po);
                    vinfo_decref(base, po);
                    PUSH(res);
                    goto fine;
                }

                case INPLACE_MULTIPLY: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_InPlaceMultiply(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case INPLACE_MATRIX_MULTIPLY: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_InPlaceMatrixMultiply(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case INPLACE_TRUE_DIVIDE: {
                    vinfo_t *dividend;
                    vinfo_t *divisor;
                    vinfo_t *quotient;
                    divisor = NTOP(1);
                    dividend = NTOP(2);
                    quotient =
                        PsycoNumber_InPlaceTrueDivide(po, dividend, divisor);
                    if (quotient == NULL) {
                        break;
                    }
                    POP(divisor);
                    POP(dividend);
                    vinfo_decref(divisor, po);
                    vinfo_decref(dividend, po);
                    PUSH(quotient);
                    goto fine;
                }

                case INPLACE_FLOOR_DIVIDE: {
                    vinfo_t *dividend;
                    vinfo_t *divisor;
                    vinfo_t *quotient;
                    divisor = NTOP(1);
                    dividend = NTOP(2);
                    quotient =
                        PsycoNumber_InPlaceFloorDivide(po, dividend, divisor);
                    if (quotient == NULL) {
                        break;
                    }
                    POP(divisor);
                    POP(dividend);
                    vinfo_decref(divisor, po);
                    vinfo_decref(dividend, po);
                    PUSH(quotient);
                    goto fine;
                }

                case INPLACE_MODULO: {
                    vinfo_t *left;
                    vinfo_t *mod;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    mod = PsycoNumber_InPlaceRemainder(po, left, right);
                    if (mod == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(mod);
                    goto fine;
                }

                case INPLACE_ADD: {
                    vinfo_t *left;
                    vinfo_t *right;
                    vinfo_t *sum;
                    right = NTOP(1);
                    left = NTOP(2);
                    sum = PsycoNumber_InPlaceAdd(po, left, right);
                    if (sum == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(sum);
                    goto fine;
                }

                case INPLACE_SUBTRACT: {
                    vinfo_t *diff;
                    vinfo_t *left;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    diff = PsycoNumber_InPlaceSubtract(po, left, right);
                    if (diff == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(diff);
                    goto fine;
                }

                case INPLACE_LSHIFT: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_InPlaceLshift(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case INPLACE_RSHIFT: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_InPlaceRshift(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case INPLACE_AND: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_InPlaceAnd(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case INPLACE_XOR: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_InPlaceXor(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case INPLACE_OR: {
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoNumber_InPlaceOr(po, left, right);
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

                case STORE_SUBSCR: {
                    vinfo_t *container;
                    bool err;
                    vinfo_t *sub;
                    vinfo_t *v;
                    sub = NTOP(1);
                    container = NTOP(2);
                    v = NTOP(3);
                    err = PsycoObject_SetItem(po, container, sub, v);
                    if (!(err)) {
                        break;
                    }
                    POP(sub);
                    POP(container);
                    POP(v);
                    vinfo_decref(sub, po);
                    vinfo_decref(container, po);
                    vinfo_decref(v, po);
                    goto fine;
                }

                case DELETE_SUBSCR: {
                    vinfo_t *container;
                    bool err;
                    vinfo_t *sub;
                    sub = NTOP(1);
                    container = NTOP(2);
                    err = PsycoObject_DelItem(po, container, sub);
                    if (!(err)) {
                        break;
                    }
                    POP(sub);
                    POP(container);
                    vinfo_decref(sub, po);
                    vinfo_decref(container, po);
                    goto fine;
                }

                case PRINT_EXPR: {
#if LE_HAS_INLINE_CALL_TSTATE_GET
                    vinfo_t *_t1;
                    vinfo_t *_t2;
                    vinfo_t *hook;
                    vinfo_t *res;
#else
                    int _t1;
                    int _t2;
                    int _t3;
                    vinfo_array_t *_t5;
#endif
                    vinfo_t *value;
                    value = NTOP(1);
#if LE_HAS_INLINE_CALL_TSTATE_GET
                    _t1 = psyco_generic_call(po, PySys_GetObject,
                                             CfCommonNewRefPyObject, "l",
                                             (long)"displayhook");
                    hook = _t1;
                    if (hook == NULL) {
                        PycException_SetString(po, PyExc_RuntimeError,
                                               LE_MSG_NO_DISPLAYHOOK);
                        break;
                    }
                    _t2 = psyco_generic_call(po, PyObject_CallOneArg,
                                             CfCommonNewRefPyObject, "vv",
                                             hook, value);
                    res = _t2;
                    vinfo_decref(hook, po);
                    if (res == NULL) {
                        break;
                    }
                    vinfo_decref(res, po);
                    POP(value);
                    vinfo_decref(value, po);
#else
                    _t1 = 1;
                    _t2 = _t1 + PyCompile_OpcodeStackEffect(70, oparg);
                    _t5 = array_new(_t2);
                    if (!psyco_generic_call(po, cimpl_print_expr_r0,
                                            CfCommonIntZeroOk, "vlvA", NTOP(1),
                                            (long)oparg, value, _t5)) {
                        array_release(_t5);
                        break;
                    }
                    {
                        int n = _t1;
                        while (n--) POP_DECREF();
                    }
                    for (_t3 = 0; _t3 < _t2; _t3++) PUSH(_t5->items[_t3]);
                    array_release(_t5);
#endif
                    goto fine;
                }

                case GET_AITER: {
                    unaryfunc _t1;
                    int _t10;
                    vinfo_array_t *_t12;
                    unaryfunc _t13;
                    int _t14;
                    int _t15;
                    int _t16;
                    vinfo_array_t *_t18;
                    int _t19;
                    int _t2;
                    int _t3;
                    int _t4;
                    vinfo_array_t *_t6;
                    int _t8;
                    int _t9;
                    unaryfunc getter;
                    vinfo_t *iter;
                    vinfo_t *obj;
                    PyTypeObject *tp_iter;
                    PyTypeObject *tp_obj;
                    PyTypeObject *type;
                    getter = NULL;
                    obj = NTOP(1);
                    tp_obj = Psyco_NeedType(po, obj);
                    if (tp_obj == NULL) {
                        break;
                    }
                    type = tp_obj;
                    if ((type->tp_as_async) != NULL) {
                        switch (Psyco_TypeSlot(po, tp_obj,
                                               &(type->tp_as_async)->am_aiter,
                                               (void **)&_t1)) {
                            case 1:
                                break;
                            case 0:
                                _t2 = 1;
                                _t3 = _t2 +
                                      PyCompile_OpcodeStackEffect(50, oparg);
                                _t6 = array_new(_t3);
                                if (!psyco_generic_call(po, cimpl_get_aiter_r0,
                                                        CfCommonIntZeroOk,
                                                        "vllvlA", NTOP(1),
                                                        (long)oparg, 0L, obj,
                                                        (long)(type), _t6)) {
                                    array_release(_t6);
                                    break;
                                }
                                {
                                    int n = _t2;
                                    while (n--) POP_DECREF();
                                }
                                for (_t4 = 0; _t4 < _t3; _t4++)
                                    PUSH(_t6->items[_t4]);
                                array_release(_t6);
                                goto fine;
                            default:
                                break;
                        }
                        getter = _t1;
                    }
                    if (getter != NULL) {
                        iter = Psyco_META1(po, getter, CfCommonNewRefPyObject,
                                           "v", obj);
                        if (iter == NULL) {
                            break;
                        }
                    }
                    else {
                        PycException_SetTypeFormat(po, PyExc_TypeError,
                                                   LE_MSG_ASYNC_FOR_NO_AITER,
                                                   NULL, type, NULL);
                        break;
                    }
                    tp_iter = Psyco_KnownType(iter);
                    if (tp_iter == NULL) {
                        _t8 = 1;
                        _t9 = _t8 + PyCompile_OpcodeStackEffect(50, oparg);
                        _t12 = array_new(_t9);
                        if (!psyco_generic_call(
                                po, cimpl_get_aiter_r1, CfCommonIntZeroOk,
                                "vlvA", NTOP(1), (long)oparg, iter, _t12)) {
                            array_release(_t12);
                            vinfo_decref(iter, po);
                            break;
                        }
                        vinfo_decref(iter, po);
                        {
                            int n = _t8;
                            while (n--) POP_DECREF();
                        }
                        for (_t10 = 0; _t10 < _t9; _t10++)
                            PUSH(_t12->items[_t10]);
                        array_release(_t12);
                        goto fine;
                    }
                    _t19 = (tp_iter->tp_as_async) == NULL;
                    if (!_t19) {
                        switch (Psyco_TypeSlot(
                            po, tp_iter, &(tp_iter->tp_as_async)->am_anext,
                            (void **)&_t13)) {
                            case 1:
                                break;
                            case 0:
                                _t14 = 1;
                                _t15 = _t14 +
                                       PyCompile_OpcodeStackEffect(50, oparg);
                                _t18 = array_new(_t15);
                                if (!psyco_generic_call(
                                        po, cimpl_get_aiter_r1,
                                        CfCommonIntZeroOk, "vlvA", NTOP(1),
                                        (long)oparg, iter, _t18)) {
                                    array_release(_t18);
                                    vinfo_decref(iter, po);
                                    break;
                                }
                                vinfo_decref(iter, po);
                                {
                                    int n = _t14;
                                    while (n--) POP_DECREF();
                                }
                                for (_t16 = 0; _t16 < _t15; _t16++)
                                    PUSH(_t18->items[_t16]);
                                array_release(_t18);
                                goto fine;
                            default:
                                vinfo_decref(iter, po);
                                break;
                        }
                        _t19 = _t13 == NULL;
                    }
                    if (_t19) {
                        PycException_SetTypeFormat(
                            po, PyExc_TypeError,
                            LE_MSG_ASYNC_FOR_AITER_NO_ANEXT, NULL, tp_iter,
                            NULL);
                        vinfo_decref(iter, po);
                        break;
                    }
                    POP(obj);
                    vinfo_decref(obj, po);
                    PUSH(iter);
                    goto fine;
                }

#if LE_HAS_CORO_GET_AWAITABLE_ITER_LINKABLE
                case GET_ANEXT: {
                    unaryfunc _t1;
                    int _t2;
                    int _t3;
                    int _t4;
                    vinfo_array_t *_t6;
                    vinfo_t *aiter;
                    vinfo_t *awaitable;
                    unaryfunc getter;
                    vinfo_t *next_iter;
                    PyTypeObject *tp_aiter;
                    PyTypeObject *type;
                    getter = NULL;
                    aiter = NTOP(1);
                    tp_aiter = Psyco_NeedType(po, aiter);
                    if (tp_aiter == NULL) {
                        break;
                    }
                    type = tp_aiter;
                    if ((type->tp_as_async) != NULL) {
                        switch (Psyco_TypeSlot(po, tp_aiter,
                                               &(type->tp_as_async)->am_anext,
                                               (void **)&_t1)) {
                            case 1:
                                break;
                            case 0:
                                _t2 = 1;
                                _t3 = _t2 +
                                      PyCompile_OpcodeStackEffect(51, oparg);
                                _t6 = array_new(_t3);
                                if (!psyco_generic_call(po, cimpl_get_anext_r0,
                                                        CfCommonIntZeroOk,
                                                        "vlvllA", NTOP(1),
                                                        (long)oparg, aiter, 0L,
                                                        (long)(type), _t6)) {
                                    array_release(_t6);
                                    break;
                                }
                                {
                                    int n = _t2;
                                    while (n--) POP_DECREF();
                                }
                                for (_t4 = 0; _t4 < _t3; _t4++)
                                    PUSH(_t6->items[_t4]);
                                array_release(_t6);
                                goto fine;
                            default:
                                break;
                        }
                        getter = _t1;
                    }
                    if (getter != NULL) {
                        next_iter = Psyco_META1(
                            po, getter, CfCommonNewRefPyObject, "v", aiter);
                        if (next_iter == NULL) {
                            break;
                        }
                    }
                    else {
                        PycException_SetTypeFormat(po, PyExc_TypeError,
                                                   LE_MSG_ASYNC_FOR_NO_ANEXT,
                                                   NULL, type, NULL);
                        break;
                    }
                    awaitable = psyco_generic_call(po, cimpl_get_anext_0,
                                                   CfCommonNewRefPyObject, "v",
                                                   next_iter);
                    if (awaitable == NULL) {
                        vinfo_decref(next_iter, po);
                        break;
                    }
                    else {
                        vinfo_decref(next_iter, po);
                    }
                    PUSH(awaitable);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_RERAISE
                case RERAISE: {
                    vinfo_t *exc;
                    vinfo_t *tb;
                    vinfo_t *val;
                    exc = NTOP(1);
                    val = NTOP(2);
                    tb = NTOP(3);
                    PycException_Reraise(po, exc, val, tb);
                    goto exception_unwind;
                }
#endif

#if LE_HAS_OPCODE_LOAD_ASSERTION_ERROR
                case LOAD_ASSERTION_ERROR: {
                    int _t1;
                    int _t2;
                    int _t3;
                    vinfo_array_t *_t5;
                    _t1 = 0;
                    _t2 = _t1 + PyCompile_OpcodeStackEffect(74, oparg);
                    _t5 = array_new(_t2);
                    if (!psyco_generic_call(po, cimpl_load_assertion_error_r0,
                                            CfCommonIntZeroOk, "lA",
                                            (long)oparg, _t5)) {
                        array_release(_t5);
                        break;
                    }
                    {
                        int n = _t1;
                        while (n--) POP_DECREF();
                    }
                    for (_t3 = 0; _t3 < _t2; _t3++) PUSH(_t5->items[_t3]);
                    array_release(_t5);
                    goto fine;
                }
#endif

                case LOAD_BUILD_CLASS: {
                    int _t1;
                    int _t2;
                    int _t3;
                    vinfo_array_t *_t5;
                    _t1 = 0;
                    _t2 = _t1 + PyCompile_OpcodeStackEffect(71, oparg);
                    _t5 = array_new(_t2);
                    if (!psyco_generic_call(po, cimpl_load_build_class_r0,
                                            CfCommonIntZeroOk, "lvA",
                                            (long)oparg, LOC_BUILTINS, _t5)) {
                        array_release(_t5);
                        break;
                    }
                    {
                        int n = _t1;
                        while (n--) POP_DECREF();
                    }
                    for (_t3 = 0; _t3 < _t2; _t3++) PUSH(_t5->items[_t3]);
                    array_release(_t5);
                    goto fine;
                }

                case STORE_ATTR: {
                    bool err;
                    vinfo_t *name;
                    vinfo_t *owner;
                    vinfo_t *v;
                    owner = NTOP(1);
                    v = NTOP(2);
                    name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
                    err = PsycoObject_SetAttr(po, owner, name, v);
                    vinfo_decref(name, po);
                    if (!(err)) {
                        break;
                    }
                    POP(owner);
                    POP(v);
                    vinfo_decref(owner, po);
                    vinfo_decref(v, po);
                    goto fine;
                }

                case DELETE_ATTR: {
                    bool err;
                    vinfo_t *name;
                    vinfo_t *owner;
                    owner = NTOP(1);
                    name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
                    err = PsycoObject_SetAttr(po, owner, name, NULL);
                    vinfo_decref(name, po);
                    if (!(err)) {
                        break;
                    }
                    POP(owner);
                    vinfo_decref(owner, po);
                    goto fine;
                }

                case STORE_GLOBAL: {
                    vinfo_t *_t1;
                    vinfo_t *err;
                    vinfo_t *name;
                    vinfo_t *v;
                    v = NTOP(1);
                    _t1 = LOC_GLOBALS;
                    name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
                    mark_varying(po, GETNAMEV(oparg));
                    err = psyco_generic_call(po, PyDict_SetItem,
                                             CfCommonIntZeroOk, "vvv", _t1,
                                             name, v);
                    vinfo_decref(name, po);
                    if (err == NULL) {
                        break;
                    }
                    POP(v);
                    vinfo_decref(v, po);
                    goto fine;
                }

                case DELETE_GLOBAL: {
                    vinfo_t *_t1;
                    vinfo_t *err;
                    vinfo_t *name;
                    name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
                    _t1 = LOC_GLOBALS;
                    mark_varying(po, GETNAMEV(oparg));
                    err =
                        psyco_generic_call(po, cimpl_delete_global_0,
                                           CfCommonIntZeroOk, "vv", name, _t1);
                    vinfo_decref(name, po);
                    if (err == NULL) {
                        break;
                    }
                    goto fine;
                }

                case DELETE_FAST: {
                    vinfo_t *_t1;
                    vinfo_t *v;
                    v = GETLOCAL(oparg);
                    if (!psyco_knowntobe(v, 0)) {
                        _t1 = psyco_vi_Zero();
                        SETLOCAL(oparg, _t1);
                        goto fine;
                    }
                    ct_format_exc_check_arg(po, PyExc_UnboundLocalError,
                                            LE_MSG_LOCAL_UNBOUND,
                                            le_code_local_name(co, oparg));
                    break;
                }

                case DELETE_DEREF: {
                    vinfo_t *_t1;
                    condition_code_t _t2;
                    int _t3;
                    bool _t4;
                    vinfo_t *_t5;
                    vinfo_t *cell;
                    vinfo_t *oldobj;
                    cell = GETLOCAL(le_code_nlocals(co) + oparg);
                    _t1 = psyco_get_field(po, cell, CELL_ob_ref);
                    if (_t1 == NULL) {
                        break;
                    }
                    oldobj = _t1;
                    _t2 = integer_non_null(po, oldobj);
                    if (_t2 == CC_ERROR) {
                        vinfo_decref(oldobj, po);
                        break;
                    }
                    _t3 = runtime_condition_t(po, _t2);
                    if (_t3) {
                        _t5 = psyco_vi_Zero();
                        _t4 = psyco_put_field(po, cell, CELL_ob_ref, _t5);
                        vinfo_decref(_t5, po);
                        if (!_t4) {
                            vinfo_decref(oldobj, po);
                            break;
                        }
                        psyco_decref_v(po, oldobj);
                        vinfo_decref(oldobj, po);
                        goto fine;
                    }
                    else {
                        vinfo_decref(oldobj, po);
                    }
                    ct_format_exc_unbound(po, co, oparg);
                    break;
                }

                case LOAD_CLOSURE: {
                    vinfo_t *cell;
                    cell = GETLOCAL(le_code_nlocals(co) + oparg);
                    vinfo_incref(cell);
                    PUSH(cell);
                    goto fine;
                }

                case LOAD_DEREF: {
                    vinfo_t *_t1;
                    condition_code_t _t2;
                    int _t3;
                    vinfo_t *cell;
                    vinfo_t *value;
                    cell = GETLOCAL(le_code_nlocals(co) + oparg);
                    _t1 = psyco_get_field(po, cell, CELL_ob_ref);
                    if (_t1 == NULL) {
                        break;
                    }
                    value = _t1;
                    _t2 = integer_non_null(po, value);
                    if (_t2 == CC_ERROR) {
                        vinfo_decref(value, po);
                        break;
                    }
                    _t2 = INVERT_CC(_t2);
                    _t3 = runtime_condition_f(po, _t2);
                    if (_t3) {
                        vinfo_decref(value, po);
                        ct_format_exc_unbound(po, co, oparg);
                        break;
                    }
                    need_reference(po, value);
                    PUSH(value);
                    goto fine;
                }

                case STORE_DEREF: {
                    vinfo_t *_t1;
                    bool _t2;
                    vinfo_t *cell;
                    vinfo_t *oldobj;
                    vinfo_t *v;
                    v = NTOP(1);
                    cell = GETLOCAL(le_code_nlocals(co) + oparg);
                    _t1 = psyco_get_field(po, cell, CELL_ob_ref);
                    if (_t1 == NULL) {
                        break;
                    }
                    oldobj = _t1;
                    _t2 = psyco_put_field(po, cell, CELL_ob_ref, v);
                    if (!_t2) {
                        vinfo_decref(oldobj, po);
                        break;
                    }
                    consume_reference(po, v);
                    psyco_xdecref_v(po, oldobj);
                    vinfo_decref(oldobj, po);
                    POP(v);
                    vinfo_decref(v, po);
                    goto fine;
                }

                case BUILD_STRING: {
                    vinfo_t *_t1;
                    vinfo_t *_t2;
                    vinfo_t *_t3;
                    vinfo_t *str;
                    _t1 = psyco_vi_Zero();
                    if (oparg <= ARRAY_ARGS_MAX) {
                        _t2 = psyco_generic_call(
                            po, cimpl_build_string_array,
                            CfCommonNewRefPyObject, "lvvvv", (long)(oparg),
                            oparg > 0 ? NTOP(oparg - 0) : _t1,
                            oparg > 1 ? NTOP(oparg - 1) : _t1,
                            oparg > 2 ? NTOP(oparg - 2) : _t1,
                            oparg > 3 ? NTOP(oparg - 3) : _t1);
                    }
                    else {
                        _t3 = PsycoTuple_New(oparg, STACK_POINTER() - (oparg));
                        _t2 =
                            psyco_generic_call(po, cimpl_build_string_array,
                                               CfCommonNewRefPyObject, "lvlll",
                                               -1L, _t3, 0L, 0L, 0L);
                        vinfo_decref(_t3, po);
                    }
                    vinfo_decref(_t1, po);
                    str = _t2;
                    if (str == NULL) {
                        break;
                    }
                    {
                        int n = (1 * oparg);
                        while (n--) POP_DECREF();
                    }
                    Psyco_AssertType(po, str, &PyUnicode_Type);
                    PUSH(str);
                    goto fine;
                }

                case BUILD_TUPLE: {
                    Py_ssize_t _t1;
                    vinfo_t *item;
                    int pop_i;
                    vinfo_t *tup;
                    _t1 = oparg;
                    tup = PsycoTuple_New(_t1, NULL);
                    pop_i = (oparg - 1);
                    for (;;) {
                        if (!(pop_i >= 0))
                            break;
                        item = NTOP(((1 * ((oparg - 1) - pop_i)) + 1));
                        PsycoTuple_GET_ITEM(tup, pop_i) = item;
                        vinfo_incref(PsycoTuple_GET_ITEM(tup, pop_i));
                        pop_i--;
                    }
                    {
                        int n = (1 * oparg);
                        while (n--) POP_DECREF();
                    }
                    PUSH(tup);
                    goto fine;
                }

                case BUILD_LIST: {
                    vinfo_t *list;
                    list = PsycoList_New(po, oparg, STACK_POINTER() - (oparg));
                    if (list == NULL) {
                        break;
                    }
                    {
                        int n = (1 * oparg);
                        while (n--) POP_DECREF();
                    }
                    PUSH(list);
                    goto fine;
                }

#if LE_HAS_OPCODE_BUILD_TUPLE_UNPACK_WITH_CALL
                case BUILD_TUPLE_UNPACK_WITH_CALL: {
                    vinfo_t *_t2;
                    vinfo_t *_t3;
                    vinfo_t *_t4;
                    Py_ssize_t i;
                    vinfo_t *none_val;
                    vinfo_t *return_value;
                    vinfo_t *sum;
                    { /* parts that are tuples with known items */
                        int i_, j_, k_, m_ = 0, n_ = 0;
                        for (i_ = 0; i_ < (1 * oparg); i_++) {
                            k_ = PsycoTuple_Load(NTOP((1 * oparg) - i_));
                            if (k_ < 0)
                                break;
                            n_ += k_;
                        }
                        if (i_ == (1 * oparg)) {
                            vinfo_t *v_, **items_ = (vinfo_t **)PyMem_Malloc(
                                             (n_ + 1) * sizeof(*items_));
                            if (items_ == NULL) {
                                Py_INCREF(Py_None); /* MemoryError(), as
                                                       PyErr_NoMemory */
                                PycException_SetObject(po, PyExc_MemoryError,
                                                       Py_None);
                                break;
                            }
                            for (i_ = 0; i_ < (1 * oparg); i_++)
                                for (j_ = 0, k_ = PsycoTuple_Load(
                                                 NTOP((1 * oparg) - i_));
                                     j_ < k_; j_++)
                                    items_[m_++] = PsycoTuple_GET_ITEM(
                                        NTOP((1 * oparg) - i_), j_);
                            v_ = PsycoTuple_New(n_, items_);
                            PyMem_Free(items_);
                            {
                                int n = (1 * oparg);
                                while (n--) POP_DECREF();
                            }
                            PUSH(v_);
                            goto fine;
                        }
                    }
                    sum = psyco_generic_call(po, PyList_New,
                                             CfCommonNewRefPyObject, "l",
                                             (long)(0));
                    if (sum == NULL) {
                        break;
                    }
                    i = oparg;
                    for (;;) {
                        if (!(i > 0))
                            break;
                        _t2 = NTOP(i);
                        _t3 = NTOP((1 + oparg));
                        _t4 = psyco_generic_call(
                            po, cimpl_build_tuple_unpack_with_call_0,
                            CfCommonNewRefPyObject, "vvv", sum, _t2, _t3);
                        none_val = _t4;
                        if (none_val == NULL) {
                            vinfo_decref(sum, po);
                            goto error_build_tuple_unpack_with_call;
                        }
                        vinfo_decref(none_val, po);
                        i--;
                    }
                    return_value = psyco_generic_call(
                        po, PyList_AsTuple,
                        CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v",
                        sum);
                    vinfo_decref(sum, po);
                    if (return_value == NULL) {
                        break;
                    }
                    {
                        int n = (1 * oparg);
                        while (n--) POP_DECREF();
                    }
                    Psyco_AssertType(po, return_value, &PyTuple_Type);
                    PUSH(return_value);
                    goto fine;
                error_build_tuple_unpack_with_call:
                    break;
                }
#endif

#if LE_HAS_OPCODE_BUILD_TUPLE_UNPACK
                case BUILD_TUPLE_UNPACK: {
                    vinfo_t *_t2;
                    Py_ssize_t i;
                    vinfo_t *none_val;
                    vinfo_t *return_value;
                    vinfo_t *sum;
                    { /* parts that are tuples with known items */
                        int i_, j_, k_, m_ = 0, n_ = 0;
                        for (i_ = 0; i_ < (1 * oparg); i_++) {
                            k_ = PsycoTuple_Load(NTOP((1 * oparg) - i_));
                            if (k_ < 0)
                                break;
                            n_ += k_;
                        }
                        if (i_ == (1 * oparg)) {
                            vinfo_t *v_, **items_ = (vinfo_t **)PyMem_Malloc(
                                             (n_ + 1) * sizeof(*items_));
                            if (items_ == NULL) {
                                Py_INCREF(Py_None); /* MemoryError(), as
                                                       PyErr_NoMemory */
                                PycException_SetObject(po, PyExc_MemoryError,
                                                       Py_None);
                                break;
                            }
                            for (i_ = 0; i_ < (1 * oparg); i_++)
                                for (j_ = 0, k_ = PsycoTuple_Load(
                                                 NTOP((1 * oparg) - i_));
                                     j_ < k_; j_++)
                                    items_[m_++] = PsycoTuple_GET_ITEM(
                                        NTOP((1 * oparg) - i_), j_);
                            v_ = PsycoTuple_New(n_, items_);
                            PyMem_Free(items_);
                            {
                                int n = (1 * oparg);
                                while (n--) POP_DECREF();
                            }
                            PUSH(v_);
                            goto fine;
                        }
                    }
                    sum = psyco_generic_call(po, PyList_New,
                                             CfCommonNewRefPyObject, "l",
                                             (long)(0));
                    if (sum == NULL) {
                        break;
                    }
                    i = oparg;
                    for (;;) {
                        if (!(i > 0))
                            break;
                        _t2 = NTOP(i);
                        none_val = psyco_generic_call(po, _PyList_Extend,
                                                      CfCommonNewRefPyObject,
                                                      "vv", sum, _t2);
                        if (none_val == NULL) {
                            vinfo_decref(sum, po);
                            goto error_build_tuple_unpack;
                        }
                        vinfo_decref(none_val, po);
                        i--;
                    }
                    return_value = psyco_generic_call(
                        po, PyList_AsTuple,
                        CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v",
                        sum);
                    vinfo_decref(sum, po);
                    if (return_value == NULL) {
                        break;
                    }
                    {
                        int n = (1 * oparg);
                        while (n--) POP_DECREF();
                    }
                    Psyco_AssertType(po, return_value, &PyTuple_Type);
                    PUSH(return_value);
                    goto fine;
                error_build_tuple_unpack:
                    break;
                }
#endif

#if LE_HAS_OPCODE_BUILD_LIST_UNPACK
                case BUILD_LIST_UNPACK: {
                    vinfo_t *_t2;
                    Py_ssize_t i;
                    vinfo_t *none_val;
                    vinfo_t *return_value;
                    vinfo_t *sum;
                    sum = psyco_generic_call(po, PyList_New,
                                             CfCommonNewRefPyObject, "l",
                                             (long)(0));
                    if (sum == NULL) {
                        break;
                    }
                    i = oparg;
                    for (;;) {
                        if (!(i > 0))
                            break;
                        _t2 = NTOP(i);
                        none_val = psyco_generic_call(po, _PyList_Extend,
                                                      CfCommonNewRefPyObject,
                                                      "vv", sum, _t2);
                        if (none_val == NULL) {
                            vinfo_decref(sum, po);
                            goto error_build_list_unpack;
                        }
                        vinfo_decref(none_val, po);
                        i--;
                    }
                    return_value = sum;
                    {
                        int n = (1 * oparg);
                        while (n--) POP_DECREF();
                    }
                    Psyco_AssertType(po, return_value, &PyList_Type);
                    PUSH(return_value);
                    goto fine;
                error_build_list_unpack:
                    break;
                }
#endif

#if LE_HAS_OPCODE_LIST_TO_TUPLE
                case LIST_TO_TUPLE: {
                    vinfo_t *list;
                    vinfo_t *tuple;
                    list = NTOP(1);
                    tuple = psyco_generic_call(
                        po, PyList_AsTuple,
                        CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v",
                        list);
                    if (tuple == NULL) {
                        break;
                    }
                    POP(list);
                    vinfo_decref(list, po);
                    PUSH(tuple);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_LIST_EXTEND
                case LIST_EXTEND: {
                    vinfo_t *iterable;
                    vinfo_t *list;
                    vinfo_t *none_val;
                    iterable = NTOP(1);
                    list = NTOP(oparg + 1);
                    none_val = psyco_generic_call(po, cimpl_list_extend_0,
                                                  CfCommonNewRefPyObject, "vv",
                                                  list, iterable);
                    if (none_val == NULL) {
                        break;
                    }
                    vinfo_decref(none_val, po);
                    POP(iterable);
                    vinfo_decref(iterable, po);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_SET_UPDATE
                case SET_UPDATE: {
                    vinfo_t *err;
                    vinfo_t *iterable;
                    vinfo_t *set;
                    iterable = NTOP(1);
                    set = NTOP(oparg + 1);
                    err = psyco_generic_call(po, _PySet_Update,
                                             CfCommonIntBoolOrError, "vv", set,
                                             iterable);
                    if (err == NULL) {
                        break;
                    }
                    else {
                        vinfo_decref(err, po);
                    }
                    POP(iterable);
                    vinfo_decref(iterable, po);
                    goto fine;
                }
#endif

                case BUILD_SET: {
                    vinfo_t *_t1;
                    vinfo_t *err;
                    int i;
                    vinfo_t *item;
                    vinfo_t *set;
                    _t1 = psyco_generic_call(po, PySet_New,
                                             CfCommonNewRefPyObject, "l", 0L);
                    set = _t1;
                    if (set == NULL) {
                        break;
                    }
                    i = oparg;
                    for (;;) {
                        if (!(i > 0))
                            break;
                        item = NTOP(i);
                        err = psyco_generic_call(
                            po, PySet_Add, CfCommonIntZeroOk, "vv", set, item);
                        if (err == NULL) {
                            vinfo_decref(set, po);
                            goto error_build_set;
                        }
                        i--;
                    }
                    {
                        int n = oparg;
                        while (n--) POP_DECREF();
                    }
                    Psyco_AssertType(po, set, &PySet_Type);
                    PUSH(set);
                    goto fine;
                error_build_set:
                    break;
                }

#if LE_HAS_OPCODE_BUILD_SET_UNPACK
                case BUILD_SET_UNPACK: {
                    vinfo_t *_t1;
                    vinfo_t *_t2;
                    vinfo_t *_tested0;
                    Py_ssize_t i;
                    vinfo_t *sum;
                    _t1 = psyco_generic_call(po, PySet_New,
                                             CfCommonNewRefPyObject, "l", 0L);
                    sum = _t1;
                    if (sum == NULL) {
                        break;
                    }
                    i = oparg;
                    for (;;) {
                        if (!(i > 0))
                            break;
                        _t2 = NTOP(i);
                        _tested0 = psyco_generic_call(po, _PySet_Update,
                                                      CfCommonIntBoolOrError,
                                                      "vv", sum, _t2);
                        if (_tested0 == NULL) {
                            vinfo_decref(sum, po);
                            goto error_build_set_unpack;
                        }
                        else {
                            vinfo_decref(_tested0, po);
                        }
                        i--;
                    }
                    {
                        int n = (1 * oparg);
                        while (n--) POP_DECREF();
                    }
                    Psyco_AssertType(po, sum, &PySet_Type);
                    PUSH(sum);
                    goto fine;
                error_build_set_unpack:
                    break;
                }
#endif

                case BUILD_MAP: {
                    vinfo_t *err;
                    Py_ssize_t i;
                    vinfo_t *key;
                    vinfo_t *map;
                    vinfo_t *value;
                    map = _PsycoDict_NewPresized(po, ((Py_ssize_t)oparg));
                    if (map == NULL) {
                        break;
                    }
                    i = oparg;
                    for (;;) {
                        if (!(i > 0))
                            break;
                        key = NTOP((2 * i));
                        value = NTOP(((2 * i) - 1));
                        err = psyco_generic_call(po, PyDict_SetItem,
                                                 CfCommonIntZeroOk, "vvv", map,
                                                 key, value);
                        if (err == NULL) {
                            vinfo_decref(map, po);
                            goto error_build_map;
                        }
                        i--;
                    }
                    {
                        int n = (2 * oparg);
                        while (n--) POP_DECREF();
                    }
                    PUSH(map);
                    goto fine;
                error_build_map:
                    break;
                }

#if LE_HAS_OPCODE_BUILD_MAP_UNPACK
                case BUILD_MAP_UNPACK: {
                    vinfo_t *_tested0;
                    vinfo_t *arg;
                    Py_ssize_t i;
                    vinfo_t *sum;
                    sum = PsycoDict_New(po);
                    if (sum == NULL) {
                        break;
                    }
                    i = oparg;
                    for (;;) {
                        if (!(i > 0))
                            break;
                        arg = NTOP(i);
                        _tested0 = psyco_generic_call(
                            po, cimpl_build_map_unpack_0,
                            CfCommonIntBoolOrError, "vv", sum, arg);
                        if (_tested0 == NULL) {
                            vinfo_decref(sum, po);
                            goto error_build_map_unpack;
                        }
                        else {
                            vinfo_decref(_tested0, po);
                        }
                        i--;
                    }
                    {
                        int n = (1 * oparg);
                        while (n--) POP_DECREF();
                    }
                    Psyco_AssertType(po, sum, &PyDict_Type);
                    PUSH(sum);
                    goto fine;
                error_build_map_unpack:
                    break;
                }
#endif

#if LE_HAS_OPCODE_BUILD_MAP_UNPACK_WITH_CALL
                case BUILD_MAP_UNPACK_WITH_CALL: {
                    vinfo_t *_t2;
                    vinfo_t *_t3;
                    vinfo_t *_tested0;
                    vinfo_t *arg;
                    Py_ssize_t i;
                    vinfo_t *sum;
                    sum = PsycoDict_New(po);
                    if (sum == NULL) {
                        break;
                    }
                    i = oparg;
                    for (;;) {
                        if (!(i > 0))
                            break;
                        arg = NTOP(i);
                        _t2 = NTOP((2 + oparg));
                        _t3 = psyco_generic_call(
                            po, cimpl_build_map_unpack_with_call_0,
                            CfCommonIntBoolOrError, "vvv", sum, arg, _t2);
                        _tested0 = _t3;
                        if (_tested0 == NULL) {
                            vinfo_decref(sum, po);
                            goto error_build_map_unpack_with_call;
                        }
                        else {
                            vinfo_decref(_tested0, po);
                        }
                        i--;
                    }
                    {
                        int n = (1 * oparg);
                        while (n--) POP_DECREF();
                    }
                    Psyco_AssertType(po, sum, &PyDict_Type);
                    PUSH(sum);
                    goto fine;
                error_build_map_unpack_with_call:
                    break;
                }
#endif

#if LE_HAS_OPCODE_DICT_UPDATE
                case DICT_UPDATE: {
                    vinfo_t *_tested0;
                    vinfo_t *dict;
                    vinfo_t *update;
                    update = NTOP(1);
                    dict = NTOP(oparg + 1);
                    _tested0 = psyco_generic_call(po, cimpl_dict_update_0,
                                                  CfCommonIntBoolOrError, "vv",
                                                  dict, update);
                    if (_tested0 == NULL) {
                        break;
                    }
                    else {
                        vinfo_decref(_tested0, po);
                    }
                    POP(update);
                    vinfo_decref(update, po);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_DICT_MERGE
                case DICT_MERGE: {
                    vinfo_t *_t1;
                    vinfo_t *_t2;
                    vinfo_t *_tested0;
                    vinfo_t *dict;
                    vinfo_t *update;
                    update = NTOP(1);
                    dict = NTOP(oparg + 1);
                    _t1 = NTOP((2 + oparg) + 1);
                    _t2 = psyco_generic_call(po, cimpl_dict_merge_0,
                                             CfCommonIntBoolOrError, "vvv",
                                             dict, update, _t1);
                    _tested0 = _t2;
                    if (_tested0 == NULL) {
                        break;
                    }
                    else {
                        vinfo_decref(_tested0, po);
                    }
                    POP(update);
                    vinfo_decref(update, po);
                    goto fine;
                }
#endif

                case MAP_ADD: {
                    vinfo_t *err;
                    vinfo_t *key;
                    vinfo_t *map;
                    vinfo_t *value;
#if LE_HAS_MAP_ADD_VALUE_ON_TOP
                    value = NTOP(1);
                    key = NTOP(2);
#else
                    key = NTOP(1);
                    value = NTOP(2);
#endif
                    map = NTOP(oparg + 2);
                    err = psyco_generic_call(po, PyDict_SetItem,
                                             CfCommonIntZeroOk, "vvv", map,
                                             key, value);
                    if (err == NULL) {
                        break;
                    }
#if LE_HAS_MAP_ADD_VALUE_ON_TOP
                    POP(value);
#endif
                    POP(key);
#if LE_HAS_MAP_ADD_VALUE_ON_TOP
                    vinfo_decref(value, po);
#else
                    POP(value);
#endif
                    vinfo_decref(key, po);
#if !LE_HAS_MAP_ADD_VALUE_ON_TOP
                    vinfo_decref(value, po);
#endif
                    goto fine;
                }

                case LOAD_ATTR: {
                    vinfo_t *name;
                    vinfo_t *owner;
                    vinfo_t *res;
                    owner = NTOP(1);
                    name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
                    res = PsycoObject_GetAttr(po, owner, name);
                    vinfo_decref(name, po);
                    if (res == NULL) {
                        break;
                    }
                    POP(owner);
                    vinfo_decref(owner, po);
                    PUSH(res);
                    goto fine;
                }

                case COMPARE_OP: {
                    vinfo_t *_t1;
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
#if !LE_HAS_CMP_OUTCOME
                    _t1 = PsycoObject_RichCompare(po, left, right, oparg);
#else
                    _t1 = cmp_outcome(po, oparg, left, right);
#endif
                    res = _t1;
                    if (res == NULL) {
                        break;
                    }
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(res);
                    goto fine;
                }

#if LE_HAS_OPCODE_IS_OP
                case IS_OP: {
                    condition_code_t _t1;
                    vinfo_t *b;
                    vinfo_t *left;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    _t1 = integer_cmp(po, left, right, Py_EQ);
                    if (_t1 == CC_ERROR) {
                        break;
                    }
                    _t1 = ((oparg) == 0   ? _t1
                           : (oparg) == 1 ? INVERT_CC(_t1)
                                          : CC_ALWAYS_TRUE);
                    b = PsycoBool_FromCondition(po, _t1);
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(b);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_CONTAINS_OP
                case CONTAINS_OP: {
                    condition_code_t _t2;
                    vinfo_t *b;
                    vinfo_t *left;
                    vinfo_t *res;
                    vinfo_t *right;
                    right = NTOP(1);
                    left = NTOP(2);
                    res = PsycoSequence_Contains(po, right, left);
                    if (res == NULL) {
                        break;
                    }
                    _t2 = integer_non_null(po, res);
                    if (_t2 == CC_ERROR) {
                        vinfo_decref(res, po);
                        break;
                    }
                    b = PsycoBool_FromCondition(
                        po, ((oparg) == 0   ? _t2
                             : (oparg) == 1 ? INVERT_CC(_t2)
                                            : CC_ALWAYS_TRUE));
                    vinfo_decref(res, po);
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    PUSH(b);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_JUMP_IF_NOT_EXC_MATCH
                case JUMP_IF_NOT_EXC_MATCH: {
                    int _t1;
                    int _t2;
                    int _t3;
                    vinfo_t *_t4;
                    condition_code_t _t5;
                    vinfo_t *_t6;
                    vinfo_t *exc;
                    Py_ssize_t i;
                    PyTypeObject *k_exc;
                    PyTypeObject *k_right;
                    vinfo_t *left;
                    Py_ssize_t length;
                    vinfo_t *res;
                    vinfo_t *right;
                    PyTypeObject *tp_exc;
                    PyTypeObject *tp_right;
                    right = NTOP(1);
                    left = NTOP(2);
                    tp_right = Psyco_KnownType(right);
                    if (tp_right == NULL) {
                        if (psyco_generic_call(
                                po, cimpl_jump_if_not_exc_match_0,
                                CfNoReturnValue | CfReturnTypeInt |
                                    CfPyErrIfNeg,
                                "v", right) == NULL) {
                            break;
                        }
                        goto checked_0;
                    }
                    if ((tp_right->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) !=
                        0) {
                        _t1 = PsycoTuple_Load(right);
                        if (_t1 < 0 || _t1 > 16) {
                            if (psyco_generic_call(
                                    po, cimpl_jump_if_not_exc_match_0,
                                    CfNoReturnValue | CfReturnTypeInt |
                                        CfPyErrIfNeg,
                                    "v", right) == NULL) {
                                break;
                            }
                            goto checked_0;
                        }
                        length = _t1;
                        i = 0;
                        for (;;) {
                            if (!(i < length))
                                break;
                            exc = PsycoTuple_GET_ITEM(right, i);
                            tp_exc = Psyco_KnownType(exc);
                            if (tp_exc == NULL) {
                                if (psyco_generic_call(
                                        po, cimpl_jump_if_not_exc_match_0,
                                        CfNoReturnValue | CfReturnTypeInt |
                                            CfPyErrIfNeg,
                                        "v", right) == NULL) {
                                    goto error_jump_if_not_exc_match;
                                }
                                goto checked_0;
                            }
                            _t2 = (tp_exc->tp_flags &
                                   Py_TPFLAGS_TYPE_SUBCLASS) != 0;
                            if (_t2) {
                                if (!is_compiletime(exc->source)) {
                                    if (psyco_generic_call(
                                            po, cimpl_jump_if_not_exc_match_0,
                                            CfNoReturnValue | CfReturnTypeInt |
                                                CfPyErrIfNeg,
                                            "v", right) == NULL) {
                                        goto error_jump_if_not_exc_match;
                                    }
                                    goto checked_0;
                                }
                                k_exc = (PyTypeObject *)CompileTime_Get(
                                            exc->source)
                                            ->value;
                                _t2 = (k_exc->tp_flags &
                                       Py_TPFLAGS_BASE_EXC_SUBCLASS) != 0;
                            }
                            if (!(_t2)) {
                                PycException_SetString(
                                    po, PyExc_TypeError,
                                    LE_MSG_CATCHING_NON_EXCEPTION);
                                goto error_jump_if_not_exc_match;
                            }
                            i++;
                        }
                    }
                    else {
                        _t3 = (tp_right->tp_flags &
                               Py_TPFLAGS_TYPE_SUBCLASS) != 0;
                        if (_t3) {
                            if (!is_compiletime(right->source)) {
                                if (psyco_generic_call(
                                        po, cimpl_jump_if_not_exc_match_0,
                                        CfNoReturnValue | CfReturnTypeInt |
                                            CfPyErrIfNeg,
                                        "v", right) == NULL) {
                                    break;
                                }
                                goto checked_0;
                            }
                            k_right =
                                (PyTypeObject *)CompileTime_Get(right->source)
                                    ->value;
                            _t3 = (k_right->tp_flags &
                                   Py_TPFLAGS_BASE_EXC_SUBCLASS) != 0;
                        }
                        if (!(_t3)) {
                            PycException_SetString(
                                po, PyExc_TypeError,
                                LE_MSG_CATCHING_NON_EXCEPTION);
                            break;
                        }
                    }
                checked_0:;
                    _t4 = psyco_generic_call(
                        po, PyErr_GivenExceptionMatches,
                        CfCommonIntBoolOrError | CfNoPython | CfNoBlock, "vv",
                        left, right);
                    res = _t4;
                    if (res == NULL) {
                        break;
                    }
                    _t5 = integer_non_null(po, res);
                    if (_t5 == CC_ERROR) {
                        vinfo_decref(res, po);
                        break;
                    }
                    vinfo_decref(res, po);
                    _t6 = psyco_vinfo_condition(po, _t5);
                    POP(right);
                    POP(left);
                    vinfo_decref(right, po);
                    vinfo_decref(left, po);
                    _t5 = integer_non_null(po, _t6);
                    vinfo_decref(_t6, po);
                    JUMP_IF_CC(INVERT_CC(_t5), JUMPTO(JUMP_DEST()), 0, 0);
                    goto fine;
                error_jump_if_not_exc_match:
                    break;
                }
#endif

                case IMPORT_FROM: {
                    vinfo_t *_t1;
                    vinfo_t *from;
                    vinfo_t *name;
                    vinfo_t *res;
                    from = NTOP(1);
                    name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
#if LE_HAS_IMPORT_FROM_SPEC
                    _t1 = psyco_generic_call(po, cimpl_import_from_whole,
                                             CfCommonNewRefPyObject, "vv",
                                             from, name);
#else
                    _t1 = psyco_generic_call(po, cimpl_import_from,
                                             CfCommonNewRefPyObject, "vv",
                                             from, name);
#endif
                    res = _t1;
                    vinfo_decref(name, po);
                    if (res == NULL) {
                        break;
                    }
                    PUSH(res);
                    goto fine;
                }

                case JUMP_FORWARD:
                case JUMP_ABSOLUTE:
                case JUMP_BACKWARD:
                case JUMP_BACKWARD_NO_INTERRUPT: {
                    JUMPTO(JUMP_DEST());
                    JUMPED();
                }

                case POP_JUMP_IF_FALSE:
                case POP_JUMP_FORWARD_IF_FALSE:
                case POP_JUMP_BACKWARD_IF_FALSE: {
                    condition_code_t _t2;
                    vinfo_t *cond;
                    vinfo_t *err;
                    cond = NTOP(1);
                    err = PsycoObject_IsTrue(po, cond);
                    if (err == NULL) {
                        break;
                    }
                    _t2 = integer_non_null(po, err);
                    if (_t2 == CC_ERROR) {
                        vinfo_decref(err, po);
                        break;
                    }
                    vinfo_decref(err, po);
                    JUMP_IF_CC(INVERT_CC(_t2), JUMPTO(JUMP_DEST()), 1, 1);
                    POP(cond);
                    vinfo_decref(cond, po);
                    goto fine;
                }

                case POP_JUMP_IF_TRUE:
                case POP_JUMP_FORWARD_IF_TRUE:
                case POP_JUMP_BACKWARD_IF_TRUE: {
                    condition_code_t _t2;
                    vinfo_t *cond;
                    vinfo_t *err;
                    cond = NTOP(1);
                    err = PsycoObject_IsTrue(po, cond);
                    if (err == NULL) {
                        break;
                    }
                    _t2 = integer_non_null(po, err);
                    if (_t2 == CC_ERROR) {
                        vinfo_decref(err, po);
                        break;
                    }
                    vinfo_decref(err, po);
                    JUMP_IF_CC(_t2, JUMPTO(JUMP_DEST()), 1, 1);
                    POP(cond);
                    vinfo_decref(cond, po);
                    goto fine;
                }

                case POP_JUMP_FORWARD_IF_NONE:
                case POP_JUMP_BACKWARD_IF_NONE:
                case POP_JUMP_FORWARD_IF_NOT_NONE:
                case POP_JUMP_BACKWARD_IF_NOT_NONE: {
                    vinfo_t *cond = NTOP(1);
                    condition_code_t cc =
                        integer_cmp_i(po, cond, (long)Py_None, Py_EQ);
                    if (cc == CC_ERROR)
                        break;
                    if (opcode == POP_JUMP_FORWARD_IF_NOT_NONE ||
                        opcode == POP_JUMP_BACKWARD_IF_NOT_NONE)
                        cc = INVERT_CC(cc);
                    JUMP_IF_CC(cc, JUMPTO(JUMP_DEST()), 1, 1);
                    POP(cond);
                    vinfo_decref(cond, po);
                    goto fine;
                }

                case JUMP_IF_FALSE_OR_POP: {
                    condition_code_t _t2;
                    vinfo_t *cond;
                    vinfo_t *err;
                    cond = NTOP(1);
                    err = PsycoObject_IsTrue(po, cond);
                    if (err == NULL) {
                        break;
                    }
                    _t2 = integer_non_null(po, err);
                    if (_t2 == CC_ERROR) {
                        vinfo_decref(err, po);
                        break;
                    }
                    vinfo_decref(err, po);
                    JUMP_IF_CC(INVERT_CC(_t2), JUMPTO(JUMP_DEST()), 0, 1);
                    POP(cond);
                    vinfo_decref(cond, po);
                    goto fine;
                }

                case JUMP_IF_TRUE_OR_POP: {
                    condition_code_t _t2;
                    vinfo_t *cond;
                    vinfo_t *err;
                    cond = NTOP(1);
                    err = PsycoObject_IsTrue(po, cond);
                    if (err == NULL) {
                        break;
                    }
                    _t2 = integer_non_null(po, err);
                    if (_t2 == CC_ERROR) {
                        vinfo_decref(err, po);
                        break;
                    }
                    vinfo_decref(err, po);
                    JUMP_IF_CC(_t2, JUMPTO(JUMP_DEST()), 0, 1);
                    POP(cond);
                    vinfo_decref(cond, po);
                    goto fine;
                }

#if LE_HAS_OPCODE_GET_LEN
                case GET_LEN: {
                    vinfo_t *_t1;
                    vinfo_t *len_i;
                    vinfo_t *len_o;
                    _t1 = NTOP(1);
                    len_i = PsycoObject_Size(po, _t1);
                    if (len_i == NULL) {
                        break;
                    }
                    len_o = PsycoLong_FromSsize_t(po, len_i);
                    vinfo_decref(len_i, po);
                    if (len_o == NULL) {
                        break;
                    }
                    PUSH(len_o);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_MATCH_CLASS
                case MATCH_CLASS: {
                    vinfo_t *_t2;
                    vinfo_t *_t3;
                    vinfo_t *_t4;
                    vinfo_t *_t5;
                    vinfo_t *_t6;
                    vinfo_t *_t7;
                    vinfo_t *attrs;
                    vinfo_t *names;
                    vinfo_t *subject;
                    PyTypeObject *tp_type;
                    vinfo_t *type;
                    names = NTOP(1);
                    type = NTOP(2);
                    subject = NTOP(3);
                    tp_type = Psyco_NeedType(po, type);
                    if (tp_type == NULL) {
                        break;
                    }
                    attrs = match_class(po, subject, type, oparg, names);
                    if (attrs == NULL) {
                        break;
                    }
                    if (!psyco_known_NULL(attrs)) {
                        _t2 = psyco_vi_One();
                        _t3 = PsycoBool_FromLong(po, _t2);
                        vinfo_decref(_t2, po);
                        _t4 = _t3;
                        if (_t4 == NULL) {
                            vinfo_decref(attrs, po);
                            break;
                        }
                        POP(names);
                        POP(type);
                        POP(subject);
                        vinfo_decref(names, po);
                        vinfo_decref(type, po);
                        vinfo_decref(subject, po);
                        PUSH(attrs);
                        PUSH(_t4);
                        goto fine;
                    }
                    else {
                        vinfo_decref(attrs, po);
                        _t5 = psyco_vi_Zero();
                        _t6 = PsycoBool_FromLong(po, _t5);
                        vinfo_decref(_t5, po);
                        _t7 = _t6;
                        if (_t7 == NULL) {
                            break;
                        }
                        POP(names);
                        POP(type);
                        vinfo_decref(names, po);
                        vinfo_decref(type, po);
                        PUSH(_t7);
                        goto fine;
                    }
                }
#endif

#if LE_HAS_OPCODE_MATCH_MAPPING
                case MATCH_MAPPING: {
                    int _t1;
                    int _t2;
                    int _t3;
                    vinfo_array_t *_t5;
                    _t1 = 1;
                    _t2 = _t1 + PyCompile_OpcodeStackEffect(31, oparg);
                    _t5 = array_new(_t2);
                    if (!psyco_generic_call(po, cimpl_match_mapping_r0,
                                            CfCommonIntZeroOk, "vlA", NTOP(1),
                                            (long)oparg, _t5)) {
                        array_release(_t5);
                        break;
                    }
                    {
                        int n = _t1;
                        while (n--) POP_DECREF();
                    }
                    for (_t3 = 0; _t3 < _t2; _t3++) PUSH(_t5->items[_t3]);
                    array_release(_t5);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_MATCH_SEQUENCE
                case MATCH_SEQUENCE: {
                    int _t1;
                    int _t2;
                    int _t3;
                    vinfo_array_t *_t5;
                    _t1 = 1;
                    _t2 = _t1 + PyCompile_OpcodeStackEffect(32, oparg);
                    _t5 = array_new(_t2);
                    if (!psyco_generic_call(po, cimpl_match_sequence_r0,
                                            CfCommonIntZeroOk, "vlA", NTOP(1),
                                            (long)oparg, _t5)) {
                        array_release(_t5);
                        break;
                    }
                    {
                        int n = _t1;
                        while (n--) POP_DECREF();
                    }
                    for (_t3 = 0; _t3 < _t2; _t3++) PUSH(_t5->items[_t3]);
                    array_release(_t5);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_MATCH_KEYS
                case MATCH_KEYS: {
                    int _t2;
                    int _t3;
                    int _t4;
                    vinfo_array_t *_t6;
                    vinfo_t *keys;
                    vinfo_t *subject;
                    vinfo_t *values_or_none;
                    keys = NTOP(1);
                    subject = NTOP(2);
                    values_or_none = match_keys(po, subject, keys);
                    if (values_or_none == NULL) {
                        break;
                    }
                    _t2 = 2;
                    _t3 = _t2 + PyCompile_OpcodeStackEffect(33, oparg);
                    _t6 = array_new(_t3);
                    if (!psyco_generic_call(po, cimpl_match_keys_r0,
                                            CfCommonIntZeroOk, "vvlvA",
                                            NTOP(2), NTOP(1), (long)oparg,
                                            values_or_none, _t6)) {
                        array_release(_t6);
                        vinfo_decref(values_or_none, po);
                        break;
                    }
                    vinfo_decref(values_or_none, po);
                    {
                        int n = _t2;
                        while (n--) POP_DECREF();
                    }
                    for (_t4 = 0; _t4 < _t3; _t4++) PUSH(_t6->items[_t4]);
                    array_release(_t6);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_COPY_DICT_WITHOUT_KEYS
                case COPY_DICT_WITHOUT_KEYS: {
                    int _t10;
                    int _t11;
                    vinfo_array_t *_t13;
                    int _t3;
                    int _t4;
                    int _t5;
                    vinfo_array_t *_t7;
                    int _t8;
                    int _t9;
                    vinfo_t *keys;
                    vinfo_t *rest;
                    vinfo_t *subject;
                    keys = NTOP(1);
                    subject = NTOP(2);
                    rest = PsycoDict_New(po);
                    _t8 = rest == NULL;
                    if (!_t8) {
                        _t3 = 2;
                        _t4 = _t3 + PyCompile_OpcodeStackEffect(34, oparg);
                        _t7 = array_new(_t4);
                        if (!psyco_generic_call(
                                po, cimpl_copy_dict_without_keys_r0,
                                CfCommonIntZeroOk, "vvlvvvA", NTOP(2), NTOP(1),
                                (long)oparg, keys, rest, subject, _t7)) {
                            array_release(_t7);
                            vinfo_decref(rest, po);
                            break;
                        }
                        vinfo_decref(rest, po);
                        {
                            int n = _t3;
                            while (n--) POP_DECREF();
                        }
                        for (_t5 = 0; _t5 < _t4; _t5++) PUSH(_t7->items[_t5]);
                        array_release(_t7);
                        goto fine;
                    }
                    if (_t8) {
                        vinfo_decref(rest, po);
                        break;
                    }
                    _t9 = 2;
                    _t10 = _t9 + PyCompile_OpcodeStackEffect(34, oparg);
                    _t13 = array_new(_t10);
                    if (!psyco_generic_call(
                            po, cimpl_copy_dict_without_keys_r1,
                            CfCommonIntZeroOk, "vvlvvA", NTOP(2), NTOP(1),
                            (long)oparg, keys, rest, _t13)) {
                        array_release(_t13);
                        vinfo_decref(rest, po);
                        break;
                    }
                    vinfo_decref(rest, po);
                    {
                        int n = _t9;
                        while (n--) POP_DECREF();
                    }
                    for (_t11 = 0; _t11 < _t10; _t11++)
                        PUSH(_t13->items[_t11]);
                    array_release(_t13);
                    goto fine;
                }
#endif

                case GET_ITER: {
                    vinfo_t *iter;
                    vinfo_t *iterable;
                    iterable = NTOP(1);
                    iter = PsycoObject_GetIter(po, iterable);
                    if (iter == NULL) {
                        break;
                    }
                    POP(iterable);
                    vinfo_decref(iterable, po);
                    PUSH(iter);
                    goto fine;
                }

                case GET_YIELD_FROM_ITER: {
                    vinfo_t *iter;
                    vinfo_t *iterable;
                    PyTypeObject *tp_iterable;
                    iterable = NTOP(1);
                    tp_iterable = Psyco_NeedType(po, iterable);
                    if (tp_iterable == NULL) {
                        break;
                    }
                    if (tp_iterable == &PyCoro_Type) {
                        if (!((co->co_flags & (0x0080 | 0x0100)))) {
                            PycException_SetString(
                                po, PyExc_TypeError,
                                LE_MSG_YIELD_FROM_COROUTINE);
                            break;
                        }
                    }
                    else {
                        if (tp_iterable != &PyGen_Type) {
                            iter = PsycoObject_GetIter(po, iterable);
                            if (iter == NULL) {
                                break;
                            }
                            POP(iterable);
                            vinfo_decref(iterable, po);
                            PUSH(iter);
                            goto fine;
                        }
                        else {
                            goto fine;
                        }
                    }
                    goto fine;
                }

#if LE_HAS_AENTER_LOOKUP_FIRST
                case BEFORE_ASYNC_WITH: {
                    vinfo_t *enter;
                    vinfo_t *exit;
                    vinfo_t *mgr;
                    vinfo_t *res;
                    mgr = NTOP(1);
                    enter = psyco_generic_call(po, cimpl_special_lookup,
                                               CfCommonNewRefPyObject, "vl",
                                               mgr, (long)(&PyId___aenter__));
                    if (enter == NULL) {
                        break;
                    }
                    exit = psyco_generic_call(po, cimpl_special_lookup,
                                              CfCommonNewRefPyObject, "vl",
                                              mgr, (long)(&PyId___aexit__));
                    if (exit == NULL) {
                        vinfo_decref(enter, po);
                        break;
                    }
                    res = psyco_generic_call(po, PyObject_CallObject,
                                             CfCommonNewRefPyObject, "vl",
                                             enter, 0L);
                    vinfo_decref(enter, po);
                    if (res == NULL) {
                        vinfo_decref(exit, po);
                        break;
                    }
                    POP(mgr);
                    vinfo_decref(mgr, po);
                    PUSH(exit);
                    PUSH(res);
                    goto fine;
                }
#else
                case BEFORE_ASYNC_WITH: {
                    vinfo_t *_t3;
                    vinfo_t *enter;
                    vinfo_t *exit;
                    vinfo_t *mgr;
                    vinfo_t *res;
                    mgr = NTOP(1);
                    exit = psyco_generic_call(po, cimpl_special_lookup,
                                              CfCommonNewRefPyObject, "vl",
                                              mgr, (long)(&PyId___aexit__));
                    if (exit == NULL) {
                        break;
                    }
                    enter = psyco_generic_call(po, cimpl_special_lookup,
                                               CfCommonNewRefPyObject, "vl",
                                               mgr, (long)(&PyId___aenter__));
                    if (enter == NULL) {
                        vinfo_decref(exit, po);
                        break;
                    }
                    _t3 = psyco_generic_call(po, PyObject_CallObject,
                                             CfCommonNewRefPyObject, "vl",
                                             enter, 0L);
                    res = _t3;
                    vinfo_decref(enter, po);
                    if (res == NULL) {
                        vinfo_decref(exit, po);
                        break;
                    }
                    POP(mgr);
                    vinfo_decref(mgr, po);
                    PUSH(exit);
                    PUSH(res);
                    goto fine;
                }
#endif

#if LE_HAS_OPCODE_WITH_EXCEPT_START
                case WITH_EXCEPT_START: {
                    int _t1;
                    int _t2;
                    int _t3;
                    vinfo_t *_t4;
                    vinfo_array_t *_t5;
                    vinfo_t *exc;
                    vinfo_t *exit_func;
                    vinfo_t *tb;
                    vinfo_t *val;
                    exc = NTOP(1);
                    val = NTOP(2);
                    tb = NTOP(3);
                    exit_func = NTOP(7);
                    _t1 = 7;
                    _t2 = _t1 + PyCompile_OpcodeStackEffect(49, oparg);
                    _t4 = PsycoTuple_New(_t1, STACK_POINTER() - _t1);
                    _t5 = array_new(_t2);
                    if (!psyco_generic_call(po, cimpl_with_except_start_r0,
                                            CfCommonIntZeroOk, "vlvvvvA", _t4,
                                            (long)oparg, exc, exit_func, tb,
                                            val, _t5)) {
                        vinfo_decref(_t4, po);
                        array_release(_t5);
                        break;
                    }
                    vinfo_decref(_t4, po);
                    {
                        int n = _t1;
                        while (n--) POP_DECREF();
                    }
                    for (_t3 = 0; _t3 < _t2; _t3++) PUSH(_t5->items[_t3]);
                    array_release(_t5);
                    goto fine;
                }
#endif

                case LOAD_METHOD: {
                    vinfo_t *meth;
                    vinfo_t *name;
                    vinfo_t *obj;
                    obj = NTOP(1);
                    name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
                    meth = PsycoObject_GetAttr(po, obj, name);
                    vinfo_decref(name, po);
                    if (meth == NULL) {
                        break;
                    }
                    POP(obj);
                    vinfo_decref(obj, po);
                    PUSH(psyco_vi_Zero());
                    PUSH(meth);
                    goto fine;
                }

                case BUILD_SLICE: {
                    vinfo_t *slice;
                    vinfo_t *start;
                    vinfo_t *step;
                    vinfo_t *stop;
                    if (oparg == 3) {
                        step = NTOP(1);
                        stop = NTOP(2);
                        start = NTOP(3);
                        slice = PsycoSlice_New(start, stop, step);
                        POP(step);
                        POP(stop);
                        POP(start);
                        vinfo_decref(step, po);
                        vinfo_decref(stop, po);
                        vinfo_decref(start, po);
                        PUSH(slice);
                        goto fine;
                    }
                    else {
                        stop = NTOP(1);
                        start = NTOP(2);
                        slice = PsycoSlice_New(start, stop, NULL);
                        POP(stop);
                        POP(start);
                        vinfo_decref(stop, po);
                        vinfo_decref(start, po);
                        PUSH(slice);
                        goto fine;
                    }
                }

                case FORMAT_VALUE: {
                    vinfo_t *_t1;
                    PyObject *(*conv_fn)(PyObject *);
                    vinfo_t *fmt_spec;
                    int have_fmt_spec;
                    vinfo_t *result;
                    PyTypeObject *tp_value;
                    vinfo_t *value;
                    int which_conversion;
                    which_conversion = (oparg & 0x3);
                    have_fmt_spec = ((oparg & 0x4)) == 0x4;
                    if (have_fmt_spec != 0) {
                        fmt_spec = NTOP(1);
                        value = NTOP(2);
                        if (which_conversion == 0x0) {
                            conv_fn = NULL;
                            tp_value = Psyco_KnownType(value);
                            _t1 = psyco_generic_call(po, PyObject_Format,
                                                     CfCommonNewRefPyObject,
                                                     "vv", value, fmt_spec);
                            result = _t1;
                            if (result == NULL) {
                                break;
                            }
                            POP(fmt_spec);
                            POP(value);
                            vinfo_decref(fmt_spec, po);
                            vinfo_decref(value, po);
                            PUSH(result);
                            goto fine;
                        }
                        else {
                            if (which_conversion == 0x1) {
                                conv_fn = PyObject_Str;
                                result = psyco_generic_call(
                                    po, PyObject_Str, CfCommonNewRefPyObject,
                                    "v", value);
                                if (result == NULL) {
                                    break;
                                }
                                value = result;
                                tp_value = Psyco_KnownType(value);
                                result =
                                    psyco_generic_call(po, PyObject_Format,
                                                       CfCommonNewRefPyObject,
                                                       "vv", value, fmt_spec);
                                vinfo_decref(value, po);
                                if (result == NULL) {
                                    break;
                                }
                                POP(fmt_spec);
                                POP(value);
                                vinfo_decref(fmt_spec, po);
                                vinfo_decref(value, po);
                                PUSH(result);
                                goto fine;
                            }
                            else {
                                if (which_conversion == 0x2) {
                                    conv_fn = PyObject_Repr;
                                    result = psyco_generic_call(
                                        po, PyObject_Repr,
                                        CfCommonNewRefPyObject, "v", value);
                                    if (result == NULL) {
                                        break;
                                    }
                                    value = result;
                                    tp_value = Psyco_KnownType(value);
                                    result = psyco_generic_call(
                                        po, PyObject_Format,
                                        CfCommonNewRefPyObject, "vv", value,
                                        fmt_spec);
                                    vinfo_decref(value, po);
                                    if (result == NULL) {
                                        break;
                                    }
                                    POP(fmt_spec);
                                    POP(value);
                                    vinfo_decref(fmt_spec, po);
                                    vinfo_decref(value, po);
                                    PUSH(result);
                                    goto fine;
                                }
                                else {
                                    if (which_conversion == 0x3) {
                                        conv_fn = PyObject_ASCII;
                                        result = psyco_generic_call(
                                            po, PyObject_ASCII,
                                            CfCommonNewRefPyObject, "v",
                                            value);
                                        if (result == NULL) {
                                            break;
                                        }
                                        value = result;
                                        tp_value = Psyco_KnownType(value);
                                        result = psyco_generic_call(
                                            po, PyObject_Format,
                                            CfCommonNewRefPyObject, "vv",
                                            value, fmt_spec);
                                        vinfo_decref(value, po);
                                        if (result == NULL) {
                                            break;
                                        }
                                        POP(fmt_spec);
                                        POP(value);
                                        vinfo_decref(fmt_spec, po);
                                        vinfo_decref(value, po);
                                        PUSH(result);
                                        goto fine;
                                    }
                                    else {
                                        PycException_SetFormat(
                                            po, PyExc_SystemError,
                                            "unexpected conversion flag %d",
                                            which_conversion);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    else {
                        value = NTOP(1);
                        if (which_conversion == 0x0) {
                            conv_fn = NULL;
                            tp_value = Psyco_KnownType(value);
                            if ((tp_value == &PyUnicode_Type && 1)) {
                                result = value;
                                POP(value);
                                PUSH(value);
                                goto fine;
                            }
                            else {
                                fmt_spec = psyco_vi_Zero();
                                result =
                                    psyco_generic_call(po, PyObject_Format,
                                                       CfCommonNewRefPyObject,
                                                       "vv", value, fmt_spec);
                                vinfo_decref(fmt_spec, po);
                                if (result == NULL) {
                                    break;
                                }
                                POP(value);
                                vinfo_decref(value, po);
                                PUSH(result);
                                goto fine;
                            }
                        }
                        else {
                            if (which_conversion == 0x1) {
                                conv_fn = PyObject_Str;
                                result = psyco_generic_call(
                                    po, PyObject_Str, CfCommonNewRefPyObject,
                                    "v", value);
                                if (result == NULL) {
                                    break;
                                }
                                value = result;
                                tp_value = Psyco_KnownType(value);
                                if ((tp_value == &PyUnicode_Type && 1)) {
                                    result = value;
                                }
                                else {
                                    fmt_spec = psyco_vi_Zero();
                                    result = psyco_generic_call(
                                        po, PyObject_Format,
                                        CfCommonNewRefPyObject, "vv", value,
                                        fmt_spec);
                                    vinfo_decref(value, po);
                                    vinfo_decref(fmt_spec, po);
                                    if (result == NULL) {
                                        break;
                                    }
                                }
                                POP(value);
                                vinfo_decref(value, po);
                                PUSH(result);
                                goto fine;
                            }
                            else {
                                if (which_conversion == 0x2) {
                                    conv_fn = PyObject_Repr;
                                    result = psyco_generic_call(
                                        po, PyObject_Repr,
                                        CfCommonNewRefPyObject, "v", value);
                                    if (result == NULL) {
                                        break;
                                    }
                                    value = result;
                                    tp_value = Psyco_KnownType(value);
                                    if ((tp_value == &PyUnicode_Type && 1)) {
                                        result = value;
                                    }
                                    else {
                                        fmt_spec = psyco_vi_Zero();
                                        result = psyco_generic_call(
                                            po, PyObject_Format,
                                            CfCommonNewRefPyObject, "vv",
                                            value, fmt_spec);
                                        vinfo_decref(value, po);
                                        vinfo_decref(fmt_spec, po);
                                        if (result == NULL) {
                                            break;
                                        }
                                    }
                                    POP(value);
                                    vinfo_decref(value, po);
                                    PUSH(result);
                                    goto fine;
                                }
                                else {
                                    if (which_conversion == 0x3) {
                                        conv_fn = PyObject_ASCII;
                                        result = psyco_generic_call(
                                            po, PyObject_ASCII,
                                            CfCommonNewRefPyObject, "v",
                                            value);
                                        if (result == NULL) {
                                            break;
                                        }
                                        value = result;
                                        tp_value = Psyco_KnownType(value);
                                        if ((tp_value == &PyUnicode_Type &&
                                             1)) {
                                            result = value;
                                        }
                                        else {
                                            fmt_spec = psyco_vi_Zero();
                                            result = psyco_generic_call(
                                                po, PyObject_Format,
                                                CfCommonNewRefPyObject, "vv",
                                                value, fmt_spec);
                                            vinfo_decref(value, po);
                                            vinfo_decref(fmt_spec, po);
                                            if (result == NULL) {
                                                break;
                                            }
                                        }
                                        POP(value);
                                        vinfo_decref(value, po);
                                        PUSH(result);
                                        goto fine;
                                    }
                                    else {
                                        PycException_SetFormat(
                                            po, PyExc_SystemError,
                                            "unexpected conversion flag %d",
                                            which_conversion);
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }

#if LE_HAS_OPCODE_ROT_N
                case ROT_N: {
                    vinfo_t *_t1;
                    vinfo_t *top;
                    top = NTOP(1);
                    _t1 = top;
                    CHKSTACK(-((oparg - 1)));
                    CHKSTACK(-(oparg));
                    memmove(STACK_POINTER() - ((oparg - 1)),
                            STACK_POINTER() - (oparg),
                            sizeof(vinfo_t *) * ((oparg - 1)));
                    CHKSTACK(-(oparg));
                    STACK_POINTER()[-(oparg)] = _t1;
                    goto fine;
                }
#endif
#pragma GCC diagnostic pop

                case BREAK_LOOP:
                    PycException_Raise(po, vinfo_new(VirtualTime_New(&EBreak)),
                                       NULL);
                    break;

                case CONTINUE_LOOP:
                    PycException_Raise(po,
                                       vinfo_new(VirtualTime_New(&EContinue)),
                                       vinfo_new(CompileTime_New(oparg)));
                    break;

                case RAISE_VARARGS:
                    /* 3.x: 0 reraise, 1 raise exc, 2 raise exc from cause */
                    v = w = x =
                        psyco_vi_Zero(); /* NULL; one reference, shared */
                    switch (oparg) {
                        case 2:
                            v = NTOP(1); /* cause */
                            w = NTOP(2); /* exc */
                            psyco_generic_call(po, cimpl_do_raise,
                                               CfPyErrAlways, "vv", w, v);
                            break;
                        case 1:
                            w = NTOP(1); /* exc */
                            psyco_generic_call(po, cimpl_do_raise,
                                               CfPyErrAlways, "vv", w, v);
                            break;
                        case 0:
                            psyco_generic_call(
                                po, cimpl_reraise, CfPyErrAlways, "lvll",
                                (long)co, LOC_GLOBALS, (long)po->pr.next_instr,
                                (long)PyCode_Addr2Line(co, po->pr.next_instr));
                            break;
                        default:
                            PycException_SetString(po, PyExc_SystemError,
                                                   "bad RAISE_VARARGS oparg");
                    }
                    vinfo_decref(x, po);
                    if (oparg ==
                        0) /* cimpl_reraise recorded the traceback entry */
                        goto exception_unwind;
                    break;

                case RETURN_VALUE:
                    POP(v);
                    if (co->co_flags & CO_GENERATOR) {
                        /* a fused generator expression: return the consumer's
                         * result */
                        int slot = GENEXPR_SLOT_MODE(co);
                        long mode =
                            CompileTime_Get(po->vlocals.items[slot]->source)
                                ->value;
                        vinfo_decref(v, po);
                        if (mode == GENEXPR_ANY)
                            v = psyco_vi_Py_False();
                        else if (mode == GENEXPR_ALL)
                            v = psyco_vi_Py_True();
                        else if (mode == GENEXPR_MIN || mode == GENEXPR_MAX) {
                            v = psyco_generic_call(
                                po, cimpl_minmax_result,
                                CfCommonNewRefPyObject, "vl",
                                po->vlocals.items[slot + 1],
                                (long)(mode == GENEXPR_MIN));
                            if (v == NULL)
                                break;
                        }
                        else {
                            v = po->vlocals.items[slot + 1];
                            vinfo_incref(v);
                        }
                    }
                    PycException_Raise(
                        po, vinfo_new(VirtualTime_New(&EReturn)), v);
                    break;

                case GEN_START:
                    /* only in a generator expression fused with its consumer,
                       which is entered with nothing pushed: no value sent to
                       pop */
                    goto fine;

                case YIELD_VALUE: {
                    /* only in a generator expression fused with its consumer
                       (GENEXPR_*): do the consumer's step, then push the value
                       send() would have, for the POP_TOP that follows */
                    int slot = GENEXPR_SLOT_MODE(co);
                    long mode =
                        CompileTime_Get(po->vlocals.items[slot]->source)
                            ->value;
                    vinfo_t *acc = po->vlocals.items[slot + 1];
                    v = TOP();
                    if (mode == GENEXPR_LIST) {
                        if (!PsycoList_Append(po, acc, v))
                            break;
                    }
                    else if (mode == GENEXPR_SET) {
                        if (!psyco_generic_call(po, PySet_Add,
                                                CfCommonIntZeroOk, "vv", acc,
                                                v))
                            break;
                    }
                    else if (mode == GENEXPR_SUM) {
                        /* as builtin_sum's generic loop (its fast paths give
                           the same results as + on ints and floats) */
                        if (Psyco_KnownType(acc) != NULL)
                            x = PsycoNumber_Add(po, acc, v);
                        else
                            x = psyco_generic_call(po, PyNumber_Add,
                                                   CfCommonNewRefPyObject,
                                                   "vv", acc, v);
                        if (x == NULL)
                            break;
                        po->vlocals.items[slot + 1] = x;
                        vinfo_decref(acc, po);
                    }
                    else if (mode == GENEXPR_MIN || mode == GENEXPR_MAX) {
                        x = psyco_generic_call(
                            po, cimpl_minmax_step, CfCommonNewRefPyObject,
                            "vvl", acc, v,
                            (long)(mode == GENEXPR_MIN ? Py_LT : Py_GT));
                        if (x == NULL)
                            break;
                        po->vlocals.items[slot + 1] = x;
                        vinfo_decref(acc, po);
                    }
                    else {
                        cc = integer_NON_NULL(po, PsycoObject_IsTrue(po, v));
                        if (cc == CC_ERROR)
                            break;
                        if (mode == GENEXPR_ALL)
                            cc = INVERT_CC(cc);
                        if (runtime_condition_f(po, cc)) {
                            PycException_Raise(
                                po, vinfo_new(VirtualTime_New(&EReturn)),
                                mode == GENEXPR_ANY ? psyco_vi_Py_True()
                                                    : psyco_vi_Py_False());
                            break;
                        }
                    }
                    POP_DECREF();
                    PUSH(psyco_vi_None());
                    goto fine;
                }

                case POP_BLOCK: {
                    le_block_t *b = block_pop(po);
                    if (LE_HAS_POP_BLOCK_UNWINDS) {
                        while (STACK_LEVEL() > b->b_level) {
                            POP(v);
                            vinfo_decref(v, po);
                        }
                    }
                    goto fine;
                }

                case POP_EXCEPT:
                    /* from 3.8 the items above the handler's three stay (a
                       return value under them, by ROT_FOUR) */
                    if (!pop_except_handler(po, LE_HAS_POP_EXCEPT_UNWINDS))
                        break;
                    goto fine;

                /* Where the bytecode has them (3.8), a finally block is
                   entered by BEGIN_FINALLY's NULL (LeJIT's 0), CALL_FINALLY's
                   offset to return to, or an exception's six items;
                   END_FINALLY and POP_FINALLY tell them apart. The offset is
                   ceval's int object, a fixed constant (finally_return), so
                   END_FINALLY's jump is made at compile time and a frame going
                   on in ceval finds what ceval pushes. */
                case BEGIN_FINALLY:
                    PUSH(psyco_vi_Zero());
                    goto fine;

                case CALL_FINALLY:
                    PUSH(finally_return(INSTR_OFFSET()));
                    JUMPBY(LE_JUMP_BYTES(oparg));
                    JUMPED();

                case POP_FINALLY: {
                    vinfo_t *res = NULL;
                    bool ok = true;
                    if (oparg)
                        POP(res);
                    if (psyco_knowntobe(TOP(), 0) || is_finally_return(TOP()))
                        POP_DECREF();
                    else
                        ok = pop_except_handler(po, true);
                    if (oparg)
                        PUSH(res);
                    if (!ok)
                        break;
                    goto fine;
                }

                case END_FINALLY:
                    /* First make extra sure no exception is pending */
                    if (PycException_Occurred(po))
                        Py_FatalError(
                            "psyco: undetected exception "
                            "in END_FINALLY");
                    POP(v);
                    if (psyco_knowntobe(v, (long)Py_None) ||
                        psyco_knowntobe(v, 0)) {
                        /* None (3.7) or NULL (3.8) on the stack: the end of a
                           finally block with no exception raised */
                        vinfo_decref(v, po);
                        goto fine;
                    }
                    if (is_finally_return(v)) {
                        /* back to just after the CALL_FINALLY */
                        JUMPTO((int)PyLong_AsLong(
                            (PyObject *)CompileTime_Get(v->source)->value));
                        vinfo_decref(v, po);
                        JUMPED();
                    }
                    if (Psyco_KnownType(v) == &PyTuple_Type &&
                        PsycoTuple_Load(v) == 3) {
                        /* 'v' is a 3- tuple. As no real Python exception is
                           a tuple object we are sure it comes from a
                           finally block (see the SETUP_FINALLY stack unwind
                           code at the end of pycompiler.c) */
                        /* Re-raise the pseudo exception. This will re-raise
                           exceptions like EReturn as well. */
                        po->pr.exc = PsycoTuple_GET_ITEM(v, 0);
                        PsycoTuple_GET_ITEM(v, 0) = NULL;
                        po->pr.val = PsycoTuple_GET_ITEM(v, 1);
                        PsycoTuple_GET_ITEM(v, 1) = NULL;
                        po->pr.tb = PsycoTuple_GET_ITEM(v, 2);
                        PsycoTuple_GET_ITEM(v, 2) = NULL;
                        vinfo_decref(v, po);
                        goto exception_unwind;
                    }
                    else {
                        /* end of an EXCEPT block: the exception stored in the
                           stack, raised again as RERAISE does */
                        POP(w);
                        POP(x);
                        PycException_Reraise(po, v, w, x);
                        vinfo_decref(v, po);
                        vinfo_decref(w, po);
                        vinfo_decref(x, po);
                        goto exception_unwind;
                    }

                case UNPACK_SEQUENCE: {
                    int i;
                    void *cimpl_unpack;
                    PyTypeObject *vtp;

                    v = TOP();
                    vtp = Psyco_NeedType(po, v);
                    if (vtp == NULL)
                        break;

                    if (vtp == &PyTuple_Type) {
                        /* shortcut: is this a virtual tuple?
                                     of the correct length? */
                        if (PsycoTuple_Load(v) != oparg) {
                            /* No, fall back to the default path:
                               load the size, compare it with oparg,
                               and if they match proceed by loading the
                               tuple item by item into the stack. */
                            vinfo_t *vsize;
                            vsize = psyco_get_const(po, v, FIX_size);
                            if (vsize == NULL)
                                break;

                            /* check the size */
                            cc = integer_cmp_i(po, vsize, oparg, Py_NE);
                            if (cc == CC_ERROR)
                                break;
                            if (runtime_condition_f(po, cc))
                                goto unpack_generic; /* which raises ceval's
                                                        error */

                            /* make sure the tuple data is loaded */
                            for (i = oparg; i--;) {
                                w = psyco_get_nth_const(po, v, TUPLE_ob_item,
                                                        i);
                                if (w == NULL)
                                    break;
                            }
                            if (i >= 0)
                                break;
                        }
                        /* copy the tuple items into the stack */
                        POP(v);
                        for (i = oparg; i--;) {
                            w = PsycoTuple_GET_ITEM(v, i);
                            vinfo_incref(w);
                            PUSH(w);
                            /* in case the tuple is freed while its items
                               are still in use: */
                            need_reference(po, w);
                        }
                        vinfo_decref(v, po);
                        goto fine;
                    }

                    if (vtp == &PyList_Type)
                        cimpl_unpack = cimpl_unpack_list;
                    else
                    unpack_generic:
                        cimpl_unpack = cimpl_unpack_iterable;

                    {
                        vinfo_array_t *array = array_new(oparg);
                        if (!psyco_generic_call(po, cimpl_unpack,
                                                CfCommonIntZeroOk, "vlA", v,
                                                (long)oparg, array)) {
                            array_release(array);
                            break;
                        }
                        POP_DECREF();
                        for (i = oparg; i--;) PUSH(array->items[i]);
                        array_release(array);
                        goto fine;
                    }
                }

                case UNPACK_EX: {
                    int i, before = oparg & 0xFF,
                           total = 1 + (oparg & 0xFF) + (oparg >> 8);
                    vinfo_array_t *array = array_new(total);
                    if (!psyco_generic_call(po, cimpl_unpack_ex,
                                            CfCommonIntZeroOk, "vlA", TOP(),
                                            (long)oparg, array)) {
                        array_release(array);
                        break;
                    }
                    Psyco_AssertType(po, array->items[before], &PyList_Type);
                    POP_DECREF();
                    for (i = total; i--;) PUSH(array->items[i]);
                    array_release(array);
                    goto fine;
                }

                /* STORE_NAME, DELETE_NAME, LOAD_NAME and IMPORT_STAR act on
                   the globals, where CPython acts on f_locals, which a
                   compiled frame has none of at its start */
                case STORE_NAME: {
                    PyObject *w = GETNAMEV(oparg);
                    mark_varying(po, w);
                    if (!psyco_generic_call(po, PyDict_SetItem,
                                            CfCommonIntZeroOk, "vlv",
                                            LOC_GLOBALS, w, TOP()))
                        break;
                    POP_DECREF();
                    goto fine;
                }

                case DELETE_NAME: {
                    PyObject *w = GETNAMEV(oparg);
                    mark_varying(po, w);
                    if (runtime_NON_NULL_f(
                            po, psyco_generic_call(po, PyDict_DelItem,
                                                   CfReturnTypeInt, "vl",
                                                   LOC_GLOBALS, w))) {
                        psyco_generic_call(po, cimpl_name_error, CfPyErrAlways,
                                           "l", w);
                        break;
                    }
                    goto fine;
                }

                /* globals that are a dict subclass are read as a dict, where
                   CPython calls their __getitem__ */
                case LOAD_NAME:
                case LOAD_GLOBAL: {
                    PyObject *namev = GETNAMEV(oparg);
                    if (!load_global_cached(po, namev, &v))
                        break;
                    if (v != NULL) {
                        PUSH(v);
                        goto fine;
                    }
                    /* not cached: globals unknown, the name varying or not
                     * found */
                    v = psyco_generic_call(po, cimpl_load_global,
                                           CfCommonNewRefPyObject, "vvl",
                                           LOC_GLOBALS, LOC_BUILTINS, namev);
                    if (v == NULL)
                        break;
                    PUSH(v);
                    goto fine;
                }

                case BUILD_CONST_KEY_MAP: {
                    /* oparg values, then a constant tuple of keys on top */
                    int i;
                    PyObject *keys;
                    if (!is_compiletime(TOP()->source) ||
                        !PyTuple_CheckExact(
                            keys = (PyObject *)CompileTime_Get(TOP()->source)
                                       ->value) ||
                        PyTuple_GET_SIZE(keys) != oparg) {
                        PycException_SetString(
                            po, PyExc_SystemError,
                            "bad BUILD_CONST_KEY_MAP keys argument");
                        break;
                    }
                    v = _PsycoDict_NewPresized(po, oparg);
                    if (v == NULL)
                        break;
                    for (i = oparg; i > 0; i--)
                        if (!PsycoDict_SetItemC(
                                po, v, PyTuple_GET_ITEM(keys, oparg - i),
                                NTOP(i + 1)))
                            break;
                    if (i > 0) {
                        vinfo_decref(v, po);
                        break;
                    }
                    for (i = oparg + 1; i--;) POP_DECREF();
                    PUSH(v);
                    goto fine;
                }

                case IMPORT_NAME: {
                    PyObject *w = GETNAMEV(oparg);
                    v = TOP();
                    u = NTOP(2);
                    x = psyco_generic_call(po, cimpl_import_name,
                                           CfCommonNewRefPyObject, "vvlvv",
                                           LOC_GLOBALS, LOC_BUILTINS, w, v, u);
                    if (x == NULL)
                        break;
                    POP_DECREF();
                    POP_DECREF();
                    PUSH(x);
                    goto fine;
                }

                case IMPORT_STAR: {
                    x = psyco_generic_call(po, cimpl_import_all_from,
                                           CfReturnTypeInt | CfPyErrIfNonNull,
                                           "vv", LOC_GLOBALS, TOP());
                    if (x == NULL)
                        break;
                    vinfo_decref(x, po);
                    POP_DECREF();
                    goto fine;
                }

                case FOR_ITER:
                    v = PsycoIter_Next(po, TOP());
                    if (v != NULL) {
                        /* iterator not exhausted */
                        PUSH(v);
                    }
                    else {
                        extra_assert(PycException_Occurred(po));

                        /* catch PyExc_StopIteration */
                        v = PycException_Matches(po, PyExc_StopIteration);
                        if (runtime_NON_NULL_t(po, v) == true) {
                            /* iterator ended normally */
                            PycException_Clear(po);
                            POP_DECREF();
                            JUMPBY(LE_JUMP_BYTES(oparg));
                            mp = psyco_next_merge_point(po->pr.merge_points,
                                                        next_instr);
                        }
                        else
                            break; /* any other exception, or error in
                                      runtime_NON_NULL_t() */
                    }
                    goto fine;

                case SETUP_LOOP:
                case SETUP_EXCEPT:
                case SETUP_FINALLY:
                    block_setup(po, opcode,
                                INSTR_OFFSET() + LE_JUMP_BYTES(oparg),
                                STACK_LEVEL());
                    goto fine;

                /* with: as 3.7 ceval, a SETUP_FINALLY block over the bound
                   __exit__. Its handler starts with WITH_CLEANUP_START,
                   entered in one of LeJIT's three finally states (see the
                   unwind loop): None on top (the body ended), a 3-tuple
                   (EReturn, EBreak, EContinue), or an exception's six items
                   under an EXCEPT_HANDLER block. */
                case SETUP_WITH: {
                    vinfo_array_t *array = array_new(2);
                    if (!psyco_generic_call(po, cimpl_setup_with,
                                            CfCommonIntZeroOk, "vA", TOP(),
                                            array)) {
                        array_release(array);
                        break;
                    }
                    POP_DECREF();
                    PUSH(array->items[0]);
                    block_setup(po, SETUP_FINALLY,
                                INSTR_OFFSET() + LE_JUMP_BYTES(oparg),
                                STACK_LEVEL());
                    PUSH(array->items[1]);
                    array_release(array);
                    goto fine;
                }

                case WITH_CLEANUP_START: {
                    le_block_t *b =
                        po->pr.iblock > 0
                            ? &po->pr.blockstack[(int)po->pr.iblock - 1]
                            : NULL;
                    vinfo_t **sp = STACK_POINTER();
                    if (b != NULL && b->b_type == EXCEPT_HANDLER &&
                        STACK_LEVEL() == b->b_level + 6) {
                        /* an exception: exit(type, value, tb), then ceval's
                           shift of the saved exception down over __exit__'s
                           slot, with NULL where the type was */
                        x = psyco_generic_call(po, cimpl_call_exit,
                                               CfCommonNewRefPyObject, "vvvv",
                                               sp[-7], sp[-1], sp[-2], sp[-3]);
                        if (x == NULL)
                            break;
                        vinfo_decref(sp[-7], po);
                        sp[-7] = sp[-6];
                        sp[-6] = sp[-5];
                        sp[-5] = sp[-4];
                        sp[-4] = psyco_vi_Zero();
                        b->b_level--;
                        u = sp[-1];
                        vinfo_incref(u);
                        PUSH(u);
                        PUSH(x);
                        goto fine;
                    }
                    /* None or a pseudo-exception's 3-tuple: exit(None, None,
                       None), and __exit__'s slot is dropped */
                    extra_assert(psyco_knowntobe(TOP(), (long)Py_None) ||
                                 psyco_knowntobe(TOP(), 0) ||
                                 Psyco_KnownType(TOP()) == &PyTuple_Type);
                    x = psyco_generic_call(
                        po, cimpl_call_exit, CfCommonNewRefPyObject, "vlll",
                        NTOP(2), (long)Py_None, (long)Py_None, (long)Py_None);
                    if (x == NULL)
                        break;
                    POP(v);
                    POP_DECREF();
                    PUSH(v);
                    PUSH(psyco_vi_None());
                    PUSH(x);
                    goto fine;
                }

                case WITH_CLEANUP_FINISH:
                    /* TOP is __exit__'s result, SECOND the exception type or
                     * None */
                    if (!psyco_knowntobe(NTOP(2), (long)Py_None)) {
                        cc = integer_NON_NULL(po,
                                              PsycoObject_IsTrue(po, TOP()));
                        if (cc == CC_ERROR)
                            break;
                        if (runtime_condition_f(po, cc)) {
                            /* a true result silences the exception: what
                               END_FINALLY does in ceval for WHY_SILENCED,
                               after which it sees None */
                            POP_DECREF();
                            POP_DECREF();
                            if (!pop_except_handler(po, true))
                                break;
                            /* for END_FINALLY: None, or NULL where
                             * BEGIN_FINALLY is */
                            PUSH(LE_HAS_OPCODE_BEGIN_FINALLY
                                     ? psyco_vi_Zero()
                                     : psyco_vi_None());
                            goto fine;
                        }
                    }
                    POP_DECREF();
                    POP_DECREF();
                    goto fine;

                case CALL_METHOD: {
                    /* always the (NULL, attribute) form: LOAD_METHOD looks the
                       attribute up and pushes NULL under it */
                    int i, stack_to_pop;
                    extra_assert(psyco_knowntobe(NTOP(oparg + 2), 0));
                    po->pr.call_site =
                        next_instr - (int)sizeof(_Py_CODEUNIT) + 1;
                    x = psyco_ext_do_calls(po, CALL_FUNCTION, oparg,
                                           STACK_POINTER(), &stack_to_pop);
                    end_call_site(po);
                    if (x == NULL)
                        break;
                    for (i = stack_to_pop + 1;
                         i--;) /* args, callable and the NULL */
                        POP_DECREF();
                    PUSH(x);
                    if (!breaker_after_call(po, next_instr))
                        break;
                    goto fine;
                }

                case CALL_FUNCTION:
                case CALL_FUNCTION_KW:
                case CALL_FUNCTION_EX: {
                    int i, stack_to_pop;
                    po->pr.call_site =
                        next_instr - (int)sizeof(_Py_CODEUNIT) + 1;
                    if (opcode == CALL_FUNCTION_EX)
                        x = psyco_ext_do_call_ex(po, oparg, STACK_POINTER(),
                                                 &stack_to_pop);
                    else
                        x = psyco_ext_do_calls(po, opcode, oparg,
                                               STACK_POINTER(), &stack_to_pop);
                    end_call_site(po);
                    if (x == NULL)
                        break;

                    /* clean up the stack (remove args and func) */
                    for (i = stack_to_pop; i--;) POP_DECREF();
                    PUSH(x);
                    if (!breaker_after_call(po, next_instr))
                        break;
                    goto fine;
                }

                case MAKE_FUNCTION: {
                    /* the optional fields in the order ceval pops them; each
                       is made fit to cross calls before anything is popped, as
                       that can fail (Restart, above), and replaces the 0 the
                       new function holds */
                    const int fields[4] = {iFUNC_CLOSURE, iFUNC_ANNOTATIONS,
                                           iFUNC_KWDEFAULTS, iFUNC_DEFAULTS};
                    int i, n = 2;
                    for (i = 0; i < 4; i++)
                        if (oparg & (0x08 >> i)) {
                            n++;
                            if (!psyco_forking(po, NTOP(n)->array))
                                break;
                        }
                    if (i < 4)
                        break;
                    x = PsycoFunction_NewWithQualName(po, NTOP(2), LOC_GLOBALS,
                                                      NTOP(1));
                    POP_DECREF();
                    POP_DECREF();
                    for (i = 0; i < 4; i++)
                        if (oparg & (0x08 >> i)) {
                            vinfo_t **field = &x->array->items[fields[i]];
                            vinfo_decref(*field, po);
                            POP(*field);
                        }
                    PUSH(x);
                    goto fine;
                }

                case EXTENDED_ARG:
                    opcode = le_op(bytecode, next_instr);
                    oparg = oparg << 8 | le_arg(bytecode, next_instr);
                    next_instr += LE_UNIT;
                    ;
                    goto dispatch_opcode;

                default:
                    fprintf(stderr, "XXX opcode: %d\n", opcode);
                    Py_FatalError("unknown opcode");

            } /* switch (opcode) */

            if (0) {
            exception_unwind:
                /* an exception raised again (ceval's label): this instruction
                   adds no traceback entry */
                reraised = 1;
            }
            /* Exit if an error occurred */
            if (PycException_Occurred(po))
                break;

        fine:
            extra_assert(!PycException_Occurred(po));
            if ((po->pr.python_call &&
                 (opcode == CALL_FUNCTION || opcode == CALL_FUNCTION_KW ||
                  opcode == CALL_FUNCTION_EX || opcode == CALL_METHOD)) ||
                po->pr.resume_pending) {
                po->pr.resume_pending = 0;
                if (!resume_if_tracing(po, next_instr, &unwound))
                    break;
            }
            /* before the next merge point compares known types */
            Psyco_ForgetClasses(po);
            extra_assert(
                mp == psyco_next_merge_point(po->pr.merge_points, next_instr));

            /* are we running out of space in the current code buffer? */
            if ((po->codelimit - po->code) < BUFFER_MARGIN) {
                if (is_respawning(po)) {
                    /* when respawning, just forget everything we
                       wrote so far and come back to the beginning
                       again */
                    PsycoObject_EmergencyCodeRoom(po);
                }
                else {
                    /* normal case: save the current position, and
                       stop compilation. When this point is reached
                       at run-time, compilation will go on in a
                       new buffer. */
                    SAVE_NEXT_INSTR(next_instr);
                    if (mp->bytecode_position != next_instr)
                        mp = NULL;
                    code1 = psyco_compile(po, mp, false);
                    goto finished;
                }
            }

            /* end the block after a guarded site (load_global_cached,
               pobject.c): a new buffer, contiguous, with its own snapshot,
               kept at this instruction so that a branch respawned from the
               site's checks can join it (vcompiler.c:psyco_compile_join) */
            if (po->pr.split_after_global) {
                po->pr.split_after_global = 0;
                if (!is_respawning(po) &&
                    mp->bytecode_position != next_instr) {
                    SAVE_NEXT_INSTR(next_instr);
                    code1 = psyco_compile_join(
                        po, psyco_join_point(po->pr.merge_points, next_instr));
                    if (code1 != NULL)
                        goto finished;
                }
            }

            /* mark merge points via a call to psyco_compile() */
            if (mp->bytecode_position == next_instr) {
                extra_assert(!is_respawning(po));
                SAVE_NEXT_INSTR(next_instr);
                /* simplify the po->vlocals array */
                if (!psyco_limit_nested_weight(po, &po->vlocals, NWI_NORMAL,
                                               NESTED_WEIGHT_END))
                    break;
                psyco_forget_dead_vars(po, &mp->entries);
                code1 = psyco_compile(po, mp, true);
                if (code1 != NULL)
                    goto finished;
                mp++;
            }
        } /* end of the main loop, exit if pseudo-exception */

        psyco_assert_coherent(po);

        /* check for the 'promotion' pseudo-exception.
           This is the only case in which the instruction
           causing the exception is restarted. */
        if (is_virtualtime(po->pr.exc->source) &&
            psyco_vsource_is_promotion(po->pr.exc->source)) {
            c_promotion_t *promotion =
                (c_promotion_t *)VirtualTime_Get(po->pr.exc->source);
            vinfo_t *promote_me = po->pr.val;
            /* NOTE: we assume that 'promote_me' is a member of the
               'po->vlocals' arrays, so that we can safely DECREF it
               now without actually releasing it. If this assumption
               is false, the psyco_finish_xxx() calls below will give
               unexpected results. */
            assert_array_contains_nonct(&po->vlocals, promote_me);
            clear_pseudo_exception(po);
            code1 = psyco_finish_promotion(po, promote_me, promotion->pflags);
            goto finished;
        }

        /* check for the 'inline' pseudo-exception. */
        if (PycException_Is(po, &EInline)) {
            vinfo_t *v;

            /* po->pr.val->array contains the new sub-frame that we
               should now start to compile, inlined into the parent frame */
            extra_assert(!po->pr.is_inlining);
            /* store po->vlocals away */
            v = psyco_save_inline_po(po);
            /* copy po->pr.val->array to po->vlocals */
            po = psyco_restore_inline_po(po, &po->pr.val->array);
            stack_a = po->vlocals.items + po->pr.stack_base;
            /* save the copy of the previous po->vlocals to LOC_INLINING */
            extra_assert(LOC_INLINING == NULL);
            LOC_INLINING = v;
            po->pr.is_inlining = true;
            /* mark this point as an inline frame entry */
            psyco_inline_enter(po);
            Psyco_ForgetClasses(po); /* the arguments' types */
            /* go on with compilation */
            clear_pseudo_exception(po);
            continue;
        }

        /* At this point, we got a real pseudo-exception. */

        if (PycException_IsPython(po) && !reraised && !unwound) {
            /* log traceback info for real Python exceptions */
            PsycoTraceBack_Here(po, po->pr.next_instr);
        }
        resumed = unwound;
        unwound = 0;
        reraised = 0;

        /* All exceptions raised inside an inlined frame are passed to the
           parent (neither exception handlers nor loops are allowed in
           inlinable code objects). */
        if (po->pr.is_inlining) {
            /* unwind the inlined frame by restoring the parent one */
            vinfo_t *v = LOC_INLINING;
            bool returned = PycException_Is(po, &EReturn);
            /* mark this point as an inline frame exit: on a return, before
               its locals go, as CPython pops the frame first */
            if (returned)
                psyco_inline_exit(po);
            LOC_INLINING = NULL;
            po = psyco_restore_inline_po(po, &v->array);
            stack_a = po->vlocals.items + po->pr.stack_base;
            vinfo_decref(v, po);
            po->pr.is_inlining = false;
            if (!returned)
                psyco_inline_exit(po);
            /* the inlined frame went on interpreted: so may the caller, after
               the instruction that called it */
            if (resumed)
                po->pr.resume_pending = 1;

            if (PycException_Is(po, &EReturn)) {
                /* caught EReturn from the inlined frame */

                /* 'po' is now in the parent frame, pointing to the instruction
                   (typically CALL_FUNCTION) which triggered the inline
                   compilation in the first place.  We restart this instruction
                   entierely.  With almost no code emitted we should reach the
                   psyco_call_pyfunc() again, which will at that point return
                   the expected result.  To this end we save the result in
                   LOC_INLINING.  It might be exceptionally possible that the
                   restarting CALL_FUNCTION does not reach psyco_call_pyfunc()
                   again in some cases; this hopefully corresponds to cases
                   that can never occur at run-time.  The waste of code is
                   minimal. */
                LOC_INLINING = po->pr.val;
                po->pr.val = NULL;
                clear_pseudo_exception(po);
                need_reference(po,
                               LOC_INLINING); /* see test5.makeSelection() */
                continue;
            }

            /* stack unwind goes on in the parent frame */
            if (PycException_IsPython(po) && compute_and_raise_exception(po)) {
                /* force the exception to be recorded on inner frame
                   before we can record its traceback on the parent frame */
                clear_pseudo_exception(po);
                PycException_Raise(po, vinfo_new(VirtualTime_New(&ERtPython)),
                                   NULL);
                PsycoTraceBack_Here(po, po->pr.next_instr);
            }
        }

        /* Unwind the Python stack until we find a handler for
           the (pseudo) exception. You will recognize here
           ceval.c's stack unwinding code. */

        while (po->pr.iblock > 0) {
            le_block_t *b = block_pop(po);

            if (b->b_type == SETUP_LOOP && PycException_Is(po, &EContinue)) {
                /* For a continue inside a try block,
                   don't pop the block for the loop. */
                int next_instr;
                block_setup(po, b->b_type, b->b_handler, b->b_level);
                JUMPTO(CompileTime_Get(po->pr.val->source)->value);
                clear_pseudo_exception(po);
                SAVE_NEXT_INSTR(next_instr);
                break;
            }

            if (b->b_type == EXCEPT_HANDLER) {
                /* leaving an except clause by any (pseudo) exception */
                unwind_except_handler(po, b, true);
                continue;
            }

            /* clear the stack up to b->b_level */
            while (STACK_LEVEL() > b->b_level) {
                POP_DECREF(); /* no NULLs here */
            }
            if (b->b_type == SETUP_LOOP && PycException_Is(po, &EBreak)) {
                int next_instr;
                clear_pseudo_exception(po);
                JUMPTO(b->b_handler);
                SAVE_NEXT_INSTR(next_instr);
                break;
            }
            if ((b->b_type == SETUP_EXCEPT || b->b_type == SETUP_FINALLY) &&
                PycException_IsPython(po)) {
                /* as 3.7 ceval: an EXCEPT_HANDLER block over six stack items
                   (see cimpl_push_exc_info), for a finally block too, so that
                   the exception is the handled one inside it; END_FINALLY
                   re-raises it from the top three. A virtual exception is
                   raised for real first, so that the run-time code can fetch
                   it. */
                int next_instr, i, handler = b->b_handler;
                vinfo_array_t *array;
                vinfo_t *word = catcher_word(po), *own, *ok;
                if (!compute_and_raise_exception(po))
                    break;
                clear_pseudo_exception(po);
                block_setup(po, EXCEPT_HANDLER, -1,
                            STACK_LEVEL()); /* reuses *b */
                array = array_new(6);
                if (word != NULL) {
                    own = own_slot();
                    ok = psyco_generic_call(po, cimpl_push_exc_info,
                                            CfNoReturnValue, "Arr", array,
                                            word, own);
                    vinfo_decref(own, po);
                }
                else
                    ok = psyco_generic_call(po, cimpl_push_exc_info,
                                            CfNoReturnValue, "All", array, 0L,
                                            0L);
                if (ok == NULL) {
                    array_release(array);
                    break;
                }
                for (i = 0; i < 6; i++) PUSH(array->items[i]);
                array_release(array);
                JUMPTO(handler);
                SAVE_NEXT_INSTR(next_instr);
                /* a callee that raised may have started tracing */
                po->pr.resume_pending = 1;
                break;
            }
            if (b->b_type == SETUP_FINALLY) {
                /* a pseudo-exception (EReturn, EBreak, EContinue) entering a
                   finally block: unlike Python, it is pushed as a single
                   3-tuple, which END_FINALLY tells apart from an exception
                   type and re-raises */
                int next_instr;
                vinfo_t *exc_info = PsycoTuple_New(3, NULL);
                PycException_Fetch(po);
                PsycoTuple_SET_ITEM(exc_info, 0, po->pr.exc);
                PsycoTuple_SET_ITEM(exc_info, 1, po->pr.val);
                PsycoTuple_SET_ITEM(exc_info, 2, po->pr.tb);
                po->pr.exc = NULL;
                po->pr.val = NULL;
                po->pr.tb = NULL;
                PUSH(exc_info);
                JUMPTO(b->b_handler);
                SAVE_NEXT_INSTR(next_instr);
                break;
            }
        } /* end of unwind stack */

        /* End the function if we still have a (pseudo) exception */
        if (PycException_Occurred(po)) {
            break;
        }
        /* a handler is reached without passing 'fine'. Not at the top
           of the main loop: an instruction restarted after a promotion would
           forget the promoted type again, for ever */
        Psyco_ForgetClasses(po);
    }

    /* function return (either by RETURN or by exception). */
    while ((code1 = exit_function(po)) == NULL) {
        /* If LOC_EXCEPTION is still set after exit_function(), it
           means an exception was raised while handling the function
           return... In this case, we do a function return again,
           this time with the newly raised exception. XXX make sure
           this loop cannot be endless */
    }

finished:
#if ALL_CHECKS
    if (PyErr_Occurred()) {
        fprintf(stderr,
                "psyco: unexpected Python exception during compilation:\n");
        PyErr_WriteUnraisable(Py_None);
    }
#endif

#if LE_CODESTATS
    if (cs_owner == po)
        codestats_flush(po);
    cs_owner = cs_saved_owner;
#endif
    Py_SetRecursionLimit(saved_rec_limit);

    PyErr_Restore(old_py_exc, old_py_val, old_py_tb);
    return code1;
}
