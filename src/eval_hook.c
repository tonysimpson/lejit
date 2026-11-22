#include "Python/frames.h"
#include "codegen.h"
#include "codemanager.h"
#include "mergepoints.h"
#include <Python.h>
/* how CPython's frames, tracing flag and recursion count are reached */
#include "mechanisms/frames.h"
#include "mechanisms/tracing.h"
#include "mechanisms/recursion.h"

extern int
le_eval_breaker_at_entry(PyCodeObject *co); /* pyruntime.c */

#if LE_HAS_EVAL_FRAME_POPS
/* the exception le_frame_finish ends a frame with when it has a result
   (le_entry_install makes it) */
static PyObject *frame_done;
#endif

static PyObject *
LeCode_Run(PyObject *codebuf, le_frame_t *f)
{
    /* the chain of compiled frames is empty until the code starts: a
       frame walk (frames.c:pvisitframes) from Python that the
       eval_breaker test below runs finds only its end mark */
    stack_frame_info_t *no_frames = (stack_frame_info_t *)-1;
    stack_frame_info_t **finfo = &no_frames;
    FrameRuntime fr;
    PyObject *result;

    PyThreadState *tstate = PyThreadState_GET();

    /* ceval counts the depth on entering every frame */
    if (le_recursion_enter())
        return le_frame_finish(tstate, f, NULL, frame_done);
    psyco_frame_start_register(&fr, f, &finfo);
    /* as ceval: the frame is the current one while its code runs, so that
       sys._getframe, frames made by calls from here (their f_back) and
       pvisitframes() see it */
    le_frame_push(tstate, f);
    /* also as ceval: frame.clear() (traceback.clear_frames, unittest's
       assertRaises) must refuse to clear it, since the compiled code borrows
       the cells and arguments in f_localsplus */
    le_frame_set_executing(f, 1);
    /* ceval's eval_breaker test at the start */
    if (le_frame_start(f) < 0 ||
        le_eval_breaker_at_entry(le_frame_code(f)) < 0) {
        PyTraceBack_Here(le_frame_object(f));
        result = NULL;
    }
    else
        result = psyco_processor_run((CodeBufferObject *)codebuf,
                                     (long *)le_frame_args(f), &finfo);
    le_frame_set_executing(f, 0);
    le_frame_set_current(tstate, le_frame_back(f));
    le_recursion_leave();
    psyco_frame_start_unregister(&fr);
    return le_frame_finish(tstate, f, result, frame_done);
}

#define DEFAULT_RECURSION 10

/* out of line: a function that calls sigsetjmp is compiled with its
   locals in memory, and le_code_to_run runs for every frame */
static __attribute__((noinline)) CodeBufferObject *
compile_entry(PyCodeObject *co, le_code_extra_t *x, PyObject *globals,
              PyObject *builtins, PyObject *mergepoints)
{
    PsycoObject *po;
    CodeBufferObject *volatile codebuf = NULL;
    le_compile_guard_t g;
    LE_OWNER_ENTER(&x->owner);
    LE_COMPILE_BEGIN(LE_ENTER_CALL);
    psyco_guard_push(&g);
    if (sigsetjmp(g.env, 0) == 0) {
        po = PsycoObject_FromCode(co, globals, builtins, mergepoints,
                                  DEFAULT_RECURSION);
        codebuf = psyco_compile_code(po, PsycoObject_Ready(po));
    }
    psyco_guard_pop(&g);
    LE_COMPILE_END(LE_ENTER_CALL);
    LE_OWNER_LEAVE();
    return codebuf;
}

/* generators are never compiled (mergepoints.c), and every resume comes
   through the hook: they go on interpreted before any lookup (and their
   frames have no caller between runs) */
#define LE_GENERATOR_FLAGS (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)

/* The compiled code to run for 'co' with these globals and builtins,
   compiled now on its first call, or NULL to run it interpreted. 'locals'
   is the frame's locals mapping (NULL for a function). */
static PyObject *
le_code_to_run(PyCodeObject *co, PyObject *globals, PyObject *locals,
               PyObject *builtins)
{
    /* run interpreted while a trace or profile function is set: compiled
       code is invisible to them */
    if (le_tracing_now(PyThreadState_GET()))
        return NULL;
    /* only function frames, whose f_locals is NULL at entry. Module code,
       class bodies and exec or eval with a locals mapping have one */
    if (locals == NULL) {
        PyObject *mergepoints;
        le_code_extra_t *x = le_code_extra(co);
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
                    return NULL; /* out of memory: this call runs interpreted
                                  */
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

/* how CPython hands calls to LeJIT: LeEval_EvalFrame and le_entry_install */
#if LE_HAS_EVAL_HOOK_TSTATE
/* Entry by the PEP 523 frame-evaluation hook, which is given the thread
   state and a frame (le_frame_t). Every frame CPython evaluates comes here
   first: one whose code LeJIT has compiled (eval_hook.c:le_code_to_run) runs
   on the frame's argument words (eval_hook.c:LeCode_Run), anything else
   goes to the evaluator installed before. The hook is read and set
   through the interpreter state's API functions. */
typedef PyObject *(*le_eval_frame_fn)(PyThreadState *, le_frame_t *, int);

static le_eval_frame_fn original_eval_frame = NULL;

static PyObject *
LeEval_EvalFrame(PyThreadState *tstate, le_frame_t *frame, int throwflag)
{
    PyObject *result, *entry_point;
    if (le_frame_code(frame)->co_flags & LE_GENERATOR_FLAGS)
        return original_eval_frame(tstate, frame, throwflag);
    entry_point =
        le_code_to_run(le_frame_code(frame), le_frame_globals(frame),
                       le_frame_locals(frame), le_frame_builtins(frame));
    if (entry_point != NULL)
        result = LeCode_Run(entry_point, frame);
    else
        result = original_eval_frame(tstate, frame, throwflag);
    if (le_frame_outlives(frame) != NULL)
        psyco_frame_returned(frame);
    return result;
}

static le_eval_frame_fn
get_eval_frame(void)
{
    return _PyInterpreterState_GetEvalFrameFunc(PyThreadState_Get()->interp);
}

static void
set_eval_frame(le_eval_frame_fn fn)
{
    _PyInterpreterState_SetEvalFrameFunc(PyThreadState_Get()->interp, fn);
}

/* ceval's own evaluation of a frame, past the hook (frames.c) */
DEFINEFN PyObject *
le_eval_frame_default(le_frame_t *f)
{
    return _PyEval_EvalFrameDefault(PyThreadState_GET(), f, 0);
}
#else
/* Entry by the PEP 523 frame-evaluation hook, which is given a
   PyFrameObject. Every frame CPython evaluates comes here first: one
   whose code LeJIT has compiled (eval_hook.c:le_code_to_run) runs on the
   frame's argument words (eval_hook.c:LeCode_Run), anything else goes to
   the evaluator installed before. The interpreter state that holds the
   hook is internal from 3.8, so Python/pyruntime.c (Py_BUILD_CORE) reads
   and writes it. */
typedef PyObject *(*le_eval_frame_fn)(PyFrameObject *, int);
void *
le_interp_eval_frame(void);
void
le_set_interp_eval_frame(void *fn);

static le_eval_frame_fn original_eval_frame = NULL;

static PyObject *
LeEval_EvalFrame(PyFrameObject *frame, int throwflag)
{
    PyObject *result, *entry_point;
    if (frame->f_code->co_flags & LE_GENERATOR_FLAGS)
        return original_eval_frame(frame, throwflag);
    entry_point = le_code_to_run(frame->f_code, frame->f_globals,
                                 frame->f_locals, frame->f_builtins);
    if (entry_point != NULL)
        result = LeCode_Run(entry_point, frame);
    else
        result = original_eval_frame(frame, throwflag);
    if (le_frame_outlives(frame) != NULL)
        psyco_frame_returned(frame);
    return result;
}

static le_eval_frame_fn
get_eval_frame(void)
{
    return (le_eval_frame_fn)le_interp_eval_frame();
}

static void
set_eval_frame(le_eval_frame_fn fn)
{
    le_set_interp_eval_frame((void *)fn);
}

DEFINEFN PyObject *
le_eval_frame_default(le_frame_t *f)
{
    return _PyEval_EvalFrameDefault(f, 0);
}
#endif

/* lejit.start: CPython hands every frame to LeJIT from now on. The evaluator
   that was there is kept, since LeJIT passes it the frames it doesn't
   compile, and it stays kept after lejit.stop for any call already on its
   way through LeJIT's. -1 with an exception set if it can't. */
DEFINEFN int
le_entry_install(void)
{
    le_eval_frame_fn now = get_eval_frame();
#if LE_HAS_EVAL_FRAME_POPS
    if (frame_done == NULL &&
        (frame_done = PyObject_CallNoArgs(PyExc_Exception)) == NULL)
        return -1;
#endif
    if (now != LeEval_EvalFrame) {
        original_eval_frame = now;
        set_eval_frame(LeEval_EvalFrame);
    }
    return 0;
}

/* lejit.stop: new frames go to the evaluator that was there before; frames
   already running compiled code go on to their end compiled. Returns -1
   if another evaluator has been installed over LeJIT's: it would still pass
   frames on to LeJIT, and can't be taken out from here. */
DEFINEFN int
le_entry_remove(void)
{
    le_eval_frame_fn now = get_eval_frame();
    if (now == LeEval_EvalFrame)
        set_eval_frame(original_eval_frame);
    else if (now != original_eval_frame)
        return -1;
    return 0;
}
