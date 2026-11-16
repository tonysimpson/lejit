#include "Python/frames.h"
#include "codemanager.h"
#include "compat2to3.h"
#include "mergepoints.h"
#include <Python.h>

static _PyFrameEvalFunction original_eval_frame = NULL;

static PyObject *NiCode_Run(PyObject *codebuf, PyFrameObject *f) {
  stack_frame_info_t **finfo;
  PyObject *result;

  PyThreadState *tstate = PyThreadState_GET();

  if (!psyco_frame_start_register(f, &finfo)) {
    return NULL;
  }
  /* as ceval: the frame is the current one while its code runs, so that
     sys._getframe, frames made by calls from here (their f_back) and
     pvisitframes() see it */
  tstate->frame = f;
  /* tdict arg is NULL here as it is not used - not sure why */
  result = psyco_processor_run((CodeBufferObject *)codebuf,
                               (long *)f->f_localsplus, &finfo);
  tstate->frame = f->f_back;
  psyco_trash_object(NULL); /* free any trashed object now */
  if (!psyco_frame_start_unregister(f)) {
    Py_XDECREF(result);
    return NULL;
  }
  return result;
}

#define DEFAULT_RECURSION 10

static PyObject *NiEval_EvalFrame(PyFrameObject *frame, int throwflag) {
  /* Don't compile modules - buggy */
  if (frame->f_globals != frame->f_locals) {
    PyCodeObject *co;
    PyObject *mergepoints;
    co = frame->f_code;
    mergepoints = psyco_get_merge_points(co, 0);
    if (mergepoints != Py_None) {
      PyObject *entry_point;
      entry_point = PyDict_GetItem(co_to_entry_point, (PyObject *)co);
      if (entry_point == NULL) {
        /* Not compiled yet */
        PsycoObject *po;
        CodeBufferObject *codebuf;
        po = PsycoObject_FromCode(co, frame->f_globals, mergepoints,
                                  DEFAULT_RECURSION);
        codebuf = psyco_compile_code(po, PsycoObject_Ready(po));
        entry_point = PyTuple_New(2);
        PyTuple_SET_ITEM(entry_point, 0, (PyObject *)codebuf);
        PyTuple_SET_ITEM(entry_point, 1, frame->f_globals);
        PyDict_SetItem(co_to_entry_point, (PyObject *)co, entry_point);
      }
      if (entry_point != Py_None &&
          PyTuple_GET_ITEM(entry_point, 1) == frame->f_globals) {
        return NiCode_Run(PyTuple_GET_ITEM(entry_point, 0), frame);
      }
    }
  }
  return original_eval_frame(frame, throwflag);
}

INITIALIZATIONFN
int ni_eval_hook_init(void) {
  PyThreadState *tstate = PyThreadState_Get();
  if (original_eval_frame == NULL) {
    original_eval_frame = tstate->interp->eval_frame;
    tstate->interp->eval_frame = NiEval_EvalFrame;
  }
  return 0;
}
