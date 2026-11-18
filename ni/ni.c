#include "Python/frames.h"
#include "Python/pycompiler.h"
#include "Objects/pmethodobject.h"
#include "Objects/pobject.h"
#include "codemanager.h"
#include "compat2to3.h"
#include "dispatcher.h"
#include "mergepoints.h"
#include "psyco.h"
#include "psyfunc.h"
#include "ni_buildinfo_gen.h"

#define PSYCO_INITIALIZATION
#include "initialize.h"

static PyObject *thread_dict_key;

DEFINEFN
PyObject *psyco_thread_dict() {
  PyObject *dict = PyThreadState_GetDict();
  PyObject *result;
  bool err;

  if (dict == NULL)
    return NULL;
  result = PyDict_GetItem(dict, thread_dict_key);
  if (result == NULL) {
    result = PyDict_New();
    if (result == NULL)
      return NULL;
    err = PyDict_SetItem(dict, thread_dict_key, result);
    Py_DECREF(result); /* one reference left in 'dict' */
    if (err)
      result = NULL;
  }
  return result;
}

DEFINEFN
void psyco_out_of_memory(char *filename, int lineno) {
  char *msg;
  if (psyco_compile_guard != NULL)
    psyco_guard_abandon();
  if (PyErr_Occurred()) {
    PyErr_Print();
    msg = "psyco cannot recover from the error above";
  } else
    msg = "psyco: out of memory";
  fprintf(stderr, "%s:%d: ", filename, lineno);
  Py_FatalError(msg);
}

#if NI_TRACE
/**************************************************************
 * Ni Debug Trace hooks
 *************************************************************/
DEFINEFN
void ni_trace_begin_simple_code(code_t *code) {}

DEFINEFN
void ni_trace_end_simple_code(code_t *code) {}

DEFINEFN
void ni_trace_begin_code(PsycoObject *po) {}

DEFINEFN
void ni_trace_end_code(PsycoObject *po) {}

DEFINEFN
void ni_trace_jump(code_t *location, code_t *target) {}

DEFINEFN
void ni_trace_jump_update(code_t *location, code_t *target) {}

DEFINEFN
void ni_trace_jump_reg(code_t *location, int reg_target) {}

DEFINEFN
void ni_trace_jump_cond(code_t *location, code_t *not_taken, code_t *taken) {}

DEFINEFN
void ni_trace_jump_cond_update(code_t *location, code_t *not_taken,
                               code_t *taken) {}

DEFINEFN
void ni_trace_jump_cond_reg(code_t *location, code_t *not_taken,
                            int reg_taken) {}

DEFINEFN
void ni_trace_call(code_t *location, code_t *call_target) {}

DEFINEFN
void ni_trace_call_reg(code_t *location, int reg_call_target) {}

DEFINEFN
void ni_trace_return(code_t *location, int stack_adjust) {}

DEFINEFN
void ni_trace_unsupported_opcode(PyCodeObject *co, int bytecode_index) {}

DEFINEFN
void ni_trace_run_fail(PyCodeObject *co, char *reason) {}

DEFINEFN
void ni_trace_unify(PsycoObject *po, CodeBufferObject *match) {}
#endif
DEFINEVAR PyObject *PyExc_PsycoError;
DEFINEVAR PyObject *CPsycoModule;

/* A record hangs off its code object as a weak reference of Ni's own
   type rather than in co_extra: code_dealloc calls the co_extra free
   functions of whichever interpreter is current, and on 3.7 a
   subinterpreter's table is never initialised (PyInterpreterState_New),
   so main-interpreter garbage that a subinterpreter's collection frees
   would call junk. A weak reference's callback runs in any interpreter.
   The record lives inside the reference, which the GC doesn't track; Ni
   holds one reference to it while it is attached (or being freed), and
   its owner releases the compiled code (codemanager.c). It is visible
   only to weakref.getweakrefs on the code object. */
typedef struct {
  PyWeakReference wr;
  char attached;
  ni_code_extra_t x;
} code_record_t;

static PyTypeObject CodeRecord_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "ni.code_record",
    .tp_basicsize = sizeof(code_record_t),
    .tp_flags = Py_TPFLAGS_DEFAULT,
};
static PyObject *record_died_fn = NULL;

#define RECORD_OF(px)                                                          \
  ((code_record_t *)((char *)(px)-offsetof(code_record_t, x)))

DEFINEFN void ni_code_extra_free(ni_code_extra_t *x) {
  psyco_owner_release(&x->owner, x->merge_points, x->entry_point);
  psyco_owner_untrack(x); /* after: releasing can keep into the owner */
  Py_DECREF(RECORD_OF(x));
}

/* the weak reference's callback; anyone can fetch it from a live
   reference (__callback__) and call it */
static PyObject *record_died(PyObject *self, PyObject *ref) {
  code_record_t *r = (code_record_t *)ref;
  if (Py_TYPE(ref) == &CodeRecord_Type && r->attached &&
      PyWeakref_GET_OBJECT(ref) == Py_None) {
    r->attached = 0;
    ni_code_extra_free(&r->x);
  }
  Py_RETURN_NONE;
}

static PyMethodDef record_died_def = {"ni_record_died", record_died, METH_O,
                                      NULL};

DEFINEFN
ni_code_extra_t *ni_code_extra_get(PyCodeObject *co) {
  PyWeakReference *r = (PyWeakReference *)co->co_weakreflist;
  for (; r != NULL; r = r->wr_next)
    if (Py_TYPE(r) == &CodeRecord_Type)
      return &((code_record_t *)r)->x;
  return NULL;
}

/* unlinked without its callback; freeing it is the caller's */
DEFINEFN
ni_code_extra_t *ni_code_extra_detach(PyCodeObject *co) {
  ni_code_extra_t *x = ni_code_extra_get(co);
  if (x != NULL) {
    RECORD_OF(x)->attached = 0;
    _PyWeakref_ClearRef(&RECORD_OF(x)->wr);
  }
  return x;
}

DEFINEFN
ni_code_extra_t *ni_code_extra(PyCodeObject *co) {
  static long next_serial = 0;
  ni_code_extra_t *x = ni_code_extra_get(co);
  PyObject *args;
  code_record_t *r;
  if (x != NULL)
    return x;
  args = PyTuple_Pack(2, (PyObject *)co, record_died_fn);
  if (args == NULL)
    return NULL;
  /* zeroed, and tracked by the GC, which has nothing to find in it */
  r = (code_record_t *)_PyWeakref_RefType.tp_new(&CodeRecord_Type, args, NULL);
  Py_DECREF(args);
  if (r == NULL)
    return NULL;
  PyObject_GC_UnTrack(r);
  r->attached = 1;
  r->x.serial = ++next_serial;
  return &r->x;
}

static int code_records_init(void) {
  CodeRecord_Type.tp_base = &_PyWeakref_RefType;
  if (PyType_Ready(&CodeRecord_Type) < 0)
    return -1;
  record_died_fn = PyCFunction_New(&record_died_def, NULL);
  return record_died_fn == NULL ? -1 : 0;
}

DEFINEFN
int psyco_fatal_error(char *msg, char *filename, int lineno) {
  fprintf(stderr, "\n%s:%d: %s\n", filename, lineno, msg);
  Py_FatalError("Psyco assertion failed");
  return 0;
}

const static char MODULE_NAME[] = "ni";

/* counters for tests and measurements, e.g. that a loop is not compiled
   afresh on every back-edge */
#if NI_CODESTATS
EXTERNVAR PyObject *psyco_codestats;
EXTERNVAR long psyco_codestats_lost;
EXTERNVAR PyObject *psyco_codestats_buffers;
EXTERNVAR PyObject *psyco_codestats_states;
/* the code size counts (pycompiler.c), handed over and reset */
static PyObject *ni_codestats(PyObject *self, PyObject *unused) {
  PyObject *r = psyco_codestats ? psyco_codestats : PyDict_New();
  PyObject *bufs = psyco_codestats_buffers ? psyco_codestats_buffers
                                            : PyDict_New();
  PyObject *states = psyco_codestats_states ? psyco_codestats_states
                                              : PyList_New(0);
  PyObject *t = Py_BuildValue("(NlNN)", r, psyco_codestats_lost, bufs, states);
  psyco_codestats_states = NULL;
  psyco_codestats = NULL;
  psyco_codestats_buffers = NULL;
  psyco_codestats_lost = 0;
  return t;
}
EXTERNFN PyObject *psyco_cs_promo_stats(void); /* pycompiler.c */
static PyObject *ni_promostats(PyObject *self, PyObject *unused) {
  return psyco_cs_promo_stats();
}
#endif

static PyObject *ni_stats(PyObject *self, PyObject *unused) {
  static const char *kinds[NI_ENTER_KINDS] = {"analyse", "call", "pause",
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
  for (i = 0; d != NULL && i < NI_ENTER_KINDS; i++) {
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
    PyObject *t = Py_BuildValue("(llll)", psyco_stat_joins[0],
                                psyco_stat_joins[1], psyco_stat_joins[2],
                                psyco_stat_joins[3]);
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
    /* compiler states kept for pauses and promotions (dispatcher.h) */
    PyObject *t = Py_BuildValue("(ll)", psyco_stat_parked_live,
                                psyco_stat_parked_frozen);
    if (t == NULL || PyDict_SetItemString(d, "parked", t) < 0)
      Py_CLEAR(d);
    Py_XDECREF(t);
  }
  if (d != NULL) {
    /* cycle scans run, the owners they freed, and the records counted
       now (codemanager.c) */
    PyObject *t = Py_BuildValue("(lll)", psyco_stat_owner_scans,
                                psyco_stat_owners_collected,
                                psyco_stat_owners_counted);
    if (t == NULL || PyDict_SetItemString(d, "owner_scans", t) < 0)
      Py_CLEAR(d);
    Py_XDECREF(t);
  }
#if NI_CODESTATS
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

/* for tests and tools: what the eval hook did with a code object */
static PyObject *ni_code_info(PyObject *self, PyObject *co) {
  ni_code_extra_t *e;
  if (!PyCode_Check(co)) {
    PyErr_SetString(PyExc_TypeError, "_code_info() needs a code object");
    return NULL;
  }
  e = ni_code_extra_get((PyCodeObject *)co);
  if (e == NULL || e->merge_points == NULL)
    return PyUnicode_FromString("not seen");
  if (e->merge_points == Py_None)
    return PyUnicode_FromString("unsupported");
  if (e->entry_point == NULL || e->entry_point == Py_None)
    return PyUnicode_FromString("no entry point");
  return PyUnicode_FromString("compiled");
}

/* for tests: {bytecode offset: [dead local names]} at each merge
   point, or None if the code isn't compiled */
static PyObject *ni_dead_locals(PyObject *self, PyObject *co) {
  PyObject *mps, *result;
  mergepoint_t *mp;
  if (!PyCode_Check(co)) {
    PyErr_SetString(PyExc_TypeError, "_dead_locals() needs a code object");
    return NULL;
  }
  mps = psyco_get_merge_points((PyCodeObject *)co, 0);
  if (mps == Py_None)
    Py_RETURN_NONE;
  result = PyDict_New();
  if (result == NULL)
    return NULL;
  for (mp = psyco_first_merge_point(mps); mp->bytecode_position >= 0; mp++) {
    PyObject *names = PyList_New(0), *key;
    Py_ssize_t i;
    if (names == NULL)
      goto fail;
    for (i = 0; i < PyList_GET_SIZE(mp->entries.fatlist); i++) {
      PyObject *o = PyList_GET_ITEM(mp->entries.fatlist, i);
      if (!PyLong_Check(o))
        break;
      if (PyList_Append(names, PyTuple_GET_ITEM(((PyCodeObject *)co)->co_varnames,
                                                PyLong_AS_LONG(o))) < 0) {
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
static PyObject *ni_inlinable(PyObject *self, PyObject *co) {
  PyObject *mps;
  if (!PyCode_Check(co)) {
    PyErr_SetString(PyExc_TypeError, "_inlinable() needs a code object");
    return NULL;
  }
  mps = psyco_get_merge_points((PyCodeObject *)co, 0);
  return PyBool_FromLong(mps != Py_None &&
                         (psyco_mp_flags(mps) & MP_FLAGS_INLINABLE) != 0);
}

/* for debugging (doc/BUILD.md): (codebuf, address of the globals) the eval
   hook runs; the address, since the globals are borrowed and may be dead */
static PyObject *ni_py_entry_point(PyObject *self, PyObject *co) {
  ni_code_extra_t *x;
  PyObject *e;
  if (!PyCode_Check(co)) {
    PyErr_SetString(PyExc_TypeError, "_entry_point() needs a code object");
    return NULL;
  }
  x = ni_code_extra_get((PyCodeObject *)co);
  e = x ? x->entry_point : NULL;
  if (e == NULL || e == Py_None)
    Py_RETURN_NONE;
  return Py_BuildValue("(On)", e,
                       (Py_ssize_t)x->entry_globals);
}

/* tools/interpreted.py profiles compiled code's callers, so it turns off
   the fallback to the interpreter while tracing (eval_hook.c) */
static PyObject *ni_set_trace_fallback(PyObject *self, PyObject *on) {
  int r = PyObject_IsTrue(on);
  if (r < 0)
    return NULL;
  ni_trace_fallback = r;
  Py_RETURN_NONE;
}

/* for tests: the metas Psyco_DefineModuleFn couldn't register */
static PyObject *ni_unregistered_metas(PyObject *self, PyObject *unused) {
  if (Psyco_Unregistered_Metas == NULL)
    return PyList_New(0);
  return PyList_GetSlice(Psyco_Unregistered_Metas, 0, PY_SSIZE_T_MAX);
}

#if ALL_CHECKS
/* for tests: _fail_after(site, n) makes the n-th allocation at 'site' from
   now fail (psyco.h:NI_FAIL); 0 cancels. Returns what was left of the
   previous count, 0 once it fired. */
static PyObject *ni_fail_after(PyObject *self, PyObject *args) {
  static const char *names[NI_FAIL_SITES] = {"psycoobject", "array",
                                             "codebuf", "lookup", "owner"};
  const char *site;
  long n, left;
  int i;
  if (!PyArg_ParseTuple(args, "sl", &site, &n))
    return NULL;
  for (i = 0; i < NI_FAIL_SITES; i++)
    if (strcmp(site, names[i]) == 0) {
      left = psyco_fail_after[i];
      psyco_fail_after[i] = n;
      return PyLong_FromLong(left);
    }
  PyErr_SetString(PyExc_ValueError, "unknown site");
  return NULL;
}
#endif

static PyMethodDef module_methods[] = {
#if ALL_CHECKS
    {"_fail_after", ni_fail_after, METH_VARARGS,
     "Make the n-th allocation at a site fail (debug builds)."},
#endif
    {"_stats", ni_stats, METH_NOARGS, "Internal counters."},
#if NI_CODESTATS
    {"_codestats", ni_codestats, METH_NOARGS, "Code size per instruction."},
    {"_promostats", ni_promostats, METH_NOARGS, "Promotion sites."},
#endif
    {"_code_info", ni_code_info, METH_O,
     "'not seen', 'unsupported', 'no entry point' or 'compiled'."},
    {"_dead_locals", ni_dead_locals, METH_O,
     "{offset: [dead local names]} at each merge point, or None."},
    {"_inlinable", ni_inlinable, METH_O,
     "True if calls to this code object are inlined."},
    {"_unregistered_metas", ni_unregistered_metas, METH_NOARGS,
     "['module.name: why'] for each meta that wasn't registered."},
    {"_set_trace_fallback", ni_set_trace_fallback, METH_O,
     "Whether code runs interpreted while sys.settrace/setprofile is set."},
    {"_entry_point", ni_py_entry_point, METH_O,
     "(codebuf, globals) run for a code object, or None."},
    {NULL, NULL, 0, NULL}};

/* ni.build_info: what this build was generated from and how it was built
   (tools/metac/buildinfo.py), read-only; nothing checks it */
static int add_str(PyObject *d, const char *key, const char *value) {
  PyObject *v = value ? PyUnicode_FromString(value) : (Py_INCREF(Py_None), Py_None);
  int err = v == NULL || PyDict_SetItemString(d, key, v) < 0;
  Py_XDECREF(v);
  return err ? -1 : 0;
}

static int add_obj(PyObject *d, const char *key, PyObject *v) {
  int err = v == NULL || PyDict_SetItemString(d, key, v) < 0;
  Py_XDECREF(v);
  return err ? -1 : 0;
}

static PyObject *build_info(void) {
  PyObject *d = PyDict_New(), *features = PyDict_New();
  if (d == NULL || features == NULL)
    goto error;
#define NI_FEATURE(name)                                                       \
  if (add_obj(features, #name, PyLong_FromLong(name)) < 0)                     \
    goto error;
  NI_BUILDINFO_FEATURES(NI_FEATURE)
#undef NI_FEATURE
#ifdef NI_SANITIZE
  const char *sanitizer = NI_SANITIZE;
#else
  const char *sanitizer = NULL;
#endif
  if (add_str(d, "source_version", NI_BUILDINFO_SOURCE_VERSION) < 0 ||
      add_str(d, "source", NI_BUILDINFO_SOURCE) < 0 ||
      add_str(d, "source_sha256", NI_BUILDINFO_SOURCE_SHA256[0] ? NI_BUILDINFO_SOURCE_SHA256 : NULL) < 0 ||
      add_str(d, "source_git_commit", NI_BUILDINFO_SOURCE_GIT_COMMIT[0] ? NI_BUILDINFO_SOURCE_GIT_COMMIT : NULL) < 0 ||
      add_str(d, "python", NI_BUILDINFO_PYTHON) < 0 ||
      add_str(d, "generator_python", NI_BUILDINFO_GENERATOR_PYTHON) < 0 ||
      add_str(d, "ni_commit", NI_BUILDINFO_NI_COMMIT) < 0 ||
      add_str(d, "build", ALL_CHECKS ? "checked" : "release") < 0 ||
      add_obj(d, "trace", PyBool_FromLong(NI_TRACE)) < 0 ||
      add_obj(d, "codestats", PyBool_FromLong(NI_CODESTATS)) < 0 ||
      add_str(d, "sanitizer", sanitizer) < 0 ||
      add_obj(d, "features", PyDictProxy_New(features)) < 0)
    goto error;
  Py_DECREF(features);
  PyObject *proxy = PyDictProxy_New(d);
  Py_DECREF(d);
  return proxy;
error:
  Py_XDECREF(features);
  Py_XDECREF(d);
  return NULL;
}

static struct PyModuleDef ni_module = {PyModuleDef_HEAD_INIT, MODULE_NAME, NULL,
                                       -1, module_methods};

PyMODINIT_FUNC PyInit_ni(void) {
  PyObject *module;
  thread_dict_key = NiCompatStr_InternFromString("PsycoT");
  if (thread_dict_key == NULL) {
    return NULL;
  }
  module = PyModule_Create(&ni_module);
  /* XXX tony: make this code handle errors properly */
  if (module) {
    if (code_records_init() < 0) {
      Py_DECREF(module);
      return NULL;
    }
    PyExc_PsycoError = PyErr_NewException("ni.error", NULL, NULL);
    if (PyExc_PsycoError == NULL) {
      Py_DECREF(module);
      return NULL;
    }
    if (PyModule_AddObject(module, "error", PyExc_PsycoError)) {
      Py_DECREF(PyExc_PsycoError);
      Py_DECREF(module);
      return NULL;
    }
    /* so that timings can tell a debug build (tools/check.sh's) */
    if (PyModule_AddIntConstant(module, "_debug_build", NI_TRACE || ALL_CHECKS)) {
      Py_DECREF(module);
      return NULL;
    }
    if (PyModule_AddObject(module, "build_info", build_info())) {
      Py_DECREF(module);
      return NULL;
    }
    initialize_all_files();
  }
  CPsycoModule = module;
  return module;
}
