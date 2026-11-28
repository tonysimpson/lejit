/* The private functions of lejit._core that tests use: the compiler's
   counters and what it made of a code object. Only checked builds
   (LEJIT_DEV_TRACE=1) have them, and the code size counts only codestats
   builds (LEJIT_DEV_CODESTATS=1). lejit.c adds them to the module. */
#include "Python/genrecord.h"
#include "Python/frames.h"
#include "Python/pycompiler.h"
#include "Objects/pmethodobject.h"
#include "Objects/pobject.h"
#include "codemanager.h"
#include "dispatcher.h"
#include "mergepoints.h"
#include "psyco.h"
#include "psyfunc.h"

EXTERNVAR int le_set_up; /* lejit.c */

/* counters for tests and measurements, e.g. that a loop is not compiled
   afresh on every back-edge */
#if LE_CODESTATS
EXTERNVAR PyObject *psyco_codestats;
EXTERNVAR long psyco_codestats_lost;
EXTERNVAR PyObject *psyco_codestats_buffers;
EXTERNVAR PyObject *psyco_codestats_states;
/* the code size counts (pycompiler.c), handed over and reset */
static PyObject *
le_codestats(PyObject *self, PyObject *unused)
{
    PyObject *r = psyco_codestats ? psyco_codestats : PyDict_New();
    PyObject *bufs =
        psyco_codestats_buffers ? psyco_codestats_buffers : PyDict_New();
    PyObject *states =
        psyco_codestats_states ? psyco_codestats_states : PyList_New(0);
    PyObject *t =
        Py_BuildValue("(NlNN)", r, psyco_codestats_lost, bufs, states);
    psyco_codestats_states = NULL;
    psyco_codestats = NULL;
    psyco_codestats_buffers = NULL;
    psyco_codestats_lost = 0;
    return t;
}
EXTERNFN PyObject *
psyco_cs_promo_stats(void); /* pycompiler.c */
static PyObject *
le_promostats(PyObject *self, PyObject *unused)
{
    return psyco_cs_promo_stats();
}
#endif

#if ALL_CHECKS
/* calls to these functions while allocations are counted
   (le_allocs_begin): a case that asks them in its rounds does work a run
   without LeJIT doesn't */
static long allocs_asked;

static PyObject *
le_stats(PyObject *self, PyObject *unused)
{
    static const char *kinds[LE_ENTER_KINDS] = {"analyse", "call", "pause",
                                                "respawn", "promotion"};
    PyObject *d = Py_BuildValue(
        "{slslslslslslslslslsl}", "code_buffers", psyco_stat_code_buffers,
        "code_buffers_freed", psyco_stat_code_buffers_freed, "code_bytes",
        psyco_stat_code_bytes, "cfunc_fast", psyco_stat_cfunc_fast,
        "cfunc_proxy", psyco_stat_cfunc_proxy, "store_misses",
        psyco_stat_store_misses, "store_refreshes", psyco_stat_store_refreshes,
        "store_type_changes", psyco_stat_store_type_changes,
        "load_type_changes", psyco_stat_load_type_changes, "load_repairs",
        psyco_stat_load_repairs);
    int i;
    allocs_asked++;
    for (i = 0; d != NULL && i < LE_ENTER_KINDS; i++) {
        PyObject *t = Py_BuildValue("(ld)", psyco_stat_compile_count[i],
                                    psyco_stat_compile_ns[i] * 1e-9);
        if (t == NULL || PyDict_SetItemString(d, kinds[i], t) < 0) {
            Py_XDECREF(t);
            Py_CLEAR(d);
            break;
        }
        Py_DECREF(t);
    }
    if (d != NULL) {
        PyObject *t =
            Py_BuildValue("(llll)", psyco_stat_joins[0], psyco_stat_joins[1],
                          psyco_stat_joins[2], psyco_stat_joins[3]);
        if (t == NULL || PyDict_SetItemString(d, "joins", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        PyObject *t = PyLong_FromLong(psyco_stat_fact_drops);
        if (t == NULL || PyDict_SetItemString(d, "fact_drops", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        /* machine code not freed yet, and references never to be dropped */
        PyObject *t = Py_BuildValue("(ll)", psyco_stat_code_bytes_live,
                                    psyco_stat_kept_for_good);
        if (t == NULL || PyDict_SetItemString(d, "code_live", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        /* virtual generators' records (Python/genrecord.c) */
        PyObject *t = Py_BuildValue(
            "{slslslslslsl}", "made", le_stat_gen_records[0], "escaped",
            le_stat_gen_records[1], "dropped_paused", le_stat_gen_records[2],
            "running", le_stat_gen_records[3], "ended", le_stat_gen_records[4],
            "closed_unrun", le_stat_gen_records[5]);
        if (t == NULL || PyDict_SetItemString(d, "gen_records", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        /* code moved after its compile, code that stayed, and relocations
           kept (codemanager.c:psyco_code_move_made) */
        PyObject *t =
            Py_BuildValue("(lll)", psyco_stat_code_moved[0],
                          psyco_stat_code_moved[1], psyco_stat_code_moved[2]);
        if (t == NULL || PyDict_SetItemString(d, "code_moved", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        /* call-site notes' map indices made, and free for reuse (codegen.c) */
        PyObject *t = Py_BuildValue("(ll)", le_notes_made, le_notes_free);
        if (t == NULL || PyDict_SetItemString(d, "call_notes", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        /* compiler states kept for pauses and promotions (dispatcher.h) */
        PyObject *t =
            Py_BuildValue("(ll)", le_parked_live, psyco_stat_parked_frozen);
        if (t == NULL || PyDict_SetItemString(d, "parked", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
#if LE_HAS_MONITORING
    if (d != NULL) {
        /* compiled code's exact tracing tests (pyruntime.c) */
        extern long le_stat_tracing_rechecks;
        PyObject *t = PyLong_FromLong(le_stat_tracing_rechecks);
        if (t == NULL || PyDict_SetItemString(d, "tracing_rechecks", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
#endif
    if (d != NULL) {
        /* frame objects made (frames.c:le_pyframe_new,
           mechanisms/frames.h:le_frame_object) */
        extern long le_stat_frame_objects;
        PyObject *t = PyLong_FromLong(le_stat_frame_objects);
        if (t == NULL || PyDict_SetItemString(d, "frame_objects", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        /* failed guards of cached globals (pycompiler.c:do_changed_global) */
        PyObject *t = PyLong_FromLong(psyco_stat_global_fails);
        if (t == NULL || PyDict_SetItemString(d, "global_fails", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        /* cycle scans run, the owners they freed, and the records counted
           now (codemanager.c) */
        PyObject *t =
            Py_BuildValue("(lll)", psyco_stat_owner_scans,
                          psyco_stat_owners_collected, le_owners_counted);
        if (t == NULL || PyDict_SetItemString(d, "owner_scans", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        /* sweeps run, the code objects whose compiled code they released,
           and its bytes (codemanager.c) */
        PyObject *t = Py_BuildValue("(lll)", le_stat_sweeps[0],
                                    le_stat_sweeps[1], le_stat_sweeps[2]);
        if (t == NULL || PyDict_SetItemString(d, "sweeps", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
    if (d != NULL) {
        /* promotion groups made and freed (codemanager.h, le_group_t) */
        PyObject *t = Py_BuildValue("(ll)", psyco_stat_groups_made,
                                    psyco_stat_groups_freed);
        if (t == NULL || PyDict_SetItemString(d, "groups", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
#if !defined(__SANITIZE_ADDRESS__)
    if (d != NULL) {
        /* vinfos and source_knowns in use: made, less those on the free list
           (blockalloc.h; not counted under ASan, where each is malloc'd) */
        long nv = psyco_ll_items_vinfo, ns = psyco_ll_items_sk;
        void **p;
        for (p = psyco_linked_list_vinfo; p != NULL; p = (void **)*p) nv--;
        for (p = psyco_linked_list_sk; p != NULL; p = (void **)*p) ns--;
        PyObject *t = Py_BuildValue("(ll)", nv, ns);
        if (t == NULL || PyDict_SetItemString(d, "vinfos", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
#endif
#if LE_CODESTATS
    if (d != NULL) {
        long *c = psyco_cs_load;
        PyObject *t = Py_BuildValue(
            "(llllllll)", c[0], psyco_stat_load_type_changes,
            psyco_stat_load_repairs, c[3], c[4], c[5], c[6], c[7]);
        if (t == NULL || PyDict_SetItemString(d, "load_guards", t) < 0)
            Py_CLEAR(d);
        Py_XDECREF(t);
    }
#endif
    return d;
}

/* the analysis functions would run code that isn't set up before the
   first start, so they refuse */
static int
le_not_set_up(const char *fn)
{
    if (le_set_up)
        return 0;
    PyErr_Format(PyExc_RuntimeError, "%s(): lejit.start() has not been called",
                 fn);
    return 1;
}

/* for tests and tools: what the eval hook did with a code object */
static PyObject *
le_code_info(PyObject *self, PyObject *co)
{
    le_code_extra_t *e;
    allocs_asked++;
    if (!PyCode_Check(co)) {
        PyErr_SetString(PyExc_TypeError, "_code_info() needs a code object");
        return NULL;
    }
    e = le_code_extra_get((PyCodeObject *)co);
    if (e == NULL || e->merge_points == NULL)
        return PyUnicode_FromString("not seen");
    if (e->merge_points == Py_None)
        return PyUnicode_FromString("unsupported");
    if (e->entry_point == NULL || e->entry_point == Py_None) {
        /* a resumable generator's body: an entry at its start or a yield */
        int i;
        for (i = 0; i < e->nresumes; i++)
            if (e->resumes[i].code != NULL && e->resumes[i].code != Py_None)
                return PyUnicode_FromString("compiled");
        return PyUnicode_FromString("no entry point");
    }
    return PyUnicode_FromString("compiled");
}

/* for tests: {bytecode offset: [dead local names]} at each merge
   point, or None if the code isn't compiled */
static PyObject *
le_dead_locals(PyObject *self, PyObject *co)
{
    PyObject *mps, *result;
    mergepoint_t *mp;
    allocs_asked++;
    if (!PyCode_Check(co)) {
        PyErr_SetString(PyExc_TypeError, "_dead_locals() needs a code object");
        return NULL;
    }
    if (le_not_set_up("_dead_locals"))
        return NULL;
    mps = psyco_get_merge_points((PyCodeObject *)co);
    if (mps == Py_None)
        Py_RETURN_NONE;
    result = PyDict_New();
    if (result == NULL)
        return NULL;
    for (mp = psyco_first_merge_point(mps); mp->bytecode_position >= 0; mp++) {
        PyObject *names = PyList_New(0), *key;
        int i, ndead;
        const int *dead = mp_dead(mp, &ndead);
        if (names == NULL)
            goto fail;
        for (i = 0; i < ndead; i++) {
            if (PyList_Append(names, le_code_local_name(((PyCodeObject *)co),
                                                        dead[i])) < 0) {
                Py_DECREF(names);
                goto fail;
            }
        }
        key = PyLong_FromLong(mp->bytecode_position);
        if (key == NULL || PyDict_SetItem(result, key, names) < 0) {
            Py_XDECREF(key);
            Py_DECREF(names);
            goto fail;
        }
        Py_DECREF(key);
        Py_DECREF(names);
    }
    return result;
fail:
    Py_DECREF(result);
    return NULL;
}

/* for tests: would calls to this code be inlined? */
static PyObject *
le_inlinable(PyObject *self, PyObject *co)
{
    PyObject *mps;
    allocs_asked++;
    if (!PyCode_Check(co)) {
        PyErr_SetString(PyExc_TypeError, "_inlinable() needs a code object");
        return NULL;
    }
    if (le_not_set_up("_inlinable"))
        return NULL;
    mps = psyco_get_merge_points((PyCodeObject *)co);
    return PyBool_FromLong(mps != Py_None &&
                           (psyco_mp_flags(mps) & MP_FLAGS_INLINABLE) != 0);
}

/* for tests: the metas Psyco_DefineModuleFn couldn't register */
static PyObject *
le_unregistered_metas(PyObject *self, PyObject *unused)
{
    allocs_asked++;
    if (Psyco_Unregistered_Metas == NULL)
        return PyList_New(0);
    return PyList_GetSlice(Psyco_Unregistered_Metas, 0, PY_SSIZE_T_MAX);
}

/* for tests: _fail_after(site, n) makes the n-th allocation at 'site' from
   now fail (psyco.h:LE_FAIL); 0 cancels. Returns what was left of the
   previous count, 0 once it fired. */
static PyObject *
le_fail_after(PyObject *self, PyObject *args)
{
    static const char *names[LE_FAIL_SITES] = {
        "psycoobject", "array", "codebuf", "lookup", "owner", "traceback"};
    const char *site;
    long n, left;
    int i;
    allocs_asked++;
    if (!PyArg_ParseTuple(args, "sl", &site, &n))
        return NULL;
    for (i = 0; i < LE_FAIL_SITES; i++)
        if (strcmp(site, names[i]) == 0) {
            left = psyco_fail_after[i];
            psyco_fail_after[i] = n;
            return PyLong_FromLong(left);
        }
    PyErr_SetString(PyExc_ValueError, "unknown site");
    return NULL;
}

/* For tests/jit: counts the object allocator's allocations (PyObject_Malloc,
   _Calloc, and _Realloc of NULL) made outside compiles, from
   le_allocs_begin to le_allocs_end. They are plain C functions, called
   through ctypes, so a run without LeJIT counts the same way without
   importing lejit. */
static PyMemAllocatorEx allocs_under;
static long allocs_made;
static bool allocs_on = false;

static void *
allocs_malloc(void *ctx, size_t size)
{
    allocs_made += le_compile_depth == 0;
    return allocs_under.malloc(allocs_under.ctx, size);
}

static void *
allocs_calloc(void *ctx, size_t nelem, size_t elsize)
{
    allocs_made += le_compile_depth == 0;
    return allocs_under.calloc(allocs_under.ctx, nelem, elsize);
}

static void *
allocs_realloc(void *ctx, void *ptr, size_t size)
{
    allocs_made += ptr == NULL && le_compile_depth == 0;
    return allocs_under.realloc(allocs_under.ctx, ptr, size);
}

static void
allocs_free(void *ctx, void *ptr)
{
    allocs_under.free(allocs_under.ctx, ptr);
}

EXTERNFN void
le_allocs_begin(void);
EXTERNFN long
le_allocs_end(void);
EXTERNFN long
le_allocs_asked(void);

DEFINEFN void
le_allocs_begin(void)
{
    PyMemAllocatorEx hook = {NULL, allocs_malloc, allocs_calloc,
                             allocs_realloc, allocs_free};
    allocs_made = allocs_asked = 0;
    if (allocs_on)
        return;
    PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &allocs_under);
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &hook);
    allocs_on = true;
}

/* the count, or -1 if another hook was put over this one meanwhile (a
   tracemalloc.start() not stopped): this one then stays, under it */
DEFINEFN long
le_allocs_end(void)
{
    PyMemAllocatorEx now;
    if (!allocs_on)
        return -1;
    PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &now);
    if (now.malloc != allocs_malloc)
        return -1;
    PyMem_SetAllocator(PYMEM_DOMAIN_OBJ, &allocs_under);
    allocs_on = false;
    return allocs_made;
}

/* the calls to lejit._core's test functions since le_allocs_begin */
DEFINEFN long
le_allocs_asked(void)
{
    return allocs_asked;
}
#endif

#if ALL_CHECKS
/* f(*args) run with no real frame (eval_hook.c:le_run_frameless), for a
   function whose call can be: positional arguments, exactly as many as it
   takes, no cells or free variables, not a generator's */
static PyObject *
le_run_frameless_test(PyObject *self, PyObject *const *args, Py_ssize_t n)
{
    PyFunctionObject *func;
    PyCodeObject *co;
    PyObject *entry;
    allocs_asked++;
    if (n < 1 || !PyFunction_Check(args[0])) {
        PyErr_SetString(PyExc_TypeError, "_run_frameless() needs a function");
        return NULL;
    }
    func = (PyFunctionObject *)args[0];
    co = (PyCodeObject *)func->func_code;
    if (co->co_argcount + co->co_kwonlyargcount != n - 1 ||
        (co->co_flags & (CO_VARARGS | CO_VARKEYWORDS | CO_GENERATOR |
                         CO_COROUTINE | CO_ASYNC_GENERATOR)) != 0 ||
        le_code_ncells(co) != 0 || le_code_nfrees(co) != 0) {
        PyErr_SetString(PyExc_ValueError,
                        "_run_frameless() can't run this call");
        return NULL;
    }
    if ((entry = le_function_entry(func)) == NULL) {
        if (!PyErr_Occurred())
            PyErr_SetString(PyExc_ValueError, "no compiled entry");
        return NULL;
    }
    return le_run_frameless(entry, func, args + 1, n - 1);
}
#endif

DEFINEVAR PyMethodDef le_testing_methods[] = {
#if LE_CODESTATS
    {"_codestats", le_codestats, METH_NOARGS, "Code size per instruction."},
    {"_promostats", le_promostats, METH_NOARGS, "Promotion sites."},
#endif
#if ALL_CHECKS
    {"_fail_after", le_fail_after, METH_VARARGS,
     "Make the n-th allocation at a site fail."},
    {"_stats", le_stats, METH_NOARGS, "Internal counters."},
    {"_run_frameless", (PyCFunction)(void (*)(void))le_run_frameless_test,
     METH_FASTCALL, "f(*args), run with no real frame."},
    {"_code_info", le_code_info, METH_O,
     "'not seen', 'unsupported', 'no entry point' or 'compiled'."},
    {"_dead_locals", le_dead_locals, METH_O,
     "{offset: [dead local names]} at each merge point, or None."},
    {"_inlinable", le_inlinable, METH_O,
     "True if calls to this code object are inlined."},
    {"_unregistered_metas", le_unregistered_metas, METH_NOARGS,
     "['module.name: why'] for each meta that wasn't registered."},
#endif
    {NULL, NULL, 0, NULL}};
