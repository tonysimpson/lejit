#include "Python/frames.h"
#include "codegen.h"
#include "codemanager.h"
#include "mergepoints.h"
#include <Python.h>
/* how CPython's frames, tracing flag and recursion count are reached */
#include "mechanisms/frames.h"
#include "mechanisms/tracing.h"
#include "mechanisms/recursion.h"

extern int ni_eval_breaker_at_entry(PyFrameObject *f); /* pyruntime.c */

static PyObject *NiCode_Run(PyObject *codebuf, ni_frame_t *f) {
  stack_frame_info_t **finfo;
  FrameRuntime fr;
  PyObject *result;

  PyThreadState *tstate = PyThreadState_GET();

  /* ceval counts the depth on entering every frame */
  if (ni_recursion_enter())
    return NULL;
  psyco_frame_start_register(&fr, ni_frame_object(f), &finfo);
  /* as ceval: the frame is the current one while its code runs, so that
     sys._getframe, frames made by calls from here (their f_back) and
     pvisitframes() see it */
  ni_frame_set_current(tstate, f);
  /* also as ceval: frame.clear() (traceback.clear_frames, unittest's
     assertRaises) must refuse to clear it, since the compiled code borrows
     the cells and arguments in f_localsplus */
  ni_frame_set_executing(f, 1);
  /* ceval's eval_breaker test at the start */
  if (ni_eval_breaker_at_entry(ni_frame_object(f)) < 0)
    result = NULL;
  else
    result = psyco_processor_run((CodeBufferObject *)codebuf,
                                 (long *)ni_frame_args(f), &finfo);
  ni_frame_set_executing(f, 0);
  ni_frame_set_current(tstate, ni_frame_back(f));
  ni_recursion_leave();
  psyco_frame_start_unregister(&fr);
  return result;
}

#define DEFAULT_RECURSION 10

/* out of line: a function that calls sigsetjmp is compiled with its
   locals in memory, and ni_code_to_run runs for every frame */
static __attribute__((noinline)) CodeBufferObject *
compile_entry(PyCodeObject *co, ni_code_extra_t *x, PyObject *globals,
              PyObject *builtins, PyObject *mergepoints) {
  PsycoObject *po;
  CodeBufferObject *volatile codebuf = NULL;
  ni_compile_guard_t g;
  NI_OWNER_ENTER(&x->owner);
  NI_COMPILE_BEGIN(NI_ENTER_CALL);
  psyco_guard_push(&g);
  if (sigsetjmp(g.env, 0) == 0) {
    po = PsycoObject_FromCode(co, globals, builtins, mergepoints,
                              DEFAULT_RECURSION);
    codebuf = psyco_compile_code(po, PsycoObject_Ready(po));
  }
  psyco_guard_pop(&g);
  NI_COMPILE_END(NI_ENTER_CALL);
  NI_OWNER_LEAVE();
  return codebuf;
}

/* The compiled code to run for 'co' with these globals and builtins,
   compiled now on its first call, or NULL to run it interpreted. 'locals'
   is the frame's locals mapping (NULL for a function). */
static PyObject *ni_code_to_run(PyCodeObject *co, PyObject *globals,
                                PyObject *locals, PyObject *builtins) {
  /* generators are never compiled (mergepoints.c), and every resume comes
     through here: leave before any lookup */
  if (co->co_flags & (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR))
    return NULL;
  /* run interpreted while a trace or profile function is set: compiled
     code is invisible to them */
  if (ni_tracing_now(PyThreadState_GET()))
    return NULL;
  /* only function frames, whose f_locals is NULL at entry. Module code,
     class bodies and exec or eval with a locals mapping have one, and the
     compiler's name opcodes (IMPORT_STAR, DELETE_NAME) act on the globals */
  if (locals == NULL) {
    PyObject *mergepoints;
    ni_code_extra_t *x = ni_code_extra(co);
    if (x == NULL) {
      PyErr_Clear(); /* out of memory: this call runs interpreted */
      return NULL;
    }
    mergepoints = x->merge_points;
    if (mergepoints == NULL)
      mergepoints = psyco_analyse_code(co, x, 0);
    if (mergepoints != Py_None) {
      PyObject *entry_point;
      entry_point = x->entry_point;
      if (entry_point == NULL) {
        /* Not compiled yet */
        CodeBufferObject *codebuf =
            compile_entry(co, x, globals, builtins, mergepoints);
        if (codebuf == NULL)
          return NULL; /* out of memory: this call runs interpreted */
        entry_point = (PyObject *)codebuf;
        Py_INCREF(entry_point);
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
#if NI_HAS_EVAL_HOOK_TSTATE
/* Entry by the PEP 523 frame-evaluation hook, which is given the thread
   state and a PyFrameObject. Every frame CPython evaluates comes here
   first: one whose code Ni has compiled (eval_hook.c:ni_code_to_run) runs
   on the frame's argument words (eval_hook.c:NiCode_Run), anything else
   goes to the evaluator installed before. The hook is read and set
   through the interpreter state's API functions. */
typedef PyObject *(*ni_eval_frame_fn)(PyThreadState *, PyFrameObject *, int);

static ni_eval_frame_fn original_eval_frame = NULL;

static PyObject *NiEval_EvalFrame(PyThreadState *tstate, PyFrameObject *frame,
                                  int throwflag) {
  PyObject *entry_point = ni_code_to_run(frame->f_code, frame->f_globals,
                                         frame->f_locals, frame->f_builtins);
  if (entry_point != NULL)
    return NiCode_Run(entry_point, frame);
  return original_eval_frame(tstate, frame, throwflag);
}

static ni_eval_frame_fn get_eval_frame(void) {
  return _PyInterpreterState_GetEvalFrameFunc(PyThreadState_Get()->interp);
}

static void set_eval_frame(ni_eval_frame_fn fn) {
  _PyInterpreterState_SetEvalFrameFunc(PyThreadState_Get()->interp, fn);
}

/* ceval's own evaluation of a frame, past the hook (frames.c) */
DEFINEFN PyObject *ni_eval_frame_default(PyFrameObject *f) {
  return _PyEval_EvalFrameDefault(PyThreadState_GET(), f, 0);
}
#else
/* Entry by the PEP 523 frame-evaluation hook, which is given a
   PyFrameObject. Every frame CPython evaluates comes here first: one
   whose code Ni has compiled (eval_hook.c:ni_code_to_run) runs on the
   frame's argument words (eval_hook.c:NiCode_Run), anything else goes to
   the evaluator installed before. The interpreter state that holds the
   hook is internal from 3.8, so Python/pyruntime.c (Py_BUILD_CORE) reads
   and writes it. */
typedef PyObject *(*ni_eval_frame_fn)(PyFrameObject *, int);
void *ni_interp_eval_frame(void);
void ni_set_interp_eval_frame(void *fn);

static ni_eval_frame_fn original_eval_frame = NULL;

static PyObject *NiEval_EvalFrame(PyFrameObject *frame, int throwflag) {
  PyObject *entry_point = ni_code_to_run(frame->f_code, frame->f_globals,
                                         frame->f_locals, frame->f_builtins);
  if (entry_point != NULL)
    return NiCode_Run(entry_point, frame);
  return original_eval_frame(frame, throwflag);
}

static ni_eval_frame_fn get_eval_frame(void) {
  return (ni_eval_frame_fn)ni_interp_eval_frame();
}

static void set_eval_frame(ni_eval_frame_fn fn) {
  ni_set_interp_eval_frame((void *)fn);
}

DEFINEFN PyObject *ni_eval_frame_default(PyFrameObject *f) {
  return _PyEval_EvalFrameDefault(f, 0);
}
#endif

/* lejit.start: CPython hands every frame to Ni from now on. The evaluator
   that was there is kept, since Ni passes it the frames it doesn't
   compile, and it stays kept after lejit.stop for any call already on its
   way through Ni's */
DEFINEFN void ni_entry_install(void) {
  ni_eval_frame_fn now = get_eval_frame();
  if (now != NiEval_EvalFrame) {
    original_eval_frame = now;
    set_eval_frame(NiEval_EvalFrame);
  }
}

/* lejit.stop: new frames go to the evaluator that was there before; frames
   already running compiled code go on to their end compiled. Returns -1
   if another evaluator has been installed over Ni's: it would still pass
   frames on to Ni, and can't be taken out from here. */
DEFINEFN int ni_entry_remove(void) {
  ni_eval_frame_fn now = get_eval_frame();
  if (now == NiEval_EvalFrame)
    set_eval_frame(original_eval_frame);
  else if (now != original_eval_frame)
    return -1;
  return 0;
}
