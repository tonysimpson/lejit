#include "Python/frames.h"
#include "bytecode.h"
#include "processor.h"
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
#if LE_RESUMES_GENERATORS && LE_HAS_TSTATE_EVAL_BREAKER
extern int
le_eval_breaker_pending(void); /* pyruntime.c */
#elif LE_RESUMES_GENERATORS
extern void *
le_eval_breaker_address(void); /* pyruntime.c */
#define le_eval_breaker_pending() (*(int *)le_eval_breaker_address() != 0)
#endif

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
    if (le_recursion_enter(tstate))
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
    else {
#if LE_HAS_FRAME_STACKREFS
        /* compiled code runs on the words as objects, which the frame
           holds: a tagged word is the object less its tag bit */
        int k, n = RUN_ARGC(codebuf);
        PyObject *args[n > 0 ? n : 1];
        for (k = 0; k < n; k++) args[k] = LE_FRAME_WORD(f, k);
#else
        PyObject **args = le_frame_args(f);
#endif
        result = psyco_processor_run((CodeBufferObject *)codebuf, (long *)args,
                                     &finfo);
    }
#if LE_HAS_EVAL_FRAME_POPS
    if (fr.frame == NULL) {
        /* ceval ran it on to its end and popped it (frames.c:
           psyco_resume_run) */
        le_recursion_leave(tstate);
        psyco_frame_start_unregister(&fr);
        return result;
    }
#endif
    le_frame_set_executing(f, 0);
    le_frame_set_current(tstate, le_frame_back(f));
    le_recursion_leave(tstate);
    psyco_frame_start_unregister(&fr);
#if LE_HAS_EVAL_FRAME_POPS
    /* before le_frame_finish pops it */
    if (le_frame_outlives(f) != NULL)
        psyco_frame_returned(f);
#endif
    return le_frame_finish(tstate, f, result, frame_done);
}

#if LE_HAS_EVAL_FRAME_POPS
/* ceval instruments code for sys.monitoring as a frame of it starts
   (RESUME) or has an exception thrown in, so a frame resumed past RESUME
   (frames.c:psyco_resume_run) would run code that gives no events: a frame
   of the same code is ended before its first instruction by a throw
   (le_frame_throw_out), which instruments the code. 'f' is a frame of
   it. */
DEFINEFN int
le_code_instrument(le_frame_t *f)
{
    PyThreadState *ts = PyThreadState_GET();
    PyCodeObject *co = le_frame_code(f);
    le_frame_t *t = le_frame_push_room(ts, co->co_framesize);
    PyObject *e;
    if (t == NULL) {
        PyErr_NoMemory();
        return -1;
    }
#if LE_HAS_FRAME_INITIALIZE_TSTATE
    _PyFrame_Initialize(ts, t, PyStackRef_FromPyObjectNew(LE_FRAME_FUNCOBJ(f)),
                        NULL, co, 0, NULL);
#else
    _PyFrame_Initialize(t, (PyFunctionObject *)Py_NewRef(f->f_funcobj), NULL,
                        co, 0);
#endif
    PyErr_SetRaisedException(Py_NewRef(frame_done));
    le_frame_throw_out(ts, t);
    e = PyErr_GetRaisedException();
    if (e == frame_done) {
        Py_DECREF(e);
        return 0;
    }
    PyErr_SetRaisedException(e);
    return -1;
}
#endif

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

/* every resume of a generator or coroutine comes through the hook: a
   resumable generator's runs compiled (LeGen_Run), any other goes on
   interpreted before any lookup (and their frames have no caller between
   runs) */
#define LE_GENERATOR_FLAGS (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)

#if LE_RESUMES_GENERATORS
/* f_lasti's bytes, as le_lasti_of's inverse */
static int
lasti_offset(int lasti)
{
    return LE_HAS_LASTI_IN_INSTRUCTIONS && lasti > 0 ? lasti * LE_UNIT : lasti;
}

/* the entry compiled for resuming 'f' after f_lasti with 'nulls'
   (le_resume_entry_t); NULL if out of memory */
static __attribute__((noinline)) CodeBufferObject *
compile_resume(le_frame_t *f, le_code_extra_t *x, PyObject *mergepoints,
               uint64_t nulls)
{
    PyCodeObject *co = le_frame_code(f);
    PsycoObject *po;
    CodeBufferObject *volatile codebuf = NULL;
    le_compile_guard_t g;
    int lasti = le_frame_lasti(f);
#if LE_HAS_FRAME_INSTR_PTR
    /* the frame's place is the instruction it goes on with */
    int start = lasti_offset(lasti);
#else
    int start = lasti < 0 ? 0 : lasti_offset(lasti) + LE_UNIT;
#endif
#if LE_HAS_INTERPRETER_FRAME
    /* no blocks: handlers are the exception table's */
    int *blocks = NULL, nblocks = 0;
#else
    int blocks[3 * CO_MAXBLOCKS], i, nblocks = f->f_iblock;
    for (i = 0; i < nblocks; i++) {
        blocks[3 * i] = f->f_blockstack[i].b_type;
        blocks[3 * i + 1] = lasti_offset(f->f_blockstack[i].b_handler);
        blocks[3 * i + 2] = f->f_blockstack[i].b_level;
    }
#endif
    LE_OWNER_ENTER(&x->owner);
    LE_COMPILE_BEGIN(LE_ENTER_CALL);
    psyco_guard_push(&g);
    if (sigsetjmp(g.env, 0) == 0) {
        po = PsycoObject_FromGenFrame(
            co, le_frame_globals(f), le_frame_builtins(f), mergepoints,
            DEFAULT_RECURSION, start, nulls, le_frame_stack_depth(f), blocks,
            nblocks);
        codebuf =
            psyco_compile_code(po, start == 0 ? PsycoObject_Ready(po) : NULL);
    }
    psyco_guard_pop(&g);
    LE_COMPILE_END(LE_ENTER_CALL);
    LE_OWNER_LEAVE();
    return codebuf;
}

/* the entries at the resume point after f_lasti, -1 for the start: the
   record has one per yield and one for the start, each taken by the
   first resume there. NULL to run interpreted: the code isn't resumable
   (nresumes -1), or out of memory. */
static le_resume_point_t *
resume_point(le_code_extra_t *x, PyCodeObject *co, int lasti)
{
    int i;
    /* a generator resumed in a loop resumes at the same yield each time */
    if (x->last_resume != NULL && x->last_resume->lasti == lasti)
        return x->last_resume;
    if (x->resumes == NULL) {
        le_code_t c = le_code(co);
        PyObject *mergepoints = x->merge_points;
        int pos, n = 1;
        if (x->nresumes < 0)
            return NULL;
        if (mergepoints == NULL)
            mergepoints = psyco_analyse_code(co, x, 0);
        if (mergepoints == NULL) {
            PyErr_Clear(); /* out of memory */
            return NULL;
        }
        if (mergepoints == Py_None ||
            !(psyco_mp_flags(mergepoints) & MP_FLAGS_RESUMABLE)) {
            x->nresumes = -1;
            return NULL;
        }
        for (pos = 0; pos < c.size;) {
            le_instr_t in = le_instr(c, pos);
            pos = in.next;
            n += in.op == YIELD_VALUE;
        }
        x->resumes = PyMem_Calloc(n, sizeof(le_resume_point_t));
        if (x->resumes == NULL)
            return NULL;
        x->nresumes = n;
        for (i = 0; i < n; i++) x->resumes[i].lasti = -2;
    }
    for (i = 0; i < x->nresumes; i++) {
        le_resume_point_t *p = &x->resumes[i];
        if (p->lasti == -2)
            p->lasti = lasti;
        if (p->lasti == lasti)
            return x->last_resume = p;
    }
    return NULL;
}

/* whether a plain generator's or a coroutine's frame 'f' may resume
   compiled: not while it is thrown into, nor once its code is known not to
   be resumable. A coroutine runs compiled only from its start: ceval
   resumes it at an await, sending into what it awaits. Its code's record,
   if it has one, in '*x'. Generators resumed in a loop are mostly of one
   code, so the last one found not resumable is kept, to answer it without
   looking up its record. */
static inline bool
gen_may_resume(le_frame_t *f, int throwflag, le_code_extra_t **x)
{
    PyCodeObject *co = le_frame_code(f);
    int kinds = CO_GENERATOR | (LE_RESUMES_COROUTINES ? CO_COROUTINE : 0);
    if (co == le_gen_interpreted || !(co->co_flags & kinds) || throwflag ||
        !le_frame_is_gen(f))
        return false;
#if LE_RESUMES_COROUTINES
    if ((co->co_flags & CO_COROUTINE) && f->f_lasti >= 0)
        return false;
#endif
    *x = le_code_extra_get(co);
    if (*x != NULL && (*x)->nresumes < 0) {
        le_gen_interpreted = co;
        le_gen_interpreted_x = *x;
        return false;
    }
    return true;
}

/* The compiled entry to resume the generator's frame 'f' with, compiled
   now if needed, or NULL to run it interpreted; 'x' is its code's record,
   if it has one. Its entries tell apart which of its locals and stack
   items are NULL. */
static inline CodeBufferObject *
le_gen_to_run(PyThreadState *tstate, le_frame_t *f, le_code_extra_t *x)
{
    PyCodeObject *co = le_frame_code(f);
    PyObject *globals = le_frame_globals(f), *builtins = le_frame_builtins(f);
    le_resume_point_t *p;
    le_resume_entry_t *e;
    CodeBufferObject *codebuf;
    PyObject **slots, **stack;
    int nlocals, nstack, i;
    uint64_t nulls = 0;
    slots = le_frame_args(f);
    stack = le_frame_stack(f);
    nlocals = le_code_nlocals(co);
    nstack = le_frame_stack_depth(f);
    /* as le_code_to_run; and ceval's test of eval_breaker as it resumes
       the frame, whose exception the frame's own handlers may catch, is
       left to ceval */
    if (le_tracing_now(tstate) || le_frame_locals(f) != NULL ||
        nlocals + nstack > 64 || le_eval_breaker_pending())
        return NULL;
    if (x == NULL && (x = le_code_extra(co)) == NULL) {
        PyErr_Clear(); /* out of memory */
        return NULL;
    }
    p = resume_point(x, co, le_frame_lasti(f));
    if (p == NULL)
        return NULL;
    for (i = 0; i < nlocals; i++) nulls |= (uint64_t)(slots[i] == NULL) << i;
    for (i = 0; i < nstack; i++)
        nulls |= (uint64_t)(stack[i] == NULL) << (nlocals + i);
    for (e = p->entries; e < p->entries + p->count; e++)
        if (e->nulls == nulls && e->globals == globals &&
            e->builtins == builtins)
            return e->code == Py_None ? NULL : (CodeBufferObject *)e->code;
    if (p->count == LE_RESUME_ENTRIES)
        return NULL;
    codebuf = compile_resume(f, x, x->merge_points, nulls);
    if (codebuf == NULL)
        return NULL; /* out of memory: this resume runs interpreted */
    Py_INCREF(codebuf);
    e->nulls = nulls;
    e->code = (PyObject *)codebuf;
    e->globals = globals;
    e->builtins = builtins;
    p->count++;
    return codebuf;
}

/* As LeCode_Run, for a resumable generator's frame: its locals and stack
   items go to the compiled code, which owns them while it runs, and it
   borrows the cells and free variables, and it is given this run's
   FrameRuntime (vcompiler.c:PsycoObject_FromGenFrame). The frame is left
   paused at a yield, or ended (le_gen_frame_end); gen_send_ex does the rest,
   with le_gen_frame_leave where the evaluator pops frames.
 */
static inline PyObject *
LeGen_Run(PyThreadState *tstate, CodeBufferObject *codebuf, le_frame_t *f)
{
    stack_frame_info_t *no_frames = (stack_frame_info_t *)-1;
    stack_frame_info_t **finfo = &no_frames;
    FrameRuntime fr;
    PyObject *result;
    PyCodeObject *co = le_frame_code(f);
    PyObject **slots = le_frame_args(f), **stack = le_frame_stack(f);
    int nlocals = le_code_nlocals(co), nplus = le_code_nlocalsplus(co);
    int nstack = le_frame_stack_depth(f), i;
    long words[nplus + nstack + 1];
    extra_assert(stack == slots + nplus);
    extra_assert(RUN_ARGC(codebuf) == nplus + nstack + 1);

    if (le_recursion_enter(tstate)) {
#if LE_HAS_EVAL_FRAME_POPS
        return le_gen_frame_leave(tstate, f, NULL, frame_done);
#else
        return NULL;
#endif
    }
    psyco_frame_start_register(&fr, f, &finfo);
    le_frame_push(tstate, f);
    le_frame_set_executing(f, 1);
    for (i = 0; i < nplus; i++) {
        words[i] = (long)slots[i];
        if (i < nlocals)
            slots[i] = NULL;
    }
    for (i = 0; i < nstack; i++) {
        words[nplus + i] = (long)stack[i];
        stack[i] = NULL;
    }
    words[nplus + nstack] = (long)&fr;
    le_gen_frame_run(f);
    result = psyco_processor_run(codebuf, words, &finfo);
#if LE_HAS_EVAL_FRAME_POPS
    if (fr.frame == NULL) {
        /* ceval ran it on to a yield or its end (frames.c:
           psyco_resume_run) */
        le_recursion_leave(tstate);
        psyco_frame_start_unregister(&fr);
        return result;
    }
#endif
    le_gen_frame_end(f, result);
    le_frame_set_current(tstate, le_frame_back(f));
    le_recursion_leave(tstate);
    psyco_frame_start_unregister(&fr);
#if LE_HAS_EVAL_FRAME_POPS
    result = le_gen_frame_leave(tstate, f, result, frame_done);
#endif
    return result;
}
#endif

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

#if LE_HAS_EVAL_FRAME_POPS
/* ceval runs a frame called from a compiled callee or inlined frame and
   pops it, giving a frame object that outlives it the real frame as
   f_back: the frame object is made first, so it is still at hand to be
   given the callee instead */
static PyObject *
eval_from_callee(PyThreadState *ts, le_frame_t *f, int throwflag)
{
    PyFrameObject *fo;
    PyObject *result;
    _Py_CODEUNIT *at = LE_FRAME_PLACE(f);
    /* complete for the while, or it would have none */
    le_frame_set_lasti(f, 0);
    fo = le_frame_object(f);
    LE_FRAME_PLACE(f) = at;
    if (fo == NULL)
        return original_eval_frame(ts, f, throwflag);
    Py_INCREF(fo);
    result = original_eval_frame(ts, f, throwflag);
    if (Py_REFCNT(fo) > 1) {
        PyFrameObject *back = psyco_frame_callee_back();
        if (back != NULL)
            Py_XSETREF(fo->f_back, back);
    }
    Py_DECREF(fo);
    return result;
}
#endif

#if LE_RESUMES_GENERATORS
/* a plain generator's or a coroutine's frame, resumed, whose code's
   record is 'x' (or NULL) */
static __attribute__((noinline)) PyObject *
LeGen_Eval(PyThreadState *tstate, le_frame_t *frame, le_code_extra_t *x)
{
    CodeBufferObject *resume = le_gen_to_run(tstate, frame, x);
    if (resume != NULL)
        return LeGen_Run(tstate, resume, frame);
    return original_eval_frame(tstate, frame, 0);
}
#endif

/* any other frame */
static __attribute__((noinline)) PyObject *
LeFunc_Eval(PyThreadState *tstate, le_frame_t *frame, int throwflag)
{
    PyObject *result, *entry_point;
    entry_point =
        le_code_to_run(le_frame_code(frame), le_frame_globals(frame),
                       le_frame_locals(frame), le_frame_builtins(frame));
    if (entry_point != NULL)
        result = LeCode_Run(entry_point, frame);
#if LE_HAS_EVAL_FRAME_POPS
    /* ceval links the frame to its caller, the current frame */
    else if (psyco_frame_called_from_callee(le_frame_current(tstate)))
        result = eval_from_callee(tstate, frame, throwflag);
#endif
    else
        result = original_eval_frame(tstate, frame, throwflag);
#if !LE_HAS_EVAL_FRAME_POPS
    /* where the frame is popped by now, LeCode_Run did this before */
    if (le_frame_outlives(frame) != NULL)
        psyco_frame_returned(frame);
#endif
    return result;
}

/* the eval hook: kept apart from both paths, so that neither pays for
   the other's frame */
static PyObject *
LeEval_EvalFrame(PyThreadState *tstate, le_frame_t *frame, int throwflag)
{
    if (le_frame_code(frame)->co_flags & LE_GENERATOR_FLAGS) {
#if LE_RESUMES_GENERATORS
        le_code_extra_t *x;
        if (gen_may_resume(frame, throwflag, &x))
            return LeGen_Eval(tstate, frame, x);
#endif
        return original_eval_frame(tstate, frame, throwflag);
    }
    return LeFunc_Eval(tstate, frame, throwflag);
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

/* a plain generator's or a coroutine's frame, resumed, whose code's
   record is 'x' (or NULL) */
static __attribute__((noinline)) PyObject *
LeGen_Eval(PyFrameObject *frame, le_code_extra_t *x)
{
    PyThreadState *tstate = PyThreadState_GET();
    CodeBufferObject *resume = le_gen_to_run(tstate, frame, x);
    if (resume != NULL)
        return LeGen_Run(tstate, resume, frame);
    return original_eval_frame(frame, 0);
}

/* any other frame */
static __attribute__((noinline)) PyObject *
LeFunc_Eval(PyFrameObject *frame, int throwflag)
{
    PyObject *result, *entry_point;
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

/* the eval hook: kept apart from both paths, so that neither pays for
   the other's frame */
static PyObject *
LeEval_EvalFrame(PyFrameObject *frame, int throwflag)
{
    if (frame->f_code->co_flags & LE_GENERATOR_FLAGS) {
        le_code_extra_t *x;
        if (gen_may_resume(frame, throwflag, &x))
            return LeGen_Eval(frame, x);
        return original_eval_frame(frame, throwflag);
    }
    return LeFunc_Eval(frame, throwflag);
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
#if LE_KEEPS_RECURSION_LIMIT
int *le_recursion_limit;
#endif

DEFINEFN int
le_entry_install(void)
{
    le_eval_frame_fn now = get_eval_frame();
#if LE_KEEPS_RECURSION_LIMIT
    le_recursion_limit = le_recursion_limit_address();
#endif
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

/* LeJIT's hook, which compiled code tests is still installed before it
   calls compiled code directly (mechanisms/tracing.h:le_direct_call_refused)
 */
DEFINEFN void *
le_eval_hook(void)
{
    return (void *)LeEval_EvalFrame;
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
