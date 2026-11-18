#include "Python/frames.h"
#include "codegen.h"
#include "codemanager.h"
#include "compat2to3.h"
#include "mergepoints.h"
#include <Python.h>
/* the version-specific mechanisms, chosen from CPython's source by
   tools/metac/mechanisms.py (D75) */
#include "mechanism_frames_gen.h"
#include "mechanism_tracing_gen.h"
#include "mechanism_recursion_gen.h"

/* run interpreted while a trace or profile function is set (KNOWN_BUGS C23):
   compiled code is invisible to them. Off only for tools/interpreted.py,
   which profiles to find what Ni didn't compile. */
DEFINEVAR int ni_trace_fallback = 1;

extern int ni_eval_breaker_at_entry(PyFrameObject *f); /* pyruntime.c */

static PyObject *NiCode_Run(PyObject *codebuf, ni_frame_t *f) {
  stack_frame_info_t **finfo;
  PyObject *result;

  PyThreadState *tstate = PyThreadState_GET();

  /* ceval counts the depth on entering every frame (KNOWN_BUGS C12) */
  if (ni_recursion_enter())
    return NULL;
  if (!psyco_frame_start_register(ni_frame_object(f), &finfo)) {
    ni_recursion_leave();
    return NULL;
  }
  /* as ceval: the frame is the current one while its code runs, so that
     sys._getframe, frames made by calls from here (their f_back) and
     pvisitframes() see it */
  ni_frame_set_current(tstate, f);
  /* also as ceval: frame.clear() (traceback.clear_frames, unittest's
     assertRaises) must refuse to clear it, since the compiled code borrows
     the cells and arguments in f_localsplus */
  ni_frame_set_executing(f, 1);
  /* ceval's eval_breaker test at the start (KNOWN_BUGS C24) */
  if (ni_eval_breaker_at_entry(ni_frame_object(f)) < 0)
    result = NULL;
  else
    /* tdict arg is NULL here as it is not used - not sure why */
    result = psyco_processor_run((CodeBufferObject *)codebuf,
                                 (long *)ni_frame_args(f), &finfo);
  ni_frame_set_executing(f, 0);
  ni_frame_set_current(tstate, ni_frame_back(f));
  ni_recursion_leave();
  psyco_trash_object(NULL); /* free any trashed object now */
  if (!psyco_frame_start_unregister(ni_frame_object(f))) {
    Py_XDECREF(result);
    return NULL;
  }
  return result;
}

#define DEFAULT_RECURSION 10

/* The compiled code to run for 'co' with these globals and builtins,
   compiled now on its first call, or NULL to run it interpreted. 'locals'
   is the frame's locals mapping (equal to globals for a module). */
static PyObject *ni_entry_point(PyCodeObject *co, PyObject *globals,
                                PyObject *locals, PyObject *builtins) {
  /* generators are never compiled (mergepoints.c), and every resume comes
     through here: leave before any lookup */
  if (co->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))
    return NULL;
  if (ni_trace_fallback && ni_tracing_now(PyThreadState_GET()))
    return NULL;
  /* Don't compile modules - buggy */
  if (globals != locals) {
    PyObject *mergepoints;
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
        po = PsycoObject_FromCode(co, globals, builtins, mergepoints,
                                  DEFAULT_RECURSION);
        codebuf = psyco_compile_code(po, PsycoObject_Ready(po));
        NI_COMPILE_END(NI_ENTER_CALL);
        entry_point = (PyObject *)codebuf;
        x->entry_point = entry_point;
        x->entry_globals = globals;
        x->entry_builtins = builtins;
      }
      if (entry_point != Py_None && x->entry_globals == globals &&
          x->entry_builtins == builtins)
        return entry_point;
    }
  }
  return NULL;
}

/* how CPython hands calls to Ni: NiEval_EvalFrame and ni_entry_install */
#include "mechanism_entry_gen.h"

INITIALIZATIONFN
int ni_eval_hook_init(void) { return ni_entry_install(); }
