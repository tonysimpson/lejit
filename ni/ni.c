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
DEFINEVAR PyObject *co_to_mp;
DEFINEVAR PyObject *co_to_entry_point;

DEFINEFN
int psyco_fatal_error(char *msg, char *filename, int lineno) {
  fprintf(stderr, "\n%s:%d: %s\n", filename, lineno, msg);
  Py_FatalError("Psyco assertion failed");
  return 0;
}

const static char MODULE_NAME[] = "ni";

/* counters for tests and measurements, e.g. that a loop is not compiled
   afresh on every back-edge */
static PyObject *ni_stats(PyObject *self, PyObject *unused) {
  return Py_BuildValue("{sl}", "code_buffers", psyco_stat_code_buffers);
}

static PyMethodDef module_methods[] = {
    {"_stats", ni_stats, METH_NOARGS, "Internal counters."},
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
    co_to_mp = PyDict_New();
    if (co_to_mp == NULL) {
      Py_DECREF(module);
      return NULL;
    }
    if (PyModule_AddObject(module, "co_to_mp", co_to_mp)) {
      Py_DECREF(co_to_mp);
      Py_DECREF(module);
      return NULL;
    }
    co_to_entry_point = PyDict_New();
    if (co_to_entry_point == NULL) {
      Py_DECREF(module);
      return NULL;
    }
    if (PyModule_AddObject(module, "co_to_entry_point", co_to_entry_point)) {
      Py_DECREF(co_to_entry_point);
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
    initialize_all_files();
  }
  CPsycoModule = module;
  return module;
}
