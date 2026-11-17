#include "Python/frames.h"
#include "Python/pycompiler.h"
#include "codemanager.h"
#include "compat2to3.h"
#include "dispatcher.h"
#include "mergepoints.h"
#include "psyco.h"
#include "psyfunc.h"

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

/* A record dies with its code object, releasing its code buffers. Compiled
   code holds its globals (a compile-time constant), so a code object whose
   only globals are its own (exec into a new dict, namedtuple) still never
   dies (KNOWN_BUGS C14). */
static Py_ssize_t code_extra_index = -1;

static void code_extra_free(void *p) {
  ni_code_extra_t *x = (ni_code_extra_t *)p;
  Py_XDECREF(x->merge_points);
  Py_XDECREF(x->entry_point);
  PyMem_Free(x);
}

DEFINEFN
ni_code_extra_t *ni_code_extra(PyCodeObject *co) {
  void *x;
  if (_PyCode_GetExtra((PyObject *)co, code_extra_index, &x) < 0)
    return NULL;
  if (x == NULL) {
    x = PyMem_Calloc(1, sizeof(ni_code_extra_t));
    if (x == NULL) {
      PyErr_NoMemory();
      return NULL;
    }
    if (_PyCode_SetExtra((PyObject *)co, code_extra_index, x) < 0) {
      PyMem_Free(x);
      return NULL;
    }
  }
  return (ni_code_extra_t *)x;
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
#endif

static PyObject *ni_stats(PyObject *self, PyObject *unused) {
  static const char *kinds[NI_ENTER_KINDS] = {"analyse", "call", "pause",
                                              "respawn", "promotion"};
  PyObject *d = Py_BuildValue("{slslsl}", "code_buffers",
                              psyco_stat_code_buffers, "code_buffers_freed",
                              psyco_stat_code_buffers_freed, "code_bytes",
                              psyco_stat_code_bytes);
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
  return d;
}

/* for tests and tools: what the eval hook did with a code object */
static PyObject *ni_code_info(PyObject *self, PyObject *co) {
  void *x;
  ni_code_extra_t *e;
  if (!PyCode_Check(co)) {
    PyErr_SetString(PyExc_TypeError, "_code_info() needs a code object");
    return NULL;
  }
  if (_PyCode_GetExtra(co, code_extra_index, &x) < 0)
    return NULL;
  e = (ni_code_extra_t *)x;
  if (e == NULL || e->merge_points == NULL)
    return PyUnicode_FromString("not seen");
  if (e->merge_points == Py_None)
    return PyUnicode_FromString("unsupported");
  if (e->entry_point == NULL || e->entry_point == Py_None)
    return PyUnicode_FromString("no entry point");
  return PyUnicode_FromString("compiled");
}

/* for tests (D15): {bytecode offset: [dead local names]} at each merge
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

/* for tests (B6): would calls to this code be inlined? */
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
static PyObject *ni_entry_point(PyObject *self, PyObject *co) {
  void *x;
  PyObject *e;
  if (!PyCode_Check(co)) {
    PyErr_SetString(PyExc_TypeError, "_entry_point() needs a code object");
    return NULL;
  }
  if (_PyCode_GetExtra(co, code_extra_index, &x) < 0)
    return NULL;
  e = x ? ((ni_code_extra_t *)x)->entry_point : NULL;
  if (e == NULL || e == Py_None)
    Py_RETURN_NONE;
  return Py_BuildValue("(On)", e,
                       (Py_ssize_t)((ni_code_extra_t *)x)->entry_globals);
}

static PyMethodDef module_methods[] = {
    {"_stats", ni_stats, METH_NOARGS, "Internal counters."},
#if NI_CODESTATS
    {"_codestats", ni_codestats, METH_NOARGS, "Code size per instruction."},
#endif
    {"_code_info", ni_code_info, METH_O,
     "'not seen', 'unsupported', 'no entry point' or 'compiled'."},
    {"_dead_locals", ni_dead_locals, METH_O,
     "{offset: [dead local names]} at each merge point, or None."},
    {"_inlinable", ni_inlinable, METH_O,
     "True if calls to this code object are inlined."},
    {"_entry_point", ni_entry_point, METH_O,
     "(codebuf, globals) run for a code object, or None."},
    {NULL, NULL, 0, NULL}};

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
    code_extra_index = _PyEval_RequestCodeExtraIndex(code_extra_free);
    if (code_extra_index < 0) {
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
    initialize_all_files();
  }
  CPsycoModule = module;
  return module;
}
