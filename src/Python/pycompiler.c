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
#include "pbltinmodule.h"
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
#include "../Objects/pclassobject.h"
#include "../Objects/ptupleobject.h"
#include "../Objects/psliceobject.h"
#include "../Objects/punicodeobject.h"

#include "pycinternal.h"
#include <ipyencoding.h>

#if LE_HAS_OPCODE_LOAD_COMMON_CONSTANT
EXTERNFN PyObject *
le_common_constant(int i); /* pyruntime.c */
#endif

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

/* names looked up by the run-time helpers, interned at start-up
   (psyco_pycompiler_init) */
static PyObject *s_initializing, *s_name, *s_spec, *s_match_args, *s_get,
    *s_build_class, *s_keys;

#if LE_HAS_IMPORT_FROM_SPEC
/* moduleobject.c:_PyModuleSpec_IsInitializing: spec._initializing is
   true; an error is cleared and counts as false */
static int
le_module_spec_is_initializing(PyObject *spec)
{
    PyObject *value;
    int rc = spec == NULL ? 0 : le_lookup_attr(spec, s_initializing, &value);
    if (rc > 0) {
        rc = PyObject_IsTrue(value);
        Py_DECREF(value);
    }
    if (rc < 0) {
        PyErr_Clear();
        rc = 0;
    }
    return rc;
}
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
    PyObject *dict = le_type_dict(tp);
    PyObject *d = dict ? PyDict_GetItemString(dict, meth_name) : NULL;
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

    va_start(vargs, fmt);
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
   and 'own' its slot; not where making the exception failed, as the
   traceback is then the failure's. */
static void
cimpl_push_exc_info(PyObject *target[], PyObject **word,
                    stack_frame_info_t **own)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *otype, *ovalue, *otb;
    PyObject *exc, *val, *tb, *raised_tb;
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
    raised_tb = tb;
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
    if (word != NULL && tb == raised_tb && PyTraceBack_Check(tb))
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

#if LE_HAS_EXCEPTION_TABLE
/* Catching an exception at a handler from the exception table, as
   ceval.c's exception_unwind: the exception being raised is taken
   (exc.h:le_exc_take), and its value returned for the handler's stack (a
   new reference). The handled exception changes only at the handler's
   PUSH_EXC_INFO. 'word' and 'own' as for cimpl_push_exc_info. */
static PyObject *
cimpl_catch(PyObject **word, stack_frame_info_t **own)
{
    PyObject *val, *tb;
    extra_assert(PyErr_Occurred());
    val = le_exc_take(PyThreadState_GET());
    tb = ((PyBaseExceptionObject *)val)->traceback;
    if (word != NULL && tb != NULL && PyTraceBack_Check(tb))
        psyco_frame_catch(((PyTracebackObject *)tb)->tb_frame, word, own);
    return val;
}

/* PUSH_EXC_INFO: 'value' becomes the handled exception, and the one it
   replaces is returned (a new reference; None for none) */
static PyObject *
cimpl_push_exc_value(PyObject *value)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *old = le_exc_info_value(tstate);
    Py_INCREF(value);
    le_exc_info_put_value(tstate, value);
    if (old == NULL) {
        old = Py_None;
        Py_INCREF(old);
    }
    return old;
}

/* POP_EXCEPT: 'value', which PUSH_EXC_INFO saved, is the handled exception
   again (not consumed: the caller decrefs it); None, for none, is stored as
   NULL, as from 3.13 ceval does: a generator's exception state then gives
   way to its caller's, which _PyErr_GetTopmostException finds past a NULL
   but from 3.13 not past None. With it, as a rule, goes the last hold on
   the frame in the catcher word 'word', as in cimpl_pop_exc_info. */
static void
cimpl_pop_exc_value(PyObject *value, PyObject **word)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *old = le_exc_info_value(tstate);
    if (value == Py_None)
        value = NULL;
    Py_XINCREF(value);
    le_exc_info_put_value(tstate, value);
    Py_XDECREF(old);
    if (word != NULL)
        psyco_frame_catcher_drop(word);
}

/* RERAISE: the exception value is raised again, with the traceback it has */
static void
cimpl_reraise_value(PyObject *value)
{
    PyObject *type = (PyObject *)Py_TYPE(value);
    Py_INCREF(type);
    Py_INCREF(value);
    PyErr_Restore(type, value, PyException_GetTraceback(value));
}

#if !LE_HAS_OPCODE_LOAD_SPECIAL
/* WITH_EXCEPT_START's call: exit_func(type, value, traceback) of the
   exception value */
static PyObject *
cimpl_with_exit_exc(PyObject *exit_func, PyObject *value)
{
    PyObject *tb = PyException_GetTraceback(value);
    PyObject *stack[3] = {(PyObject *)Py_TYPE(value), value, tb};
    Py_XDECREF(tb);
    return PyObject_Vectorcall(exit_func, stack, 3, NULL);
}
#endif
#endif

#if LE_HAS_OPCODE_LOAD_SPECIAL

/* LOAD_SPECIAL: the special method (__enter__, __exit__ and the async
   ones) on the type of 'owner', in out[0], as ceval's
   _PyObject_LookupSpecialMethod finds it: 1 for a method descriptor,
   which is called with 'owner' as its self; else 0, for the attribute
   bound to 'owner'. -1 with ceval's error where the type has none. */
static int
cimpl_load_special(PyObject *owner, long which, PyObject *out[])
{
    PyObject *found =
        _PyType_Lookup(Py_TYPE(owner), _Py_SpecialMethods[which].name);
    descrgetfunc get;
    if (found == NULL) {
        PyErr_Format(PyExc_TypeError,
                     _PyEval_SpecialMethodCanSuggest(owner, (int)which)
                         ? _Py_SpecialMethods[which].error_suggestion
                         : _Py_SpecialMethods[which].error,
                     owner);
        return -1;
    }
    Py_INCREF(found);
    if (PyType_HasFeature(Py_TYPE(found), Py_TPFLAGS_METHOD_DESCRIPTOR)) {
        out[0] = found;
        return 1;
    }
    get = Py_TYPE(found)->tp_descr_get;
    if (get != NULL) {
        out[0] = get(found, owner, (PyObject *)Py_TYPE(owner));
        Py_DECREF(found);
        if (out[0] == NULL)
            return -1;
    }
    else
        out[0] = found;
    return 0;
}

/* WITH_EXCEPT_START's call where LOAD_SPECIAL may have left __exit__
   beside its self: exit_func([self,] type, value, traceback) */
static PyObject *
cimpl_with_exit_exc_self(PyObject *exit_func, PyObject *self, PyObject *value)
{
    PyObject *tb = PyException_GetTraceback(value);
    PyObject *stack[4] = {self, (PyObject *)Py_TYPE(value), value, tb};
    PyObject *res;
    Py_XDECREF(tb);
    if (self != NULL)
        res = PyObject_Vectorcall(exit_func, stack, 4, NULL);
    else
        res = PyObject_Vectorcall(exit_func, stack + 1, 3, NULL);
    return res;
}
#endif

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

/* the frame object that already stands for a compiled frame running code:
   the real one, one asked for, or its catcher word's; NULL if none */
static PyFrameObject *
existing_frame(PyCodeObject *code, PyObject **word)
{
    PyFrameObject *f = psyco_running_real_frame(code);
    if (f == NULL)
        f = psyco_innermost_activation(code);
    if (f == NULL && word != NULL)
        f = (PyFrameObject *)*word;
    return f;
}

/* a re-raise's place in the frame, where it has one: the exception leaves
   the frame from there, with no new traceback entry */
static void
cimpl_reraise_place(PyCodeObject *code, int pos, int lineno, PyObject **word)
{
    PyFrameObject *f = existing_frame(code, word);
    if (f != NULL)
        set_frame_place(f, pos, lineno);
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
    PyFrameObject *result = existing_frame(code, word);
    le_frame_t *back = le_frame_current(tstate);
    if (result != NULL) {
        Py_INCREF(result);
        set_frame_place(result, lasti, lineno);
        return result;
    }
    /* frame objects are not created in stack order with Psyco, so it's
       probably better not to create plain wrong chained lists */
    le_frame_set_current(tstate, NULL);
    result = le_pyframe_new(tstate, code, globals);
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
#if LE_HAS_CURRENT_EXCEPTION
    /* PyTraceBack_Here needs an exception being raised, which there may not
       be yet: the entry is made directly */
    newtb = le_traceback_entry(oldtb, f);
    Py_XDECREF(oldtb);
    Py_DECREF(f);
    return newtb;
#endif
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

#if LE_HAS_INTERPRETER_FRAME
#define FRAME_localsplus FMUT(CPY_ARRAY(le_frame_t, localsplus))
#define FRAME_frame_obj FMUT(CPY_FIELD(le_frame_t, frame_obj, NO_PREV_FIELD))
#else
#define FRAME_localsplus FMUT(CPY_ARRAY(PyFrameObject, f_localsplus))
#endif

static bool
is_offset_int(vinfo_t *v);

#if LE_RESUMES_ON_FRAME
/* A resumed generator's word i (a local, cell, free variable or stack
   item, in the frame's order) that its frame still holds
   (vcompiler.c:PsycoObject_FromGenFrame): a virtual value, one source
   per word, read only where the code uses it. A local or stack item is
   then taken from the frame, which is left NULL there, as a value that may
   be NULL (RunTime_MaybeNull) and owns its reference: a read tests it
   (local_bound, stack_words_taken), and a store over it drops it there,
   as ceval's does (set_local). A cell or free variable is only read, and
   stays the frame's. So the frame holds every word still virtual, and
   only the others are stored at a pause (yield_resumable). */
static source_virtual_t frame_words[LE_FRAME_WORDS_MAX];

DEFINEFN vinfo_t *
psyco_frame_word(int i)
{
    return vinfo_new(VirtualTime_New(&frame_words[i]));
}

DEFINEFN int
psyco_frame_word_index(vinfo_t *v)
{
    source_virtual_t *sv;
    if (v == NULL || !is_virtualtime(v->source))
        return -1;
    sv = VirtualTime_Get(v->source);
    if (sv < frame_words || sv >= frame_words + LE_FRAME_WORDS_MAX)
        return -1;
    return (int)(sv - frame_words);
}

DEFINEFN vinfo_t *
psyco_frame_words_frame(PsycoObject *po)
{
    return po->vlocals.items[GENEXPR_SLOT_ACC(po->pr.co)];
}

/* whether word i is a cell or free variable, which the frame keeps */
static bool
frame_word_kept(PyCodeObject *co, int i)
{
    if (i >= le_code_nlocals(co))
        return i < le_code_nlocalsplus(co);
    return le_code_local_is_cell(co, i);
}

/* word i as the frame holds it now, borrowed, NULL for none */
static vinfo_t *
frame_word_read(PsycoObject *po, int i)
{
#if LE_HAS_FRAME_STACKREFS
    /* PyStackRef_AsPyObjectBorrow: the word less its tag bit */
    vinfo_t *w = psyco_get_nth_field(po, psyco_frame_words_frame(po),
                                     FRAME_localsplus, i);
    vinfo_t *mask, *o;
    if (w == NULL)
        return NULL;
    mask = vinfo_new(CompileTime_New(~(long)Py_TAG_REFCNT));
    o = integer_and(po, w, mask);
    vinfo_decref(mask, po);
    vinfo_decref(w, po);
    return o;
#else
    return psyco_get_nth_field(po, psyco_frame_words_frame(po),
                               FRAME_localsplus, i);
#endif
}

/* the word that leaves word i of the frame empty: NULL, or PyStackRef_NULL
   where the words are _PyStackRefs */
static vinfo_t *
frame_word_empty(void)
{
#if LE_HAS_FRAME_STACKREFS
    return vinfo_new(CompileTime_New((long)PyStackRef_NULL.bits));
#else
    return psyco_vi_Zero();
#endif
}

/* word i taken from the frame, which is left NULL there: a reference,
   NULL for none */
static vinfo_t *
frame_word_take(PsycoObject *po, int i)
{
    vinfo_t *frame = psyco_frame_words_frame(po), *w, *empty;
    bool ok;
    w = psyco_get_nth_field(po, frame, FRAME_localsplus, i);
    if (w == NULL)
        return NULL;
    empty = frame_word_empty();
    ok = psyco_put_nth_field(po, frame, FRAME_localsplus, i, empty);
    vinfo_decref(empty, po);
    if (!ok) {
        vinfo_decref(w, po);
        return NULL;
    }
#if LE_HAS_FRAME_STACKREFS
    {
        vinfo_t *o = psyco_stackref_steal(po, w);
        vinfo_decref(w, po);
        w = o;
    }
#endif
    w->source = add_rtref(w->source) | RunTime_MaybeNull;
    return w;
}

static bool
compute_frame_word(PsycoObject *po, vinfo_t *v)
{
    int i = psyco_frame_word_index(v);
    vinfo_t *w = frame_word_kept(po->pr.co, i) ? frame_word_read(po, i)
                                               : frame_word_take(po, i);
    if (w == NULL)
        return false;
    vinfo_move(po, v, w);
    return true;
}

/* from a locals map (codegen.c:lm_value), whose only item is the frame */
static PyObject *
direct_compute_frame_word(vinfo_t *v, char *data)
{
    vinfo_t *f = v->array->items[0];
    le_frame_t *frame;
    PyObject *o;
    if (is_compiletime(f->source))
        frame = (le_frame_t *)CompileTime_Get(f->source)->value;
    else
        frame = *(le_frame_t **)(data + getstack(f->source));
    o = LE_FRAME_WORD(frame, psyco_frame_word_index(v));
    Py_XINCREF(o);
    return o;
}

/* a word still the frame's, for a call that reads it (locals_calls,
   resume_in_ceval): read, not taken. Any other value as it is. The result
   is a new vinfo reference. */
static vinfo_t *
frame_word_peek(PsycoObject *po, vinfo_t *v)
{
    int i = psyco_frame_word_index(v);
    if (i < 0) {
        vinfo_incref(v);
        return v;
    }
    return frame_word_read(po, i);
}

/* a value that may be NULL (RunTime_MaybeNull), tested: 1 on the path
   where it isn't, from then on known not to be; 0 on the path where it
   is, where it owns nothing (and its holder replaces it with the NULL
   constant); -1 on an error */
static int
null_tested(PsycoObject *po, vinfo_t *v)
{
    condition_code_t cc = integer_non_null(po, v);
    if (cc == CC_ERROR)
        return -1;
    if (!runtime_condition_f(po, INVERT_CC(cc))) {
        v->source &= ~RunTime_MaybeNull;
        return 1;
    }
    v->source = remove_rtref(v->source);
    return 0;
}

#if LE_HAS_GET_ITER_INDEX
static int
cimpl_frame_iterators_real(le_frame_t *f, long nstack)
{
    if (le_frame_iterators_real(f, (int)nstack))
        return 0;
    PyErr_NoMemory();
    return -1;
}

/* Where ceval made or paused the resumed frame, an index (a tagged int)
   may be above an exact list, tuple, str or bytes on its stack. Compiled
   code that steps the generator directly (psyfunc.c:gen_send) passes no
   eval hook to make each such pair an iterator and NULL
   (le_frame_iterators_real), so the entry does, before any item is taken. */
static bool
stack_iterators_real(PsycoObject *po)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *frame = psyco_frame_words_frame(po);
    int nplus = le_code_nlocalsplus(po->pr.co), nstack = 0, k;
    while (nstack < po->pr.stack_level &&
           psyco_frame_word_index(stack_a[nstack]) == nplus + nstack)
        nstack++;
    for (k = nstack - 1; k >= 1; k--) {
        vinfo_t *w = psyco_get_nth_field(po, frame, FRAME_localsplus,
                                         nplus + k),
                *mask, *tag;
        condition_code_t cc;
        if (w == NULL)
            return false;
        mask = vinfo_new(CompileTime_New(Py_TAG_BITS));
        tag = integer_and(po, w, mask);
        vinfo_decref(mask, po);
        vinfo_decref(w, po);
        if (tag == NULL)
            return false;
        cc = integer_cmp_i(po, tag, Py_INT_TAG, Py_EQ);
        vinfo_decref(tag, po);
        if (cc == CC_ERROR)
            return false;
        if (runtime_condition_f(po, cc))
            return psyco_generic_call(po, cimpl_frame_iterators_real,
                                      CfCommonIntZeroOk | CfNoPython, "vl",
                                      frame, (long)nstack) != NULL;
    }
    return true;
}
#else
#define stack_iterators_real(po) true
#endif

#if LE_HAS_TAGGED_LASTI
/* stack item 'v', still the frame's word, where the entry may be inside an
   exception handler: a handler's lasti there is a tagged int, no object,
   which is taken and promoted to the constant compiled code pushes for it
   (offset_int). True when it isn't one; false when the promotion or an
   error is pending */
static bool
stack_word_not_lasti(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *frame = psyco_frame_words_frame(po), *w, *mask, *tag, *empty;
    int i = psyco_frame_word_index(v);
    condition_code_t cc;
    bool ok;
    w = psyco_get_nth_field(po, frame, FRAME_localsplus, i);
    if (w == NULL)
        return false;
    mask = vinfo_new(CompileTime_New(Py_INT_TAG));
    tag = integer_and(po, w, mask);
    vinfo_decref(mask, po);
    cc = tag == NULL ? CC_ERROR : integer_cmp_i(po, tag, Py_INT_TAG, Py_EQ);
    vinfo_xdecref(tag, po);
    if (cc == CC_ERROR || !runtime_condition_f(po, cc)) {
        vinfo_decref(w, po);
        return cc != CC_ERROR;
    }
    empty = frame_word_empty();
    ok = psyco_put_nth_field(po, frame, FRAME_localsplus, i, empty);
    vinfo_decref(empty, po);
    if (!ok) {
        vinfo_decref(w, po);
        return false;
    }
    vinfo_move(po, v, w);
    psyco_atcompiletime(po, v);
    return false;
}
#endif

/* the stack items a resumed entry starts with, taken from the frame and
   tested before any instruction reads them: a NULL one (PUSH_NULL's) must
   be the NULL constant */
static bool
stack_words_taken(PsycoObject *po)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int k, bound;
#if LE_HAS_TAGGED_LASTI
    int in_handler = -1;
#endif
    if (!stack_iterators_real(po))
        return false;
    for (k = 0; k < po->pr.stack_level; k++) {
        vinfo_t *v = stack_a[k];
        if (psyco_frame_word_index(v) < 0)
            continue;
#if LE_HAS_TAGGED_LASTI
        if (in_handler < 0)
            in_handler = psyco_in_handler(po->pr.co, po->pr.next_instr);
        if (in_handler && !stack_word_not_lasti(po, v))
            return false;
#endif
        if (!compute_vinfo(v, po) || (bound = null_tested(po, v)) < 0)
            return false;
        if (!bound) {
            stack_a[k] = psyco_vi_Zero();
            vinfo_decref(v, po);
        }
    }
    return true;
}

/* whether the run holds local i, not its frame: neither the word the
   frame still holds nor a cell, which the frame keeps, nor NULL (no longer
   used, the frame keeps whatever it has) */
static bool
local_is_runs(PsycoObject *po, int i)
{
    vinfo_t *v = LOC_LOCALS_PLUS[i];
    return v != NULL && psyco_frame_word_index(v) != i &&
           !le_code_local_is_cell(po->pr.co, i);
}

/* a pause (or end) of a run of the frame, listing the locals the run
   holds (frames.c:psyco_gen_pause); its stores come after */
static le_gen_pause_t *
gen_pause_new(PsycoObject *po, int lasti, int nstack)
{
    int i, n = 0, nlocals = le_code_nlocals(po->pr.co);
    le_gen_pause_t *p;
    for (i = 0; i < nlocals; i++) n += local_is_runs(po, i);
    p = psyco_owner_alloc(offsetof(le_gen_pause_t, taken) +
                          (n > 0 ? n : 1) * sizeof(int));
    p->lasti = lasti;
    p->nstack = nstack;
    p->ntaken = 0;
    for (i = 0; i < nlocals; i++)
        if (local_is_runs(po, i))
            p->taken[p->ntaken++] = i;
    return p;
}

/* word k of the frame becomes 'v', a reference it takes, or NULL for the
   NULL constant or none */
static bool
frame_word_store(PsycoObject *po, int k, vinfo_t *v)
{
    vinfo_t *frame = psyco_frame_words_frame(po), *w;
    bool ok;
    if (v == NULL || psyco_knowntobe(v, 0)) {
        vinfo_t *empty = frame_word_empty();
        ok = psyco_put_nth_field(po, frame, FRAME_localsplus, k, empty);
        vinfo_decref(empty, po);
        return ok;
    }
#if LE_HAS_TAGGED_LASTI
    if (is_offset_int(v))
        return psyco_put_nth_field(po, frame, FRAME_localsplus, k, v);
#endif
#if LE_HAS_FRAME_STACKREFS
    w = psyco_stackref_from_steal(po, v);
#else
    w = v;
    vinfo_incref(w);
#endif
    ok = psyco_put_nth_field(po, frame, FRAME_localsplus, k, w);
    vinfo_decref(w, po);
    if (!ok)
        return false;
    consume_reference(po, v);
    return true;
}

/* A resumable generator's run that ends (returns or raises) leaves its
   locals in its frame, as ceval does, for gen_send_ex2 to free in the
   frame's order: those the run holds are stored, a virtual one made real
   on a return; on a raise it is let go (making it could see the pending
   exception), as only compiled code saw it. The slots are emptied. With
   'lasti' (not -1), the frame is also left at that place with 'nstack'
   items on its stack (psyco_gen_pause), which the caller stores. */
static bool
gen_locals_end(PsycoObject *po, int lasti, int nstack)
{
    int i, nlocals = le_code_nlocals(po->pr.co);
    le_gen_pause_t *end = NULL;
    for (i = lasti >= 0 ? -1 : 0; i < nlocals; i++) {
        vinfo_t *v = i < 0 ? NULL : LOC_LOCALS_PLUS[i];
        if (i >= 0 && !local_is_runs(po, i))
            continue;
        if (end == NULL) {
            /* the frame's words that Python wrote meanwhile go first:
               only Python holding the frame's object can have; and a
               frame left at a place always pauses there */
            vinfo_t *frame = psyco_frame_words_frame(po);
            vinfo_t *fo = psyco_get_field(po, frame, FRAME_frame_obj);
            condition_code_t cc =
                fo == NULL ? CC_ERROR : object_non_null(po, fo);
            vinfo_xdecref(fo, po);
            if (cc == CC_ERROR)
                return false;
            end = gen_pause_new(po, lasti, nstack);
            if ((lasti >= 0 || runtime_condition_f(po, cc)) &&
                psyco_generic_call(po, psyco_gen_pause,
                                   CfNoReturnValue | CfNoPython, "vl", frame,
                                   (long)end) == NULL)
                return false;
        }
        if (i < 0)
            continue;
        if (is_virtualtime(v->source) && PycException_Is(po, &EReturn) &&
            !compute_vinfo(v, po))
            return false;
        if (!is_virtualtime(v->source) && !psyco_knowntobe(v, 0) &&
            !frame_word_store(po, i, v))
            return false;
        vinfo_decref(v, po);
        LOC_LOCALS_PLUS[i] = NULL;
    }
    return true;
}
#else
#define psyco_frame_word_index(v) (-1)
#define frame_word_peek(po, v) (vinfo_incref(v), (v))
#endif

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
    bool ok;
    for (i = 0; i < n; i += 5) {
        char tags[8] = "?lvvvvv";
        long args[5];
        vinfo_t *peeked[5];
        tags[0] = tag;
        for (j = 0; j < 5; j++) {
            vinfo_t *v = i + j < n ? LOC_LOCALS_PLUS[i + j] : NULL;
            if (v == NULL || v->source == SOURCE_NOT_IMPORTANT) {
                tags[2 + j] = 'l';
                args[j] = v == NULL && i + j < le_code_nlocals(co)
                              ? 0
                              : (long)LE_LOCAL_UNKNOWN;
                peeked[j] = NULL;
            }
            else
                args[j] = (long)(peeked[j] = frame_word_peek(po, v));
        }
        ok = psyco_generic_call(po, fn, CfNoReturnValue, tags, target, (long)i,
                                args[0], args[1], args[2], args[3],
                                args[4]) != NULL;
        for (j = 0; j < 5; j++) vinfo_xdecref(peeked[j], po);
        if (!ok)
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
            is_virtualtime(v->source) && psyco_frame_word_index(v) < 0)
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
   frame (psyco_catcher_flush) and taken back as it returns
   (psyco_catcher_unflush), and once more as it ends, while something
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
#if LE_HAS_EXCEPTION_TABLE
    le_handler_t h;
    return catcher_word(po) != NULL &&
           le_handler_find(po->pr.co, po->pr.next_instr, &h);
#else
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
#endif
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

/* after the inlined call: the locals catcher_flush wrote for it go again
   (frames.c:psyco_frame_catcher_unflush). The test is compiled out of line
   when first taken. */
DEFINEFN
bool
psyco_catcher_unflush(PsycoObject *po)
{
    vinfo_t *word = catcher_word(po), *now;
    int on;
    if (word == NULL)
        return true;
    now = vinfo_new(RunTime_NewStack(getstack(word->source), false, false));
    on = runtime_NON_NULL_f(po, now);
    if (on <= 0)
        return on == 0;
    return psyco_generic_call(po, psyco_frame_catcher_unflush, CfNoReturnValue,
                              "r", word) != NULL;
}

/* Whether the handler at 'handler' drops the exception unseen
   ('except E: pass' or 'except: pass', with E a name): then nothing can
   reach the traceback's frame. If E doesn't match, END_FINALLY (RERAISE
   from 3.9) re-raises it and puts the locals in then. The test of E is
   COMPARE_OP's exception match and a jump, or from 3.9 one
   JUMP_IF_NOT_EXC_MATCH, or where handlers are in an exception table
   CHECK_EXC_MATCH and a jump, after the handler's PUSH_EXC_INFO and with
   one POP_TOP for the exception. */
static bool
handler_drops_exception(PyCodeObject *co, int handler)
{
    le_code_t c = le_code(co);
    int p = handler, q, i;
#define OP_AT(pos) ((pos) < c.size ? le_instr(c, (pos)).op : -1)
#define NEXT(pos) (le_instr(c, (pos)).next)
#if LE_HAS_EXCEPTION_TABLE
    if (OP_AT(p) != PUSH_EXC_INFO)
        return false;
    p = NEXT(p);
    i = OP_AT(p);
    if (i == LOAD_GLOBAL || i == LOAD_FAST || i == LOAD_DEREF) {
        q = NEXT(p);
        if (OP_AT(q) == CHECK_EXC_MATCH && OP_AT(NEXT(q)) == POP_JUMP_IF_FALSE)
            p = NEXT(NEXT(q));
    }
    return OP_AT(p) == POP_TOP && OP_AT(NEXT(p)) == POP_EXCEPT;
#else
    if (OP_AT(p) == DUP_TOP) {
        q = NEXT(p);
        i = OP_AT(q);
        if (i == LOAD_GLOBAL || i == LOAD_NAME || i == LOAD_FAST ||
            i == LOAD_DEREF) {
            q = NEXT(q);
            if (OP_AT(q) == COMPARE_OP &&
                le_instr(c, q).arg == PyCmp_EXC_MATCH &&
                OP_AT(NEXT(q)) == POP_JUMP_IF_FALSE)
                p = NEXT(NEXT(q));
            else if (OP_AT(q) == JUMP_IF_NOT_EXC_MATCH)
                p = NEXT(q);
        }
    }
    for (i = 0; i < 3; i++, p = NEXT(p))
        if (OP_AT(p) != POP_TOP)
            return false;
    return OP_AT(p) == POP_EXCEPT;
#endif
#undef NEXT
#undef OP_AT
}

/* Whether an exception raised now puts the frame's locals in its entry's
   frame now: not if this frame catches it (catcher_word), nor for an
   inlined frame whose caller drops it unseen. A frame it leaves has them
   as they are when it leaves. */
static bool
locals_wanted_now(PsycoObject *po)
{
    PyCodeObject *co;
#if LE_HAS_EXCEPTION_TABLE
    le_handler_t h;
    int call;
#else
    int i;
#endif
    if (!po->pr.is_inlining)
        return !caught_here(po);
    co = (PyCodeObject *)CompileTime_Get(
             LOC_INLINING->array->items[LOC_INLINE_CO]->source)
             ->value;
#if LE_HAS_EXCEPTION_TABLE
    /* the caller's handler for its call, in the caller's code */
    call = (int)CompileTime_Get(
               LOC_INLINING->array->items[LOC_INLINE_NEXT_INSTR]->source)
               ->value;
    return !le_handler_find(co, call, &h) ||
           !handler_drops_exception(co, h.handler);
#else
    /* an inlined frame's blocks in 'pr' are its caller's, with their
       handlers in the caller's code */
    for (i = po->pr.iblock; i-- > 0;) {
        le_block_t *b = &po->pr.blockstack[i];
        if (b->b_type == SETUP_LOOP)
            continue;
        return !((b->b_type == SETUP_EXCEPT || b->b_type == SETUP_FINALLY) &&
                 handler_drops_exception(co, b->b_handler));
    }
    return true;
#endif
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

/* the frame's place set to pos at run time, for an exception raised again
   (cimpl_reraise_place) */
static void
reraise_place(PsycoObject *po, int pos)
{
    vinfo_t *word = catcher_word(po);
    psyco_generic_call(po, cimpl_reraise_place, CfNoReturnValue,
                       word != NULL ? "lllr" : "llll", (long)po->pr.co,
                       (long)pos, (long)PyCode_Addr2Line(po->pr.co, pos),
                       word != NULL ? (long)word : 0L);
}

/* record a traceback entry for the current virtual Python exception */
PSY_INLINE void
PsycoTraceBack_Here(PsycoObject *po, int pos)
{
    int lasti = le_place(le_code(po->pr.co), pos);
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

/* the builtin super, called with no arguments before an attribute load
   (super_call_known), holding the class and first argument
   typeobject.c:super_init finds in the frame: they are read before the
   class is promoted, and the call compiled again after the promotion
   finds them here. Forced, it is the builtin super. */
static source_virtual_t pending_super;
#define iSUPER_CLASS (iOB_TYPE + 1)
#define iSUPER_SELF (iOB_TYPE + 2)

static bool
compute_pending_super(PsycoObject *po, vinfo_t *v);

INITIALIZATIONFN
void
psyco_pycompiler_init(void)
{
    s_builtin_object = PyUnicode_InternFromString("__builtins__");
    s_initializing = PyUnicode_InternFromString("_initializing");
    s_name = PyUnicode_InternFromString("__name__");
    s_spec = PyUnicode_InternFromString("__spec__");
    s_match_args = PyUnicode_InternFromString("__match_args__");
    s_get = PyUnicode_InternFromString("get");
    s_build_class = PyUnicode_InternFromString("__build_class__");
    s_keys = PyUnicode_InternFromString("keys");
    if (s_builtin_object == NULL || s_initializing == NULL || s_name == NULL ||
        s_spec == NULL || s_match_args == NULL || s_get == NULL ||
        s_build_class == NULL || s_keys == NULL)
        Py_FatalError("lejit: no names for the compiler");
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

#if LE_RESUMES_ON_FRAME
    {
        int i;
        for (i = 0; i < LE_FRAME_WORDS_MAX; i++)
            INIT_SVIRTUAL(frame_words[i], compute_frame_word,
                          direct_compute_frame_word, 0, 0, 0);
    }
#endif
    ERtPython = psyco_vsource_not_important;
    EReturn = psyco_vsource_not_important;
    EContinue = psyco_vsource_not_important;
    EBreak = psyco_vsource_not_important;
    EInline = psyco_vsource_not_important;
    INIT_SVIRTUAL_NOCALL(pending_super, compute_pending_super, 1);
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
#define POP_DECREF_N(n)              \
    do {                             \
        int pops = (n);              \
        while (pops--) POP_DECREF(); \
    } while (0)

/* the cases that replace the top items with what psycofn makes of them */
#define UNARY_OPCODE(opcode, psycofn) \
    case opcode:                      \
        x = psycofn(po, TOP());       \
        if (x == NULL)                \
            break;                    \
        POP_DECREF();                 \
        PUSH(x);                      \
        goto fine
#define BINARY_OPCODE(opcode, psycofn)     \
    case opcode:                           \
        x = psycofn(po, NTOP(2), NTOP(1)); \
        if (x == NULL)                     \
            break;                         \
        POP_DECREF();                      \
        POP_DECREF();                      \
        PUSH(x);                           \
        goto fine
/* ** and **=, whose third argument is None */
#define POWER_OPCODE(opcode, psycofn)         \
    case opcode:                              \
        u = psyco_vi_None();                  \
        x = psycofn(po, NTOP(2), NTOP(1), u); \
        vinfo_decref(u, po);                  \
        if (x == NULL)                        \
            break;                            \
        POP_DECREF();                         \
        POP_DECREF();                         \
        PUSH(x);                              \
        goto fine

/*#define GETCODEOBJ()    ((PyCodeObject*)(KNOWN_SOURCE(LOC_CODE)->value))*/
#define GETCONST(i) (PyTuple_GET_ITEM(co->co_consts, i))
#define GETNAMEV(i) (PyTuple_GET_ITEM(co->co_names, i))

#define GETLOCAL(i) (LOC_LOCALS_PLUS[i])
#define SETLOCAL(i, v) set_local(po, i, v)

/* local i becomes 'v', and what it was goes: where that is a word its
   frame still holds, it is taken from the frame first, to be dropped here
   as ceval drops it */
static inline void
set_local(PsycoObject *po, int i, vinfo_t *v)
{
    vinfo_t *old = LOC_LOCALS_PLUS[i];
    if (psyco_frame_word_index(old) >= 0 && !compute_vinfo(old, po)) {
        /* out of memory: the frame keeps it */
        vinfo_decref(old, po);
        LOC_LOCALS_PLUS[i] = NULL;
    }
    psyco_rebind_local(po, &LOC_LOCALS_PLUS[i], v);
}

#define STACK_POINTER() (stack_a + po->pr.stack_level)
/* a callable and the NULL beside it, for a call */
#if LE_HAS_CALLABLE_UNDER_NULL
#define PUSH_WITH_NULL(v) (PUSH(v), PUSH(psyco_vi_Zero()))
#else
#define PUSH_WITH_NULL(v) (PUSH(psyco_vi_Zero()), PUSH(v))
#endif
#define INSTR_OFFSET() (next_instr)
#define STACK_LEVEL() (po->pr.stack_level)
#define JUMPBY(offset) (next_instr += (offset))
#define JUMPTO(target) (next_instr = (target))

/* a place ceval keeps on the value stack: CALL_FINALLY's return offset, or
   the lasti a handler is given. ceval's int object for it, or, where the
   frame holds a handler's lasti as a tagged int, that word, which is no
   object. One constant per value, so that two states pushing it compare
   equal (a fixed constant must match exactly) */
static vinfo_t *
offset_int(int offset)
{
#if LE_HAS_TAGGED_LASTI
    return vinfo_new(CompileTime_NewSk(
        sk_new((long)PyStackRef_TagInt(offset).bits, SkFlagFixed)));
#else
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
#endif
}

static bool
is_offset_int(vinfo_t *v)
{
    source_known_t *sk;
    if (!is_compiletime(v->source))
        return false;
    sk = CompileTime_Get(v->source);
#if LE_HAS_TAGGED_LASTI
    return !(sk->refcount1_flags & SkFlagPyObj) &&
           (sk->value & Py_INT_TAG) == Py_INT_TAG;
#else
    return (sk->refcount1_flags & SkFlagPyObj) &&
           PyLong_CheckExact((PyObject *)sk->value);
#endif
}

static int
offset_int_value(vinfo_t *v)
{
    long w = CompileTime_Get(v->source)->value;
#if LE_HAS_TAGGED_LASTI
    return (int)PyStackRef_UntagInt((_PyStackRef){.bits = (uintptr_t)w});
#else
    return (int)PyLong_AsLong((PyObject *)w);
#endif
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
   the keys of globals still have the shape, and hold the same name and
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

/* The shape of the keys of a dict and where in them the entry holding
   name and value has its key, its value in the next word: false if there
   is none (a key equal to the name but another object, values kept apart
   from the keys, or keys too large for the code's 32-bit fields). */
static bool
global_entry_find(PyObject *globals, PyObject *name, PyObject *value,
                  long *shape, long *offset)
{
    PyDictObject *mp = (PyDictObject *)globals;
    le_dictkeys_t *keys = (le_dictkeys_t *)mp->ma_keys;
    Py_ssize_t i;
    if (mp->ma_values != NULL)
        return false;
    for (i = 0; i < keys->dk_nentries; i++) {
        PyObject **key = le_dict_key_slot(keys, i);
        if (key[0] == name && key[1] == value) {
            *shape = le_dict_shape(keys);
            *offset = (char *)key - (char *)keys;
            return *offset + 2 * sizeof(PyObject *) <= INT32_MAX;
        }
    }
    return false;
}

static void
global_tags_set(global_guard_t *guard, PyObject *globals, PyObject *builtins)
{
#if LE_HAS_DICT_VERSION_TAG
    guard->tags[0] = ((PyDictObject *)globals)->ma_version_tag;
    guard->tags[1] = ((PyDictObject *)builtins)->ma_version_tag;
#else
    /* the entry form, which reads none, has them where there are none */
    uint64_t *g = le_dict_counter((PyDictObject *)globals);
    uint64_t *b = le_dict_counter((PyDictObject *)builtins);
    guard->tags[0] = g != NULL ? *g : 0;
    guard->tags[1] = b != NULL ? *b : 0;
#endif
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
    long shape, offset;
    code_t *code;
    changed_global_t *prev = respawning_global;
    PyObject *x = PyDict_GetItem(cg->globals, cg->name);
    LE_STAT(psyco_stat_global_fails++);
    if (x == NULL && cg->form != GLOBAL_ENTRY)
        x = PyDict_GetItem(cg->builtins, cg->name);
    if (x != cg->value) {
        /* the respawn compiles a version that never comes back here */
        if (cg->form == GLOBAL_ENTRY && x != NULL &&
            global_entry_find(cg->globals, cg->name, x, &shape, &offset))
            psyco_global_guard_move(&cg->guard->entry, shape, offset);
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
    else if (global_entry_find(cg->globals, cg->name, x, &shape, &offset))
        psyco_global_guard_move(&cg->guard->entry, shape, offset);
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
    long shape, offset;
    PyObject *x = cimpl_load_global(vg->globals, vg->builtins, vg->name);
    if (x != NULL &&
        global_entry_find(vg->globals, vg->name, x, &shape, &offset))
        psyco_global_guard_move(&vg->entry, shape, offset);
    return x;
}

/* Emitted whether or not globals hold the name now (a shape of -1 never
   matches), so that a replay changes the compiler's state the same way. */
static bool
load_global_varying(PsycoObject *po, PyObject *globals, PyObject *builtins,
                    PyObject *name, varying_global_t **vgp, vinfo_t **result)
{
    static varying_global_t respawn_vg; /* written for trashed code only */
    varying_global_t *vg;
    PyObject *x = PyDict_GetItem(globals, name);
    long shape = -1, offset = 0;
    vinfo_t *v;
    if (x != NULL && !global_entry_find(globals, name, x, &shape, &offset))
        shape = -1;
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
    v = psyco_global_load(po, globals, name, &vg->entry, shape, offset,
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
    long shape, offset;
    PyObject *x = PyDict_GetItem(sg->globals, sg->name);
    if (x == NULL ||
        !global_entry_find(sg->globals, sg->name, x, &shape, &offset))
        return sg->varying; /* its load calls C */
    psyco_global_guard_move(sg->guard, shape, offset);
    psyco_global_guard_move(&sg->check, shape, offset);
    psyco_global_guard_move(&sg->vg->entry, shape, offset);
    return x == sg->value ? sg->resume : sg->varying;
}

/* the varying version of cg's guard, just respawned: the code is entered
   from the guard's jump, with the state the guard left */
static bool
load_global_rearmed(PsycoObject *po, changed_global_t *cg, vinfo_t **result)
{
    long shape = -1, offset = 0;
    code_t *stub;
    stale_global_t *sg;
    PyObject *x = PyDict_GetItem(cg->globals, cg->name);
    if (x != NULL &&
        !global_entry_find(cg->globals, cg->name, x, &shape, &offset))
        shape = -1;
    sg = psyco_new_stub(&do_stale_global, sizeof(stale_global_t), &stub);
    sg->guard = &cg->guard->entry;
    sg->globals = cg->globals;
    sg->name = cg->name;
    sg->value = cg->value;
    sg->resume = cg->resume;
    psyco_global_stale(po, cg->globals, cg->name, &sg->check, shape, offset,
                       stub);
    sg->varying = po->code;
    return load_global_varying(po, cg->globals, cg->builtins, cg->name,
                               &sg->vg, result);
}

/* o is a lazy import's proxy (PEP 810), which only a load at run time
   may replace with its module */
static inline bool
le_lazy_import(PyObject *o)
{
#if LE_HAS_LAZY_IMPORT_REIFY_LINKABLE
    return PyLazyImport_CheckExact(o);
#else
    (void)o;
    return false;
#endif
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
    long shape = 0, offset = 0;
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
        if (global_entry_find(globals, name, value, &shape, &offset))
            form = GLOBAL_ENTRY;
    }
    else {
        value = PyDict_GetItem(builtins, name);
        if (value != NULL && psyco_global_absent(globals, name, &absent))
            form = GLOBAL_ABSENT;
    }
#if !LE_HAS_DICT_VERSION_TAG
    /* the other forms compare the counts of LeJIT's dict watcher */
    if (form != GLOBAL_ENTRY &&
        (le_dict_counter((PyDictObject *)globals) == NULL ||
         le_dict_counter((PyDictObject *)builtins) == NULL))
        return load_global_varying(po, globals, builtins, name, NULL, result);
#endif

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
                            form == GLOBAL_ENTRY ? &guard->entry : NULL, shape,
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
       later definition. A lazy import's proxy is replaced by the run-time
       load, which imports then, as ceval does; the guard then fails. */
    if (value != NULL && !le_lazy_import(value)) {
        Py_INCREF(value);
        *result =
            vinfo_new(CompileTime_NewSk(sk_new((long)value, SkFlagPyObj)));
    }
    return true;
}

/* CALL_FUNCTION (oparg positional arguments), CALL_FUNCTION_KW (oparg
   arguments, the last ones named by the constant tuple on top) and CALL
   (oparg arguments, the last ones named by 'kwnames', its KW_NAMES
   constant, or none), as 3.7's call_function(). The stack, top down:
   [names tuple], arguments, and the callable under them or under the NULL
   beside them. *stack_to_pop counts everything including the callable
   (not the NULL). Keyword arguments are pending keywords
   (pdictobject.c), which a Python function's call binds, a vectorcall
   passes with their names tuple, and anything else makes into the dict
   CPython makes: duplicates are impossible here (the
   bytecode compiler rejects them) and the callee checks the rest. */
#if LE_HAS_OPCODE_CALL
/* where the KW_NAMES naming the keyword arguments of the CALL at 'pos'
   (its first unit) starts, with its EXTENDED_ARGs, or -1 for none; -2 if
   no PRECALL comes right before where CPython has one (mergepoints.c
   refuses such code). */
static int
call_kwnames_start(le_code_t c, int pos)
{
#if LE_HAS_OPCODE_PRECALL
    if (pos <= 0 || le_instr(c, pos = le_prev(c, pos)).op != PRECALL)
        return -2;
#endif
    if (pos <= 0 || le_instr(c, pos = le_prev(c, pos)).op != KW_NAMES)
        return -1;
    return pos;
}

/* the keyword names of the CALL at 'pos': the constant of its KW_NAMES,
   or NULL for none. False with SystemError if it has no PRECALL where
   CPython has one. */
static bool
call_kwnames(PsycoObject *po, PyCodeObject *co, int pos, PyObject **kwnames)
{
    le_code_t c = le_code(co);
    int p = call_kwnames_start(c, pos);
    *kwnames = NULL;
    if (p == -2) {
        PycException_SetString(po, PyExc_SystemError, "CALL without PRECALL");
        return false;
    }
    if (p >= 0)
        *kwnames = PyTuple_GET_ITEM(co->co_consts, le_instr(c, p).arg);
    return true;
}
#endif

static void
end_call_site(PsycoObject *po)
{
    po->pr.call_site = 0;
}

static vinfo_t *
psyco_ext_do_calls(PsycoObject *po, int opcode, int oparg, PyObject *kwnames,
                   vinfo_t *callable, vinfo_t **stack_top, int *stack_to_pop)
{
    int na = oparg, nk = 0;
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
        stack_top--;
    }
    if (kwnames != NULL) {
        nk = PyTuple_GET_SIZE(kwnames);
        na = oparg - nk;
    }
    args = stack_top - oparg;
    *stack_to_pop = oparg + 1 + (opcode == CALL_FUNCTION_KW);

    if (nk == 0)
        wdict = psyco_vi_Zero(); /* no keyword arguments */
    else
        wdict = PsycoDict_PendingKeywords(kwnames, args + na);
    vargs = PsycoTuple_New(na, args); /* virtual tuple */
    result = PsycoObject_Call(po, callable, vargs, wdict);
    vinfo_xdecref(vargs, po);
    vinfo_xdecref(wdict, po);
    return result;
}

/***************************************************************/
/***   Run-time implementation of various opcodes            ***/

/* the code of the following functions is "copy-pasted" from ceval.c */

/* a frame's own cells, which CPython made before the frame reached LeJIT
   (PsycoObject_FromCode maps them as borrowed words) or the caller's
   binder made (psyfunc.c:bind_cells); a cell can change under any call
   (nonlocal in a callee), so its content is read each time.
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
        vinfo_t *cell = GETLOCAL(le_code_deref_slot(co, oparg)), *zero;
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
   f-string's BUILD_STRING with only loads, conversions and other
   FORMAT_VALUEs between, read from the bytecode */
static bool
feeds_build_string(PyCodeObject *co, int next_instr)
{
    le_code_t c = le_code(co);
    int depth = 1;
    while (next_instr < c.size) {
        le_instr_t in = le_instr(c, next_instr);
        int op = in.op, arg = in.arg;
        next_instr = in.next;
        if (op == LOAD_CONST || op == LOAD_FAST || op == LOAD_DEREF)
            depth++;
        else if (op == LOAD_FAST_LOAD_FAST)
            depth += 2;
        else if (op == CONVERT_VALUE)
            continue;
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
/* ceval.c's code, copied for the cases to call at run time: its helpers
   (do_call_core, format_kwargs_error...) and the parts of cases that the
   compiled cases leave to CPython */
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
#if LE_HAS_CEVAL_TRACE_INFO
typedef struct {
    PyCodeObject *code;
    PyCodeAddressRange bounds;
    CFrame cframe;
} PyTraceInfo;

static void
LeLineTable_InitAddressRange(const char *linetable, Py_ssize_t length,
                             int firstlineno, PyCodeAddressRange *range)
{
    range->opaque.lo_next = linetable;
    range->opaque.limit = range->opaque.lo_next + length;
    range->ar_start = -1;
    range->ar_end = 0;
    range->opaque.computed_line = firstlineno;
    range->ar_line = -1;
}

static int
_LeCode_InitAddressRange(PyCodeObject *co, PyCodeAddressRange *bounds)
{
    const char *linetable = PyBytes_AS_STRING(co->co_linetable);
    Py_ssize_t length = PyBytes_GET_SIZE(co->co_linetable);
    LeLineTable_InitAddressRange(linetable, length, co->co_firstlineno,
                                 bounds);
    return bounds->ar_line;
}

static void
cimpl_initialize_trace_info(PyTraceInfo *trace_info, PyFrameObject *frame)
{
    if (trace_info->code != frame->f_code) {
        trace_info->code = frame->f_code;
        _LeCode_InitAddressRange(frame->f_code, &trace_info->bounds);
    }
}

/* the trace info that 3.10's ceval hands to the functions that trace */
#define TRACE_INFO_PARAM PyTraceInfo *trace_info,
#define TRACE_INFO_ARG trace_info,
#define CALL_USE_TRACING (trace_info->cframe.use_tracing)
#define TSTATE_USE_TRACING(tstate) ((tstate)->cframe->use_tracing)
#else
#define TRACE_INFO_PARAM
#define TRACE_INFO_ARG
#define CALL_USE_TRACING (tstate->use_tracing)
#define TSTATE_USE_TRACING(tstate) ((tstate)->use_tracing)
#endif

/* where ceval's do_call_core sends the profiler's c_call events itself */
#define TRACES_C_CALLS \
    ((!LE_HAS_CFRAME && !LE_HAS_MONITORING) || LE_HAS_CEVAL_TRACE_INFO)

#if TRACES_C_CALLS
static int
cimpl_call_trace(Py_tracefunc func, PyObject *obj, PyFrameObject *frame,
                 TRACE_INFO_PARAM int what, PyObject *arg)
{
    PyThreadState *tstate = PyThreadState_GET();
    int result;
    if (tstate->tracing)
        return 0;
    tstate->tracing++;
    TSTATE_USE_TRACING(tstate) = 0;
#if LE_HAS_CEVAL_TRACE_INFO
    if (frame->f_lasti < 0) {
        frame->f_lineno = frame->f_code->co_firstlineno;
    }
    else {
        cimpl_initialize_trace_info(trace_info, frame);
        frame->f_lineno = _PyCode_CheckLineNumber(
            frame->f_lasti * sizeof(_Py_CODEUNIT), &trace_info->bounds);
    }
#endif
    result = func(obj, frame, what, arg);
#if LE_HAS_CEVAL_TRACE_INFO
    frame->f_lineno = 0;
#endif
    TSTATE_USE_TRACING(tstate) =
        ((tstate->c_tracefunc != NULL) || (tstate->c_profilefunc != NULL));
    tstate->tracing--;
    return result;
}

static int
cimpl_call_trace_protected(Py_tracefunc func, PyObject *obj,
                           PyFrameObject *frame, TRACE_INFO_PARAM int what,
                           PyObject *arg)
{
    PyObject *type, *value, *traceback;
    int err;
    PyErr_Fetch(&type, &value, &traceback);
    err = cimpl_call_trace(func, obj, frame, TRACE_INFO_ARG what, arg);
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

#if LE_RESUMES_COROUTINES
/* ceval's 'return' events of a frame that yields 'value', when tracing
   started while it ran compiled, before its await (an awaited body,
   started by the await, set it): a body that yields compiled is otherwise
   not traced. -1 with an exception, as ceval's yield then raises. */
DEFINEFN int
psyco_trace_yield(PyFrameObject *frame, PyObject *value)
{
    PyThreadState *tstate = PyThreadState_GET();
#if LE_HAS_CEVAL_TRACE_INFO
    PyTraceInfo trace_info_here = {.code = NULL, .cframe = *tstate->cframe};
    PyTraceInfo *trace_info = &trace_info_here;
#endif
    if (!TSTATE_USE_TRACING(tstate))
        return 0;
    if (tstate->c_tracefunc != NULL &&
        cimpl_call_trace_protected(tstate->c_tracefunc, tstate->c_traceobj,
                                   frame, TRACE_INFO_ARG PyTrace_RETURN,
                                   value) < 0)
        return -1;
    if (tstate->c_profilefunc != NULL &&
        cimpl_call_trace_protected(tstate->c_profilefunc, tstate->c_profileobj,
                                   frame, TRACE_INFO_ARG PyTrace_RETURN,
                                   value) < 0)
        return -1;
    return 0;
}
#endif

#define C_TRACE(x, call)                                                      \
    if (CALL_USE_TRACING && tstate->c_profilefunc) {                          \
        if (cimpl_call_trace(tstate->c_profilefunc, tstate->c_profileobj,     \
                             tstate->frame, TRACE_INFO_ARG PyTrace_C_CALL,    \
                             func)) {                                         \
            x = NULL;                                                         \
        }                                                                     \
        else {                                                                \
            x = call;                                                         \
            if (tstate->c_profilefunc != NULL) {                              \
                if (x == NULL) {                                              \
                    cimpl_call_trace_protected(                               \
                        tstate->c_profilefunc, tstate->c_profileobj,          \
                        tstate->frame, TRACE_INFO_ARG PyTrace_C_EXCEPTION,    \
                        func);                                                \
                }                                                             \
                else {                                                        \
                    if (cimpl_call_trace(tstate->c_profilefunc,               \
                                         tstate->c_profileobj, tstate->frame, \
                                         TRACE_INFO_ARG PyTrace_C_RETURN,     \
                                         func)) {                             \
                        Py_DECREF(x);                                         \
                        x = NULL;                                             \
                    }                                                         \
                }                                                             \
            }                                                                 \
        }                                                                     \
    }                                                                         \
    else {                                                                    \
        x = call;                                                             \
    }

static PyObject *
cimpl_do_call_core(TRACE_INFO_PARAM PyObject *func, PyObject *callargs,
                   PyObject *kwdict)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *result;

#if LE_HAS_FASTCALL_DICT_TSTATE
    if (PyCFunction_CheckExact(func) || PyCMethod_CheckExact(func)) {
        C_TRACE(result, PyObject_Call(func, callargs, kwdict));
        return result;
    }
#else
    if (PyCFunction_Check(func)) {
        C_TRACE(result, PyCFunction_Call(func, callargs, kwdict));
        return result;
    }
#endif
#if LE_HAS_CALL_TRACE_METHOD_DESCR
    else if (Py_TYPE(func) == &PyMethodDescr_Type) {
        Py_ssize_t nargs = PyTuple_GET_SIZE(callargs);
        if (nargs > 0 && CALL_USE_TRACING) {
            /* We need to create a temporary bound method as argument
               for profiling.

               If nargs == 0, then this cannot work because we have no
               "self". In any case, the call itself would raise
               TypeError (foo needs an argument), so we just skip
               profiling. */
            PyObject *self = PyTuple_GET_ITEM(callargs, 0);
            func = Py_TYPE(func)->tp_descr_get(func, self,
                                               (PyObject *)Py_TYPE(self));
            if (func == NULL) {
                return NULL;
            }

            C_TRACE(result,
                    le_fastcall_dict_tstate(
                        tstate, func, &(_PyTuple_CAST(callargs)->ob_item)[1],
                        nargs - 1, kwdict));
            Py_DECREF(func);
            return result;
        }
    }
#endif
    return PyObject_Call(func, callargs, kwdict);
}
#else
/* ceval's do_call_core where it keeps its trace info in the thread state,
   or sys.monitoring traces calls, without the profiler's c_call events
   around a C function: compiled code makes no such call on these CPythons
   yet (mergepoints.c refuses every code object) */
static PyObject *
cimpl_do_call_core(PyObject *func, PyObject *callargs, PyObject *kwdict)
{
    return PyObject_Call(func, callargs, kwdict);
}
#endif

/* IMPORT_FROM of name from v, at place 'lasti' of code co */
static PyObject *
cimpl_import_from(PyObject *v, PyObject *name, PyCodeObject *co, long lasti)
{
    PyObject *x;
    PyObject *fullmodname, *pkgname, *pkgpath, *pkgname_or_unknown, *errmsg;

#if LE_HAS_LAZY_IMPORT_REIFY_LINKABLE
    if (PyLazyImport_CheckExact(v)) {
        /* another proxy, for the attribute, which keeps where it was made:
           the code and place are all it reads of the frame */
        le_frame_t f;
        f.f_executable = PyStackRef_FromPyObjectBorrow((PyObject *)co);
        LE_FRAME_PLACE(&f) = _PyCode_CODE(co) + lasti;
        return _PyEval_LazyImportFrom(PyThreadState_GET(), &f, v, name);
    }
#else
    (void)co;
    (void)lasti;
#endif

    if (le_lookup_attr(v, name, &x) != 0) {
        return x;
    }
    /* Issue #17636: in case this failed because of a circular relative
       import, try to fallback on reading the module directly from
       sys.modules. */
    pkgname = PyObject_GetAttr(v, s_name);
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
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, NULL);
    }
    else {
#if LE_HAS_IMPORT_FROM_SPEC
        PyObject *spec = PyObject_GetAttr(v, s_spec);
        const char *fmt = le_module_spec_is_initializing(spec)
                              ? LE_MSG_IMPORT_NAME_CIRCULAR
                              : LE_MSG_IMPORT_NAME_FROM_FILE;
        Py_XDECREF(spec);
        errmsg = PyUnicode_FromFormat(fmt, name, pkgname_or_unknown, pkgpath);
#else
        errmsg = PyUnicode_FromFormat(LE_MSG_IMPORT_NAME_FROM_FILE, name,
                                      pkgname_or_unknown, pkgpath);
#endif
        /* NULL checks for errmsg and pkgname done by PyErr_SetImportError. */
        PyErr_SetImportError(errmsg, pkgname, pkgpath);
    }

    Py_XDECREF(errmsg);
    Py_XDECREF(pkgname_or_unknown);
    Py_XDECREF(pkgpath);
    return NULL;
}

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
        match_args = PyObject_GetAttr(type, s_match_args);
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
    PyObject *get = PyObject_GetAttr(map, s_get);
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

/* ceval.c PRINT_EXPR */
static int
cimpl_print_expr(PyObject *value)
{
    PyObject *hook = PySys_GetObject("displayhook");
    PyObject *res;
    if (hook == NULL) {
        PyErr_SetString(PyExc_RuntimeError, LE_MSG_NO_DISPLAYHOOK);
        return -1;
    }
#if LE_HAS_CALL_ONE_ARG
    res = PyObject_CallOneArg(hook, value);
#else
    res = PyObject_CallFunctionObjArgs(hook, value, NULL);
#endif
    if (res == NULL)
        return -1;
    Py_DECREF(res);
    return 0;
}

#if LE_HAS_OPCODE_LOAD_SUPER_ATTR
#if LE_HAS_SUPER_LOOKUP_LINKABLE
/* declared only in an internal header */
PyAPI_FUNC(PyObject *) _PySuper_Lookup(PyTypeObject *su_type, PyObject *su_obj,
                                       PyObject *name, int *meth_found);
#endif
/* ceval.c LOAD_SUPER_ATTR: out[0] the attribute 'name' of super(class,
   self), or of the global super called with no arguments if oparg's bit 1
   is clear. For the builtin super and a class, where libpython exports
   it, CPython's specialised forms' lookup, which makes no super object,
   and for a method load (oparg's bit 0) no bound method either: then 1
   says out[0] is the function, to call with self. */
static int
cimpl_load_super_attr(PyObject *global_super, PyObject *class, PyObject *self,
                      PyObject *name, long oparg, PyObject *out[])
{
    PyObject *stack[] = {class, self};
    PyObject *super;
    long n = oparg & 2;
#if LE_HAS_SUPER_LOOKUP_LINKABLE
    if (global_super == (PyObject *)&PySuper_Type && PyType_Check(class)) {
        int found = 0;
        bool method = (oparg & 1) &&
                      Py_TYPE(self)->tp_getattro == PyObject_GenericGetAttr;
        out[0] = _PySuper_Lookup((PyTypeObject *)class, self, name,
                                 method ? &found : NULL);
        return out[0] == NULL ? -1 : found;
    }
#endif
    /* the builtin super is given both, as the zero-argument form finds
       them */
    if (global_super == (PyObject *)&PySuper_Type)
        n = 2;
    super = PyObject_Vectorcall(global_super, stack, n, NULL);
    if (super == NULL)
        return -1;
    out[0] = PyObject_GetAttr(super, name);
    Py_DECREF(super);
    return out[0] == NULL ? -1 : 0;
}
#endif

/* super(class, self).name resolved while compiling
   (PsycoObject_SuperMethod), for the site whose next instruction is at
   'site': 1 with *res the function, to call with self, for a method load,
   else the bound method its __get__ makes; 0 to load as CPython does; -1 */
static int
super_attr_known(PsycoObject *po, vinfo_t *vclass, vinfo_t *self,
                 PyObject *name, bool method, int site, vinfo_t **res)
{
    PyObject *f;
    vinfo_t *v;
    int found = PsycoObject_SuperMethod(
        po, vclass, self, name, (void *)(le_code(po->pr.co).bytes + site), &f);
    if (found <= 0)
        return found;
    Py_INCREF(f);
    v = vinfo_new(CompileTime_NewSk(sk_new((long)f, SkFlagPyObj)));
    if (method) {
        *res = v;
        return 1;
    }
    *res = PsycoMethod_New(v, self);
    vinfo_decref(v, po);
    return 1;
}

/* the object in 'cell', a new reference: 1, or 0 on the path where the
   cell is empty; -1 */
static int
cell_value(PsycoObject *po, vinfo_t *cell, vinfo_t **value)
{
    condition_code_t cc;
    *value = psyco_get_field(po, cell, CELL_ob_ref);
    if (*value == NULL)
        return -1;
    cc = integer_non_null(po, *value);
    if (cc != CC_ERROR && !runtime_condition_f(po, INVERT_CC(cc))) {
        need_reference(po, *value);
        return 1;
    }
    vinfo_decref(*value, po);
    return cc == CC_ERROR ? -1 : 0;
}

static bool
compute_pending_super(PsycoObject *po, vinfo_t *v)
{
    vinfo_array_shrink(po, v, iOB_TYPE + 1);
    vinfo_move(po, v, vinfo_new(CompileTime_New((long)&PySuper_Type)));
    return true;
}

/* the pending super for a call of the builtin super in *callable_at, in
   code with a __class__ cell and a first argument: 1 with it there, 0 on
   the paths where super_init would raise; -1 */
static int
pending_super_new(PsycoObject *po, vinfo_t **callable_at)
{
    PyCodeObject *co = po->pr.co;
    int fv = psyco_class_cell(co), i, r;
    vinfo_t *v, *vclass, *obj = NULL;
    if (!psyco_knowntobe(*callable_at, (long)&PySuper_Type) || fv < 0 ||
        co->co_argcount == 0)
        return 0;
    /* the first argument, or the cell it went into */
    for (i = 0; i < le_code_ncells(co); i++)
        if (le_code_cell_arg(co, i) == 0) {
            r = cell_value(po, LOC_LOCALS_PLUS[le_code_cell_slot(co, i)],
                           &obj);
            if (r <= 0)
                return r;
        }
    if (obj == NULL) {
        obj = LOC_LOCALS_PLUS[0];
        if (obj != NULL && psyco_frame_word_index(obj) >= 0 &&
            !compute_vinfo(obj, po))
            return -1;
        if (obj == NULL || is_maybe_null(obj->source) ||
            psyco_knowntobe(obj, 0))
            return 0;
        vinfo_incref(obj);
    }
    r = cell_value(po, LOC_LOCALS_PLUS[le_code_free_slot(co, fv)], &vclass);
    if (r <= 0) {
        vinfo_decref(obj, po);
        return r;
    }
    v = vinfo_new(VirtualTime_New(&pending_super));
    v->array = array_new(iSUPER_SELF + 1);
    v->array->items[iOB_TYPE] = vinfo_new(CompileTime_New((long)&PyType_Type));
    v->array->items[iSUPER_CLASS] = vclass;
    v->array->items[iSUPER_SELF] = obj;
    vinfo_decref(*callable_at, po);
    *callable_at = v;
    return 1;
}

/* super().name where CPython's bytecode calls super with no arguments,
   the callable in *callable_at, and then loads the attribute (LOAD_ATTR)
   or method (LOAD_METHOD) at next_instr: super_attr_known with the
   pending super's class and first argument. 1 with *res, *self the self
   to push after it for a method load (else NULL) and *after the
   instruction after the load; 0 to call super as CPython does; -1. */
static int
super_call_known(PsycoObject *po, vinfo_t **callable_at, int next_instr,
                 vinfo_t **res, vinfo_t **self, int *after)
{
    le_code_t c = le_code(po->pr.co);
    le_instr_t in;
    vinfo_t *v;
    int r;
    if (next_instr >= c.size)
        return 0;
    in = le_instr(c, next_instr);
    if (in.op != LOAD_ATTR && in.op != LOAD_METHOD)
        return 0;
    v = *callable_at;
    if (!is_virtualtime(v->source) ||
        VirtualTime_Get(v->source) != &pending_super) {
        r = pending_super_new(po, callable_at);
        if (r <= 0)
            return r;
        v = *callable_at;
    }
    r = super_attr_known(po, v->array->items[iSUPER_CLASS],
                         v->array->items[iSUPER_SELF],
                         PyTuple_GET_ITEM(po->pr.co->co_names, in.arg),
                         in.op == LOAD_METHOD, in.next, res);
    if (r == 0 && !compute_vinfo(v, po))
        return -1;
    *self = NULL;
    if (r > 0 && in.op == LOAD_METHOD) {
        *self = v->array->items[iSUPER_SELF];
        vinfo_incref(*self);
    }
    *after = in.next;
    return r;
}

/* the end of ceval.c GET_AITER: iter must have __anext__ */
static int
cimpl_check_aiter(PyObject *iter)
{
    if ((Py_TYPE(iter)->tp_as_async == NULL) ||
        (Py_TYPE(iter)->tp_as_async->am_anext == NULL)) {
        PyErr_Format(PyExc_TypeError, LE_MSG_ASYNC_FOR_AITER_NO_ANEXT,
                     Py_TYPE(iter)->tp_name);
        return -1;
    }
    return 0;
}

/* ceval.c GET_AITER */
static PyObject *
cimpl_get_aiter(PyObject *obj)
{
    unaryfunc getter = NULL;
    PyObject *iter;
    PyTypeObject *type = Py_TYPE(obj);

    if (type->tp_as_async != NULL) {
        getter = type->tp_as_async->am_aiter;
    }
    if (getter == NULL) {
        PyErr_Format(PyExc_TypeError, LE_MSG_ASYNC_FOR_NO_AITER,
                     type->tp_name);
        return NULL;
    }
    iter = (*getter)(obj);
    if (iter == NULL)
        return NULL;
    if (cimpl_check_aiter(iter) < 0) {
        Py_DECREF(iter);
        return NULL;
    }
    return iter;
}

/* ceval.c LOAD_BUILD_CLASS */
static PyObject *
cimpl_load_build_class(PyObject *builtins)
{
    PyObject *bc = PyObject_GetItem(builtins, s_build_class);
    if (bc == NULL && PyErr_ExceptionMatches(PyExc_KeyError))
        PyErr_SetString(PyExc_NameError, LE_MSG_NO_BUILD_CLASS);
    return bc;
}

static void
cimpl_format_exc_check_arg(PyObject *exc, const char *format_str,
                           PyObject *obj)
{
    const char *obj_str;
    if (!obj)
        return;
    obj_str = PyUnicode_AsUTF8(obj);
    if (!obj_str)
        return;
    PyErr_Format(exc, format_str, obj_str);
#if LE_HAS_NAME_ERROR_NAME
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
#endif
}

/* raised when it runs, by CPython's code, which sets a NameError's name */
static void
ct_format_exc_check_arg(PsycoObject *po, PyObject *exc, const char *format_str,
                        PyObject *obj)
{
    psyco_generic_call(po, cimpl_format_exc_check_arg, CfPyErrAlways, "lll",
                       (long)exc, (long)format_str, (long)obj);
}

/* whether local i is bound, raising UnboundLocalError if not. A local
   that may be unbound (RunTime_MaybeNull, or a word its frame still holds,
   which is taken first) is tested: on the path where it is NULL it becomes
   the unbound constant, and on the other it is known bound from then
   on. */
static bool
local_bound(PsycoObject *po, int i)
{
    PyCodeObject *co = po->pr.co;
    vinfo_t *v = GETLOCAL(i);
    if (psyco_frame_word_index(v) >= 0 && !compute_vinfo(v, po))
        return false;
    if (v != NULL && is_maybe_null(v->source)) {
        condition_code_t cc = integer_non_null(po, v);
        if (cc == CC_ERROR)
            return false;
        if (!runtime_condition_f(po, INVERT_CC(cc))) {
            v->source &= ~RunTime_MaybeNull;
            return true;
        }
        /* NULL owns nothing */
        v->source = remove_rtref(v->source);
        SETLOCAL(i, psyco_vi_Zero());
        v = GETLOCAL(i);
    }
    if (psyco_knowntobe(v, 0)) {
        ct_format_exc_check_arg(po, PyExc_UnboundLocalError,
                                LE_MSG_LOCAL_UNBOUND,
                                le_code_local_name(co, i));
        return false;
    }
    return true;
}

/* ceval.c DELETE_GLOBAL */
static int
cimpl_delete_global(PyObject *name, PyObject *globals)
{
    int err = PyDict_DelItem(globals, name);
    if (err != 0) {
#if LE_HAS_DELETE_GLOBAL_KEYERROR_ONLY
        if (PyErr_ExceptionMatches(PyExc_KeyError))
#endif
            cimpl_format_exc_check_arg(PyExc_NameError,
                                       LE_MSG_NAME_NOT_DEFINED, name);
    }
    return err;
}

static void
ct_format_exc_unbound(PsycoObject *po, PyCodeObject *co, int oparg)
{
    PyObject *name;
    if (PycException_Occurred(po))
        return;
    name = le_code_deref_name(co, oparg);
    if (le_code_deref_slot(co, oparg) < le_code_free_slot(co, 0))
        ct_format_exc_check_arg(po, PyExc_UnboundLocalError,
                                LE_MSG_LOCAL_UNBOUND, name);
    else
        ct_format_exc_check_arg(po, PyExc_NameError, LE_MSG_FREE_UNBOUND,
                                name);
}

/* ceval.c BUILD_STRING */
static PyObject *
cimpl_build_string_array(PyObject **items, long count)
{
    PyObject *empty = PyUnicode_New(0, 0), *str;
    if (empty == NULL)
        return NULL;
    str = _PyUnicode_JoinArray(empty, items, count);
    Py_DECREF(empty);
    return str;
}

#if LE_HAS_OPCODE_LIST_EXTEND
/* ceval.c LIST_EXTEND */
static PyObject *
cimpl_list_extend(PyObject *list, PyObject *iterable)
{
    PyObject *none_val = _PyList_Extend((PyListObject *)list, iterable);
    if (none_val == NULL) {
        if (PyErr_ExceptionMatches(PyExc_TypeError) &&
            (Py_TYPE(iterable)->tp_iter == NULL &&
             !PySequence_Check(iterable))) {
            PyErr_Clear();
            PyErr_Format(PyExc_TypeError, LE_MSG_LIST_STAR_NOT_ITERABLE,
                         Py_TYPE(iterable)->tp_name);
        }
    }
    return none_val;
}
#endif

#if LE_HAS_UNPACK_KEEPS_ATTRIBUTE_ERROR
/* the AttributeError {**o} or f(**o) raised says o is not a mapping only
   where o has no keys: then it is cleared and 1; else 0, with it, or the
   error looking for keys raised with it as its context */
static int
le_not_a_mapping(PyObject *o)
{
    PyObject *exc = PyErr_GetRaisedException();
    int has_keys = PyObject_HasAttrWithError(o, s_keys);
    if (has_keys == 0) {
        Py_DECREF(exc);
        return 1;
    }
    _PyErr_ChainExceptions1(exc);
    return 0;
}
#else
#define le_not_a_mapping(o) 1
#endif

#if LE_HAS_OPCODE_DICT_UPDATE
/* ceval.c DICT_UPDATE */
static int
cimpl_dict_update(PyObject *dict, PyObject *update)
{
    int err = PyDict_Update(dict, update);
    if (err < 0) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError) &&
            le_not_a_mapping(update)) {
#ifdef LE_MSG_DICT_STAR_NOT_MAPPING_QUALIFIED
            PyErr_Format(PyExc_TypeError,
                         LE_MSG_DICT_STAR_NOT_MAPPING_QUALIFIED,
                         Py_TYPE(update));
#else
            PyErr_Format(PyExc_TypeError, LE_MSG_DICT_STAR_NOT_MAPPING,
                         Py_TYPE(update)->tp_name);
#endif
        }
    }
    return err;
}
#endif

/* ceval.c:format_kwargs_error, and what came before it inline */
static void
cimpl_format_kwargs_error(PyObject *func, PyObject *kwargs)
{
    if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        if (!le_not_a_mapping(kwargs))
            return;
#ifdef LE_MSG_KWARGS_NOT_MAPPING_NO_FUNCTION
        PyErr_Clear();
        PyErr_Format(PyExc_TypeError, LE_MSG_KWARGS_NOT_MAPPING_NO_FUNCTION,
                     Py_TYPE(kwargs));
#elif LE_HAS_FUNCTION_STR
        PyErr_Clear();
        PyObject *funcstr = _PyObject_FunctionStr(func);
        if (funcstr != NULL) {
            PyErr_Format(PyExc_TypeError,
                         LE_MSG_KWARGS_NOT_MAPPING_FUNCTION_STR, funcstr,
                         Py_TYPE(kwargs)->tp_name);
            Py_DECREF(funcstr);
        }
#else
        PyErr_Format(PyExc_TypeError, LE_MSG_KWARGS_NOT_MAPPING_FUNC_NAME,
                     PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                     kwargs->ob_type->tp_name);
#endif
    }
    else if (PyErr_ExceptionMatches(PyExc_KeyError)) {
#if LE_HAS_CURRENT_EXCEPTION
        /* the KeyError is an instance: the key is in its args */
        PyObject *exc = PyErr_GetRaisedException();
        PyObject *args = ((PyBaseExceptionObject *)exc)->args;
        if (PyTuple_Check(args) && PyTuple_GET_SIZE(args) == 1) {
#else
        PyObject *exc, *val, *tb;
        PyErr_Fetch(&exc, &val, &tb);
        PyObject *args = val;
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
#endif
#ifdef LE_MSG_KWARGS_DUPLICATE_OR_FUNCTION
            /* a function whose name can't be made is shown as "function" */
            PyObject *funcstr = _PyObject_FunctionStr(func);
            PyErr_Format(PyExc_TypeError, LE_MSG_KWARGS_DUPLICATE_OR_FUNCTION,
                         funcstr, "function", PyTuple_GET_ITEM(args, 0));
            Py_XDECREF(funcstr);
#elif LE_HAS_FUNCTION_STR
            PyErr_Clear();
            PyObject *funcstr = _PyObject_FunctionStr(func);
            if (funcstr != NULL) {
                PyObject *key = PyTuple_GET_ITEM(args, 0);
                PyErr_Format(PyExc_TypeError,
                             LE_MSG_KWARGS_DUPLICATE_FUNCTION_STR, funcstr,
                             key);
                Py_DECREF(funcstr);
            }
#else
        PyObject *key = PyTuple_GET_ITEM(args, 0);
        if (!PyUnicode_Check(key)) {
            PyErr_Format(PyExc_TypeError, LE_MSG_KWARGS_NOT_STRINGS_FUNC_NAME,
                         PyEval_GetFuncName(func), PyEval_GetFuncDesc(func));
        }
        else {
            PyErr_Format(PyExc_TypeError, LE_MSG_KWARGS_DUPLICATE_FUNC_NAME,
                         PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                         key);
        }
#endif
#if LE_HAS_CURRENT_EXCEPTION
            Py_DECREF(exc);
        }
        else {
            PyErr_SetRaisedException(exc);
        }
#else
            Py_XDECREF(exc);
            Py_XDECREF(val);
            Py_XDECREF(tb);
        }
        else {
            PyErr_Restore(exc, val, tb);
        }
#endif
    }
}

#if LE_HAS_OPCODE_DICT_MERGE || LE_HAS_OPCODE_BUILD_MAP_UNPACK_WITH_CALL
/* ceval.c DICT_MERGE, and each mapping of BUILD_MAP_UNPACK_WITH_CALL */
static int
cimpl_dict_merge(PyObject *dict, PyObject *update, PyObject *func)
{
    int err = le_dict_merge_unique(dict, update);
    if (err < 0) {
        cimpl_format_kwargs_error(func, update);
    }
    return err;
}
#endif

#if LE_HAS_CMP_OUTCOME || LE_HAS_OPCODE_JUMP_IF_NOT_EXC_MATCH || \
    LE_HAS_EXCEPTION_TABLE
static int
cimpl_check_except_type_valid(PyObject *right)
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

/* ceval.c:check_except_type_valid, at compile time for a known class or a
   tuple of known classes */
static bool
except_type_valid(PsycoObject *po, vinfo_t *right)
{
    PyTypeObject *tp = Psyco_KnownType(right);
    bool tuple = tp != NULL && (tp->tp_flags & Py_TPFLAGS_TUPLE_SUBCLASS) != 0;
    int i, n = tuple ? PsycoTuple_Load(right) : 1;
    if (n < 0 || n > 16)
        goto at_run_time;
    for (i = 0; i < n; i++) {
        vinfo_t *e = tuple ? PsycoTuple_GET_ITEM(right, i) : right;
        if (!is_compiletime(e->source))
            goto at_run_time;
        if (!PyExceptionClass_Check(
                (PyObject *)CompileTime_Get(e->source)->value)) {
            PycException_SetString(po, PyExc_TypeError,
                                   LE_MSG_CATCHING_NON_EXCEPTION);
            return false;
        }
    }
    return true;
at_run_time:
    return psyco_generic_call(po, cimpl_check_except_type_valid,
                              CfNoReturnValue | CfReturnTypeInt | CfPyErrIfNeg,
                              "v", right) != NULL;
}
#endif

#if LE_HAS_OPCODE_MATCH_MAPPING
/* ceval.c MATCH_MAPPING */
static PyObject *
cimpl_match_mapping(PyObject *subject)
{
    int match = Py_TYPE(subject)->tp_flags & Py_TPFLAGS_MAPPING;
    PyObject *res = match ? Py_True : Py_False;
    Py_INCREF(res);
    return res;
}
#endif

#if LE_HAS_OPCODE_MATCH_SEQUENCE
/* ceval.c MATCH_SEQUENCE */
static PyObject *
cimpl_match_sequence(PyObject *subject)
{
    int match = Py_TYPE(subject)->tp_flags & Py_TPFLAGS_SEQUENCE;
    PyObject *res = match ? Py_True : Py_False;
    Py_INCREF(res);
    return res;
}
#endif

#if LE_HAS_OPCODE_COPY_DICT_WITHOUT_KEYS
/* ceval.c COPY_DICT_WITHOUT_KEYS: rest, which takes the place of keys */
static PyObject *
cimpl_copy_dict_without_keys(PyObject *subject, PyObject *keys)
{
    PyObject *rest = PyDict_New();
    if (rest == NULL || PyDict_Update(rest, subject)) {
        Py_XDECREF(rest);
        return NULL;
    }
    for (Py_ssize_t i = 0; i < PyTuple_GET_SIZE(keys); i++) {
        if (PyDict_DelItem(rest, PyTuple_GET_ITEM(keys, i))) {
            Py_DECREF(rest);
            return NULL;
        }
    }
    return rest;
}
#endif

static int
cimpl_check_args_iterable(PyObject *func, PyObject *args)
{
#ifdef LE_MSG_STAR_NOT_ITERABLE_NO_FUNCTION
    if (Py_TYPE(args)->tp_iter == NULL && !PySequence_Check(args)) {
        PyErr_Format(PyExc_TypeError, LE_MSG_STAR_NOT_ITERABLE_NO_FUNCTION,
                     Py_TYPE(args)->tp_name);
        return -1;
    }
#elif LE_HAS_FUNCTION_STR
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
/* each iterable of ceval.c BUILD_TUPLE_UNPACK_WITH_CALL */
static PyObject *
cimpl_build_tuple_unpack_with_call(PyObject *sum, PyObject *iterable,
                                   PyObject *func)
{
    PyObject *none_val = _PyList_Extend((PyListObject *)sum, iterable);
    if (none_val == NULL) {
        if (PyErr_ExceptionMatches(PyExc_TypeError)) {
            cimpl_check_args_iterable(func, iterable);
        }
    }
    return none_val;
}
#endif

#if LE_HAS_OPCODE_BUILD_MAP_UNPACK
/* each mapping of ceval.c BUILD_MAP_UNPACK */
static int
cimpl_build_map_unpack(PyObject *sum, PyObject *arg)
{
    int err = PyDict_Update(sum, arg);
    if (err < 0) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
            PyErr_Format(PyExc_TypeError, LE_MSG_DICT_STAR_NOT_MAPPING,
                         arg->ob_type->tp_name);
        }
    }
    return err;
}
#endif

/* ceval.c CALL_FUNCTION_EX, for kwargs that are not a dict or callargs that
   are not a tuple; kwargs may be NULL */
static PyObject *
cimpl_call_function_ex(PyObject *func, PyObject *callargs, PyObject *kwargs)
{
#if LE_HAS_CEVAL_TRACE_INFO
    PyThreadState *tstate = PyThreadState_GET();
    PyTraceInfo trace_info_here = {.code = NULL, .cframe = *tstate->cframe};
    PyTraceInfo *trace_info = &trace_info_here;
#endif
    PyObject *result;

    Py_INCREF(callargs);
    Py_XINCREF(kwargs);
    if (kwargs != NULL && !PyDict_CheckExact(kwargs)) {
        PyObject *d = PyDict_New();
        if (d == NULL)
            goto error;
#if LE_HAS_KWARGS_MERGE_EX
        if (le_dict_merge_unique(d, kwargs) < 0) {
            Py_DECREF(d);
            cimpl_format_kwargs_error(func, kwargs);
            goto error;
        }
#else
        if (PyDict_Update(d, kwargs) != 0) {
            Py_DECREF(d);
            if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
                cimpl_format_kwargs_error(func, kwargs);
            }
            goto error;
        }
#endif
        Py_SETREF(kwargs, d);
    }
    if (!PyTuple_CheckExact(callargs)) {
        if (cimpl_check_args_iterable(func, callargs) < 0) {
            goto error;
        }
        Py_SETREF(callargs, PySequence_Tuple(callargs));
        if (callargs == NULL) {
            goto error;
        }
    }
    result = cimpl_do_call_core(TRACE_INFO_ARG func, callargs, kwargs);
    Py_DECREF(callargs);
    Py_XDECREF(kwargs);
    return result;
error:
    Py_XDECREF(callargs);
    Py_XDECREF(kwargs);
    return NULL;
}

#if LE_HAS_CMP_OUTCOME
static vinfo_t *
cmp_outcome(PsycoObject *po, int op, vinfo_t *v, vinfo_t *w)
{
    condition_code_t cc;
    vinfo_t *res;
    switch (op) {
        case PyCmp_IS:
        case PyCmp_IS_NOT:
            cc = integer_cmp(po, v, w, op == PyCmp_IS ? Py_EQ : Py_NE);
            if (cc == CC_ERROR) {
                return NULL;
            }
            return PsycoBool_FromCondition(po, cc);
        case PyCmp_IN:
        case PyCmp_NOT_IN:
            res = PsycoSequence_Contains(po, w, v);
            if (res == NULL) {
                return NULL;
            }
            break;
        case PyCmp_EXC_MATCH:
            if (!except_type_valid(po, w)) {
                return NULL;
            }
            res = psyco_generic_call(po, PyErr_GivenExceptionMatches,
                                     CfReturnTypeInt | CfNoPython | CfNoBlock,
                                     "vv", v, w);
            break;
        default:
            return PsycoObject_RichCompare(po, v, w, op);
    }
    cc = integer_non_null(po, res);
    if (cc == CC_ERROR) {
        vinfo_decref(res, po);
        return NULL;
    }
    if (op == PyCmp_NOT_IN)
        cc = INVERT_CC(cc);
    v = PsycoBool_FromCondition(po, cc);
    vinfo_decref(res, po);
    return v;
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
    if (PsycoTuple_Load(keys) == 0) {
        return PsycoTuple_New(0, NULL);
    }
    return psyco_generic_call(po, cimpl_match_keys, CfCommonNewRefPyObject,
                              "vv", map, keys);
}
#endif

/* a global's NameError: ceval's NAME_ERROR_MSG through its own function */
static void
cimpl_name_error(PyObject *w)
{
    cimpl_format_exc_check_arg(PyExc_NameError, LE_MSG_NAME_NOT_DEFINED, w);
}

/* CALL_FUNCTION_EX: f(*args) or f(*args, **kwargs). The stack, top down:
   [kwargs], args, and the callable beside its NULL. The kwargs slot is
   there whenever argument bit 0 is set, or always where CALL_EX_KWARGS_SLOT
   (a NULL there for a call with none). f(*tuple), and f(*tuple, **k)
   while k is empty, go straight to PsycoObject_Call, which can specialise
   the call; anything else runs CPython's whole case at run time (its copy
   cimpl_call_function_ex). */
static vinfo_t *
psyco_ext_do_call_ex(PsycoObject *po, int oparg, vinfo_t **stack_top,
                     int *stack_to_pop)
{
    int slot = LE_HAS_CALL_EX_KWARGS_SLOT || (oparg & 1);
    vinfo_t *kwargs = slot ? stack_top[-1] : NULL;
    vinfo_t *callargs = stack_top[-1 - slot];
    vinfo_t *func = stack_top[-2 - slot - LE_HAS_CALLABLE_UNDER_NULL];
    vinfo_t *zero = NULL, *result, **kwvalues;
    PyTypeObject *t;
    /* and the NULL under the callable, where PUSH_NULL puts one there */
    *stack_to_pop = 2 + slot + LE_HAS_OPCODE_PUSH_NULL;
    if (kwargs != NULL && psyco_knowntobe(kwargs, 0))
        kwargs = NULL;

    t = Psyco_NeedType(po, callargs);
    if (t == NULL)
        return NULL;
    if (kwargs == NULL)
        kwargs = zero = psyco_vi_Zero();
    if (t == &PyTuple_Type && zero == NULL &&
        PsycoDict_PendingNames(kwargs, &kwvalues) != NULL) {
        /* f(*args, **k) with k empty (DICT_MERGE): a function's or a
           method's vectorcall takes an empty dict as none */
        PyTypeObject *ft = Psyco_NeedType(po, func);
        if (ft == NULL)
            return NULL;
        if (ft == &PyFunction_Type || ft == &PyMethod_Type)
            kwargs = zero = psyco_vi_Zero();
        result = PsycoObject_Call(po, func, callargs, kwargs);
        goto done;
    }
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
   __exit__, out[1] __enter__'s result, the exit method looked up first
   where 'exit_first' says. Unlike ceval, mgr stays on the stack (one
   reference more) while __enter__ runs. */
static int
with_enter(PyObject *mgr, _Py_Identifier *enter_id, _Py_Identifier *exit_id,
           bool exit_first, PyObject *out[])
{
    PyObject *enter = NULL, *exit = NULL, *res;
    if (exit_first && (exit = special_lookup(mgr, exit_id)) == NULL)
        return -1;
    enter = special_lookup(mgr, enter_id);
    if (enter == NULL) {
        Py_XDECREF(exit);
        return -1;
    }
    if (!exit_first && (exit = special_lookup(mgr, exit_id)) == NULL) {
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

static int
cimpl_setup_with(PyObject *mgr, PyObject *out[])
{
    _Py_IDENTIFIER(__exit__);
    _Py_IDENTIFIER(__enter__);
    return with_enter(mgr, &PyId___enter__, &PyId___exit__, false, out);
}

/* ceval.c BEFORE_ASYNC_WITH, as cimpl_setup_with: out[1] is what
   __aenter__ returns, to be awaited */
static int
cimpl_before_async_with(PyObject *mgr, PyObject *out[])
{
    _Py_IDENTIFIER(__aexit__);
    _Py_IDENTIFIER(__aenter__);
    return with_enter(mgr, &PyId___aenter__, &PyId___aexit__,
                      LE_HAS_AWITH_EXIT_FIRST, out);
}

#if LE_HAS_EXCEPTION_TABLE && LE_HAS_OPCODE_BEFORE_WITH
/* ceval.c BEFORE_WITH and BEFORE_ASYNC_WITH: out[0] gets the bound exit
   method, out[1] what the enter method returns ('no_enter' and 'no_exit'
   the messages for a missing one). Unlike ceval, mgr stays on the stack
   (one reference more) while the enter method runs. */
static int
before_with(PyObject *mgr, _Py_Identifier *enter_id, _Py_Identifier *exit_id,
            const char *no_enter, const char *no_exit, PyObject *out[])
{
    PyObject *enter = le_lookup_special_id(mgr, enter_id), *exit, *res;
    if (enter == NULL) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_TypeError, no_enter, Py_TYPE(mgr)->tp_name);
        return -1;
    }
    exit = le_lookup_special_id(mgr, exit_id);
    if (exit == NULL) {
        if (!PyErr_Occurred())
            PyErr_Format(PyExc_TypeError, no_exit, Py_TYPE(mgr)->tp_name);
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

static int
cimpl_before_with(PyObject *mgr, PyObject *out[])
{
    _Py_IDENTIFIER(__exit__);
    _Py_IDENTIFIER(__enter__);
    return before_with(mgr, &PyId___enter__, &PyId___exit__,
                       LE_MSG_WITH_NO_ENTER, LE_MSG_WITH_NO_EXIT, out);
}

static int
cimpl_before_awith(PyObject *mgr, PyObject *out[])
{
    _Py_IDENTIFIER(__aexit__);
    _Py_IDENTIFIER(__aenter__);
    return before_with(mgr, &PyId___aenter__, &PyId___aexit__,
                       LE_MSG_AWITH_NO_AENTER, LE_MSG_AWITH_NO_AEXIT, out);
}
#else
#define cimpl_before_awith cimpl_before_async_with
#endif

/* WITH_CLEANUP_START's call */
static PyObject *
cimpl_call_exit(PyObject *exit_func, PyObject *exc, PyObject *val,
                PyObject *tb)
{
    return PyObject_CallFunctionObjArgs(exit_func, exc, val, tb, NULL);
}

/* x, a new reference, as the global w found in 'namespace': where x is a
   lazy import's proxy, the module it imports, which also replaces it there
   (ceval.c:_PyEval_LoadGlobalStackRef). Takes the reference. */
static PyObject *
loaded_global(PyObject *x, PyObject *w, PyObject *namespace)
{
#if LE_HAS_LAZY_IMPORT_REIFY_LINKABLE
    if (x != NULL && PyLazyImport_CheckExact(x)) {
        PyObject *module =
            _PyLazyImport_Reify(PyThreadState_GET(), x, w, namespace);
        Py_DECREF(x);
        return module;
    }
#else
    (void)w;
    (void)namespace;
#endif
    return x;
}

static PyObject *
cimpl_load_global(PyObject *globals, PyObject *builtins, PyObject *w)
{
    PyObject *x, *namespace = globals;
    le_recursion_suspend();
    if (!PyDict_CheckExact(globals) || !PyDict_CheckExact(builtins)) {
        /* as ceval does: their __getitem__ and __missing__ run */
        x = PyObject_GetItem(globals, w);
        if (x == NULL && PyErr_ExceptionMatches(PyExc_KeyError)) {
            PyErr_Clear();
            namespace = builtins;
            x = PyObject_GetItem(builtins, w);
            if (x == NULL && PyErr_ExceptionMatches(PyExc_KeyError))
                cimpl_name_error(w);
        }
        x = loaded_global(x, w, namespace);
        le_recursion_resume();
        return x;
    }
    x = PyDict_GetItem(globals, w);
    if (x == NULL) {
        namespace = builtins;
        x = PyDict_GetItem(builtins, w);
        if (x == NULL) {
            cimpl_name_error(w);
            le_recursion_resume();
            return NULL;
        }
    }
    Py_INCREF(x);
    x = loaded_global(x, w, namespace);
    le_recursion_resume();
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
#ifdef LE_MSG_UNPACK_TOO_MANY_SIZED
        if (PyList_CheckExact(v) || PyTuple_CheckExact(v) ||
            PyDict_CheckExact(v)) {
            ll = PyDict_CheckExact(v) ? PyDict_Size(v) : Py_SIZE(v);
            if (ll > argcnt) {
                PyErr_Format(PyExc_ValueError, LE_MSG_UNPACK_TOO_MANY_SIZED,
                             argcnt, ll);
                goto Error;
            }
        }
#endif
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
            if (LE_HAS_RAISE_CAUSE_CHECKED &&
                !PyExceptionInstance_Check(fixed_cause)) {
                PyErr_Format(PyExc_TypeError, LE_MSG_RAISE_NOT_INSTANCE, cause,
                             Py_TYPE(fixed_cause));
                Py_DECREF(fixed_cause);
                goto raise_error;
            }
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
    x = PyObject_CallObject(x, w);
    Py_DECREF(w);
    return x;
}

/***************************************************************/
/***                         Main loop                         ***/
/***************************************************************/

/* PyErr_Restore of a virtual exception, raised as the interpreter would
   have raised it: with the exception being handled as its context (as
   PyErr_SetObject chains it). The traceback it has so far goes with it.
   Takes the references, as PyErr_Restore. */
static void
cimpl_raise_virtual(PyObject *type, PyObject *value, PyObject *tb)
{
    PyObject *t, *v, *none;
    PyErr_SetObject(type, value);
    Py_DECREF(type);
    Py_XDECREF(value);
    if (tb != NULL) {
        PyErr_Fetch(&t, &v, &none);
        Py_XDECREF(none);
        PyErr_Restore(t, v, tb);
    }
}

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
        return (psyco_generic_call(po, cimpl_raise_virtual, CfNoReturnValue,
                                   args, po->pr.exc, po->pr.val,
                                   po->pr.tb) != NULL);
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
cimpl_generator_stop(long coroutine)
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
    PyErr_SetString(PyExc_RuntimeError, coroutine
                                            ? LE_MSG_CORO_STOP_ITERATION
                                            : LE_MSG_GENERATOR_STOP_ITERATION);
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

/* what CPython's _PyAsyncGenValueWrapperNew makes (eval_hook.c:
   psyco_async_gen_wrapper): a wrapper made the way that makes one past
   its free list, which its dealloc takes back */
static PyObject *
cimpl_async_gen_wrap(PyObject *v)
{
    PyObject *o = _PyObject_GC_New(psyco_async_gen_wrapper);
    if (o == NULL)
        return NULL;
    Py_INCREF(v);
    *(PyObject **)(o + 1) = v;
    PyObject_GC_Track(o);
    return o;
}

#ifdef INTRINSIC_BUILD_FROZENSET
/* frozenset() of a set display or comprehension: the new set becomes the
   frozenset, through CPython's own intrinsic, which _PySet_Freeze (not
   exported) does */
static PyObject *
cimpl_build_frozenset(PyObject *set)
{
    return _PyIntrinsics_UnaryFunctions[INTRINSIC_BUILD_FROZENSET].func(
        PyThreadState_GET(), set);
}
#endif

#if LE_HAS_OPCODE_CALL_INTRINSIC_1
/* intrinsics.c:stopiteration_error, for a fused generator's code, the
   only generator code compiled (mergepoints.c:_build_merge_points):
   StopIteration leaving it becomes RuntimeError */
static PyObject *
cimpl_stopiteration_error(PyObject *exc, const char *msg,
                          const char *async_msg)
{
    PyObject *error;
    if (async_msg != NULL &&
        PyErr_GivenExceptionMatches(exc, PyExc_StopAsyncIteration))
        msg = async_msg;
    else if (!PyErr_GivenExceptionMatches(exc, PyExc_StopIteration)) {
        Py_INCREF(exc);
        return exc;
    }
    error = PyObject_CallFunction(PyExc_RuntimeError, "s", msg);
    if (error == NULL)
        return NULL;
    Py_INCREF(exc);
    PyException_SetCause(error, exc);
    Py_INCREF(exc);
    PyException_SetContext(error, exc);
    return error;
}
#endif

/* the return instruction a frame taken over at a loop head (GENEXPR_OSR)
   is left at when its compiled code returns from 'pos': that one, or,
   where the return ran a finally block or a with's exit first and ends at
   its END_FINALLY, the code's last RETURN_VALUE, which ceval runs alike
   with no blocks left */
static int
osr_return_place(PyCodeObject *co, int pos)
{
    le_code_t c = le_code(co);
    le_instr_t in = le_instr(c, pos);
    int at, last = pos;
    if (in.op == RETURN_VALUE || in.op == RETURN_CONST)
        return pos;
    for (at = 0; at < c.size; at = in.next) {
        in = le_instr(c, at);
        if (in.op == RETURN_VALUE)
            last = at;
    }
    extra_assert(le_instr(c, last).op == RETURN_VALUE);
    return last;
}

/* the value compiled code returns where the frame went on in ceval
   (resume_in_ceval) */
static bool
osr_went_on(vinfo_t *retval)
{
    return is_compiletime(retval->source) &&
           CompileTime_Get(retval->source)->value ==
               (long)le_osr_ceval_goes_on;
}

#if LE_RESUMES_ON_FRAME
/* A frame taken over at a loop head (GENEXPR_OSR) that returns is left
   for ceval to finish as its return instruction would: its locals in it,
   its place at that instruction and the value on its stack where the
   instruction takes it from there (RETURN_VALUE, not RETURN_CONST), a
   reference of its own; the compiled code then returns the value. */
static bool
osr_return(PsycoObject *po)
{
    PyCodeObject *co = po->pr.co;
    vinfo_t *retval = po->pr.val;
    int at = osr_return_place(co, po->pr.next_instr);
    int push = le_instr(le_code(co), at).op == RETURN_VALUE;
    if (osr_went_on(retval))
        return true;
    if (push && !compute_vinfo(retval, po))
        return false;
    if (!gen_locals_end(po, le_lasti_of(at), push))
        return false;
    return !push || frame_word_store(po, le_code_nlocalsplus(co), retval);
}
#else
/* As above, where the run holds the frame's locals (the GENEXPR_SLOT_ACC
   word is the run, psyco_gen_pause gives its frame): each is stored back,
   made real first. */
static bool
osr_return(PsycoObject *po)
{
    PyCodeObject *co = po->pr.co;
    int nlocals = le_code_nlocals(co), nplus = le_code_nlocalsplus(co), i;
    vinfo_t *retval = po->pr.val, *frame, *v;
    int at = osr_return_place(co, po->pr.next_instr);
    int push = le_instr(le_code(co), at).op == RETURN_VALUE;
    le_gen_pause_t *pause;
    if (osr_went_on(retval))
        return true;
    for (i = 0; i < nlocals + push; i++) {
        v = i < nlocals ? LOC_LOCALS_PLUS[i] : retval;
        if (v != NULL && !psyco_knowntobe(v, 0) && !compute_vinfo(v, po))
            return false;
    }
    pause = psyco_owner_alloc(sizeof(le_gen_pause_t));
    pause->lasti = le_lasti_of(at);
    pause->nstack = push;
    pause->iblock = 0;
    frame = psyco_generic_call(
        po, psyco_gen_pause, CfReturnTypePtr | CfNoPython, "vl",
        po->vlocals.items[GENEXPR_SLOT_ACC(co)], (long)pause);
    if (frame == NULL)
        return false;
    for (i = 0; i < nlocals + push; i++) {
        v = i < nlocals ? LOC_LOCALS_PLUS[i] : retval;
        if (v != NULL && !psyco_knowntobe(v, 0)) {
            if (!psyco_put_nth_field(po, frame, FRAME_localsplus,
                                     i < nlocals ? i : nplus, v)) {
                vinfo_decref(frame, po);
                return false;
            }
            consume_reference(po, v);
        }
        if (i < nlocals && v != NULL) {
            vinfo_decref(v, po);
            LOC_LOCALS_PLUS[i] = NULL;
        }
    }
    vinfo_decref(frame, po);
    return true;
}
#endif

static code_t *
exit_function(PsycoObject *po)
{
    vinfo_t **locals_plus;
    vinfo_t **pp;
    long i;
    Source retsource;
    long mode = psyco_generator_mode(po); /* before the locals go */
    extra_assert(!po->pr.is_inlining);

    /* the frame that stood for the call keeps its locals as they end */
    if (!catcher_flush(po, true))
        return NULL;
#if LE_RESUMES_ON_FRAME
    if (mode == GENEXPR_RESUMABLE && !gen_locals_end(po, -1, 0))
        return NULL;
#endif
    if (mode == GENEXPR_OSR && PycException_Is(po, &EReturn) &&
        !osr_return(po))
        return NULL;
    /* clear the locals, first to last as frame_dealloc does (finalisers run
       in that order), then the stack; where FRAME_CLEARED_LAST_FIRST, the
       stack top down, then the locals last to first. On a return CPython
       has popped the frame first. */
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
    for (i = 0; locals_plus + i < po->vlocals.items + po->vlocals.count; i++) {
        pp = LE_HAS_FRAME_CLEARED_LAST_FIRST
                 ? po->vlocals.items + po->vlocals.count - 1 - i
                 : locals_plus + i;
        if (*pp != NULL) {
            vinfo_decref(*pp, po);
            *pp = NULL;
        }
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
        /* PEP 479, as gen_send_ex: StopIteration leaving a fused generator
           or a direct-started coroutine becomes RuntimeError. A resumable
           one's gen_send_ex does it, once the frame has gone, so the
           traceback has no more entries. */
        if (mode != 0 && mode != GENEXPR_RESUMABLE && mode != GENEXPR_OSR &&
            !psyco_generic_call(po, cimpl_generator_stop, CfNoReturnValue, "l",
                                (long)(mode == GENEXPR_DIRECT)))
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
   calls, and a direct call before it (psyfunc.c:psyco_call_pyfunc). The test
   is part of the CALL_*, not of the next instruction: a merge point there is
   matched before the next instruction is compiled, and code compiled after a
   call that needed no test would then serve one that did. None when the next
   instruction tests anyway (a loop head), or is one ceval skips, after which
   mergepoints.c marks a test that is always made. False with an exception set.
 */
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

/* the LeJIT instruction at *p, and *p moved past it; op -1 at the end */
static le_instr_t
fusion_next(le_code_t c, int *p)
{
    le_instr_t in = {-1, 0, -1, -1, c.size};
    if (*p < c.size) {
        in = le_instr(c, *p);
        *p = in.next;
    }
    return in;
}

/* the LeJIT instruction before *p, and *p moved to its start; op -1 at
   the start */
static le_instr_t
fusion_prev(le_code_t c, int *p)
{
    le_instr_t in = {-1, 0, -1, -1, c.size};
    if (*p > 0) {
        int after = *p;
        *p = le_prev(c, after);
        in = le_instr(c, *p);
        extra_assert(in.next == after);
    }
    return in;
}

/* A subscript by a slice whose bounds are all constants is a constant
   slice object where the compiler folds them; for a container whose
   slicing compiled code does itself, the subscript becomes a virtual slice
   of those constants, as BINARY_SLICE and STORE_SLICE make it, which is the
   constant again wherever it is forced. 'sub' is borrowed; returns a new
   reference. */
static vinfo_t *
unfolded_slice(vinfo_t *container, vinfo_t *sub)
{
    PyTypeObject *tp = Psyco_KnownType(container);
    if (!is_compiletime(sub->source) ||
        Py_TYPE((PyObject *)CompileTime_Get(sub->source)->value) !=
            &PySlice_Type ||
        (tp != &PyList_Type && tp != &PyTuple_Type && tp != &PyUnicode_Type &&
         tp != &PyBytes_Type)) {
        vinfo_incref(sub);
        return sub;
    }
    return PsycoSlice_FromConstant(
        (PyObject *)CompileTime_Get(sub->source)->value);
}

/* The STORE_SUBSCR fused with the get at byte offset 'i', or -1, as LeJIT
   instructions (bytecode.h). The forms, d and k local variables, y a
   constant or a local:
     d[k] = d.get(k[, x]) OP y   LOAD_FAST d, LOAD_METHOD get, LOAD_FAST k,
                                 [LOAD_CONST/LOAD_FAST x,] [PRECALL,]
                                 CALL_METHOD or CALL (i), LOAD y, OP,
                                 LOAD_FAST d, LOAD_FAST k, STORE_SUBSCR
     d[k] = d[k] OP y            LOAD_FAST d, LOAD_FAST k, BINARY_SUBSCR (i),
                                 LOAD y, OP, LOAD_FAST d, LOAD_FAST k,
                                 STORE_SUBSCR
     d[k] OP= y                  DUP_TOP_TWO (or COPY 2, COPY 2),
                                 BINARY_SUBSCR (i), LOAD y, OP, ROT_THREE (or
                                 SWAP 3, SWAP 2), STORE_SUBSCR
   with no merge point from the instruction after the first to the store,
   so the store is reached only through the get. Only loads stand between
   the two besides OP, so d and k are the get's objects. A LOAD_FAST d,
   LOAD_FAST k pair may be one LOAD_FAST_LOAD_FAST, and y a LOAD_SMALL_INT.
   '*dlocal' is set to d's local for the get form. */
static int
fused_store(PsycoObject *po, int i, int *dlocal)
{
    PyCodeObject *co = po->pr.co;
    le_code_t c = le_code(co);
    le_instr_t get, load, op, in;
    int first, s, p = i;
    mergepoint_t *mp;
#define FUSION_LOAD(in)                               \
    ((in).op == LOAD_CONST || (in).op == LOAD_FAST || \
     (in).op == LOAD_SMALL_INT)
    get = fusion_next(c, &p);
    load = fusion_next(c, &p);
    op = fusion_next(c, &p);
    if (!FUSION_LOAD(load) || !fusion_operator(op.op))
        return -1;
    p = i;
    in = fusion_prev(c, &p);
    if (get.op == BINARY_SUBSCR &&
        (in.op == DUP_TOP_TWO || (in.op == COPY && in.arg == 2))) {
        if (in.op == COPY) {
            in = fusion_prev(c, &p);
            if (in.op != COPY || in.arg != 2)
                return -1;
        }
        p = op.next;
        in = fusion_next(c, &p);
        if (in.op == SWAP && in.arg == 3) {
            in = fusion_next(c, &p);
            if (in.op != SWAP || in.arg != 2)
                return -1;
        }
        else if (in.op != ROT_THREE)
            return -1;
        s = p;
        if (fusion_next(c, &p).op != STORE_SUBSCR)
            return -1;
        first = i;
    }
    else {
        le_instr_t kload, dload, method;
        if (get.op == BINARY_SUBSCR && in.op == LOAD_FAST_LOAD_FAST) {
            first = p;
            dload = kload = in;
            dload.op = kload.op = LOAD_FAST;
            dload.arg = in.arg >> 4;
            kload.arg = in.arg & 15;
            goto loaded;
        }
        if (get.op == BINARY_SUBSCR)
            kload = in;
        else if ((get.op == CALL_METHOD || get.op == CALL) &&
                 (get.arg == 1 || get.arg == 2)) {
            if (in.op == PRECALL)
                in = fusion_prev(c, &p);
            if (get.arg == 2) {
                if (!FUSION_LOAD(in))
                    return -1;
                in = fusion_prev(c, &p);
            }
            kload = in;
            method = fusion_prev(c, &p);
            if (method.op != LOAD_METHOD ||
                PyUnicode_CompareWithASCIIString(
                    PyTuple_GET_ITEM(co->co_names, method.arg), "get") != 0)
                return -1;
        }
        else
            return -1;
        first = p;
        dload = fusion_prev(c, &p);
    loaded:
        if (dload.op != LOAD_FAST || kload.op != LOAD_FAST)
            return -1;
        p = op.next;
        in = fusion_next(c, &p);
        if (in.op == LOAD_FAST_LOAD_FAST) {
            if (in.arg >> 4 != dload.arg || (in.arg & 15) != kload.arg)
                return -1;
        }
        else {
            if (in.op != LOAD_FAST || in.arg != dload.arg)
                return -1;
            in = fusion_next(c, &p);
            if (in.op != LOAD_FAST || in.arg != kload.arg)
                return -1;
        }
        s = p;
        if (fusion_next(c, &p).op != STORE_SUBSCR)
            return -1;
        if (dlocal != NULL)
            *dlocal = dload.arg;
    }
#undef FUSION_LOAD
    mp = psyco_next_merge_point(po->pr.merge_points, first);
    if (mp->bytecode_position >= 0 && mp->bytecode_position <= s)
        return -1;
    return s;
}

/* The get then store of one str key in one dict (pdictobject.c's probe):
   the get records the slot it finds and the store reuses it if the dict
   is unchanged (its keys and version tag, or where dicts have none, the
   key the entry holds), checked at run time, else
   probes again. Called for BINARY_SUBSCR, CALL_METHOD, CALL and
   STORE_SUBSCR, before their caches; 1 when done, 0 for the translated
   case, -1 on an error. A get of the forms above that is not this probe
   clears the record, so that the store never meets an older one. */
static int
dict_str_fusion(PsycoObject *po, int next_instr)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int i = po->pr.next_instr, p, j, dlocal = -1;
    le_code_t c = le_code(po->pr.co);
    le_instr_t in = le_instr(c, i);
    int op = in.op, argc = in.arg;
    vinfo_t *d, *k, *res;
    if (op == STORE_SUBSCR) {
        /* the get, four or five instructions back */
        for (p = i, j = 0; j < 4 && p > 0; j++) p = le_prev(c, p);
        if (j < 4 || (fused_store(po, p, NULL) != i &&
                      (p == 0 || fused_store(po, le_prev(c, p), NULL) != i)))
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
    if (fused_store(po, i, &dlocal) < 0)
        return 0;
    if (op == BINARY_SUBSCR) {
        d = NTOP(2);
        k = NTOP(1);
        if (Psyco_NeedType(po, d) == NULL)
            return -1;
    }
    else {
        d = GETLOCAL(dlocal);
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
   fused generator, nor while a finally block entered by a
   pseudo-exception holds its 3-tuple. 1 to go on, 0 with a
   pseudo-exception set. */
static int
resume_in_ceval(PsycoObject *po, int next_instr, int *unwound);

/* An item stored or deleted by a call may be a frame-locals proxy's,
   which writes the locals of a frame (frames.c:expose): not where the
   container's type is known and isn't the proxy's */
static inline char
may_write_locals_proxy(vinfo_t *container)
{
#ifdef PyFrameLocalsProxy_Check
    PyTypeObject *tp = Psyco_KnownType(container);
    return tp == NULL || PyType_IsSubtype(tp, &PyFrameLocalsProxy_Type);
#else
    (void)container;
    return 0;
#endif
}

static int
resume_if_tracing(PsycoObject *po, int next_instr, int *unwound)
{
    int on;
    if (!psyco_can_go_interpreted(po))
        return 1;
    on = psyco_tracing_on(po);
    if (on <= 0)
        return on == 0;
#ifdef PyFrameLocalsProxy_Check
    {
        /* the test above is also on while any frame's locals are
           exposed: this frame goes on interpreted if they are its own */
        condition_code_t cc;
        vinfo_t *v = psyco_generic_call(
            po, psyco_frame_goes_interpreted,
            CfReturnTypeInt | CfNoBlock | CfNoPython, "l", (long)po->pr.co);
        if (v == NULL)
            return 0;
        cc = integer_non_null(po, v);
        vinfo_decref(v, po);
        if (cc == CC_ERROR)
            return 0;
        if (!runtime_condition_f(po, cc))
            return 1;
    }
#endif
    return resume_in_ceval(po, next_instr, unwound);
}

DEFINEFN
long
psyco_generator_mode(PsycoObject *po)
{
    PyCodeObject *co = po->pr.co;
    /* a function's code has the slots only in a frame taken over at a
       loop head (GENEXPR_OSR): its stack starts after them */
    if (!(co->co_flags & LE_BODY_FLAGS) &&
        po->pr.stack_base == GENEXPR_SLOT_MODE(co))
        return 0;
    return CompileTime_Get(po->vlocals.items[GENEXPR_SLOT_MODE(co)]->source)
        ->value;
}

DEFINEFN
bool
psyco_can_go_interpreted(PsycoObject *po)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int i, nstack = STACK_LEVEL();
    long mode = psyco_generator_mode(po);
    if ((po->pr.co->co_flags & LE_BODY_FLAGS) && mode != GENEXPR_RESUMABLE &&
        mode != GENEXPR_DIRECT)
        return false;
    for (i = 0; i < nstack; i++)
        if (is_pseudo_exception_tuple(stack_a[i]))
            return false;
    return true;
}

/* What ceval needs to go on with the frame (frames.c:psyco_resume_begin):
   its locals, the first 'nstack' items of the stack and its blocks. NULL
   with an exception. */
static vinfo_t *
resume_state(PsycoObject *po, int nstack)
{
    PyCodeObject *co = po->pr.co;
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int nlocals = le_code_nlocalsplus(co);
    /* an inlined frame has no blocks of its own: those in 'pr' are its
       caller's, which goes on compiled once the call returns */
    int nblocks = po->pr.is_inlining ? 0 : po->pr.iblock;
    int i;
    vinfo_t *st, *word = catcher_word(po);

    st = psyco_generic_call(
        po, psyco_resume_begin, CfReturnTypePtr | CfPyErrIfNull,
        word != NULL ? "lvvlr" : "lvvll", co, LOC_GLOBALS, LOC_BUILTINS,
        (long)(nlocals + nstack), (long)word);
    if (st == NULL)
        return 0;
    for (i = 0; i < nlocals + nstack; i++) {
        vinfo_t *v = i < nlocals ? LOC_LOCALS_PLUS[i] : stack_a[i - nlocals];
        vinfo_t *ok;
        if (v == NULL)
            ok = psyco_generic_call(po, psyco_resume_value, CfNoReturnValue,
                                    "vll", st, (long)i, 0L);
        else {
            /* a word the frame still holds is given as it is */
            v = frame_word_peek(po, v);
            ok = v == NULL ? NULL
                           : psyco_generic_call(po, psyco_resume_value,
                                                CfNoReturnValue, "vlv", st,
                                                (long)i, v);
            vinfo_xdecref(v, po);
        }
        if (ok == NULL) {
            vinfo_decref(st, po);
            return NULL;
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
            return NULL;
        }
    }
#endif
    return st;
}

/* The rare path itself: ceval runs the frame from 'next_instr' and this
   frame ends with its result. 0, with EReturn or an exception set. */
static int
resume_in_ceval(PsycoObject *po, int next_instr, int *unwound)
{
    PyCodeObject *co = po->pr.co;
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int nstack = STACK_LEVEL();
    vinfo_t *st, *result;

    st = resume_state(po, nstack);
    if (st == NULL)
        return 0;
#if LE_HAS_OPCODE_CALL
    /* ceval keeps KW_NAMES's names outside the frame, for the CALL after
       it: a CALL that has them is run from its KW_NAMES */
    if (le_instr(le_code(co), next_instr).op == CALL) {
        int kw = call_kwnames_start(le_code(co), next_instr);
        if (kw >= 0)
            next_instr = kw;
    }
#endif
    /* a coroutine's body started by its await goes on as its coroutine */
    if (psyco_generator_mode(po) == GENEXPR_DIRECT)
        result = psyco_generic_call(
            po, psyco_resume_run_coro, CfCommonNewRefPyObject, "vllv", st,
            (long)(next_instr - sizeof(_Py_CODEUNIT)), (long)nstack,
            po->vlocals.items[GENEXPR_SLOT_ACC(co)]);
    else
        result = psyco_generic_call(
            po, psyco_resume_run, CfCommonNewRefPyObject, "vll", st,
            (long)(next_instr - sizeof(_Py_CODEUNIT)), (long)nstack);
    vinfo_decref(st, po);
    if (result == NULL && !PycException_Is(po, &ERtPython))
        return 0;
    while (STACK_LEVEL() > 0) POP_DECREF();
    /* the generator's frame is ceval's now: the end stores nothing in it
       (gen_locals_end); nor in a frame taken over at a loop head, which
       the ceval running it goes on with, given the marker that says so
       (osr_return) */
    if (!po->pr.is_inlining &&
        ((LE_RESUMES_ON_FRAME &&
          psyco_generator_mode(po) == GENEXPR_RESUMABLE) ||
         psyco_generator_mode(po) == GENEXPR_OSR)) {
        int i;
        for (i = 0; i < le_code_nlocals(co); i++) {
            vinfo_xdecref(LOC_LOCALS_PLUS[i], po);
            LOC_LOCALS_PLUS[i] = NULL;
        }
    }
    if (!po->pr.is_inlining && psyco_generator_mode(po) == GENEXPR_OSR &&
        result != NULL) {
        vinfo_decref(result, po);
        Py_INCREF(le_osr_ceval_goes_on);
        result = vinfo_new(CompileTime_NewSk(
            sk_new((long)le_osr_ceval_goes_on, SkFlagPyObj)));
    }
    if (!po->pr.is_inlining)
        po->pr.iblock = 0;
    *unwound = 1;
    if (result != NULL)
        PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), result);
    return 0;
}

#if LE_HAS_OPCODE_BUILD_TUPLE_UNPACK || \
    LE_HAS_OPCODE_BUILD_TUPLE_UNPACK_WITH_CALL
/* BUILD_TUPLE_UNPACK of the top n items where they are all tuples with
   known items: 1 when the tuple of their items has replaced them, 0 when
   they are not, -1 on error */
static int
unpack_known_tuples(PsycoObject *po, int n)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *tuple, **items;
    int i, j, k, count = 0, m = 0;
    for (i = n; i > 0; i--) {
        k = PsycoTuple_Load(NTOP(i));
        if (k < 0)
            return 0;
        count += k;
    }
    items = (vinfo_t **)PyMem_Malloc((count + 1) * sizeof(*items));
    if (items == NULL) {
        Py_INCREF(Py_None); /* MemoryError(), as PyErr_NoMemory */
        PycException_SetObject(po, PyExc_MemoryError, Py_None);
        return -1;
    }
    for (i = n; i > 0; i--) {
        k = PsycoTuple_Load(NTOP(i));
        for (j = 0; j < k; j++) items[m++] = PsycoTuple_GET_ITEM(NTOP(i), j);
    }
    tuple = PsycoTuple_New(count, items);
    PyMem_Free(items);
    POP_DECREF_N(n);
    PUSH(tuple);
    return 1;
}
#endif

/* what a family's cases leave the main loop to do */
enum {
    OPCODE_ELSEWHERE, /* not one of the family's opcodes */
    OPCODE_ENDED,     /* the case ended: done, or an exception is set */
    OPCODE_FINE,      /* the case succeeded */
    OPCODE_RERAISED,  /* an exception is raised again (ceval's
                         exception_unwind) */
};

/* locals, constants, cells and the value stack */
static int
compile_locals_and_stack(PsycoObject *po, int opcode, int oparg, int target,
                         int *next_instr_io, mergepoint_t **mp_io,
                         int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    PyCodeObject *co = po->pr.co;
    vinfo_t *v, *x;
    condition_code_t cc;
    int next_instr = *next_instr_io;
    int ended;
    /* some versions have no case that uses these */
    (void)x;

    switch (opcode) {
        case NOP: {
            goto fine;
        }

        case LOAD_FAST:
        case LOAD_FAST_CHECK: {
            vinfo_t *value;
            if (!local_bound(po, oparg))
                break;
            value = GETLOCAL(oparg);
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

#if LE_HAS_OPCODE_LOAD_SMALL_INT
        case LOAD_SMALL_INT: {
            /* CPython's small ints are immortal */
            PyObject *small = PyLong_FromLong(oparg);
            Py_DECREF(small);
            PUSH(vinfo_new(CompileTime_New((long)small)));
            goto fine;
        }

        case LOAD_COMMON_CONSTANT:
            PUSH(vinfo_new(CompileTime_New((long)le_common_constant(oparg))));
            goto fine;
#endif

        /* an inlined comprehension keeps its outer variable's
           value, or NULL, on the stack and restores it with
           STORE_FAST */
        case LOAD_FAST_AND_CLEAR: {
            vinfo_t *value;
            value = GETLOCAL(oparg);
            vinfo_incref(value);
            PUSH(value);
            SETLOCAL(oparg, psyco_vi_Zero());
            goto fine;
        }

        case STORE_FAST: {
            vinfo_t *value;
            value = NTOP(1);
            if (!catcher_local_readable(po, value))
                break;
            /* a pending generator a local holds is never fused (more
               than a temporary holds it), and the frame's locals maps
               can't give it: it is made as it becomes a local */
            if (PsycoGenexpr_Check(value) && !compute_vinfo(value, po))
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

        /* two locals in one instruction, the first in the high four
           bits */
        case LOAD_FAST_LOAD_FAST: {
            int first = oparg >> 4, second = oparg & 15;
            if (!local_bound(po, first) || !local_bound(po, second))
                break;
            vinfo_incref(GETLOCAL(first));
            PUSH(GETLOCAL(first));
            vinfo_incref(GETLOCAL(second));
            PUSH(GETLOCAL(second));
            goto fine;
        }

        case STORE_FAST_LOAD_FAST: {
            vinfo_t *value = NTOP(1);
            if (!catcher_local_readable(po, value))
                break;
            SETLOCAL(oparg >> 4, value);
            POP(value);
            if (!local_bound(po, oparg & 15))
                break;
            value = GETLOCAL(oparg & 15);
            vinfo_incref(value);
            PUSH(value);
            goto fine;
        }

        case STORE_FAST_STORE_FAST: {
            vinfo_t *top = NTOP(1), *second = NTOP(2);
            if (!catcher_local_readable(po, top) ||
                !catcher_local_readable(po, second))
                break;
            SETLOCAL(oparg >> 4, top);
            POP(top);
            SETLOCAL(oparg & 15, second);
            POP(second);
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

        /* the frame's start (LeCode_Run, or a direct caller, tested
           eval_breaker), or a generator's resumption, which isn't compiled */
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

        case DELETE_FAST: {
            if (!local_bound(po, oparg))
                break;
            SETLOCAL(oparg, psyco_vi_Zero());
            goto fine;
        }

        case DELETE_DEREF: {
            vinfo_t *cell;
            vinfo_t *oldobj;
            bool ok;
            cell = GETLOCAL(le_code_deref_slot(co, oparg));
            oldobj = psyco_get_field(po, cell, CELL_ob_ref);
            if (oldobj == NULL) {
                break;
            }
            cc = integer_non_null(po, oldobj);
            if (cc == CC_ERROR) {
                vinfo_decref(oldobj, po);
                break;
            }
            if (runtime_condition_t(po, cc)) {
                v = psyco_vi_Zero();
                ok = psyco_put_field(po, cell, CELL_ob_ref, v);
                vinfo_decref(v, po);
                if (!ok) {
                    vinfo_decref(oldobj, po);
                    break;
                }
                psyco_decref_v(po, oldobj);
                vinfo_decref(oldobj, po);
                goto fine;
            }
            vinfo_decref(oldobj, po);
            ct_format_exc_unbound(po, co, oparg);
            break;
        }

        case LOAD_CLOSURE: {
            vinfo_t *cell;
            cell = GETLOCAL(le_code_deref_slot(co, oparg));
            /* read here, not where the closure is made */
            if (!compute_vinfo(cell, po))
                break;
            vinfo_incref(cell);
            PUSH(cell);
            goto fine;
        }

        case LOAD_DEREF: {
            vinfo_t *cell;
            vinfo_t *value;
            cell = GETLOCAL(le_code_deref_slot(co, oparg));
            value = psyco_get_field(po, cell, CELL_ob_ref);
            if (value == NULL) {
                break;
            }
            cc = integer_non_null(po, value);
            if (cc == CC_ERROR) {
                vinfo_decref(value, po);
                break;
            }
            if (runtime_condition_f(po, INVERT_CC(cc))) {
                vinfo_decref(value, po);
                ct_format_exc_unbound(po, co, oparg);
                break;
            }
            need_reference(po, value);
            PUSH(value);
            goto fine;
        }

        case STORE_DEREF: {
            vinfo_t *cell;
            vinfo_t *oldobj;
            vinfo_t *v;
            v = NTOP(1);
            cell = GETLOCAL(le_code_deref_slot(co, oparg));
            oldobj = psyco_get_field(po, cell, CELL_ob_ref);
            if (oldobj == NULL) {
                break;
            }
            if (!psyco_put_field(po, cell, CELL_ob_ref, v)) {
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

#if LE_HAS_OPCODE_ROT_N
        case ROT_N:
            x = TOP();
            CHKSTACK(-oparg);
            memmove(STACK_POINTER() - (oparg - 1), STACK_POINTER() - oparg,
                    sizeof(vinfo_t *) * (oparg - 1));
            STACK_POINTER()[-oparg] = x;
            goto fine;
#endif

        /* after the start, an inlined comprehension's own cell, in
           the place LOAD_FAST_AND_CLEAR emptied */
        case MAKE_CELL:
#if LE_HAS_INTERPRETER_FRAME
            if (next_instr > co->_co_firsttraceable * LE_UNIT) {
                x = psyco_generic_call(po, PyCell_New,
                                       CfCommonNewRefPyObject | CfNoPython,
                                       "v", GETLOCAL(oparg));
                if (x == NULL)
                    break;
                SETLOCAL(oparg, x);
            }
#endif
            goto fine;

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    return ended;
}

/* bool(v), a new reference: v itself where it is known to be a bool, as
   ceval's TO_BOOL leaves it. Only a value in the frame can be promoted:
   the truth of a temporary of unknown type is a run-time call. */
static vinfo_t *
to_bool(PsycoObject *po, vinfo_t *v, bool in_frame)
{
    vinfo_t *x;
    condition_code_t cc;
    PyTypeObject *t = Psyco_KnownType(v);
    if (t == &PyBool_Type) {
        vinfo_incref(v);
        return v;
    }
    if (in_frame || t != NULL)
        x = PsycoObject_IsTrue(po, v);
    else
        x = psyco_generic_call(po, PyObject_IsTrue, CfCommonInquiry, "v", v);
    if (x == NULL)
        return NULL;
    cc = integer_non_null(po, x);
    vinfo_decref(x, po);
    if (cc == CC_ERROR)
        return NULL;
    return PsycoBool_FromCondition(po, cc);
}

/* unary, binary and in-place operators, subscripts and slices */
static int
compile_operators(PsycoObject *po, int opcode, int oparg, int target,
                  int *next_instr_io, mergepoint_t **mp_io,
                  int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *u, *x;
    condition_code_t cc;
    int next_instr = *next_instr_io;
    int ended;

    switch (opcode) {
        UNARY_OPCODE(UNARY_POSITIVE, PsycoNumber_Positive);

        UNARY_OPCODE(UNARY_NEGATIVE, PsycoNumber_Negative);

        case UNARY_NOT:
            x = PsycoObject_IsTrue(po, TOP());
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR)
                break;
            x = PsycoBool_FromCondition(po, INVERT_CC(cc));
            POP_DECREF();
            PUSH(x);
            goto fine;

        case TO_BOOL:
            x = to_bool(po, TOP(), true);
            if (x == NULL)
                break;
            POP_DECREF();
            PUSH(x);
            goto fine;

            UNARY_OPCODE(UNARY_INVERT, PsycoNumber_Invert);

            POWER_OPCODE(BINARY_POWER, PsycoNumber_Power);

            BINARY_OPCODE(BINARY_MULTIPLY, PsycoNumber_Multiply);

            BINARY_OPCODE(BINARY_MATRIX_MULTIPLY, PsycoNumber_MatrixMultiply);

            BINARY_OPCODE(BINARY_TRUE_DIVIDE, PsycoNumber_TrueDivide);

            BINARY_OPCODE(BINARY_FLOOR_DIVIDE, PsycoNumber_FloorDivide);

            BINARY_OPCODE(BINARY_MODULO, PsycoNumber_Remainder);

            BINARY_OPCODE(BINARY_ADD, PsycoNumber_Add);

            BINARY_OPCODE(BINARY_SUBTRACT, PsycoNumber_Subtract);

        case BINARY_SUBSCR: {
            vinfo_t *sub = unfolded_slice(NTOP(2), NTOP(1));
            x = PsycoObject_GetItem(po, NTOP(2), sub);
            vinfo_decref(sub, po);
            if (x == NULL)
                break;
            POP_DECREF();
            POP_DECREF();
            PUSH(x);
            goto fine;
        }

            BINARY_OPCODE(BINARY_LSHIFT, PsycoNumber_Lshift);

            BINARY_OPCODE(BINARY_RSHIFT, PsycoNumber_Rshift);

            BINARY_OPCODE(BINARY_AND, PsycoNumber_And);

            BINARY_OPCODE(BINARY_XOR, PsycoNumber_Xor);

            BINARY_OPCODE(BINARY_OR, PsycoNumber_Or);

            POWER_OPCODE(INPLACE_POWER, PsycoNumber_InPlacePower);

            BINARY_OPCODE(INPLACE_MULTIPLY, PsycoNumber_InPlaceMultiply);

            BINARY_OPCODE(INPLACE_MATRIX_MULTIPLY,
                          PsycoNumber_InPlaceMatrixMultiply);

            BINARY_OPCODE(INPLACE_TRUE_DIVIDE, PsycoNumber_InPlaceTrueDivide);

            BINARY_OPCODE(INPLACE_FLOOR_DIVIDE,
                          PsycoNumber_InPlaceFloorDivide);

            BINARY_OPCODE(INPLACE_MODULO, PsycoNumber_InPlaceRemainder);

            BINARY_OPCODE(INPLACE_ADD, PsycoNumber_InPlaceAdd);

            BINARY_OPCODE(INPLACE_SUBTRACT, PsycoNumber_InPlaceSubtract);

            BINARY_OPCODE(INPLACE_LSHIFT, PsycoNumber_InPlaceLshift);

            BINARY_OPCODE(INPLACE_RSHIFT, PsycoNumber_InPlaceRshift);

            BINARY_OPCODE(INPLACE_AND, PsycoNumber_InPlaceAnd);

            BINARY_OPCODE(INPLACE_XOR, PsycoNumber_InPlaceXor);

            BINARY_OPCODE(INPLACE_OR, PsycoNumber_InPlaceOr);

        case STORE_SUBSCR: {
            vinfo_t *container;
            bool err;
            vinfo_t *sub;
            vinfo_t *v;
            container = NTOP(2);
            v = NTOP(3);
            sub = unfolded_slice(container, NTOP(1));
            err = PsycoObject_SetItem(po, container, sub, v);
            po->pr.proxy_write = may_write_locals_proxy(container);
            vinfo_decref(sub, po);
            if (!err) {
                break;
            }
            POP_DECREF();
            POP_DECREF();
            POP_DECREF();
            goto fine;
        }

        case DELETE_SUBSCR: {
            vinfo_t *container;
            bool err;
            vinfo_t *sub;
            container = NTOP(2);
            sub = unfolded_slice(container, NTOP(1));
            err = PsycoObject_DelItem(po, container, sub);
            po->pr.proxy_write = may_write_locals_proxy(container);
            vinfo_decref(sub, po);
            if (!err) {
                break;
            }
            POP_DECREF();
            POP_DECREF();
            goto fine;
        }

        case BINARY_SLICE: {
            vinfo_t *slice = PsycoSlice_New(NTOP(2), NTOP(1), NULL);
            x = PsycoObject_GetItem(po, NTOP(3), slice);
            vinfo_decref(slice, po);
            if (x == NULL)
                break;
            POP_DECREF();
            POP_DECREF();
            POP_DECREF();
            PUSH(x);
            goto fine;
        }

        case STORE_SLICE: {
            vinfo_t *slice = PsycoSlice_New(NTOP(2), NTOP(1), NULL);
            bool ok = PsycoObject_SetItem(po, NTOP(3), slice, NTOP(4));
            vinfo_decref(slice, po);
            if (!ok)
                break;
            POP_DECREF();
            POP_DECREF();
            POP_DECREF();
            POP_DECREF();
            goto fine;
        }

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    return ended;
}

/* comparisons and pattern matching */
static int
compile_compares(PsycoObject *po, int opcode, int oparg, int target,
                 int *next_instr_io, mergepoint_t **mp_io, int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *v, *x;
    condition_code_t cc;
    int next_instr = *next_instr_io;
    int ended;
    /* some versions have no case that uses these */
    (void)v;
    (void)x;
    (void)cc;

    switch (opcode) {
#if LE_HAS_CMP_OUTCOME
        case COMPARE_OP:
            x = cmp_outcome(po, oparg, NTOP(2), NTOP(1));
            if (x == NULL)
                break;
            POP_DECREF();
            POP_DECREF();
            PUSH(x);
            goto fine;
#else
        case COMPARE_OP:
            x = PsycoObject_RichCompare(po, NTOP(2), NTOP(1),
                                        LE_COMPARE_OP(oparg));
            if (x == NULL)
                break;
            /* the bit that asks for bool() of the result */
            if (LE_HAS_COMPARE_OP_SHIFT_5 && (oparg & 16)) {
                v = to_bool(po, x, false);
                vinfo_decref(x, po);
                if (v == NULL)
                    break;
                x = v;
            }
            POP_DECREF();
            POP_DECREF();
            PUSH(x);
            goto fine;
#endif

#if LE_HAS_OPCODE_IS_OP
        case IS_OP:
            cc = integer_cmp(po, NTOP(2), NTOP(1), Py_EQ);
            if (cc == CC_ERROR)
                break;
            x = PsycoBool_FromCondition(po, oparg ? INVERT_CC(cc) : cc);
            POP_DECREF();
            POP_DECREF();
            PUSH(x);
            goto fine;
#endif

#if LE_HAS_OPCODE_CONTAINS_OP
        case CONTAINS_OP:
            x = PsycoSequence_Contains(po, NTOP(1), NTOP(2));
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            if (cc == CC_ERROR) {
                vinfo_decref(x, po);
                break;
            }
            v = PsycoBool_FromCondition(po, oparg ? INVERT_CC(cc) : cc);
            vinfo_decref(x, po);
            POP_DECREF();
            POP_DECREF();
            PUSH(v);
            goto fine;
#endif

#if LE_HAS_OPCODE_GET_LEN
        case GET_LEN:
            v = PsycoObject_Size(po, TOP());
            if (v == NULL)
                break;
            x = PsycoLong_FromSsize_t(po, v);
            vinfo_decref(v, po);
            if (x == NULL)
                break;
            PUSH(x);
            goto fine;
#endif

#if LE_HAS_OPCODE_MATCH_CLASS
        case MATCH_CLASS: {
            vinfo_t *attrs;
            if (Psyco_NeedType(po, NTOP(2)) == NULL) {
                break;
            }
            attrs = match_class(po, NTOP(3), NTOP(2), oparg, TOP());
            if (attrs == NULL) {
                break;
            }
#if !LE_HAS_MATCH_CLASS_PUSHES_BOOL
            /* the attributes in the subject's place, or None */
            if (psyco_known_NULL(attrs)) {
                vinfo_decref(attrs, po);
                attrs = psyco_vi_None();
            }
            POP_DECREF_N(3);
            PUSH(attrs);
#else
            if (!psyco_known_NULL(attrs)) {
                POP_DECREF_N(3);
                PUSH(attrs);
                PUSH(PsycoBool_FROM_WORD(psyco_vi_One()));
            }
            else {
                vinfo_decref(attrs, po);
                POP_DECREF_N(2);
                PUSH(PsycoBool_FROM_WORD(psyco_vi_Zero()));
            }
#endif
            goto fine;
        }
#endif

#if LE_HAS_OPCODE_MATCH_MAPPING
        case MATCH_MAPPING:
            x = psyco_generic_call(po, cimpl_match_mapping,
                                   CfCommonNewRefPyObject, "v", TOP());
            if (x == NULL)
                break;
            PUSH(x);
            goto fine;
#endif

#if LE_HAS_OPCODE_MATCH_SEQUENCE
        case MATCH_SEQUENCE:
            x = psyco_generic_call(po, cimpl_match_sequence,
                                   CfCommonNewRefPyObject, "v", TOP());
            if (x == NULL)
                break;
            PUSH(x);
            goto fine;
#endif

#if LE_HAS_OPCODE_MATCH_KEYS
        case MATCH_KEYS:
            x = match_keys(po, NTOP(2), TOP());
            if (x == NULL)
                break;
#if LE_HAS_MATCH_KEYS_PUSHES_BOOL
            cc = integer_cmp_i(po, x, (long)Py_None, Py_NE);
            if (cc == CC_ERROR) {
                vinfo_decref(x, po);
                break;
            }
            PUSH(x);
            PUSH(PsycoBool_FromCondition(po, cc));
#else
            PUSH(x);
#endif
            goto fine;
#endif

#if LE_HAS_OPCODE_COPY_DICT_WITHOUT_KEYS
        case COPY_DICT_WITHOUT_KEYS:
            x = psyco_generic_call(po, cimpl_copy_dict_without_keys,
                                   CfCommonNewRefPyObject, "vv", NTOP(2),
                                   TOP());
            if (x == NULL)
                break;
            Psyco_AssertType(po, x, &PyDict_Type);
            POP_DECREF();
            PUSH(x);
            goto fine;
#endif

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    return ended;
}

/* attributes, globals and builtins */
static int
compile_attributes_and_globals(PsycoObject *po, int opcode, int oparg,
                               int target, int *next_instr_io,
                               mergepoint_t **mp_io, int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    PyCodeObject *co = po->pr.co;
    vinfo_t *v, *x;
    int next_instr = *next_instr_io;
    int ended;

    switch (opcode) {
#if LE_HAS_OPCODE_LOAD_ASSERTION_ERROR
        case LOAD_ASSERTION_ERROR:
            PUSH(vinfo_new(CompileTime_New((long)PyExc_AssertionError)));
            goto fine;
#endif

        case LOAD_BUILD_CLASS:
            x = psyco_generic_call(po, cimpl_load_build_class,
                                   CfCommonNewRefPyObject, "v", LOC_BUILTINS);
            if (x == NULL)
                break;
            PUSH(x);
            goto fine;

        case STORE_ATTR: {
            bool err;
            vinfo_t *name;
            vinfo_t *owner;
            vinfo_t *v;
            owner = NTOP(1);
            v = NTOP(2);
            name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
            /* a NULL value (`del o.a` where DELETE_ATTR is gone) deletes */
            err = PsycoObject_SetAttr(po, owner, name,
                                      psyco_knowntobe(v, 0) ? NULL : v);
            vinfo_decref(name, po);
            if (!err) {
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
            if (!err) {
                break;
            }
            POP(owner);
            vinfo_decref(owner, po);
            goto fine;
        }

        case STORE_GLOBAL: {
            vinfo_t *err;
            vinfo_t *name;
            vinfo_t *v;
            v = NTOP(1);
            name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
            mark_varying(po, GETNAMEV(oparg));
            /* a NULL value (`del g` where DELETE_GLOBAL is gone) deletes */
            if (psyco_knowntobe(v, 0))
                err = psyco_generic_call(po, cimpl_delete_global,
                                         CfCommonIntZeroOk, "vv", name,
                                         LOC_GLOBALS);
            else
                err = psyco_generic_call(po, PyDict_SetItem, CfCommonIntZeroOk,
                                         "vvv", LOC_GLOBALS, name, v);
            vinfo_decref(name, po);
            if (err == NULL) {
                break;
            }
            POP(v);
            vinfo_decref(v, po);
            goto fine;
        }

        case DELETE_GLOBAL: {
            vinfo_t *err;
            vinfo_t *name;
            name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
            mark_varying(po, GETNAMEV(oparg));
            err =
                psyco_generic_call(po, cimpl_delete_global, CfCommonIntZeroOk,
                                   "vv", name, LOC_GLOBALS);
            vinfo_decref(name, po);
            if (err == NULL) {
                break;
            }
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

        /* ceval's two forms: the function and self, or the attribute
           and the NULL */
        case LOAD_METHOD: {
            vinfo_t *meth;
            vinfo_t *name;
            vinfo_t *obj;
            int unbound;
            obj = NTOP(1);
            name = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
            unbound = PsycoObject_GetMethod(po, obj, name, &meth);
            vinfo_decref(name, po);
            if (unbound < 0) {
                break;
            }
            POP(obj);
            if (unbound) {
                PUSH(meth);
                PUSH(obj);
            }
            else {
                vinfo_decref(obj, po);
                PUSH_WITH_NULL(meth);
            }
            goto fine;
        }

        /* super().name, or super(class, self).name: the attribute, or
           for a method call the function and self as LOAD_METHOD
           leaves them */
        case LOAD_SUPER_ATTR: {
#if LE_HAS_OPCODE_LOAD_SUPER_ATTR
            vinfo_array_t *out;
            vinfo_t *self;
            int found = 0;
            if (psyco_knowntobe(NTOP(3), (long)&PySuper_Type))
                found =
                    super_attr_known(po, NTOP(2), TOP(), GETNAMEV(oparg >> 2),
                                     oparg & 1, next_instr, &v);
            if (found < 0)
                break;
            if (found) {
                POP(self);
                POP_DECREF();
                POP_DECREF();
                PUSH(v);
                if (oparg & 1)
                    PUSH(self);
                else
                    vinfo_decref(self, po);
                goto fine;
            }
            out = array_new(1);
            x = psyco_generic_call(po, cimpl_load_super_attr,
                                   CfCommonIntBoolOrError, "vvvllA", NTOP(3),
                                   NTOP(2), TOP(), (long)GETNAMEV(oparg >> 2),
                                   (long)oparg, out);
            if (x == NULL) {
                array_release(out);
                break;
            }
            v = out->items[0];
            array_release(out);
            found = (oparg & 1) ? runtime_NON_NULL_t(po, x) : 0;
            if (!(oparg & 1))
                vinfo_decref(x, po);
            if (found < 0) {
                vinfo_decref(v, po);
                break;
            }
            POP(self);
            POP_DECREF();
            POP_DECREF();
            if (found) {
                PUSH(v);
                PUSH(self);
            }
            else {
                vinfo_decref(self, po);
                if (oparg & 1)
                    PUSH_WITH_NULL(v);
                else
                    PUSH(v);
            }
#endif
            goto fine;
        }

        case LOAD_GLOBAL: {
            bool push_null = LE_GLOBAL_PUSHES_NULL(oparg);
            PyObject *namev = GETNAMEV(LE_GLOBAL_NAME(oparg));
            if (!load_global_cached(po, namev, &v))
                break;
            /* not cached: globals unknown, the name varying, not found or
             * a lazy import */
            if (v == NULL) {
                v = psyco_generic_call(po, cimpl_load_global,
                                       CfCommonNewRefPyObject, "vvl",
                                       LOC_GLOBALS, LOC_BUILTINS, namev);
                if (v == NULL)
                    break;
            }
            if (push_null)
                PUSH_WITH_NULL(v);
            else
                PUSH(v);
            goto fine;
        }

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    return ended;
}

#if LE_HAS_OPCODE_DICT_MERGE
/* whether a BUILD_MAP 0 ending at next_instr makes the dict of a call
   f(*a, **k) with k a local: a load of k, DICT_MERGE, CALL_FUNCTION_EX */
static bool
forwards_local_dict(PyCodeObject *co, int next_instr)
{
    le_code_t c = le_code(co);
    le_instr_t in;
    if (next_instr >= c.size ||
        (in = le_instr(c, next_instr)).op != LOAD_FAST || in.next >= c.size ||
        (in = le_instr(c, in.next)).op != DICT_MERGE || in.arg != 1 ||
        in.next >= c.size)
        return false;
    return le_instr(c, in.next).op == CALL_FUNCTION_EX;
}
#endif

/* building and unpacking tuples, lists, sets, dicts, slices and strings */
static int
compile_build_and_unpack(PsycoObject *po, int opcode, int oparg, int target,
                         int *next_instr_io, mergepoint_t **mp_io,
                         int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *u, *v, *w, *x;
    condition_code_t cc;
    int next_instr = *next_instr_io;
    int ended;
    /* some versions have no case that uses these */
    (void)x;

    switch (opcode) {
        case LIST_APPEND: {
            bool err;
            vinfo_t *list;
            vinfo_t *v;
            v = NTOP(1);
            list = NTOP(oparg + 1);
            err = PsycoList_Append(po, list, v);
            if (!err) {
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
            err = psyco_generic_call(po, PySet_Add, CfCommonIntZeroOk, "vv",
                                     set, v);
            if (err == NULL) {
                break;
            }
            POP(v);
            vinfo_decref(v, po);
            goto fine;
        }

        case BUILD_STRING: {
            vinfo_array_t *items = array_new(oparg);
            vinfo_t *str;
            int i;
            for (i = 0; i < oparg; i++) items->items[i] = NTOP(oparg - i);
            str = psyco_generic_call(po, cimpl_build_string_array,
                                     CfCommonNewRefPyObject, "il", items,
                                     (long)oparg);
            array_release(items);
            if (str == NULL) {
                break;
            }
            POP_DECREF_N(oparg);
            Psyco_AssertType(po, str, &PyUnicode_Type);
            PUSH(str);
            goto fine;
        }

        case BUILD_TUPLE: {
            vinfo_t *tup = PsycoTuple_New(oparg, NULL);
            int i;
            for (i = 0; i < oparg; i++) {
                PsycoTuple_GET_ITEM(tup, i) = NTOP(oparg - i);
                vinfo_incref(PsycoTuple_GET_ITEM(tup, i));
            }
            POP_DECREF_N(oparg);
            PUSH(tup);
            goto fine;
        }

        case BUILD_LIST: {
            vinfo_t *list;
            list = PsycoList_New(po, oparg, STACK_POINTER() - (oparg));
            if (list == NULL) {
                break;
            }
            POP_DECREF_N(oparg);
            PUSH(list);
            goto fine;
        }

#if LE_HAS_OPCODE_BUILD_TUPLE_UNPACK_WITH_CALL
        case BUILD_TUPLE_UNPACK_WITH_CALL: {
            vinfo_t *none_val;
            vinfo_t *return_value;
            vinfo_t *sum;
            int i, known;
            known = unpack_known_tuples(po, oparg);
            if (known < 0)
                break;
            if (known > 0)
                goto fine;
            sum = psyco_generic_call(po, PyList_New, CfCommonNewRefPyObject,
                                     "l", 0L);
            if (sum == NULL) {
                break;
            }
            for (i = oparg; i > 0; i--) {
                none_val =
                    psyco_generic_call(po, cimpl_build_tuple_unpack_with_call,
                                       CfCommonNewRefPyObject, "vvv", sum,
                                       NTOP(i), NTOP(1 + oparg));
                if (none_val == NULL) {
                    break;
                }
                vinfo_decref(none_val, po);
            }
            if (i > 0) {
                vinfo_decref(sum, po);
                break;
            }
            return_value = psyco_generic_call(
                po, PyList_AsTuple,
                CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v", sum);
            vinfo_decref(sum, po);
            if (return_value == NULL) {
                break;
            }
            POP_DECREF_N(oparg);
            Psyco_AssertType(po, return_value, &PyTuple_Type);
            PUSH(return_value);
            goto fine;
        }
#endif

#if LE_HAS_OPCODE_BUILD_TUPLE_UNPACK
        case BUILD_TUPLE_UNPACK:
#if LE_HAS_OPCODE_BUILD_LIST_UNPACK
        case BUILD_LIST_UNPACK:
#endif
        {
            vinfo_t *none_val;
            vinfo_t *return_value;
            vinfo_t *sum;
            int i, known;
            if (opcode == BUILD_TUPLE_UNPACK) {
                known = unpack_known_tuples(po, oparg);
                if (known < 0)
                    break;
                if (known > 0)
                    goto fine;
            }
            sum = psyco_generic_call(po, PyList_New, CfCommonNewRefPyObject,
                                     "l", 0L);
            if (sum == NULL) {
                break;
            }
            for (i = oparg; i > 0; i--) {
                none_val = psyco_generic_call(po, _PyList_Extend,
                                              CfCommonNewRefPyObject, "vv",
                                              sum, NTOP(i));
                if (none_val == NULL) {
                    break;
                }
                vinfo_decref(none_val, po);
            }
            if (i > 0) {
                vinfo_decref(sum, po);
                break;
            }
            if (opcode == BUILD_TUPLE_UNPACK) {
                return_value = psyco_generic_call(
                    po, PyList_AsTuple,
                    CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v", sum);
                vinfo_decref(sum, po);
                if (return_value == NULL) {
                    break;
                }
                Psyco_AssertType(po, return_value, &PyTuple_Type);
            }
            else {
                return_value = sum;
                Psyco_AssertType(po, return_value, &PyList_Type);
            }
            POP_DECREF_N(oparg);
            PUSH(return_value);
            goto fine;
        }
#endif

        case LIST_TO_TUPLE: {
            vinfo_t *list;
            vinfo_t *tuple;
            list = NTOP(1);
            tuple = psyco_generic_call(
                po, PyList_AsTuple,
                CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v", list);
            if (tuple == NULL) {
                break;
            }
            POP(list);
            vinfo_decref(list, po);
            PUSH(tuple);
            goto fine;
        }

#if LE_HAS_OPCODE_LIST_EXTEND
        case LIST_EXTEND: {
            vinfo_t *iterable;
            vinfo_t *list;
            vinfo_t *none_val;
            iterable = NTOP(1);
            list = NTOP(oparg + 1);
            none_val = psyco_generic_call(po, cimpl_list_extend,
                                          CfCommonNewRefPyObject, "vv", list,
                                          iterable);
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
            err = psyco_generic_call(po, _PySet_Update, CfCommonIntBoolOrError,
                                     "vv", set, iterable);
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
            vinfo_t *err;
            vinfo_t *set;
            int i;
            set = psyco_generic_call(po, PySet_New, CfCommonNewRefPyObject,
                                     "l", 0L);
            if (set == NULL) {
                break;
            }
            for (i = oparg; i > 0; i--) {
                err = psyco_generic_call(po, PySet_Add, CfCommonIntZeroOk,
                                         "vv", set, NTOP(i));
                if (err == NULL) {
                    break;
                }
            }
            if (i > 0) {
                vinfo_decref(set, po);
                break;
            }
            POP_DECREF_N(oparg);
            Psyco_AssertType(po, set, &PySet_Type);
            PUSH(set);
            goto fine;
        }

#if LE_HAS_OPCODE_BUILD_SET_UNPACK
        case BUILD_SET_UNPACK: {
            vinfo_t *err;
            vinfo_t *sum;
            int i;
            sum = psyco_generic_call(po, PySet_New, CfCommonNewRefPyObject,
                                     "l", 0L);
            if (sum == NULL) {
                break;
            }
            for (i = oparg; i > 0; i--) {
                err = psyco_generic_call(po, _PySet_Update,
                                         CfCommonIntBoolOrError, "vv", sum,
                                         NTOP(i));
                if (err == NULL) {
                    break;
                }
                vinfo_decref(err, po);
            }
            if (i > 0) {
                vinfo_decref(sum, po);
                break;
            }
            POP_DECREF_N(oparg);
            Psyco_AssertType(po, sum, &PySet_Type);
            PUSH(sum);
            goto fine;
        }
#endif

        case BUILD_MAP: {
            vinfo_t *err;
            vinfo_t *map;
            int i;
#if LE_HAS_OPCODE_DICT_MERGE
            if (oparg == 0 && forwards_local_dict(po->pr.co, next_instr)) {
                PUSH(PsycoDict_PendingEmpty());
                goto fine;
            }
#endif
            map = _PsycoDict_NewPresized(po, (Py_ssize_t)oparg);
            if (map == NULL) {
                break;
            }
            for (i = oparg; i > 0; i--) {
                err = psyco_generic_call(po, PyDict_SetItem, CfCommonIntZeroOk,
                                         "vvv", map, NTOP(2 * i),
                                         NTOP(2 * i - 1));
                if (err == NULL) {
                    break;
                }
            }
            if (i > 0) {
                vinfo_decref(map, po);
                break;
            }
            POP_DECREF_N(2 * oparg);
            PUSH(map);
            goto fine;
        }

#if LE_HAS_OPCODE_BUILD_MAP_UNPACK
        case BUILD_MAP_UNPACK: {
            vinfo_t *err;
            vinfo_t *sum;
            int i;
            sum = PsycoDict_New(po);
            if (sum == NULL) {
                break;
            }
            for (i = oparg; i > 0; i--) {
                err = psyco_generic_call(po, cimpl_build_map_unpack,
                                         CfCommonIntBoolOrError, "vv", sum,
                                         NTOP(i));
                if (err == NULL) {
                    break;
                }
                vinfo_decref(err, po);
            }
            if (i > 0) {
                vinfo_decref(sum, po);
                break;
            }
            POP_DECREF_N(oparg);
            Psyco_AssertType(po, sum, &PyDict_Type);
            PUSH(sum);
            goto fine;
        }
#endif

#if LE_HAS_OPCODE_BUILD_MAP_UNPACK_WITH_CALL
        case BUILD_MAP_UNPACK_WITH_CALL: {
            vinfo_t *err;
            vinfo_t *sum;
            int i;
            sum = PsycoDict_New(po);
            if (sum == NULL) {
                break;
            }
            for (i = oparg; i > 0; i--) {
                err = psyco_generic_call(po, cimpl_dict_merge,
                                         CfCommonIntBoolOrError, "vvv", sum,
                                         NTOP(i), NTOP(2 + oparg));
                if (err == NULL) {
                    break;
                }
                vinfo_decref(err, po);
            }
            if (i > 0) {
                vinfo_decref(sum, po);
                break;
            }
            POP_DECREF_N(oparg);
            Psyco_AssertType(po, sum, &PyDict_Type);
            PUSH(sum);
            goto fine;
        }
#endif

#if LE_HAS_OPCODE_DICT_UPDATE
        case DICT_UPDATE:
            x = psyco_generic_call(po, cimpl_dict_update,
                                   CfCommonIntBoolOrError, "vv",
                                   NTOP(oparg + 1), TOP());
            if (x == NULL)
                break;
            vinfo_decref(x, po);
            POP_DECREF();
            goto fine;
#endif

#if LE_HAS_OPCODE_DICT_MERGE
        /* an error names the callable: under the arguments, and under
           its NULL too where CALLABLE_UNDER_NULL */
        case DICT_MERGE: {
            int empty = PsycoDict_MergeEmpty(po, NTOP(oparg + 1), TOP());
            if (empty < 0)
                break;
            if (empty == 0) {
                x = psyco_generic_call(
                    po, cimpl_dict_merge, CfCommonIntBoolOrError, "vvv",
                    NTOP(oparg + 1), TOP(),
                    NTOP(oparg + 3 + LE_HAS_CALLABLE_UNDER_NULL));
                if (x == NULL)
                    break;
                vinfo_decref(x, po);
            }
            POP_DECREF();
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
            err = psyco_generic_call(po, PyDict_SetItem, CfCommonIntZeroOk,
                                     "vvv", map, key, value);
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
            PyObject *(*conv_fn)(PyObject *) = NULL;
            int which_conversion = oparg & 0x3; /* FVC_MASK */
            int have_fmt_spec = (oparg & 0x4) != 0;
            vinfo_t *fmt_spec = have_fmt_spec ? TOP() : NULL;
            vinfo_t *value = NTOP(1 + have_fmt_spec);
            vinfo_t *result;
            switch (which_conversion) {
                case 0x0: /* FVC_NONE */
                    break;
                case 0x1: /* FVC_STR */
                    conv_fn = PyObject_Str;
                    break;
                case 0x2: /* FVC_REPR */
                    conv_fn = PyObject_Repr;
                    break;
                case 0x3: /* FVC_ASCII */
                    conv_fn = PyObject_ASCII;
                    break;
            }
            if (conv_fn != NULL) {
                value = psyco_generic_call(po, conv_fn, CfCommonNewRefPyObject,
                                           "v", value);
                if (value == NULL)
                    break;
            }
            else {
                vinfo_incref(value);
            }
            if (fmt_spec == NULL &&
                Psyco_KnownType(value) == &PyUnicode_Type) {
                result = value;
            }
            else {
                u = fmt_spec != NULL ? fmt_spec : psyco_vi_Zero();
                result =
                    psyco_generic_call(po, PyObject_Format,
                                       CfCommonNewRefPyObject, "vv", value, u);
                if (fmt_spec == NULL)
                    vinfo_decref(u, po);
                vinfo_decref(value, po);
                if (result == NULL)
                    break;
            }
            POP_DECREF_N(1 + have_fmt_spec);
            PUSH(result);
            goto fine;
        }

        /* the conversion FORMAT_VALUE's argument has, on its own */
        case CONVERT_VALUE: {
            PyObject *(*conv_fn)(PyObject *) = oparg == 1   ? PyObject_Str
                                               : oparg == 2 ? PyObject_Repr
                                                            : PyObject_ASCII;
            x = psyco_generic_call(po, conv_fn, CfCommonNewRefPyObject, "v",
                                   TOP());
            if (x == NULL)
                break;
            POP_DECREF();
            PUSH(x);
            goto fine;
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
                        w = psyco_get_nth_const(po, v, TUPLE_ob_item, i);
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
                if (!psyco_generic_call(po, cimpl_unpack, CfCommonIntZeroOk,
                                        "vlA", v, (long)oparg, array)) {
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
            if (!psyco_generic_call(po, cimpl_unpack_ex, CfCommonIntZeroOk,
                                    "vlA", TOP(), (long)oparg, array)) {
                array_release(array);
                break;
            }
            Psyco_AssertType(po, array->items[before], &PyList_Type);
            POP_DECREF();
            for (i = total; i--;) PUSH(array->items[i]);
            array_release(array);
            goto fine;
        }

        case BUILD_CONST_KEY_MAP: {
            /* oparg values, then a constant tuple of keys on top */
            int i;
            PyObject *keys;
            if (!is_compiletime(TOP()->source) ||
                !PyTuple_CheckExact(
                    keys =
                        (PyObject *)CompileTime_Get(TOP()->source)->value) ||
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
                        po, v, PyTuple_GET_ITEM(keys, oparg - i), NTOP(i + 1)))
                    break;
            if (i > 0) {
                vinfo_decref(v, po);
                break;
            }
            for (i = oparg + 1; i--;) POP_DECREF();
            PUSH(v);
            goto fine;
        }

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    return ended;
}

/* jumps */
static int
compile_jumps(PsycoObject *po, int opcode, int oparg, int target,
              int *next_instr_io, mergepoint_t **mp_io, int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *x;
    condition_code_t cc;
    int next_instr = *next_instr_io;
    mergepoint_t *mp = *mp_io;
    int ended;

    switch (opcode) {
        case JUMP_FORWARD:
        case JUMP_ABSOLUTE:
        case JUMP_BACKWARD:
        case JUMP_BACKWARD_NO_INTERRUPT: {
            JUMPTO(target);
            JUMPED();
        }

        case POP_JUMP_IF_FALSE:
            x = PsycoObject_IsTrue(po, TOP());
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR)
                break;
            JUMP_IF_CC(INVERT_CC(cc), JUMPTO(target), 1, 1);
            POP_DECREF();
            goto fine;

        case POP_JUMP_IF_TRUE:
            x = PsycoObject_IsTrue(po, TOP());
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR)
                break;
            JUMP_IF_CC(cc, JUMPTO(target), 1, 1);
            POP_DECREF();
            goto fine;

        case POP_JUMP_IF_NONE:
        case POP_JUMP_IF_NOT_NONE: {
            vinfo_t *cond = NTOP(1);
            condition_code_t cc =
                integer_cmp_i(po, cond, (long)Py_None, Py_EQ);
            if (cc == CC_ERROR)
                break;
            if (opcode == POP_JUMP_IF_NOT_NONE)
                cc = INVERT_CC(cc);
            JUMP_IF_CC(cc, JUMPTO(target), 1, 1);
            POP(cond);
            vinfo_decref(cond, po);
            goto fine;
        }

        case JUMP_IF_FALSE_OR_POP:
            x = PsycoObject_IsTrue(po, TOP());
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR)
                break;
            JUMP_IF_CC(INVERT_CC(cc), JUMPTO(target), 0, 1);
            POP_DECREF();
            goto fine;

        case JUMP_IF_TRUE_OR_POP:
            x = PsycoObject_IsTrue(po, TOP());
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR)
                break;
            JUMP_IF_CC(cc, JUMPTO(target), 0, 1);
            POP_DECREF();
            goto fine;

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    *mp_io = mp;
    return ended;
}

#if LE_HAS_OPCODE_SEND && LE_RESUMES_ON_FRAME
/* GET_YIELD_FROM_ITER in a plain generator: what yield from iterates, a
   new reference, 'o' itself if it is a generator, or a coroutine where
   'iterable_coro' (a types.coroutine generator's body) */
static PyObject *
cimpl_yield_from_iter(PyObject *o, long iterable_coro)
{
    if (PyCoro_CheckExact(o) && !iterable_coro) {
        PyErr_SetString(PyExc_TypeError, LE_MSG_YIELD_FROM_COROUTINE);
        return NULL;
    }
    if (PyGen_CheckExact(o) || PyCoro_CheckExact(o)) {
        Py_INCREF(o);
        return o;
    }
    return PyObject_GetIter(o);
}
#endif

#if LE_HAS_OPCODE_CLEANUP_THROW && LE_RESUMES_ON_FRAME
/* CLEANUP_THROW: the value of 'exc', a StopIteration, a new reference */
static PyObject *
cimpl_stop_iteration_value(PyObject *exc)
{
    PyObject *v = ((PyStopIterationObject *)exc)->value;
    Py_INCREF(v);
    return v;
}
#endif

static int
start_directly(PsycoObject *po, int depth, bool drop);

/* iterators and for loops */
static int
compile_iteration(PsycoObject *po, int opcode, int oparg, int target,
                  int *next_instr_io, mergepoint_t **mp_io,
                  int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *v, *x;
    int next_instr = *next_instr_io;
    mergepoint_t *mp = *mp_io;
    int ended;

    switch (opcode) {
        case GET_AITER: {
            unaryfunc getter = NULL, anext = NULL;
            vinfo_t *obj = TOP(), *iter;
            PyTypeObject *type, *tp_iter;
            int known;
            type = Psyco_NeedType(po, obj);
            if (type == NULL)
                break;
            if (type->tp_as_async != NULL) {
                known = Psyco_TypeSlot(po, type, &type->tp_as_async->am_aiter,
                                       (void **)&getter);
                if (known < 0)
                    break;
                if (known == 0) {
                    iter = psyco_generic_call(
                        po, cimpl_get_aiter, CfCommonNewRefPyObject, "v", obj);
                    if (iter == NULL)
                        break;
                    POP_DECREF();
                    PUSH(iter);
                    goto fine;
                }
            }
            if (getter == NULL) {
                PycException_SetTypeFormat(po, PyExc_TypeError,
                                           LE_MSG_ASYNC_FOR_NO_AITER, NULL,
                                           type, NULL);
                break;
            }
            iter = Psyco_META1(po, getter, CfCommonNewRefPyObject, "v", obj);
            if (iter == NULL)
                break;
            tp_iter = Psyco_KnownType(iter);
            known = tp_iter != NULL;
            if (known && tp_iter->tp_as_async != NULL) {
                known = Psyco_TypeSlot(po, tp_iter,
                                       &tp_iter->tp_as_async->am_anext,
                                       (void **)&anext);
                if (known < 0) {
                    vinfo_decref(iter, po);
                    break;
                }
            }
            if (!known) {
                if (!psyco_generic_call(po, cimpl_check_aiter,
                                        CfCommonIntZeroOk, "v", iter)) {
                    vinfo_decref(iter, po);
                    break;
                }
            }
            else if (anext == NULL) {
                PycException_SetTypeFormat(po, PyExc_TypeError,
                                           LE_MSG_ASYNC_FOR_AITER_NO_ANEXT,
                                           NULL, tp_iter, NULL);
                vinfo_decref(iter, po);
                break;
            }
            POP_DECREF();
            PUSH(iter);
            goto fine;
        }

        /* where GET_ITER leaves an index or NULL above the iterator, the
           iterator is always a real one, so the index is NULL */
        case GET_ITER:
            x = PsycoObject_GetIter(po, TOP());
            if (x == NULL)
                break;
            POP_DECREF();
            PUSH(x);
            if (LE_HAS_GET_ITER_INDEX)
                PUSH(psyco_vi_Zero());
            goto fine;

        /* the index or NULL, then the iterator */
        case POP_ITER:
            POP_DECREF();
            POP_DECREF();
            goto fine;

#if LE_HAS_OPCODE_SEND && LE_RESUMES_ON_FRAME
        /* yield from, in a resumable plain generator
           (mergepoints.c:body_compilable): what it iterates */
        case GET_YIELD_FROM_ITER:
            if (Psyco_KnownType(TOP()) != &PyGen_Type) {
                x = psyco_generic_call(
                    po, cimpl_yield_from_iter, CfCommonNewRefPyObject, "vl",
                    TOP(),
                    (long)(po->pr.co->co_flags & CO_ITERABLE_COROUTINE));
                if (x == NULL)
                    break;
                POP_DECREF();
                PUSH(x);
            }
            if (LE_HAS_GET_ITER_INDEX)
                PUSH(psyco_vi_Zero());
            goto fine;

        /* its loop: the value on top is sent into the iterator under it,
           which yields, for the YIELD_VALUE after, which pauses the frame
           with the iterator under what it yielded, as ceval leaves it; or
           returns, going to the loop's end, where the iterator is popped
           (by END_SEND where there is one). Where GET_ITER leaves an index
           or NULL, a NULL is between the iterator and the value, as an
           await's PUSH_NULL leaves it too. */
        case SEND: {
            vinfo_array_t *array = array_new(1);
            condition_code_t cc;
            bool yielded;
            int depth = LE_HAS_GET_ITER_INDEX ? 3 : 2;
            vinfo_t *iter = NTOP(depth);
            PyTypeObject *tp;
            if (PsycoGenexpr_Check(iter)) {
                int start = psyco_coro_start(po, iter);
                if (start > 0) {
                    array_release(array);
                    start = start_directly(po, depth, !LE_HAS_OPCODE_END_SEND);
                    if (start < 0)
                        break;
                    if (start > 0)
                        goto fine; /* to the yield */
                    JUMPTO(target);
                    mp = psyco_next_merge_point(po->pr.merge_points,
                                                next_instr);
                    goto fine;
                }
                if (start < 0) {
                    array_release(array);
                    break;
                }
            }
            tp = Psyco_NeedType(po, iter);
            if (tp == NULL) {
                array_release(array);
                break;
            }
            if (tp == &PyGen_Type || tp == &PyCoro_Type)
                x = psyco_gen_send_status(po, iter, TOP(), array);
            else
                x = psyco_generic_call(po, le_iter_send,
                                       CfReturnTypeInt | CfPyErrIfNeg, "vvA",
                                       iter, TOP(), array);
            if (x == NULL) {
                array_release(array);
                break;
            }
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR) {
                vinfo_decref(array->items[0], po);
                array_release(array);
                break;
            }
            yielded = runtime_condition_t(po, cc);
            POP_DECREF(); /* the value sent */
            if (!yielded && !LE_HAS_OPCODE_END_SEND)
                POP_DECREF(); /* the iterator */
            PUSH(array->items[0]);
            array_release(array);
            if (yielded)
                goto fine;
            JUMPTO(target);
            mp = psyco_next_merge_point(po->pr.merge_points, next_instr);
            goto fine;
        }

        case END_SEND:
            POP(v);
            if (LE_HAS_GET_ITER_INDEX)
                POP_DECREF(); /* the NULL */
            POP_DECREF();     /* the iterator */
            PUSH(v);
            goto fine;
#endif

        case END_FOR:
            POP_DECREF();
            if (!LE_HAS_FOR_ITER_EXIT_PAST_POP_TOP &&
                !LE_HAS_FOR_ITER_EXIT_KEEPS_ITER)
                POP_DECREF();
            goto fine;

        case FOR_ITER:
            v = PsycoIter_Next(po, LE_HAS_GET_ITER_INDEX ? NTOP(2) : TOP());
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
                    if (!LE_HAS_FOR_ITER_EXIT_KEEPS_ITER)
                        POP_DECREF();
                    JUMPTO(target);
                    mp = psyco_next_merge_point(po->pr.merge_points,
                                                next_instr);
                }
                else
                    break; /* any other exception, or error in
                              runtime_NON_NULL_t() */
            }
            goto fine;

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    *mp_io = mp;
    return ended;
}

/* calls */
static int
compile_calls(PsycoObject *po, int opcode, int oparg, int target,
              int *next_instr_io, mergepoint_t **mp_io, int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    PyCodeObject *co = po->pr.co;
    vinfo_t *x, *super_self;
    int next_instr = *next_instr_io;
    int ended, found, super_after, super_pop;
    /* some versions have no case that uses these */
    (void)co;

    switch (opcode) {
        case PRINT_EXPR:
            if (!psyco_generic_call(po, cimpl_print_expr, CfCommonIntZeroOk,
                                    "v", TOP()))
                break;
            POP_DECREF();
            goto fine;

        /* an async generator's yield: its value wrapped
           (psyco_async_gen_wrapper) */
        case ASYNC_GEN_WRAP:
            x = psyco_generic_call(po, cimpl_async_gen_wrap,
                                   CfCommonNewRefPyObject | CfNoPython, "v",
                                   TOP());
            if (x == NULL)
                break;
            POP_DECREF();
            PUSH(x);
            goto fine;

        /* the intrinsics left as CALL_INTRINSIC_1 (bytecode.h:le_instr)
           that mergepoints.c:compiled_intrinsic allows: a generator's
           PEP 479 handler, frozenset() of a new set, and the REPL's print */
        case CALL_INTRINSIC_1:
#ifdef INTRINSIC_BUILD_FROZENSET
            if (oparg == INTRINSIC_BUILD_FROZENSET) {
                x = psyco_generic_call(
                    po, cimpl_build_frozenset,
                    CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v",
                    TOP());
                if (x == NULL)
                    break;
                POP_DECREF();
                Psyco_AssertType(po, x, &PyFrozenSet_Type);
                PUSH(x);
                goto fine;
            }
#endif
#if LE_HAS_OPCODE_CALL_INTRINSIC_1
            if (oparg == INTRINSIC_STOPITERATION_ERROR) {
                int flags = po->pr.co->co_flags;
                extra_assert(flags & LE_BODY_FLAGS);
                x = psyco_generic_call(
                    po, cimpl_stopiteration_error,
                    CfCommonNewRefPyObject | CfNoPython, "vll", TOP(),
                    (long)(flags & CO_ASYNC_GENERATOR
                               ? LE_MSG_ASYNC_GEN_STOP_ITERATION
                           : flags & CO_COROUTINE
                               ? LE_MSG_CORO_STOP_ITERATION
                               : LE_MSG_GENERATOR_STOP_ITERATION),
                    (long)(flags & CO_ASYNC_GENERATOR
                               ? LE_MSG_ASYNC_GEN_STOP_ASYNC_ITERATION
                               : NULL));
                if (x == NULL)
                    break;
                POP_DECREF();
                PUSH(x);
                goto fine;
            }
            extra_assert(oparg == INTRINSIC_PRINT);
            if (!psyco_generic_call(po, cimpl_print_expr, CfCommonIntZeroOk,
                                    "v", TOP()))
                break;
            POP_DECREF();
            PUSH(psyco_vi_None());
#endif
            goto fine;

        /* the one intrinsic mergepoints.c:compiled_intrinsic_2 allows: a
           class pattern with no arguments, isinstance with
           _PyEval_MatchClass's error for a pattern that is not a class */
        case CALL_INTRINSIC_2: {
#ifdef INTRINSIC_MATCH_CLASS_ISINSTANCE
            vinfo_t *is;
            PyTypeObject *tp_type;
            extra_assert(oparg == INTRINSIC_MATCH_CLASS_ISINSTANCE);
            tp_type = Psyco_NeedType(po, TOP());
            if (tp_type == NULL)
                break;
            if ((tp_type->tp_flags & Py_TPFLAGS_TYPE_SUBCLASS) == 0) {
                PycException_SetFormat(po, PyExc_TypeError,
                                       LE_MSG_MATCH_NOT_A_TYPE);
                break;
            }
            is = PsycoObject_IsInstance(po, NTOP(2), TOP());
            if (is == NULL)
                break;
            x = PsycoBool_FromZeroOrOne(po, is);
            vinfo_decref(is, po);
            POP_DECREF_N(2);
            PUSH(x);
#endif
            goto fine;
        }

        /* the bound method PRECALL would split is called as it
           is; KW_NAMES's names are read by its CALL. The frame's
           cells and free variables are in place before the code
           runs (frames.h:le_frame_start). */
        case PRECALL:
        case KW_NAMES:
        case COPY_FREE_VARS:
            goto fine;

        case CALL_METHOD:
        case CALL:
        case CALL_KW: {
            /* LOAD_METHOD pushes a function and its self, or the
               attribute and NULL beside it, as PUSH_NULL does beside
               other callables: the (NULL, callable) form, or
               (callable, NULL) where the callable is under its NULL.
               The function-and-self form also comes from elsewhere (a
               decorator, a with statement's exit).
               CALL_KW's keyword names are on top. */
            int i, stack_to_pop, null = 1, kw = opcode == CALL_KW;
            PyObject *kwnames = NULL;
            vinfo_t *callable;
#if LE_HAS_CALLABLE_UNDER_NULL
            callable = NTOP(oparg + 2 + kw);
            if (!psyco_knowntobe(NTOP(oparg + 1 + kw), 0)) {
                null = 0;
                oparg++;
            }
#else
            if (!psyco_knowntobe(NTOP(oparg + 2), 0)) {
                /* or a function under its self, as LOAD_METHOD leaves
                   them */
                null = 0;
                oparg++;
            }
            callable = NTOP(oparg + 1);
#endif
            if (oparg == 0 && !kw) {
                /* super and its NULL */
                vinfo_t **callable_at =
                    STACK_POINTER() - 1 - LE_HAS_CALLABLE_UNDER_NULL;
                found = super_call_known(po, callable_at, next_instr, &x,
                                         &super_self, &super_after);
                if (found < 0)
                    break;
                super_pop = 2;
                if (found)
                    goto super_known;
                callable = *callable_at;
            }
#if LE_HAS_OPCODE_CALL
            if (opcode == CALL &&
                !call_kwnames(po, co, po->pr.next_instr, &kwnames))
                break;
#endif
            po->pr.call_site =
                next_instr - LE_CACHE_BYTES(opcode) - LE_UNIT + 1;
            x = psyco_ext_do_calls(po, kw ? CALL_FUNCTION_KW : CALL_FUNCTION,
                                   oparg, kwnames, callable, STACK_POINTER(),
                                   &stack_to_pop);
            end_call_site(po);
            if (x == NULL)
                break;
            for (i = stack_to_pop + null;
                 i--;) /* args, callable and the NULL */
                POP_DECREF();
            PUSH(x);
#if LE_HAS_BREAKER_SKIPPED_AFTER_LOAD_SPECIAL
            /* ceval doesn't test eval_breaker after the call that follows
               LOAD_SPECIAL (__enter__'s, __aenter__'s), so an interrupt
               landing there is raised inside the with block, whose
               __exit__ runs */
            if (le_op(le_code(co), next_instr - LE_CACHE_BYTES(opcode) -
                                       2 * LE_UNIT) == LOAD_SPECIAL)
                goto fine;
#endif
            if (!breaker_after_call(po, next_instr))
                break;
            goto fine;
        }

        case CALL_FUNCTION:
        case CALL_FUNCTION_KW:
        case CALL_FUNCTION_EX: {
            int i, stack_to_pop;
            if (opcode == CALL_FUNCTION && oparg == 0) {
                found = super_call_known(po, STACK_POINTER() - 1, next_instr,
                                         &x, &super_self, &super_after);
                if (found < 0)
                    break;
                super_pop = 1;
                if (found)
                    goto super_known;
            }
            po->pr.call_site =
                next_instr - LE_CACHE_BYTES(opcode) - LE_UNIT + 1;
            if (opcode == CALL_FUNCTION_EX)
                x = psyco_ext_do_call_ex(po, oparg, STACK_POINTER(),
                                         &stack_to_pop);
            else
                x = psyco_ext_do_calls(
                    po, opcode, oparg, NULL,
                    NTOP(oparg + 1 + (opcode == CALL_FUNCTION_KW)),
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

        /* super().name, the call and the load in one step, as
           LOAD_SUPER_ATTR: the load's place may have been a merge point */
        super_known:
            while (super_pop--) POP_DECREF();
            PUSH(x);
            if (super_self != NULL)
                PUSH(super_self);
            next_instr = super_after;
            *mp_io = psyco_next_merge_point(po->pr.merge_points, next_instr);
            goto fine;

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    return ended;
}

#if LE_RESUMES_COROUTINES || LE_RESUMES_ON_FRAME
/* whether coroutine 'coro' is awaiting something, as _PyGen_yf (not
   exported) tells: paused just before a YIELD_FROM, or where the frame
   is on the coroutine, at a yield whose resume is after an await */
static int
coro_awaits(PyGenObject *coro)
{
#if LE_RESUMES_ON_FRAME
#ifdef FRAME_STATE_SUSPENDED
    return coro->gi_frame_state == FRAME_SUSPENDED_YIELD_FROM;
#else
    le_frame_t *f =
        (le_frame_t *)((char *)coro + offsetof(PyGenObject, gi_iframe));
    PyCodeObject *co = LE_FRAME_CODE(f);
    int at;
    le_instr_t in;
    if (coro->gi_frame_state >= FRAME_CLEARED ||
        coro->gi_frame_state == FRAME_CREATED)
        return 0;
    at = (int)((char *)LE_FRAME_PLACE(f) - (char *)_PyCode_CODE(co)) + LE_UNIT;
    if (at < 0 || at >= le_code(co).size)
        return 0;
    in = le_instr(le_code(co), at);
    return in.op == RESUME && in.arg >= 2;
#endif
#else
    PyFrameObject *f = coro->gi_frame;
    const unsigned char *code;
    if (f == NULL || f->f_lasti < 0)
        return 0;
#if !LE_HAS_FRAME_STATE
    if (f->f_stacktop == NULL)
        return 0;
#endif
    code = (const unsigned char *)PyBytes_AS_STRING(f->f_code->co_code);
    return code[(LE_HAS_LASTI_IN_INSTRUCTIONS ? f->f_lasti * LE_UNIT
                                              : f->f_lasti) +
                LE_UNIT] == YIELD_FROM;
#endif
}

/* a generator made by a types.coroutine function */
static inline int
is_iterable_coroutine(PyObject *o)
{
#if LE_RESUMES_ON_FRAME
    return PyGen_CheckExact(o) &&
           (LE_FRAME_CODE(
                (le_frame_t *)((char *)o + offsetof(PyGenObject, gi_iframe)))
                ->co_flags &
            CO_ITERABLE_COROUTINE);
#else
    return PyGen_CheckExact(o) &&
           (((PyCodeObject *)((PyGenObject *)o)->gi_code)->co_flags &
            CO_ITERABLE_COROUTINE);
#endif
}

/* which await GET_AWAITABLE at 'at' (with argument 'oparg') is, for its
   error, as ceval's format_awaitable_error tells: 1 that of __aenter__'s
   result, 2 that of __aexit__'s, 0 another */
static int
awaitable_kind(le_code_t c, int at, int oparg)
{
#if LE_HAS_AWAIT_ERROR_BY_ARG
    (void)c;
    (void)at;
    return oparg;
#else
    int prev = at >= LE_UNIT ? le_op(c, at - LE_UNIT) : -1;
    (void)oparg;
    if (prev == BEFORE_ASYNC_WITH)
        return 1;
#if LE_HAS_OPCODE_WITH_CLEANUP_START
    return prev == WITH_CLEANUP_START ? 2 : 0;
#else
    if (prev == WITH_EXCEPT_START ||
        (prev == CALL_FUNCTION && at >= 2 * LE_UNIT &&
         le_op(c, at - 2 * LE_UNIT) == DUP_TOP))
        return 2;
    return 0;
#endif
#endif
}

/* GET_ANEXT's kind of await: _PyCoro_GetAwaitableIter's alone, without
   GET_AWAITABLE's test of a coroutine awaiting already */
#define AWAITABLE_ITER 3

/* GET_AWAITABLE: what awaiting 'o' iterates, a new reference, as
   _PyCoro_GetAwaitableIter (not exported) makes it and ceval checks it;
   'kind' as awaitable_kind gives it, or AWAITABLE_ITER */
static PyObject *
cimpl_get_awaitable(PyObject *o, long kind)
{
    PyTypeObject *t = Py_TYPE(o);
    unaryfunc getter;
    PyObject *res;
    if (PyCoro_CheckExact(o) || is_iterable_coroutine(o)) {
        if (kind != AWAITABLE_ITER && PyCoro_CheckExact(o) &&
            coro_awaits((PyGenObject *)o)) {
            PyErr_SetString(PyExc_RuntimeError, LE_MSG_CORO_AWAITED_ALREADY);
            return NULL;
        }
        Py_INCREF(o);
        return o;
    }
    getter = t->tp_as_async != NULL ? t->tp_as_async->am_await : NULL;
    if (getter == NULL) {
        PyErr_Format(PyExc_TypeError,
                     kind == 1   ? LE_MSG_AWITH_ENTER_NOT_AWAITABLE
                     : kind == 2 ? LE_MSG_AWITH_EXIT_NOT_AWAITABLE
                                 : LE_MSG_AWAIT_NOT_AWAITABLE,
                     t->tp_name);
        return NULL;
    }
    res = getter(o);
    if (res == NULL)
        return NULL;
    if (PyCoro_CheckExact(res) || is_iterable_coroutine(res)) {
#ifdef LE_MSG_AWAIT_RETURNED_COROUTINE_QUALIFIED
        PyErr_Format(PyExc_TypeError,
                     LE_MSG_AWAIT_RETURNED_COROUTINE_QUALIFIED, t);
#else
        PyErr_SetString(PyExc_TypeError, LE_MSG_AWAIT_RETURNED_COROUTINE);
#endif
        Py_DECREF(res);
        return NULL;
    }
    if (!PyIter_Check(res)) {
#ifdef LE_MSG_AWAIT_NON_ITERATOR_QUALIFIED
        PyErr_Format(PyExc_TypeError, LE_MSG_AWAIT_NON_ITERATOR_QUALIFIED, t,
                     Py_TYPE(res));
#else
        PyErr_Format(PyExc_TypeError, LE_MSG_AWAIT_NON_ITERATOR,
                     Py_TYPE(res)->tp_name);
#endif
        Py_DECREF(res);
        return NULL;
    }
    return res;
}

/* GET_ANEXT, as ceval's (_PyEval_GetANext): what awaiting the async
   iterator's __anext__() iterates, a new reference */
static PyObject *
cimpl_get_anext(PyObject *aiter)
{
    PyTypeObject *type = Py_TYPE(aiter);
    unaryfunc getter =
        type->tp_as_async != NULL ? type->tp_as_async->am_anext : NULL;
    PyObject *next_iter, *awaitable, *exc, *val, *tb;
    if (PyAsyncGen_CheckExact(aiter))
        return getter(aiter);
    if (getter == NULL) {
        PyErr_Format(PyExc_TypeError, LE_MSG_ASYNC_FOR_NO_ANEXT,
                     type->tp_name);
        return NULL;
    }
    next_iter = getter(aiter);
    if (next_iter == NULL)
        return NULL;
    awaitable = cimpl_get_awaitable(next_iter, AWAITABLE_ITER);
    if (awaitable == NULL) {
        /* as _PyErr_FormatFromCause: the error is the new one's cause and
           context */
        PyErr_Fetch(&exc, &val, &tb);
        PyErr_NormalizeException(&exc, &val, &tb);
        if (tb != NULL)
            PyException_SetTraceback(val, tb);
        Py_DECREF(exc);
        Py_XDECREF(tb);
        PyErr_Format(PyExc_TypeError, LE_MSG_ASYNC_FOR_BAD_ANEXT,
                     Py_TYPE(next_iter)->tp_name);
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
    Py_DECREF(next_iter);
    return awaitable;
}

#if LE_RESUMES_COROUTINES
/* YIELD_FROM's send of 'v' into 'receiver', as ceval does it with no
   trace function: 1 with the value it yielded in out[0], 0 with the value
   it returned, -1 with its exception */
static int
cimpl_await_send(PyObject *receiver, PyObject *v, PyObject *out[])
{
#if LE_HAS_ITER_SEND
    PySendResult r = PyIter_Send(receiver, v, &out[0]);
    if (r == PYGEN_ERROR)
        return -1;
    return r == PYGEN_NEXT;
#else
    _Py_IDENTIFIER(send);
    PyObject *res;
    if (PyGen_CheckExact(receiver) || PyCoro_CheckExact(receiver))
        res = _PyGen_Send((PyGenObject *)receiver, v);
    else if (v == Py_None)
        res = Py_TYPE(receiver)->tp_iternext(receiver);
    else
        res = _PyObject_CallMethodIdObjArgs(receiver, &PyId_send, v, NULL);
    if (res != NULL) {
        out[0] = res;
        return 1;
    }
    if (_PyGen_FetchStopIterationValue(&out[0]) < 0)
        return -1;
    return 0;
#endif
}
#endif

#endif

#if LE_RESUMES_ON_FRAME
/* A resumable generator's YIELD_VALUE at 'at': its frame (in the
   GENEXPR_SLOT_ACC word) is left as ceval's YIELD_VALUE leaves it, and the
   run ends returning the value (EReturn). The frame already holds the
   words still its own, and its cells and free variables; the run's
   locals and the stack items under the value are stored, each a reference
   the frame takes (a local's word is NULL already where the run holds
   NULL). Its caught exception's frame is let go first, given the locals
   (catcher_flush). */
static void
yield_resumable(PsycoObject *po, int at, bool after_await)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    PyCodeObject *co = po->pr.co;
    int nlocals = le_code_nlocals(co), nplus = le_code_nlocalsplus(co);
    int nstack = STACK_LEVEL() - 1, i;
    vinfo_t *v;
    le_gen_pause_t *pause;
    (void)after_await;
    if (!catcher_flush(po, true))
        return;
    /* every item real before anything is stored: making one may call */
    for (i = 0; i < nlocals + nstack; i++) {
        v = i < nlocals ? LOC_LOCALS_PLUS[i] : stack_a[i - nlocals];
        if ((i >= nlocals || local_is_runs(po, i)) && v != NULL &&
            !psyco_knowntobe(v, 0) && !compute_vinfo(v, po))
            return;
    }
    pause = gen_pause_new(po, le_lasti_of(at), nstack);
    if (psyco_generic_call(po, psyco_gen_pause, CfNoReturnValue | CfNoPython,
                           "vl", psyco_frame_words_frame(po),
                           (long)pause) == NULL)
        return;
    for (i = 0; i < nlocals + nstack; i++) {
        vinfo_t **slot =
            i < nlocals ? &LOC_LOCALS_PLUS[i] : &stack_a[i - nlocals];
        v = *slot;
        /* a stack slot of a frame CPython reused may still hold an old
           word: a NULL item (PUSH_NULL's) is stored */
        if ((i >= nlocals ||
             (local_is_runs(po, i) && !psyco_knowntobe(v, 0))) &&
            !frame_word_store(po, i < nlocals ? i : nplus + i - nlocals, v))
            return;
        if (v != NULL) {
            vinfo_decref(v, po);
            *slot = NULL;
        }
    }
    v = stack_a[nstack];
    stack_a[nstack] = NULL;
    po->pr.stack_level = 0;
    po->pr.iblock = 0;
    PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), v);
}
#else
/* A resumable generator's YIELD_VALUE at 'at': its frame (that of the
   run in the GENEXPR_SLOT_ACC word) is left as ceval's YIELD_VALUE leaves it,
   with the locals and the stack items under the value, each a reference it
   takes, and the run ends returning the value (EReturn), with no block
   run. The cells and free variables stay the frame's. Its caught
   exception's frame is let go first, given the locals (catcher_flush). */
static void
yield_resumable(PsycoObject *po, int at, bool after_await)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    PyCodeObject *co = po->pr.co;
    int nlocals = le_code_nlocals(co), nplus = le_code_nlocalsplus(co);
    int nstack = STACK_LEVEL() - 1, i;
    vinfo_t *run = po->vlocals.items[GENEXPR_SLOT_ACC(co)], *frame, *v;
    le_gen_pause_t *pause;
#if LE_RESUMES_COROUTINES
    int traced = 0;
    /* the await ran code, which may have started tracing: tested while
       everything is still in the frame's slots, as a rare branch is */
    if (after_await && (traced = psyco_tracing_on(po)) < 0)
        return;
#else
    (void)after_await;
#endif
    if (!catcher_flush(po, true))
        return;
    /* every item real before anything is stored: making one may call */
    for (i = 0; i < nlocals + nstack; i++) {
        v = i < nlocals ? LOC_LOCALS_PLUS[i] : stack_a[i - nlocals];
        if (v != NULL && !psyco_knowntobe(v, 0) && !compute_vinfo(v, po))
            return;
    }
    pause = psyco_owner_alloc(sizeof(le_gen_pause_t) +
                              po->pr.iblock * sizeof(PyTryBlock));
    pause->lasti = le_lasti_of(at);
    pause->nstack = nstack;
    pause->iblock = po->pr.iblock;
    for (i = 0; i < po->pr.iblock; i++) {
        pause->blocks[i].b_type = po->pr.blockstack[i].b_type;
        pause->blocks[i].b_handler =
            le_lasti_of(po->pr.blockstack[i].b_handler);
        pause->blocks[i].b_level = po->pr.blockstack[i].b_level;
    }
    frame =
        psyco_generic_call(po, psyco_gen_pause, CfReturnTypePtr | CfNoPython,
                           "vl", run, (long)pause);
    if (frame == NULL)
        return;
    for (i = 0; i < nlocals + nstack; i++) {
        vinfo_t **slot =
            i < nlocals ? &LOC_LOCALS_PLUS[i] : &stack_a[i - nlocals];
        v = *slot;
        /* an unbound local's slot is already empty (LeGen_Run took the
           locals), but a stack slot of a frame CPython reused may still
           hold an old word: a NULL item (LOAD_METHOD's, under a method) is
           stored */
        if ((v == NULL || psyco_knowntobe(v, 0)) && i >= nlocals) {
            vinfo_t *null = v != NULL ? v : psyco_vi_Zero();
            bool ok = psyco_put_nth_field(
                po, frame, FRAME_localsplus,
                i < nlocals ? i : nplus + i - nlocals, null);
            if (v == NULL)
                vinfo_decref(null, po);
            if (!ok) {
                vinfo_decref(frame, po);
                return;
            }
        }
        else if (v != NULL && !psyco_knowntobe(v, 0)) {
            if (!psyco_put_nth_field(po, frame, FRAME_localsplus,
                                     i < nlocals ? i : nplus + i - nlocals,
                                     v)) {
                vinfo_decref(frame, po);
                return;
            }
            consume_reference(po, v);
        }
        if (v != NULL) {
            vinfo_decref(v, po);
            *slot = NULL;
        }
    }
    v = stack_a[nstack];
#if LE_RESUMES_COROUTINES
    if (traced) {
        vinfo_t *ok =
            psyco_generic_call(po, psyco_trace_yield,
                               CfReturnTypeInt | CfPyErrIfNeg, "vv", frame, v);
        if (ok == NULL) {
            vinfo_decref(frame, po);
            return;
        }
        vinfo_decref(ok, po);
    }
#endif
    vinfo_decref(frame, po);
    stack_a[nstack] = NULL;
    po->pr.stack_level = 0;
    po->pr.iblock = 0;
    PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), v);
}
#endif

/* An await at 'at' that suspends in a coroutine's body run as a call
   (GENEXPR_DIRECT): the coroutine CPython would have made is made, its
   frame (the call's, if one was asked for) holding the locals and the
   stack items under the value, paused as ceval leaves it at the await's
   yield (frames.c:psyco_resume_suspend), and the call returns the marker
   that says so, its await taking the coroutine and the value. */
static void
yield_direct(PsycoObject *po, int at)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    int nstack = STACK_LEVEL() - 1;
    vinfo_t *st, *v;
    st = resume_state(po, nstack);
    if (st == NULL)
        return;
    v = psyco_generic_call(po, psyco_resume_suspend, CfCommonNewRefPyObject,
                           "vllvv", st, (long)at, (long)nstack,
                           po->vlocals.items[GENEXPR_SLOT_ACC(po->pr.co)],
                           stack_a[nstack]);
    vinfo_decref(st, po);
    if (v == NULL)
        return;
    while (STACK_LEVEL() > 0) POP_DECREF();
    po->pr.iblock = 0;
    PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), v);
}

/* The await of the coroutine pending 'depth' deep on the stack, made by
   the call just before and awaited at once, runs its body as a call
   (psyfunc.c:psyco_coro_start), the value sent popped first. 1 if the
   body suspended: the coroutine made for it is in the pending one's place
   and the value it yielded is pushed. 0 if it returned: its value is
   pushed, the pending coroutine popped first if 'drop'. -1 with an
   exception. */
static int
start_directly(PsycoObject *po, int depth, bool drop)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    vinfo_t *pending = NTOP(depth), *x;
    vinfo_array_t *array;
    condition_code_t cc;
    /* the body returns the await's value, or the marker that it
       suspended, having made its coroutine */
    x = psyco_call_genexpr(po, pending, GENEXPR_DIRECT,
                           pending->array->items[iGENEXPR_FUNC], NULL);
    if (x == NULL)
        return -1;
    cc = integer_cmp_i(po, x, (long)psyco_coro_suspended, Py_EQ);
    if (cc == CC_ERROR) {
        vinfo_decref(x, po);
        return -1;
    }
    POP_DECREF(); /* the value sent */
    if (runtime_condition_f(po, cc)) {
        vinfo_decref(x, po);
        array = array_new(2);
        if (psyco_generic_call(po, psyco_coro_taken,
                               CfNoReturnValue | CfNoPython, "A",
                               array) == NULL) {
            array_release(array);
            return -1;
        }
        vinfo_decref(NTOP(depth - 1), po);
        stack_a[po->pr.stack_level - (depth - 1)] = array->items[0];
        PUSH(array->items[1]);
        array_release(array);
        return 1;
    }
    if (drop)
        POP_DECREF();
    PUSH(x);
    return 0;
}

/* returning, yielding and making functions */
static int
compile_functions_and_returns(PsycoObject *po, int opcode, int oparg,
                              int target, int *next_instr_io,
                              mergepoint_t **mp_io, int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    PyCodeObject *co = po->pr.co;
    vinfo_t *v, *x;
    condition_code_t cc;
    int next_instr = *next_instr_io;
    int ended;

    switch (opcode) {
        case RETURN_CONST:
            v = vinfo_new(CompileTime_New((long)GETCONST(oparg)));
            goto return_value;

        case RETURN_VALUE:
            POP(v);
        return_value:
            /* a resumable generator returns its value, which gen_send_ex
               makes StopIteration's */
            if ((co->co_flags & CO_GENERATOR) &&
                psyco_generator_mode(po) != GENEXPR_RESUMABLE) {
                /* a fused generator: return the consumer's result; the
                 * generator's own return value is dropped, as the
                 * consumers drop StopIteration's value */
                int slot = GENEXPR_SLOT_MODE(co);
                long mode =
                    CompileTime_Get(po->vlocals.items[slot]->source)->value;
                vinfo_decref(v, po);
                if (mode == GENEXPR_ANY)
                    v = psyco_vi_Py_False();
                else if (mode == GENEXPR_ALL)
                    v = psyco_vi_Py_True();
                else if (mode == GENEXPR_MIN || mode == GENEXPR_MAX) {
                    v = psyco_generic_call(po, cimpl_minmax_result,
                                           CfCommonNewRefPyObject, "vl",
                                           po->vlocals.items[slot + 1],
                                           (long)(mode == GENEXPR_MIN));
                    if (v == NULL)
                        break;
                }
#if LE_HAS_SUM_COMPENSATED
                else if (mode == GENEXPR_SUM_FLOAT ||
                         mode == GENEXPR_SUM_COMPLEX) {
                    v = psyco_sum_result(po, (int)mode,
                                         po->vlocals.items[slot + 1],
                                         po->vlocals.items[slot + 2]);
                    if (v == NULL)
                        break;
                }
#endif
                else {
                    v = po->vlocals.items[slot + 1];
                    vinfo_incref(v);
                }
            }
            PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), v);
            break;

        case GEN_START:
            /* a resumable generator pops the None gen_send_ex pushed; one
               fused with its consumer is entered with nothing pushed */
            if (psyco_generator_mode(po) == GENEXPR_RESUMABLE)
                POP_DECREF();
            goto fine;

        case RETURN_GENERATOR:
            /* only in a generator fused with its consumer: push the
               None its first send() would have, for the
               POP_TOP that follows */
            PUSH(psyco_vi_None());
            goto fine;

        case YIELD_VALUE: {
            /* in a generator fused with its consumer (GENEXPR_*): do the
               consumer's step, then push the value send() would have, for the
               POP_TOP that follows */
            int slot = GENEXPR_SLOT_MODE(co);
            long mode =
                CompileTime_Get(po->vlocals.items[slot]->source)->value;
            vinfo_t *acc = po->vlocals.items[slot + 1];
#if LE_HAS_YIELD_VALUE_WRAPS
            /* an async generator's yield wraps its value, as ASYNC_GEN_WRAP
               does where that is an instruction of its own */
            if (co->co_flags & CO_ASYNC_GENERATOR) {
                x = psyco_generic_call(po, cimpl_async_gen_wrap,
                                       CfCommonNewRefPyObject | CfNoPython,
                                       "v", TOP());
                if (x == NULL)
                    break;
                POP_DECREF();
                PUSH(x);
            }
#endif
            v = TOP();
            if (mode == GENEXPR_RESUMABLE) {
                yield_resumable(po, next_instr - LE_UNIT, false);
                break;
            }
            if (mode == GENEXPR_DIRECT) {
                yield_direct(po, next_instr - LE_UNIT);
                break;
            }
            if (mode == GENEXPR_LIST) {
                if (!PsycoList_Append(po, acc, v))
                    break;
            }
            else if (mode == GENEXPR_SET) {
                if (!psyco_generic_call(po, PySet_Add, CfCommonIntZeroOk, "vv",
                                        acc, v))
                    break;
            }
#if LE_HAS_SUM_COMPENSATED
            else if (mode == GENEXPR_SUM || mode == GENEXPR_SUM_FLOAT ||
                     mode == GENEXPR_SUM_COMPLEX ||
                     mode == GENEXPR_SUM_GENERIC) {
                /* builtin_sum_impl's loop for the phase it is in; another
                   phase is another version of the fused code */
                int phase = (int)mode;
                if (!psyco_sum_step(po, &phase, &po->vlocals.items[slot + 1],
                                    &po->vlocals.items[slot + 2], v))
                    break;
                if (phase != mode) {
                    vinfo_decref(po->vlocals.items[slot], po);
                    po->vlocals.items[slot] = vinfo_new(
                        CompileTime_NewSk(sk_new(phase, SkFlagFixed)));
                }
            }
#else
            else if (mode == GENEXPR_SUM) {
                /* as builtin_sum's generic loop (its fast paths give
                   the same results as + on ints and floats) */
                if (Psyco_KnownType(acc) != NULL)
                    x = PsycoNumber_Add(po, acc, v);
                else
                    x = psyco_generic_call(po, PyNumber_Add,
                                           CfCommonNewRefPyObject, "vv", acc,
                                           v);
                if (x == NULL)
                    break;
                po->vlocals.items[slot + 1] = x;
                vinfo_decref(acc, po);
            }
#endif
            else if (mode == GENEXPR_MIN || mode == GENEXPR_MAX) {
                x = psyco_generic_call(
                    po, cimpl_minmax_step, CfCommonNewRefPyObject, "vvl", acc,
                    v, (long)(mode == GENEXPR_MIN ? Py_LT : Py_GT));
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

        /* async for's next value: what awaiting __anext__() iterates,
           above the async iterator; the await follows */
        case GET_ANEXT:
            x = psyco_generic_call(po, cimpl_get_anext, CfCommonNewRefPyObject,
                                   "v", TOP());
            if (x == NULL)
                break;
            PUSH(x);
            goto fine;

#if LE_RESUMES_ON_FRAME
        /* a resumable coroutine's await (psyco_is_resumable_generator):
           GET_AWAITABLE, then SEND's loop */
        case GET_AWAITABLE:
            /* a coroutine awaited as soon as it is made stays pending: the
               SEND may run its body as a call */
            if (PsycoGenexpr_Check(TOP()))
                goto fine;
            x = psyco_generic_call(
                po, cimpl_get_awaitable, CfCommonNewRefPyObject, "vl", TOP(),
                (long)awaitable_kind(le_code(po->pr.co), po->pr.next_instr,
                                     oparg));
            if (x == NULL)
                break;
            POP_DECREF();
            PUSH(x);
            goto fine;
#endif
#if LE_RESUMES_COROUTINES
        /* a resumable coroutine's await (psyco_is_resumable_generator):
           GET_AWAITABLE, LOAD_CONST None, YIELD_FROM */
        case GET_AWAITABLE:
            /* a coroutine awaited as soon as it is made stays pending: the
               YIELD_FROM may run its body as a call */
            if (PsycoGenexpr_Check(TOP()))
                goto fine;
            x = psyco_generic_call(
                po, cimpl_get_awaitable, CfCommonNewRefPyObject, "vl", TOP(),
                (long)awaitable_kind(le_code(po->pr.co), po->pr.next_instr,
                                     oparg));
            if (x == NULL)
                break;
            POP_DECREF();
            PUSH(x);
            goto fine;

        case YIELD_FROM: {
            /* what the awaited object yields pauses the frame as ceval's
               YIELD_FROM leaves it, at the instruction before, so that ceval
               resumes it at the YIELD_FROM with the receiver on the stack */
            vinfo_array_t *array;
            if (PsycoGenexpr_Check(NTOP(2))) {
                int start = psyco_coro_start(po, NTOP(2));
                if (start > 0) {
                    start = start_directly(po, 2, true);
                    if (start < 0)
                        break;
                    if (start == 0)
                        goto fine;
                    goto pause_at_await;
                }
                if (start < 0)
                    break;
            }
            array = array_new(1);
            x = psyco_generic_call(po, cimpl_await_send,
                                   CfReturnTypeInt | CfPyErrIfNeg, "vvA",
                                   NTOP(2), TOP(), array);
            if (x == NULL) {
                array_release(array);
                break;
            }
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR) {
                vinfo_decref(array->items[0], po);
                array_release(array);
                break;
            }
            POP_DECREF(); /* the value sent */
            if (runtime_condition_f(po, cc)) {
                PUSH(array->items[0]);
                array_release(array);
                goto pause_at_await;
            }
            POP_DECREF(); /* the receiver */
            PUSH(array->items[0]);
            array_release(array);
            goto fine;

        pause_at_await:
            if (psyco_generator_mode(po) == GENEXPR_DIRECT)
                yield_direct(po, next_instr - 2 * LE_UNIT);
            else
                yield_resumable(po, next_instr - 2 * LE_UNIT, true);
            break;
        }
#endif

        case MAKE_FUNCTION: {
            /* the optional fields in the order ceval pops them; each
               is made fit to cross calls before anything is popped, as
               that can fail (Restart, above), and replaces the 0 the
               new function holds */
            const int fields[4] = {iFUNC_CLOSURE, iFUNC_ANNOTATIONS,
                                   iFUNC_KWDEFAULTS, iFUNC_DEFAULTS};
            /* the code object, and its qualified name where the code
               object hasn't one */
            const int named = !LE_HAS_CODE_QUALNAME;
            int i, n = 1 + named;
            for (i = 0; i < 4; i++)
                if (oparg & (0x08 >> i)) {
                    n++;
                    if (!psyco_forking(po, NTOP(n)->array))
                        break;
                }
            if (i < 4)
                break;
            if (named) {
                x = PsycoFunction_NewWithQualName(po, NTOP(2), LOC_GLOBALS,
                                                  NTOP(1));
                POP_DECREF();
            }
            else {
                /* NULL: PyFunction_NewWithQualName takes the code's */
                vinfo_t *none = psyco_vi_Zero();
                x = PsycoFunction_NewWithQualName(po, NTOP(1), LOC_GLOBALS,
                                                  none);
                vinfo_decref(none, po);
            }
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

        /* one of the fields MAKE_FUNCTION's flags name, set on the
           function it made */
        case SET_FUNCTION_ATTRIBUTE: {
            int which = oparg == 0x10   ? 4  /* __annotate__ */
                        : oparg == 0x08 ? 0  /* the closure */
                        : oparg == 0x04 ? 1  /* the annotations */
                        : oparg == 0x02 ? 2  /* the keyword defaults */
                                        : 3; /* the defaults */
            if (!PsycoFunction_SetField(po, TOP(), which, NTOP(2)))
                break;
            POP(x);
            POP_DECREF();
            PUSH(x);
            goto fine;
        }

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    return ended;
}

/* blocks, with statements and exceptions */
static int
compile_blocks_and_exceptions(PsycoObject *po, int opcode, int oparg,
                              int target, int *next_instr_io,
                              mergepoint_t **mp_io, int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    PyCodeObject *co = po->pr.co;
    vinfo_t *u, *v, *w, *x;
    condition_code_t cc;
    int next_instr = *next_instr_io;
    mergepoint_t *mp = *mp_io;
    int raise_lasti = *raise_lasti_io;
    int ended;

    switch (opcode) {
#if LE_HAS_EXCEPTION_TABLE
        /* the exception value on top raised again; with an
           argument, from the place under it, which a handler that
           wants one is given (raise_lasti) */
        case RERAISE:
            if (oparg) {
                v = NTOP(oparg + 1);
                if (is_offset_int(v))
                    raise_lasti = offset_int_value(v);
                else {
                    /* a frame resumed inside the handler holds the int
                       ceval or a pause left there (a tagged one is read as
                       the constant, stack_words_taken): its value is
                       promoted, one continuation per place raised from */
                    PyTypeObject *t = Psyco_NeedType(po, v);
                    long lasti;
                    if (t == NULL)
                        break;
                    if (!PyType_IsSubtype(t, &PyLong_Type)) {
                        PycException_SetString(po, PyExc_SystemError,
                                               "lasti is not an int");
                        break;
                    }
                    x = psyco_get_field(po, v, LONG_ob_digit0);
                    if (x == NULL)
                        break;
                    lasti = psyco_atcompiletime(po, x);
                    vinfo_decref(x, po);
                    if (lasti == -1 && PycException_Occurred(po))
                        break;
                    raise_lasti = (int)lasti;
                }
            }
            psyco_generic_call(po, cimpl_reraise_value,
                               CfPyErrAlways | CfNoPython, "v", TOP());
            /* the place of the instruction that first raised, or
               this one's */
            reraise_place(po, raise_lasti < 0 ? po->pr.next_instr
                              : LE_HAS_LASTI_IN_INSTRUCTIONS
                                  ? raise_lasti * LE_UNIT
                                  : raise_lasti);
            if (locals_wanted_now(po))
                traceback_locals(po, NULL);
            goto exception_unwind;

        /* async for's handler, for its await of __anext__(), with the
           exception above the async iterator: StopAsyncIteration ends the
           loop, any other is raised again */
        case END_ASYNC_FOR:
            x = psyco_generic_call(
                po, PyErr_GivenExceptionMatches,
                CfCommonIntBoolOrError | CfNoPython | CfNoBlock, "vl", TOP(),
                (long)PyExc_StopAsyncIteration);
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR)
                break;
            if (runtime_condition_f(po, cc)) {
                POP_DECREF(); /* the exception */
                POP_DECREF(); /* the async iterator */
                goto fine;
            }
            psyco_generic_call(po, cimpl_reraise_value,
                               CfPyErrAlways | CfNoPython, "v", TOP());
            reraise_place(po, po->pr.next_instr);
            if (locals_wanted_now(po))
                traceback_locals(po, NULL);
            goto exception_unwind;

#if LE_HAS_OPCODE_CLEANUP_THROW && LE_RESUMES_ON_FRAME
        /* where a throw into yield from goes, under the iterator and the
           value last sent: the StopIteration it ended with gives the
           loop's value (with a None under it), any other exception is
           raised again */
        case CLEANUP_THROW:
            x = psyco_generic_call(
                po, PyErr_GivenExceptionMatches,
                CfCommonIntBoolOrError | CfNoPython | CfNoBlock, "vl", TOP(),
                (long)PyExc_StopIteration);
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR)
                break;
            if (runtime_condition_f(po, cc)) {
                v = psyco_generic_call(po, cimpl_stop_iteration_value,
                                       CfCommonNewRefPyObject | CfNoPython,
                                       "v", TOP());
                if (v == NULL)
                    break;
                POP_DECREF(); /* the exception */
                POP_DECREF(); /* the value last sent */
                if (LE_HAS_GET_ITER_INDEX)
                    POP_DECREF(); /* the NULL, pushed again */
                POP_DECREF();     /* the iterator */
                PUSH(psyco_vi_None());
                if (LE_HAS_GET_ITER_INDEX)
                    PUSH(psyco_vi_Zero());
                PUSH(v);
                goto fine;
            }
            psyco_generic_call(po, cimpl_reraise_value,
                               CfPyErrAlways | CfNoPython, "v", TOP());
            reraise_place(po, po->pr.next_instr);
            if (locals_wanted_now(po))
                traceback_locals(po, NULL);
            goto exception_unwind;
#endif
#elif LE_HAS_OPCODE_RERAISE
        case RERAISE: {
            vinfo_t *exc;
            vinfo_t *tb;
            vinfo_t *val;
            exc = NTOP(1);
            val = NTOP(2);
            tb = NTOP(3);
            /* with oparg the place stays the first raise's, which its
               traceback entry set */
            if (!oparg)
                reraise_place(po, po->pr.next_instr);
            PycException_Reraise(po, exc, val, tb);
            goto exception_unwind;
        }
#endif

#if LE_HAS_OPCODE_JUMP_IF_NOT_EXC_MATCH
        case JUMP_IF_NOT_EXC_MATCH:
            if (!except_type_valid(po, TOP()))
                break;
            x = psyco_generic_call(
                po, PyErr_GivenExceptionMatches,
                CfCommonIntBoolOrError | CfNoPython | CfNoBlock, "vv", NTOP(2),
                TOP());
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            if (cc == CC_ERROR) {
                vinfo_decref(x, po);
                break;
            }
            vinfo_decref(x, po);
            x = psyco_vinfo_condition(po, cc);
            POP_DECREF();
            POP_DECREF();
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            JUMP_IF_CC(INVERT_CC(cc), JUMPTO(target), 0, 0);
            goto fine;
#endif

#if LE_HAS_OPCODE_LOAD_SPECIAL
        /* __exit__ and its self or NULL (LOAD_SPECIAL) are fifth and
           fourth, under the lasti and the exception saved by
           PUSH_EXC_INFO */
        case WITH_EXCEPT_START:
            x = psyco_generic_call(po, cimpl_with_exit_exc_self,
                                   CfCommonNewRefPyObject, "vvv", NTOP(5),
                                   NTOP(4), TOP());
            if (x == NULL)
                break;
            PUSH(x);
            goto fine;
#elif LE_HAS_EXCEPTION_TABLE
        /* __exit__ is fourth, under the lasti and the exception
           saved by PUSH_EXC_INFO */
        case WITH_EXCEPT_START:
            x = psyco_generic_call(po, cimpl_with_exit_exc,
                                   CfCommonNewRefPyObject, "vv", NTOP(4),
                                   TOP());
            if (x == NULL)
                break;
            PUSH(x);
            goto fine;
#elif LE_HAS_OPCODE_WITH_EXCEPT_START
        case WITH_EXCEPT_START:
            x = psyco_generic_call(po, cimpl_call_exit, CfCommonNewRefPyObject,
                                   "vvvv", NTOP(7), TOP(), NTOP(2), NTOP(3));
            if (x == NULL)
                break;
            PUSH(x);
            goto fine;
#endif

        case BREAK_LOOP:
            PycException_Raise(po, vinfo_new(VirtualTime_New(&EBreak)), NULL);
            break;

        case CONTINUE_LOOP:
            PycException_Raise(po, vinfo_new(VirtualTime_New(&EContinue)),
                               vinfo_new(CompileTime_New(oparg)));
            break;

        case RAISE_VARARGS:
            /* 3.x: 0 reraise, 1 raise exc, 2 raise exc from cause */
            v = w = x = psyco_vi_Zero(); /* NULL; one reference, shared */
            switch (oparg) {
                case 2:
                    v = NTOP(1); /* cause */
                    w = NTOP(2); /* exc */
                    psyco_generic_call(po, cimpl_do_raise, CfPyErrAlways, "vv",
                                       w, v);
                    break;
                case 1:
                    w = NTOP(1); /* exc */
                    psyco_generic_call(po, cimpl_do_raise, CfPyErrAlways, "vv",
                                       w, v);
                    break;
                case 0:
                    psyco_generic_call(
                        po, cimpl_reraise, CfPyErrAlways, "lvll", (long)co,
                        LOC_GLOBALS, (long)po->pr.next_instr,
                        (long)PyCode_Addr2Line(co, po->pr.next_instr));
                    break;
                default:
                    PycException_SetString(po, PyExc_SystemError,
                                           "bad RAISE_VARARGS oparg");
            }
            vinfo_decref(x, po);
            if (oparg == 0) /* cimpl_reraise recorded the traceback entry */
                goto exception_unwind;
            break;

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

#if LE_HAS_EXCEPTION_TABLE
        /* the handled exception PUSH_EXC_INFO saved is put back */
        case POP_EXCEPT: {
            vinfo_t *word = catcher_word(po);
            if (psyco_generic_call(po, cimpl_pop_exc_value, CfNoReturnValue,
                                   word != NULL ? "vr" : "vl", TOP(),
                                   (long)word) == NULL)
                break;
            POP_DECREF();
            goto fine;
        }

        /* the exception caught becomes the handled one; the one it
           replaces goes under it */
        case PUSH_EXC_INFO:
            x = psyco_generic_call(po, cimpl_push_exc_value,
                                   CfCommonNewRefPyObject | CfNoPython, "v",
                                   TOP());
            if (x == NULL)
                break;
            POP(v);
            PUSH(x);
            PUSH(v);
            goto fine;

        /* whether the exception under the class (or tuple) on top
           is one of them */
        case CHECK_EXC_MATCH:
            if (!except_type_valid(po, TOP()))
                break;
            x = psyco_generic_call(
                po, PyErr_GivenExceptionMatches,
                CfCommonIntBoolOrError | CfNoPython | CfNoBlock, "vv", NTOP(2),
                TOP());
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR)
                break;
            x = PsycoBool_FromCondition(po, cc);
            if (x == NULL)
                break;
            POP_DECREF();
            PUSH(x);
            goto fine;

#if LE_HAS_OPCODE_LOAD_SPECIAL
        /* with: the special method and its self over it, or the bound
           attribute and a NULL */
        case LOAD_SPECIAL: {
            vinfo_array_t *array = array_new(1);
            int unbound;
            u = psyco_generic_call(po, cimpl_load_special,
                                   CfCommonIntBoolOrError, "vlA", TOP(),
                                   (long)oparg, array);
            if (u == NULL) {
                array_release(array);
                break;
            }
            x = array->items[0];
            array_release(array);
            unbound = runtime_NON_NULL_t(po, u); /* consumes u */
            if (unbound < 0) {
                vinfo_decref(x, po);
                break;
            }
            POP(v);
            PUSH(x);
            if (unbound)
                PUSH(v);
            else {
                vinfo_decref(v, po);
                PUSH(psyco_vi_Zero());
            }
            goto fine;
        }
#endif
#if LE_HAS_OPCODE_BEFORE_WITH
        /* with: the bound __exit__ replaces the manager, and
           __enter__'s result goes over it */
        case BEFORE_WITH: {
            vinfo_array_t *array = array_new(2);
            if (!psyco_generic_call(po, cimpl_before_with, CfCommonIntZeroOk,
                                    "vA", TOP(), array)) {
                array_release(array);
                break;
            }
            POP_DECREF();
            PUSH(array->items[0]);
            PUSH(array->items[1]);
            array_release(array);
            goto fine;
        }
#endif
#else
        case POP_EXCEPT:
            /* from 3.8 the items above the handler's three stay (a
               return value under them, by ROT_FOUR) */
            if (!pop_except_handler(po, LE_HAS_POP_EXCEPT_UNWINDS))
                break;
            goto fine;
#endif

        /* Where the bytecode has them (3.8), a finally block is
           entered by BEGIN_FINALLY's NULL (LeJIT's 0), CALL_FINALLY's
           offset to return to, or an exception's six items;
           END_FINALLY and POP_FINALLY tell them apart. The offset is
           ceval's int object, a fixed constant (offset_int), so
           END_FINALLY's jump is made at compile time and a frame going
           on in ceval finds what ceval pushes. */
        case BEGIN_FINALLY:
            PUSH(psyco_vi_Zero());
            goto fine;

        case CALL_FINALLY:
            PUSH(offset_int(INSTR_OFFSET()));
            JUMPBY(LE_JUMP_BYTES(oparg));
            JUMPED();

        case POP_FINALLY: {
            vinfo_t *res = NULL;
            bool ok = true;
            if (oparg)
                POP(res);
            if (psyco_knowntobe(TOP(), 0) || is_offset_int(TOP()))
                POP_DECREF();
            else
                ok = pop_except_handler(po, true);
            if (oparg)
                PUSH(res);
            if (!ok)
                break;
            goto fine;
        }

#if !LE_HAS_EXCEPTION_TABLE
        /* async for's handler, for its await of __anext__(), entered
           with an exception's six items above the async iterator:
           StopAsyncIteration ends the loop, any other is raised again */
        case END_ASYNC_FOR:
            x = psyco_generic_call(
                po, PyErr_GivenExceptionMatches,
                CfCommonIntBoolOrError | CfNoPython | CfNoBlock, "vl", TOP(),
                (long)PyExc_StopAsyncIteration);
            if (x == NULL)
                break;
            cc = integer_non_null(po, x);
            vinfo_decref(x, po);
            if (cc == CC_ERROR)
                break;
            if (runtime_condition_f(po, cc)) {
                POP_DECREF(); /* the exception */
                if (!pop_except_handler(po, true))
                    break;
                POP_DECREF(); /* the async iterator */
                goto fine;
            }
            POP(v);
            POP(w);
            POP(x);
            reraise_place(po, po->pr.next_instr);
            PycException_Reraise(po, v, w, x);
            vinfo_decref(v, po);
            vinfo_decref(w, po);
            vinfo_decref(x, po);
            goto exception_unwind;
#endif

        case END_FINALLY:
            /* First make extra sure no exception is pending */
            if (PycException_Occurred(po))
                Py_FatalError(
                    "psyco: undetected exception "
                    "in END_FINALLY");
            POP(v);
            if (psyco_knowntobe(v, (long)Py_None) || psyco_knowntobe(v, 0)) {
                /* None (3.7) or NULL (3.8) on the stack: the end of a
                   finally block with no exception raised */
                vinfo_decref(v, po);
                goto fine;
            }
            if (is_offset_int(v)) {
                /* back to just after the CALL_FINALLY */
                JUMPTO(offset_int_value(v));
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
                reraise_place(po, po->pr.next_instr);
                PycException_Reraise(po, v, w, x);
                vinfo_decref(v, po);
                vinfo_decref(w, po);
                vinfo_decref(x, po);
                goto exception_unwind;
            }

        case SETUP_LOOP:
        case SETUP_EXCEPT:
        case SETUP_FINALLY:
            block_setup(po, opcode, INSTR_OFFSET() + LE_JUMP_BYTES(oparg),
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
            if (!psyco_generic_call(po, cimpl_setup_with, CfCommonIntZeroOk,
                                    "vA", TOP(), array)) {
                array_release(array);
                break;
            }
            POP_DECREF();
            PUSH(array->items[0]);
            block_setup(po, SETUP_FINALLY,
                        INSTR_OFFSET() + LE_JUMP_BYTES(oparg), STACK_LEVEL());
            PUSH(array->items[1]);
            array_release(array);
            goto fine;
        }

        /* async with: as SETUP_WITH, in two steps around the await of
           __aenter__'s result, whose result the block goes under */
        case BEFORE_ASYNC_WITH: {
            vinfo_array_t *array = array_new(2);
            if (!psyco_generic_call(po,
                                    LE_HAS_EXCEPTION_TABLE
                                        ? cimpl_before_awith
                                        : cimpl_before_async_with,
                                    CfCommonIntZeroOk, "vA", TOP(), array)) {
                array_release(array);
                break;
            }
            POP_DECREF();
            PUSH(array->items[0]);
            PUSH(array->items[1]);
            array_release(array);
            goto fine;
        }

        case SETUP_ASYNC_WITH:
            POP(v);
            block_setup(po, SETUP_FINALLY,
                        INSTR_OFFSET() + LE_JUMP_BYTES(oparg), STACK_LEVEL());
            PUSH(v);
            goto fine;

        case WITH_CLEANUP_START: {
            le_block_t *b = po->pr.iblock > 0
                                ? &po->pr.blockstack[(int)po->pr.iblock - 1]
                                : NULL;
            vinfo_t **sp = STACK_POINTER();
            if (b != NULL && b->b_type == EXCEPT_HANDLER &&
                STACK_LEVEL() == b->b_level + 6) {
                /* an exception: exit(type, value, tb), then ceval's
                   shift of the saved exception down over __exit__'s
                   slot, with NULL where the type was */
                x = psyco_generic_call(po, cimpl_call_exit,
                                       CfCommonNewRefPyObject, "vvvv", sp[-7],
                                       sp[-1], sp[-2], sp[-3]);
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
            x = psyco_generic_call(po, cimpl_call_exit, CfCommonNewRefPyObject,
                                   "vlll", NTOP(2), (long)Py_None,
                                   (long)Py_None, (long)Py_None);
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
                cc = integer_NON_NULL(po, PsycoObject_IsTrue(po, TOP()));
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
                    PUSH(LE_HAS_OPCODE_BEGIN_FINALLY ? psyco_vi_Zero()
                                                     : psyco_vi_None());
                    goto fine;
                }
            }
            POP_DECREF();
            POP_DECREF();
            goto fine;

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
    goto out;
exception_unwind:
    ended = OPCODE_RERAISED;
out:
    *next_instr_io = next_instr;
    *mp_io = mp;
    *raise_lasti_io = raise_lasti;
    return ended;
}

/* imports */
static int
compile_imports(PsycoObject *po, int opcode, int oparg, int target,
                int *next_instr_io, mergepoint_t **mp_io, int *raise_lasti_io)
{
    vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
    PyCodeObject *co = po->pr.co;
    vinfo_t *u, *v, *x;
    int next_instr = *next_instr_io;
    int ended;

    switch (opcode) {
        case IMPORT_FROM:
            v = vinfo_new(CompileTime_New((long)GETNAMEV(oparg)));
            x = psyco_generic_call(
                po, cimpl_import_from, CfCommonNewRefPyObject, "vvll", TOP(),
                v, (long)po->pr.co,
                (long)le_lasti_of(
                    le_place(le_code(po->pr.co), po->pr.next_instr)));
            vinfo_decref(v, po);
            if (x == NULL)
                break;
            PUSH(x);
            goto fine;

        case IMPORT_NAME: {
            PyObject *w = GETNAMEV(LE_IMPORT_NAME(oparg));
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

        default:
            return OPCODE_ELSEWHERE;
    } /* switch (opcode) */
    ended = OPCODE_ENDED;
    goto out;
fine:
    ended = OPCODE_FINE;
out:
    *next_instr_io = next_instr;
    return ended;
}

/* whether a pieces list (plistobject.c) may live on into an instruction:
   one that runs no Python code and changes no list itself, or that forces
   it where it would (codegen.c:psyco_generic_call, plist_ass_item) */
static int
keeps_pieces(int opcode)
{
    switch (opcode) {
        case LOAD_FAST:
        case LOAD_FAST_CHECK:
        case LOAD_FAST_LOAD_FAST:
        case LOAD_CONST:
        case LOAD_SMALL_INT:
        case DUP_TOP:
        case DUP_TOP_TWO:
        case ROT_TWO:
        case ROT_THREE:
        case ROT_FOUR:
        case COPY:
        case SWAP:
        case NOP:
        case BUILD_SLICE:
        case BINARY_SLICE:
        case BINARY_SUBSCR:
        case BINARY_ADD:
        case BINARY_SUBTRACT:
        case UNARY_NEGATIVE:
        case STORE_SUBSCR:
        case STORE_SLICE:
            return 1;
    }
    return 0;
}

/* the families, in the order ceval.c has their cases */
static int (*const compile_families[])(PsycoObject *po, int opcode, int oparg,
                                       int target, int *next_instr_io,
                                       mergepoint_t **mp_io,
                                       int *raise_lasti_io) = {
    compile_locals_and_stack,
    compile_operators,
    compile_compares,
    compile_attributes_and_globals,
    compile_build_and_unpack,
    compile_jumps,
    compile_iteration,
    compile_calls,
    compile_functions_and_returns,
    compile_blocks_and_exceptions,
    compile_imports,
};
#define COMPILE_FAMILIES \
    ((int)(sizeof(compile_families) / sizeof(compile_families[0])))

/* the case for opcode, from the family that has it */
static int
compile_opcode(PsycoObject *po, int opcode, int oparg, int target,
               int *next_instr_io, mergepoint_t **mp_io, int *raise_lasti_io)
{
    /* each opcode's family, plus one, once a family has compiled it */
    static unsigned char family_of[512];
    int i, ended;
    if (opcode >= 0 && opcode < (int)sizeof(family_of) && family_of[opcode])
        return compile_families[family_of[opcode] - 1](
            po, opcode, oparg, target, next_instr_io, mp_io, raise_lasti_io);
    for (i = 0; i < COMPILE_FAMILIES; i++) {
        ended = compile_families[i](po, opcode, oparg, target, next_instr_io,
                                    mp_io, raise_lasti_io);
        if (ended != OPCODE_ELSEWHERE) {
            if (opcode >= 0 && opcode < (int)sizeof(family_of))
                family_of[opcode] = (unsigned char)(i + 1);
            return ended;
        }
    }
    return OPCODE_ELSEWHERE;
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
    le_instr_t in;  /* the instruction, as bytecode.h:le_instr reads it */
    code_t *code1;
    int saved_rec_limit;
    int unwound = 0;  /* the frame went on interpreted (resume_if_tracing) */
    int reraised = 0; /* exception_unwind: an exception raised again */
    /* the place, as ceval's lasti, of an exception RERAISE raises again
       from the place under it; else -1 */
    int raise_lasti = -1;
    int resumed = 0;
    int ended; /* what the instruction's case left to do */
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
           up to the next merge point, every back-edge a new copy of the loop.
           A replay checks it too, as its original compile did: making dead
           virtual locals real there can emit a respawn point */
        POP_DECREF();
        po->pr.next_instr &= ~NEXT_INSTR_POP;
        at_jump_target = 1;
    }

    while (1) {
        /* 'co' is the code object we are interpreting/compiling */
        PyCodeObject *co = po->pr.co;
        le_code_t bytecode = le_code(co);
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
#if LE_RESUMES_ON_FRAME
            if (!stack_words_taken(po))
                break;
#endif
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
            in = le_instr(bytecode, next_instr);
            opcode = in.op;
            oparg = in.arg;
            next_instr = in.next;
            /* at loop heads and after calls */
            if (psyco_breaker_check_at(po->pr.merge_points,
                                       po->pr.next_instr) &&
                !psyco_check_eval_breaker(po))
                break;
            po->blocking_call = 0;
            po->pr.python_call = 0;
            po->pr.proxy_write = 0;

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

            /* Every opcode compiled here is LE_OPF_COMPILED in
               bytecode.h:le_op_flags; code with any other runs in ceval. */

            if (opcode == BINARY_SUBSCR || opcode == CALL_METHOD ||
                opcode == CALL || opcode == STORE_SUBSCR) {
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

            ended = compile_opcode(po, opcode, oparg, in.target, &next_instr,
                                   &mp, &raise_lasti);
            if (ended == OPCODE_ELSEWHERE) {
                fprintf(stderr, "XXX opcode: %d\n", opcode);
                Py_FatalError("unknown opcode");
            }
            if (ended == OPCODE_FINE)
                goto fine;
            if (ended == OPCODE_RERAISED)
                /* an exception raised again (ceval's label): this
                   instruction adds no traceback entry */
                reraised = 1;
            /* Exit if an error occurred */
            if (PycException_Occurred(po))
                break;

        fine:
            extra_assert(!PycException_Occurred(po));
            if ((po->pr.python_call && ((le_op_flags(opcode) & LE_OPF_CALL) ||
                                        po->pr.proxy_write)) ||
                po->pr.resume_pending) {
                po->pr.resume_pending = 0;
                if (!resume_if_tracing(po, next_instr, &unwound))
                    break;
            }
            /* no pieces list lives on where code could change its sources,
               nor into a snapshot */
            if (po->pr.pieces &&
                (next_instr >= bytecode.size ||
                 !keeps_pieces(le_instr(bytecode, next_instr).op) ||
                 mp->bytecode_position == next_instr ||
                 po->pr.split_after_global ||
                 (po->codelimit - po->code) < BUFFER_MARGIN) &&
                !PsycoList_ForcePieces(po, NULL))
                break;
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
                SAVE_NEXT_INSTR(next_instr);
                /* the last place a replay can stop in this block */
                if (!psyco_make_dead_virtuals_real(po, mp))
                    break;
                extra_assert(!is_respawning(po));
                /* simplify the po->vlocals array */
                if (!psyco_limit_nested_weight(po, &po->vlocals, NWI_NORMAL,
                                               NESTED_WEIGHT_END))
                    break;
                psyco_forget_dead_vars(po, &mp->entries, true);
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
            resumed = 0; /* the caller's handlers are still to run */

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

#if LE_HAS_EXCEPTION_TABLE
        /* the handler the exception table gives for the instruction that
           raised, as ceval's exception_unwind: the stack cut to the
           handler's depth, the place of the instruction pushed if the
           handler wants it, then the exception caught (cimpl_catch). A
           virtual exception is raised for real first, so that the run-time
           code can fetch it. Pseudo-exceptions have no handler, and an
           exception from a frame ceval went on with has been through its
           handlers there. */
        if (PycException_IsPython(po) && !resumed) {
            le_handler_t h;
            int lasti = raise_lasti >= 0
                            ? raise_lasti
                            : le_lasti_of(le_place(le_code(po->pr.co),
                                                   po->pr.next_instr));
            raise_lasti = -1;
            if (le_handler_find(po->pr.co, po->pr.next_instr, &h)) {
                int next_instr;
                vinfo_t *word = catcher_word(po), *own, *v;
                if (!compute_and_raise_exception(po))
                    break;
                clear_pseudo_exception(po);
                while (STACK_LEVEL() > h.depth) POP_DECREF();
                if (h.lasti)
                    PUSH(offset_int(lasti));
                if (word != NULL) {
                    own = own_slot();
                    v = psyco_generic_call(po, cimpl_catch,
                                           CfCommonNewRefPyObject, "rr", word,
                                           own);
                    vinfo_decref(own, po);
                }
                else
                    v = psyco_generic_call(
                        po, cimpl_catch, CfCommonNewRefPyObject, "ll", 0L, 0L);
                if (v == NULL)
                    break;
                PUSH(v);
                JUMPTO(h.handler);
                SAVE_NEXT_INSTR(next_instr);
                /* a callee that raised may have started tracing */
                po->pr.resume_pending = 1;
            }
        }
#endif

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
                /* leaving an except clause by any (pseudo) exception; a
                   virtual one is raised first, while the exception handled
                   here is still its context */
                if (PycException_IsPython(po) &&
                    !PycException_Is(po, &ERtPython)) {
                    if (!compute_and_raise_exception(po))
                        break;
                    clear_pseudo_exception(po);
                    PycException_Raise(
                        po, vinfo_new(VirtualTime_New(&ERtPython)), NULL);
                }
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
                /* a coroutine's handler that can await runs in ceval,
                   which suspends the coroutine in it with its handled
                   exception (mergepoints.c:handler_yields_kept) */
                if (psyco_handler_awaits(po->pr.co, handler)) {
                    extra_assert(psyco_can_go_interpreted(po));
                    po->pr.go_interpreted = 1;
                }
                else
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
