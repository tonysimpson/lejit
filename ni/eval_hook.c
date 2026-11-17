#include "Python/frames.h"
#include "codemanager.h"
#include "compat2to3.h"
#include "mergepoints.h"
#include <Python.h>

static _PyFrameEvalFunction original_eval_frame = NULL;

/* run interpreted while a trace or profile function is set (KNOWN_BUGS C23):
   compiled code is invisible to them. Off only for tools/interpreted.py,
   which profiles to find what Ni didn't compile. */
DEFINEVAR int ni_trace_fallback = 1;

EXTERNFN int ni_eval_breaker_at_entry(PyFrameObject *f); /* pyruntime.c */

static PyObject *NiCode_Run(PyObject *codebuf, PyFrameObject *f) {
  stack_frame_info_t **finfo;
  PyObject *result;

  PyThreadState *tstate = PyThreadState_GET();

  /* ceval counts the depth on entering every frame (KNOWN_BUGS C12) */
  if (Py_EnterRecursiveCall(""))
    return NULL;
  if (!psyco_frame_start_register(f, &finfo)) {
    Py_LeaveRecursiveCall();
    return NULL;
  }
  /* as ceval: the frame is the current one while its code runs, so that
     sys._getframe, frames made by calls from here (their f_back) and
     pvisitframes() see it */
  tstate->frame = f;
  /* also as ceval: frame.clear() (traceback.clear_frames, unittest's
     assertRaises) must refuse to clear it, since the compiled code borrows
     the cells and arguments in f_localsplus */
  f->f_executing = 1;
  /* ceval's eval_breaker test at the start (KNOWN_BUGS C24) */
  if (ni_eval_breaker_at_entry(f) < 0)
    result = NULL;
  else
    /* tdict arg is NULL here as it is not used - not sure why */
    result = psyco_processor_run((CodeBufferObject *)codebuf,
                                 (long *)f->f_localsplus, &finfo);
  f->f_executing = 0;
  tstate->frame = f->f_back;
  Py_LeaveRecursiveCall();
  psyco_trash_object(NULL); /* free any trashed object now */
  if (!psyco_frame_start_unregister(f)) {
    Py_XDECREF(result);
    return NULL;
  }
  return result;
}

#define DEFAULT_RECURSION 10

static PyObject *NiEval_EvalFrame(PyFrameObject *frame, int throwflag) {
  /* generators are never compiled (mergepoints.c), and every resume comes
     through here: leave before any lookup */
  if (frame->f_code->co_flags &
      (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))
    return original_eval_frame(frame, throwflag);
  if (ni_trace_fallback && PyThreadState_GET()->use_tracing)
    return original_eval_frame(frame, throwflag);
  /* Don't compile modules - buggy */
  if (frame->f_globals != frame->f_locals) {
    PyCodeObject *co;
    PyObject *mergepoints;
    co = frame->f_code;
    mergepoints = psyco_get_merge_points(co, 0);
    if (mergepoints != Py_None) {
      PyObject *entry_point;
      ni_code_extra_t *x = ni_code_extra(co); /* made by the call above */
      entry_point = x->entry_point;
      if (entry_point == NULL) {
        /* Not compiled yet */
        PsycoObject *po;
        CodeBufferObject *codebuf;
        NI_COMPILE_BEGIN(NI_ENTER_CALL);
        po = PsycoObject_FromCode(co, frame->f_globals, mergepoints,
                                  DEFAULT_RECURSION);
        codebuf = psyco_compile_code(po, PsycoObject_Ready(po));
        NI_COMPILE_END(NI_ENTER_CALL);
        entry_point = (PyObject *)codebuf;
        x->entry_point = entry_point;
        x->entry_globals = frame->f_globals;
      }
      if (entry_point != Py_None && x->entry_globals == frame->f_globals) {
        return NiCode_Run(entry_point, frame);
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
