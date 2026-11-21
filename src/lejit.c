#include "Python/frames.h"
#include "Python/pycompiler.h"
#include "Objects/pmethodobject.h"
#include "Objects/pobject.h"
#include "codemanager.h"
#include "dispatcher.h"
#include "mergepoints.h"
#include "psyco.h"
#include "psyfunc.h"
#include "le_features_gen.h"

#define PSYCO_INITIALIZATION
#include "initialize.h"

DEFINEFN
void
psyco_out_of_memory(char *filename, int lineno)
{
    char *msg;
    if (psyco_compile_guard != NULL)
        psyco_guard_abandon();
    if (PyErr_Occurred()) {
        PyErr_Print();
        msg = "psyco cannot recover from the error above";
    }
    else
        msg = "psyco: out of memory";
    fprintf(stderr, "%s:%d: ", filename, lineno);
    Py_FatalError(msg);
}

#if LE_TRACE
/**************************************************************
 * LeJIT Debug Trace hooks
 *************************************************************/
DEFINEFN
void
le_trace_begin_simple_code(code_t *code)
{
}

DEFINEFN
void
le_trace_end_simple_code(code_t *code)
{
}

DEFINEFN
void
le_trace_begin_code(PsycoObject *po)
{
}

DEFINEFN
void
le_trace_end_code(PsycoObject *po)
{
}

DEFINEFN
void
le_trace_jump(code_t *location, code_t *target)
{
}

DEFINEFN
void
le_trace_jump_update(code_t *location, code_t *target)
{
}

DEFINEFN
void
le_trace_jump_reg(code_t *location, int reg_target)
{
}

DEFINEFN
void
le_trace_jump_cond(code_t *location, code_t *not_taken, code_t *taken)
{
}

DEFINEFN
void
le_trace_jump_cond_update(code_t *location, code_t *not_taken, code_t *taken)
{
}

DEFINEFN
void
le_trace_jump_cond_reg(code_t *location, code_t *not_taken, int reg_taken)
{
}

DEFINEFN
void
le_trace_call(code_t *location, code_t *call_target)
{
}

DEFINEFN
void
le_trace_call_reg(code_t *location, int reg_call_target)
{
}

DEFINEFN
void
le_trace_return(code_t *location, int stack_adjust)
{
}

DEFINEFN
void
le_trace_unsupported_opcode(PyCodeObject *co, int bytecode_index)
{
}

DEFINEFN
void
le_trace_run_fail(PyCodeObject *co, char *reason)
{
}

DEFINEFN
void
le_trace_unify(PsycoObject *po, CodeBufferObject *match)
{
}
#endif

/* A record hangs off its code object as a weak reference of LeJIT's own
   type rather than in co_extra: code_dealloc calls the co_extra free
   functions of whichever interpreter is current, and on 3.7 a
   subinterpreter's table is never initialised (PyInterpreterState_New),
   so main-interpreter garbage that a subinterpreter's collection frees
   would call junk. A weak reference's callback runs in any interpreter.
   The record lives inside the reference, which the GC doesn't track; LeJIT
   holds one reference to it while it is attached (or being freed), and
   its owner releases the compiled code (codemanager.c). It is visible
   only to weakref.getweakrefs on the code object. */
typedef struct {
    PyWeakReference wr;
    char attached;
    le_code_extra_t x;
} code_record_t;

static PyTypeObject CodeRecord_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "lejit._core.code_record",
    .tp_basicsize = sizeof(code_record_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
};
static PyObject *record_died_fn = NULL;

#define RECORD_OF(px) \
    ((code_record_t *)((char *)(px) - offsetof(code_record_t, x)))

DEFINEFN void
le_code_extra_free(le_code_extra_t *x)
{
    psyco_owner_release(&x->owner, x->merge_points, x->entry_point);
    psyco_owner_untrack(x); /* after: releasing can keep into the owner */
    Py_DECREF(RECORD_OF(x));
}

static int
weakref_is_dead(PyObject *ref)
{
#ifdef PyWeakref_GET_OBJECT
    return PyWeakref_GET_OBJECT(ref) == Py_None;
#else
    PyObject *obj;
    if (PyWeakref_GetRef(ref, &obj) <= 0)
        return 1;
    Py_DECREF(obj);
    return 0;
#endif
}

/* the weak reference's callback; anyone can fetch it from a live
   reference (__callback__) and call it */
static PyObject *
record_died(PyObject *self, PyObject *ref)
{
    code_record_t *r = (code_record_t *)ref;
    if (Py_TYPE(ref) == &CodeRecord_Type && r->attached &&
        weakref_is_dead(ref)) {
        r->attached = 0;
        le_code_extra_free(&r->x);
    }
    Py_RETURN_NONE;
}

static PyMethodDef record_died_def = {"le_record_died", record_died, METH_O,
                                      NULL};

DEFINEFN
le_code_extra_t *
le_code_extra_get(PyCodeObject *co)
{
    PyWeakReference *r = (PyWeakReference *)co->co_weakreflist;
    for (; r != NULL; r = r->wr_next)
        if (Py_TYPE(r) == &CodeRecord_Type)
            return &((code_record_t *)r)->x;
    return NULL;
}

/* unlinked without its callback; freeing it is the caller's */
DEFINEFN
le_code_extra_t *
le_code_extra_detach(PyCodeObject *co)
{
    le_code_extra_t *x = le_code_extra_get(co);
    if (x != NULL) {
        RECORD_OF(x)->attached = 0;
        _PyWeakref_ClearRef(&RECORD_OF(x)->wr);
    }
    return x;
}

DEFINEFN
le_code_extra_t *
le_code_extra(PyCodeObject *co)
{
    static long next_serial = 0;
    le_code_extra_t *x = le_code_extra_get(co);
    PyObject *args;
    code_record_t *r;
    if (x != NULL)
        return x;
    args = PyTuple_Pack(2, (PyObject *)co, record_died_fn);
    if (args == NULL)
        return NULL;
    /* zeroed, and tracked by the GC, which has nothing to find in it */
    r = (code_record_t *)_PyWeakref_RefType.tp_new(&CodeRecord_Type, args,
                                                   NULL);
    Py_DECREF(args);
    if (r == NULL)
        return NULL;
    PyObject_GC_UnTrack(r);
    r->attached = 1;
    r->x.serial = ++next_serial;
    return &r->x;
}

static int
code_records_init(void)
{
    CodeRecord_Type.tp_base = &_PyWeakref_RefType;
    if (PyType_Ready(&CodeRecord_Type) < 0)
        return -1;
    record_died_fn = PyCFunction_New(&record_died_def, NULL);
    return record_died_fn == NULL ? -1 : 0;
}

DEFINEFN
int
psyco_fatal_error(char *msg, char *filename, int lineno)
{
    fprintf(stderr, "\n%s:%d: %s\n", filename, lineno, msg);
    Py_FatalError("Psyco assertion failed");
    return 0;
}

static const char MODULE_NAME[] = "lejit._core";

/* Importing lejit._core sets nothing up and hooks nothing: the first _start
   sets up the compiler, and _start and _stop put in or take out the eval
   hook; each does nothing if that is done already. The API is in Python
   (lejit/__init__.py). */
DEFINEVAR int le_set_up = 0; /* testing.c reads it */
static int le_running = 0;

static PyObject *
le_start(PyObject *self, PyObject *unused)
{
    if (!le_set_up) {
        initialize_all_files();
        le_set_up = 1;
    }
    if (!le_running) {
        le_entry_install();
        le_running = 1;
    }
    Py_RETURN_NONE;
}

static PyObject *
le_stop(PyObject *self, PyObject *unused)
{
    if (le_running) {
        if (le_entry_remove() < 0) {
            PyErr_SetString(
                PyExc_RuntimeError,
                "lejit.stop(): another frame evaluator was installed"
                " over LeJIT's, so it can't be taken out");
            return NULL;
        }
        le_running = 0;
    }
    Py_RETURN_NONE;
}

/* The structs psyco.h copies from CPython's source, because no installed
   header declares them, can't be checked against the headers when LeJIT is
   built. lejit._core._build['layouts'] has the size of each copy; _layouts()
   what the running interpreter shows of it: the same size where the fields
   of one it makes read through the copy as they should, or what is wrong.
   lejit.runtime_features calls it in a subprocess. */
static int
add_layout(PyObject *d, const char *name, Py_ssize_t size, int fields_ok)
{
    PyObject *v = fields_ok ? PyLong_FromSsize_t(size)
                            : PyUnicode_FromFormat(
                                  "not as LeJIT's copy (size %zd)", size);
    int err = v == NULL || PyDict_SetItemString(d, name, v) < 0;
    Py_XDECREF(v);
    return err ? -1 : 0;
}

static PyObject *
le_layouts(PyObject *self, PyObject *unused)
{
    PyObject *result = PyDict_New(), *list = PyList_New(0), *d = PyDict_New();
    PyObject *it = NULL, *r = NULL, *rit = NULL, *one = NULL;
    int ok;
    if (result == NULL || list == NULL || d == NULL)
        goto error;

    /* Objects/iterobject.c's seqiterobject */
    it = PySeqIter_New(list);
    if (it == NULL)
        goto error;
    ok = PySeqIter_Type.tp_basicsize == sizeof(seqiterobject) &&
         ((seqiterobject *)it)->it_seq == list &&
         ((seqiterobject *)it)->it_index == 0;
    if (add_layout(result, "seqiterobject", PySeqIter_Type.tp_basicsize, ok) <
        0)
        goto error;

    /* Objects/rangeobject.c's rangeobject and rangeiterobject, of range(1, 7,
       2): 3 items */
    r = PyObject_CallFunction((PyObject *)&PyRange_Type, "iii", 1, 7, 2);
    rit = r ? PyObject_GetIter(r) : NULL;
    if (rit == NULL)
        goto error;
    ok = PyRange_Type.tp_basicsize == sizeof(rangeobject);
    if (ok) {
        rangeobject *ro = (rangeobject *)r;
        PyObject *fields[4] = {ro->start, ro->stop, ro->step, ro->length};
        long want[4] = {1, 7, 2, 3};
        int i;
        for (i = 0; ok && i < 4; i++)
            ok = PyLong_CheckExact(fields[i]) &&
                 PyLong_AsLong(fields[i]) == want[i];
    }
    if (add_layout(result, "rangeobject", PyRange_Type.tp_basicsize, ok) < 0)
        goto error;
    ok = Py_TYPE(rit) == &PyRangeIter_Type &&
         PyRangeIter_Type.tp_basicsize == sizeof(rangeiterobject) &&
         ((rangeiterobject *)rit)->index == 0 &&
         ((rangeiterobject *)rit)->start == 1 &&
         ((rangeiterobject *)rit)->step == 2 &&
         ((rangeiterobject *)rit)->len == 3;
    if (add_layout(result, "rangeiterobject", PyRangeIter_Type.tp_basicsize,
                   ok) < 0)
        goto error;

    /* Objects/dict-common.h's keys and entries: a dict of one item has keys
       of its own, whose size _PyDict_SizeOf counts from the real structs */
    one = PyLong_FromLong(1);
    if (one == NULL || PyDict_SetItem(d, one, one) < 0)
        goto error;
    {
        le_dictkeys_t *k = (le_dictkeys_t *)((PyDictObject *)d)->ma_keys;
        Py_ssize_t size = k->dk_size, usable = size * 2 / 3;
        Py_ssize_t whole = _PyDict_SizeOf((PyDictObject *)d), header = 0;
        ok = k->dk_refcnt == 1 && size >= 8 && (size & (size - 1)) == 0 &&
             size <= 0xff && k->dk_nentries == 1 &&
             k->dk_usable == usable - 1 && k->dk_lookup != NULL;
        if (ok) {
            le_dictentry_t *e = le_dict_entries(k);
            /* what the interpreter's own keys header takes, past the index
               bytes and the entries */
            header = whole - (Py_ssize_t)sizeof(PyDictObject) - size -
                     (Py_ssize_t)sizeof(le_dictentry_t) * usable;
            ok = e[0].me_key == one && e[0].me_value == one &&
                 e[0].me_hash == 1;
        }
        if (add_layout(result, "PyDictKeysObject", ok ? header : whole, ok) <
            0)
            goto error;
    }
    Py_DECREF(one);
    Py_DECREF(rit);
    Py_DECREF(r);
    Py_DECREF(it);
    Py_DECREF(list);
    Py_DECREF(d);
    return result;
error:
    Py_XDECREF(one);
    Py_XDECREF(rit);
    Py_XDECREF(r);
    Py_XDECREF(it);
    Py_XDECREF(list);
    Py_XDECREF(d);
    Py_XDECREF(result);
    return NULL;
}

static PyMethodDef module_methods[] = {
    {"_start", le_start, METH_NOARGS, "Put in the eval hook (lejit.start)."},
    {"_stop", le_stop, METH_NOARGS, "Take out the eval hook (lejit.stop)."},
    {"_layouts", le_layouts, METH_NOARGS,
     "{struct: size} of the copied structs, as this interpreter shows them."},
    {NULL, NULL, 0, NULL}};

/* lejit._core._build, for lejit.build_features and the tests: the CPython
   source the features were detected in, the features, messages and copied
   structs' sizes compiled in, and how LeJIT was built */
static int
add_str(PyObject *d, const char *key, const char *value)
{
    PyObject *v =
        value ? PyUnicode_FromString(value) : (Py_INCREF(Py_None), Py_None);
    int err = v == NULL || PyDict_SetItemString(d, key, v) < 0;
    Py_XDECREF(v);
    return err ? -1 : 0;
}

static int
add_obj(PyObject *d, const char *key, PyObject *v)
{
    int err = v == NULL || PyDict_SetItemString(d, key, v) < 0;
    Py_XDECREF(v);
    return err ? -1 : 0;
}

static PyObject *
build_dict(void)
{
    PyObject *d = PyDict_New(), *features = PyDict_New(),
             *messages = PyDict_New(), *layouts = PyDict_New();
    if (d == NULL || features == NULL || messages == NULL || layouts == NULL)
        goto error;
#define LE_FEATURE(name)                                     \
    if (add_obj(features, #name, PyLong_FromLong(name)) < 0) \
        goto error;
    LE_FEATURES(LE_FEATURE)
#undef LE_FEATURE
#define LE_MESSAGE(name)                    \
    if (add_str(messages, #name, name) < 0) \
        goto error;
    LE_MESSAGES(LE_MESSAGE)
#undef LE_MESSAGE
    if (add_layout(layouts, "seqiterobject", sizeof(seqiterobject), 1) < 0 ||
        add_layout(layouts, "rangeobject", sizeof(rangeobject), 1) < 0 ||
        add_layout(layouts, "rangeiterobject", sizeof(rangeiterobject), 1) <
            0 ||
        add_layout(layouts, "PyDictKeysObject", sizeof(le_dictkeys_t), 1) < 0)
        goto error;
#ifdef LE_SANITIZE
    const char *sanitizer = LE_SANITIZE;
#else
    const char *sanitizer = NULL;
#endif
    if (add_str(d, "source_version", LE_FEATURES_SOURCE_VERSION) < 0 ||
        add_str(d, "build", ALL_CHECKS ? "checked" : "release") < 0 ||
        add_obj(d, "trace", PyBool_FromLong(LE_TRACE)) < 0 ||
        add_obj(d, "codestats", PyBool_FromLong(LE_CODESTATS)) < 0 ||
        add_str(d, "sanitizer", sanitizer) < 0 ||
        PyDict_SetItemString(d, "features", features) < 0 ||
        PyDict_SetItemString(d, "messages", messages) < 0 ||
        PyDict_SetItemString(d, "layouts", layouts) < 0)
        goto error;
    Py_DECREF(features);
    Py_DECREF(messages);
    Py_DECREF(layouts);
    return d;
error:
    Py_XDECREF(features);
    Py_XDECREF(messages);
    Py_XDECREF(layouts);
    Py_XDECREF(d);
    return NULL;
}

/* the private functions for tests (testing.c) */
EXTERNVAR PyMethodDef le_testing_methods[];

static struct PyModuleDef le_module = {PyModuleDef_HEAD_INIT, MODULE_NAME,
                                       NULL, -1, module_methods};

/* Importing lejit._core starts nothing: lejit.start calls _start. */
PyMODINIT_FUNC
PyInit__core(void)
{
    PyObject *module = PyModule_Create(&le_module), *build;
    if (module == NULL)
        return NULL;
    if (code_records_init() < 0)
        goto error;
    if (PyModule_AddFunctions(module, le_testing_methods) < 0)
        goto error;
    build = build_dict();
    if (build == NULL || PyModule_AddObject(module, "_build", build)) {
        Py_XDECREF(build);
        goto error;
    }
    return module;
error:
    Py_DECREF(module);
    return NULL;
}
