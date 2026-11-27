#include "Python/frames.h"
#include "bytecode.h"
#include "processor.h"
#include "codegen.h"
#include "codemanager.h"
#include "mergepoints.h"
#include "Python/pycompiler.h"
#include <Python.h>
/* how CPython's frames, tracing flag and recursion count are reached */
#include "mechanisms/frames.h"
#include "mechanisms/tracing.h"
#include "mechanisms/recursion.h"

extern int
le_eval_breaker_at_entry(PyThreadState *ts,
                         PyCodeObject *co); /* pyruntime.c */
#if LE_HAS_TSTATE_EVAL_BREAKER
/* whether an event is flagged for the thread: its eval_breaker's low byte
   (pyruntime.c:le_eval_breaker_handler) */
static inline int
le_eval_breaker_pending(PyThreadState *ts)
{
    return (_Py_atomic_load_uintptr_relaxed(&ts->eval_breaker) & 0xff) != 0;
}
#else
extern void *
le_eval_breaker_address(void); /* pyruntime.c */
#define le_eval_breaker_pending(ts) (*(int *)le_eval_breaker_address() != 0)
#endif

#if LE_HAS_EVAL_FRAME_POPS
/* the exception le_frame_finish ends a frame with when it has a result
   (le_entry_install makes it) */
static PyObject *frame_done;
#endif

#if LE_HAS_EX_CALL_SKIPS_RECURSION_TEST
DEFINEVAR PyObject *le_ex_callee = NULL;
#endif

#if LE_HAS_FRAME_STACKREFS
/* compiled code runs on the words as objects, which the frame holds: a
   tagged word is the object less its tag bit. Up to this many are copied
   into a fixed array (the glue copies them on to the machine stack), more
   into one sized for them, out of line. */
#define LE_RUN_FEW_WORDS 16

static __attribute__((noinline)) PyObject *
run_on_many_words(CodeBufferObject *codebuf, le_frame_t *f, int n,
                  stack_frame_info_t ***finfo)
{
    PyObject *args[n];
    int k;
    for (k = 0; k < n; k++) args[k] = LE_FRAME_WORD(f, k);
    return psyco_processor_run(codebuf, (long *)args, finfo);
}

static inline PyObject *
run_on_words(CodeBufferObject *codebuf, le_frame_t *f,
             stack_frame_info_t ***finfo)
{
    PyObject *args[LE_RUN_FEW_WORDS];
    int k, n = RUN_ARGC(codebuf);
    if (n > LE_RUN_FEW_WORDS)
        return run_on_many_words(codebuf, f, n, finfo);
    for (k = 0; k < n; k++) args[k] = LE_FRAME_WORD(f, k);
    return psyco_processor_run(codebuf, (long *)args, finfo);
}
#else
#define run_on_words(codebuf, f, finfo) \
    psyco_processor_run(codebuf, (long *)le_frame_args(f), finfo)
#endif

/* 'tstate' is the thread's, which the hook is given */
static PyObject *
LeCode_Run(PyThreadState *tstate, PyObject *codebuf, le_frame_t *f)
{
    /* the chain of compiled frames is empty until the code starts: a
       frame walk (frames.c:pvisitframes) from Python that the
       eval_breaker test below runs finds only its end mark */
    stack_frame_info_t *no_frames = (stack_frame_info_t *)-1;
    stack_frame_info_t **finfo = &no_frames;
    FrameRuntime fr;
    PyObject *result;

    /* ceval counts the depth on entering every frame */
#if LE_HAS_EX_CALL_SKIPS_RECURSION_TEST
    if (le_ex_callee != NULL && le_ex_callee == LE_FRAME_FUNCOBJ(f)) {
        le_ex_callee = NULL;
        if (le_recursion_enter_untested(tstate))
            return le_frame_finish(tstate, f, NULL, frame_done);
    }
    else
#endif
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
        le_eval_breaker_at_entry(tstate, le_frame_code(f)) < 0) {
        PyTraceBack_Here(le_frame_object(f));
        result = NULL;
    }
    else
        result = run_on_words((CodeBufferObject *)codebuf, f, &finfo);
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

/* f_lasti's bytes, as le_lasti_of's inverse */
static int
lasti_offset(int lasti)
{
    return LE_HAS_LASTI_IN_INSTRUCTIONS && lasti > 0 ? lasti * LE_UNIT : lasti;
}

/* where a frame paused at f_lasti 'lasti' goes on */
static inline int
resume_start(int lasti)
{
#if LE_HAS_FRAME_INSTR_PTR
    /* the frame's place is the instruction it goes on with */
    return lasti_offset(lasti);
#else
    return lasti < 0 ? 0 : lasti_offset(lasti) + LE_UNIT;
#endif
}

#if LE_RESUMES_ON_FRAME
/* the entry compiled for resuming 'f' after f_lasti, with 'nstack' stack
   items under the value sent; NULL if out of memory */
static __attribute__((noinline)) CodeBufferObject *
compile_resume(le_frame_t *f, le_code_extra_t *x, PyObject *mergepoints,
               int nstack)
#else
/* the entry compiled for resuming 'f' after f_lasti with 'nulls'
   (le_resume_entry_t); NULL if out of memory */
static __attribute__((noinline)) CodeBufferObject *
compile_resume(le_frame_t *f, le_code_extra_t *x, PyObject *mergepoints,
               uint64_t nulls)
#endif
{
    PyCodeObject *co = le_frame_code(f);
    PsycoObject *po;
    CodeBufferObject *volatile codebuf = NULL;
    le_compile_guard_t g;
    int lasti = le_frame_lasti(f);
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
        int start = resume_start(lasti);
#if LE_RESUMES_ON_FRAME
        (void)blocks;
        (void)nblocks;
        po = PsycoObject_FromGenFrame(co, le_frame_globals(f),
                                      le_frame_builtins(f), mergepoints,
                                      DEFAULT_RECURSION, start, nstack);
#else
        po = PsycoObject_FromGenFrame(
            co, le_frame_globals(f), le_frame_builtins(f), mergepoints,
            DEFAULT_RECURSION, start, nulls, le_frame_stack_depth(f), blocks,
            nblocks);
#endif
        codebuf =
            psyco_compile_code(po, start == 0 ? PsycoObject_Ready(po) : NULL);
    }
    psyco_guard_pop(&g);
    LE_COMPILE_END(LE_ENTER_CALL);
    LE_OWNER_LEAVE();
    return codebuf;
}

/* the entries at the resume point after f_lasti, -1 for the start: the
   record has one per yield, one per SEND (a throw into a yield from whose
   iterator returns resumes the frame after its loop, where the evaluator
   doesn't jump there itself) and one for the start, each taken by the
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
            mergepoints = psyco_analyse_code(co, x);
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
            n += in.op == YIELD_VALUE || in.op == SEND;
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
    int kinds = CO_GENERATOR | (LE_RESUMES_COROUTINES_COMPILED
                                    ? CO_COROUTINE | CO_ASYNC_GENERATOR
                                    : 0);
    if (co == le_gen_interpreted || !(co->co_flags & kinds) || throwflag ||
        !le_frame_is_gen(f))
        return false;
#if LE_RESUMES_COROUTINES
    if ((co->co_flags & (CO_COROUTINE | CO_ASYNC_GENERATOR)) &&
        f->f_lasti >= 0)
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

#if LE_RESUMES_GENERATORS_DIRECTLY
static size_t
direct_size(PyCodeObject *co)
{
    return offsetof(le_gen_direct_t, entries) +
           (le_code(co).size / LE_UNIT + 2) * sizeof(code_t *);
}

DEFINEFN le_gen_direct_t *
le_gen_direct_table(PyCodeObject *co, le_code_extra_t *x)
{
    PyObject *mergepoints;
    if (x->direct != NULL)
        return x->direct;
    if (x->nresumes < 0 || !(co->co_flags & (CO_GENERATOR | CO_COROUTINE)) ||
        le_code_nlocalsplus(co) + co->co_stacksize > LE_FRAME_WORDS_MAX)
        return NULL;
    mergepoints = psyco_get_merge_points(co);
    if (mergepoints == Py_None ||
        !(psyco_mp_flags(mergepoints) & MP_FLAGS_RESUMABLE))
        return NULL;
    x->direct = PyMem_Calloc(1, direct_size(co));
    return x->direct;
}

DEFINEFN void
le_gen_direct_clear(PyCodeObject *co, le_code_extra_t *x)
{
    if (x->direct != NULL)
        memset(x->direct, 0, direct_size(co));
}

/* the entry 'codebuf' resumes 'f' with, into its code's table of direct
   entries if it has one for these globals and builtins */
static inline void
direct_entry_put(le_code_extra_t *x, le_frame_t *f, CodeBufferObject *codebuf)
{
    le_gen_direct_t *t = x->direct;
    if (t == NULL)
        return;
    if (t->globals == NULL) {
        t->globals = le_frame_globals(f);
        t->builtins = le_frame_builtins(f);
    }
    if (t->globals == le_frame_globals(f) &&
        t->builtins == le_frame_builtins(f)) {
        t->entries[le_gen_direct_index(f)] = codebuf->codestart;
        LE_RELOC_SLOT(&t->entries[le_gen_direct_index(f)]);
    }
}
#else
#define direct_entry_put(x, f, codebuf) ((void)0)
#endif

#if LE_RESUMES_ON_FRAME
/* The compiled entry to resume the generator's frame 'f' with, compiled
   now if needed, or NULL to run it interpreted; 'x' is its code's record,
   if it has one. 'nstack' items are on its stack under the value sent. */
static inline CodeBufferObject *
le_gen_to_run(PyThreadState *tstate, le_frame_t *f, le_code_extra_t *x,
              int nstack)
{
    PyCodeObject *co = le_frame_code(f);
    PyObject *globals = le_frame_globals(f), *builtins = le_frame_builtins(f);
    le_resume_point_t *p;
    le_resume_entry_t *e;
    CodeBufferObject *codebuf;
    /* as le_code_to_run; and ceval's test of eval_breaker as it resumes
       the frame, whose exception the frame's own handlers may catch, is
       left to ceval */
    if (le_tracing_now(tstate, co) || le_frame_locals(f) != NULL ||
        le_code_nlocalsplus(co) + co->co_stacksize > LE_FRAME_WORDS_MAX ||
        le_eval_breaker_pending(tstate))
        return NULL;
    if (x == NULL && (x = le_code_extra(co)) == NULL) {
        PyErr_Clear(); /* out of memory */
        return NULL;
    }
    x->cold = 0;
    p = resume_point(x, co, le_frame_lasti(f));
    if (p == NULL || !le_frame_iterators_real(f, nstack))
        return NULL;
    e = p->entries;
    if (p->count > 0) {
        if (e->code == Py_None || e->globals != globals ||
            e->builtins != builtins)
            return NULL;
        direct_entry_put(x, f, (CodeBufferObject *)e->code);
        return (CodeBufferObject *)e->code;
    }
    codebuf = compile_resume(f, x, x->merge_points, nstack);
    if (codebuf == NULL)
        return NULL; /* out of memory */
    Py_INCREF(codebuf);
    e->code = (PyObject *)codebuf;
    e->globals = globals;
    e->builtins = builtins;
    p->count = 1;
    direct_entry_put(x, f, codebuf);
    return codebuf;
}

/* As LeCode_Run, for a resumable generator's frame, whose compiled code is
   given the frame and the value 'sent', which it takes, and reads the
   frame's words itself (vcompiler.c:PsycoObject_FromGenFrame). The frame
   is left paused at a yield, or ended (le_gen_frame_end); gen_send_ex does
   the rest, with le_gen_frame_leave where the evaluator pops frames. */
static inline PyObject *
LeGen_Run(PyThreadState *tstate, CodeBufferObject *codebuf, le_frame_t *f,
          PyObject *sent)
{
    stack_frame_info_t *no_frames = (stack_frame_info_t *)-1;
    stack_frame_info_t **finfo = &no_frames;
    FrameRuntime fr;
    PyObject *result;
    long args[2];
    extra_assert(RUN_ARGC(codebuf) == 2);
    if (le_recursion_enter(tstate)) {
        Py_DECREF(sent);
#if LE_HAS_EVAL_FRAME_POPS
        return le_gen_frame_leave(tstate, f, NULL, frame_done);
#else
        return NULL;
#endif
    }
    psyco_frame_start_register(&fr, f, &finfo);
    le_frame_push(tstate, f);
    le_frame_set_executing(f, 1);
    args[0] = (long)f;
    args[1] = (long)sent;
    le_gen_frame_run(f);
    result = psyco_processor_run(codebuf, args, &finfo);
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
#else
/* A resumed frame's words, at most this many, go to its compiled entry
   (LeGen_Run) in one array, with the run's FrameRuntime after them */
#define LE_GEN_WORDS 64

/* The compiled entry to resume the generator's frame 'f' with, compiled
   now if needed, or NULL to run it interpreted; 'x' is its code's record,
   if it has one. Its entries tell apart which of its words are NULL. The
   frame's words are read into 'words' for it: its locals and stack items
   are taken (where they are plain pointers), the cells and free variables
   only read. */
static inline CodeBufferObject *
le_gen_to_run(PyThreadState *tstate, le_frame_t *f, le_code_extra_t *x,
              long *words)
{
    PyCodeObject *co = le_frame_code(f);
    PyObject *globals = le_frame_globals(f), *builtins = le_frame_builtins(f);
    le_resume_point_t *p;
    le_resume_entry_t *e;
    CodeBufferObject *codebuf;
    int nplus, nstack, i, nlocals = le_code_nlocals(co);
    uint64_t nulls = 0;
    nplus = le_code_nlocalsplus(co);
    nstack = le_frame_stack_depth(f);
    /* as le_code_to_run; and ceval's test of eval_breaker as it resumes
       the frame, whose exception the frame's own handlers may catch, is
       left to ceval */
    if (le_tracing_now(tstate, co) || le_frame_locals(f) != NULL ||
        nplus + nstack > LE_GEN_WORDS || le_eval_breaker_pending(tstate))
        return NULL;
    if (x == NULL && (x = le_code_extra(co)) == NULL) {
        PyErr_Clear(); /* out of memory */
        return NULL;
    }
    x->cold = 0;
    p = resume_point(x, co, le_frame_lasti(f));
    if (p == NULL)
        return NULL;
    for (i = 0; i < nplus + nstack; i++) {
        PyObject *o = LE_FRAME_WORD(f, i);
        words[i] = (long)o;
        nulls |= (uint64_t)(o == NULL) << i;
        if (i < nlocals || i >= nplus)
            LE_FRAME_WORD(f, i) = NULL;
    }
    for (e = p->entries; e < p->entries + p->count; e++)
        if (e->nulls == nulls && e->globals == globals &&
            e->builtins == builtins) {
            if (e->code != Py_None)
                return (CodeBufferObject *)e->code;
            goto interpreted;
        }
    if (p->count == LE_RESUME_ENTRIES)
        goto interpreted;
    codebuf = compile_resume(f, x, x->merge_points, nulls);
    if (codebuf == NULL)
        goto interpreted; /* out of memory */
    Py_INCREF(codebuf);
    e->nulls = nulls;
    e->code = (PyObject *)codebuf;
    e->globals = globals;
    e->builtins = builtins;
    p->count++;
    return codebuf;
interpreted:
    for (i = 0; i < nplus + nstack; i++)
        LE_FRAME_WORD(f, i) = (PyObject *)words[i];
    return NULL;
}

/* As LeCode_Run, for a resumable generator's frame: its locals and stack
   items go to the compiled code, which owns them while it runs, and it
   borrows the cells and free variables, and it is given this run's
   FrameRuntime (vcompiler.c:PsycoObject_FromGenFrame). The frame is left
   paused at a yield, or ended (le_gen_frame_end); gen_send_ex does the
   rest. */
static inline PyObject *
LeGen_Run(PyThreadState *tstate, CodeBufferObject *codebuf, le_frame_t *f,
          long *words)
{
    stack_frame_info_t *no_frames = (stack_frame_info_t *)-1;
    stack_frame_info_t **finfo = &no_frames;
    FrameRuntime fr;
    PyObject *result;
    PyCodeObject *co = le_frame_code(f);
    int nplus = le_code_nlocalsplus(co), nstack = le_frame_stack_depth(f);
    extra_assert(RUN_ARGC(codebuf) == nplus + nstack + 1);

    if (le_recursion_enter(tstate)) {
        int i;
        for (i = 0; i < nplus + nstack; i++)
            LE_FRAME_WORD(f, i) = (PyObject *)words[i]; /* given back */
        return NULL;
    }
    psyco_frame_start_register(&fr, f, &finfo);
    le_frame_push(tstate, f);
    le_frame_set_executing(f, 1);
    words[nplus + nstack] = (long)&fr;
    le_gen_frame_run(f);
    result = psyco_processor_run(codebuf, words, &finfo);
    le_gen_frame_end(f, result);
    le_frame_set_current(tstate, le_frame_back(f));
    le_recursion_leave(tstate);
    psyco_frame_start_unregister(&fr);
    return result;
}
#endif

/* The compiled code to run for 'co' with these globals and builtins,
   compiled now on its first call, or NULL to run it interpreted. 'locals'
   is the frame's locals mapping (NULL for a function). */
static PyObject *
le_code_to_run(PyThreadState *tstate, PyCodeObject *co, PyObject *globals,
               PyObject *locals, PyObject *builtins)
{
    /* run interpreted while a trace or profile function is set: compiled
       code is invisible to them */
    if (le_tracing_now(tstate, co))
        return NULL;
    /* function frames, whose f_locals is NULL at entry, and module code
       (or exec's) whose locals mapping is its globals, an exact dict: its
       names are compiled as globals. Class bodies, and exec or eval with
       locals of their own, run interpreted. The test is made on every
       entry, as the same code may run with other locals. */
    if ((co->co_flags & CO_OPTIMIZED)
            ? locals == NULL
            : locals == globals && PyDict_CheckExact(globals)) {
        PyObject *mergepoints;
        le_code_extra_t *x = le_code_extra(co);
        if (x == NULL) {
            PyErr_Clear(); /* out of memory: this call runs interpreted */
            return NULL;
        }
        mergepoints = x->merge_points;
        if (mergepoints == NULL)
            mergepoints = psyco_analyse_code(co, x);
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
                x->entry_builtins == builtins) {
                x->cold = 0;
                return entry_point;
            }
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

/* a plain generator's or a coroutine's frame, resumed, whose code's
   record is 'x' (or NULL) */
static __attribute__((noinline)) PyObject *
LeGen_Eval(PyThreadState *tstate, le_frame_t *frame, le_code_extra_t *x)
{
#if LE_RESUMES_ON_FRAME
    int nstack = le_frame_stack_depth(frame) - 1;
    CodeBufferObject *resume = le_gen_to_run(tstate, frame, x, nstack);
    if (resume != NULL) {
        /* the value sent, as gen_send_ex2 pushed it */
        PyObject *sent = le_frame_word_take(
            frame, le_code_nlocalsplus(le_frame_code(frame)) + nstack);
        le_gen_frame_set_depth(frame, nstack);
        return LeGen_Run(tstate, resume, frame, sent);
    }
#else
    long words[LE_GEN_WORDS + 1];
    CodeBufferObject *resume = le_gen_to_run(tstate, frame, x, words);
    if (resume != NULL)
        return LeGen_Run(tstate, resume, frame, words);
#endif
    return original_eval_frame(tstate, frame, 0);
}

#if LE_RESUMES_GENERATORS_DIRECTLY
#if LE_HAS_EVAL_FRAME_POPS
/* exported by CPython, declared in its internal headers where it has them */
PyAPI_FUNC(int) _PyGen_SetStopIterationValue(PyObject *);
#else
/* A generator's run that ended (returned or raised) in le_gen_iternext,
   left for gen_send_ex2 to finish as it finishes any run: the frame, and
   what the run gave, which the hook gives back at once (LeEval_EvalFrame).
   Not per thread: the GIL is held from setting them to the hook. */
static le_frame_t *gen_ending_frame;
static PyObject *gen_ending_result;
#endif

/* gen_iternext, for compiled code: a generator paused at a yield, or not
   started, whose resume there is compiled, runs it without gen_send_ex2 and
   the hook, doing what gen_send_ex2 does around the run. A run that ends
   is finished by gen_send_ex2 itself, or where the evaluator pops frames
   (and the run leaves the frame as ceval does), as gen_iternext finishes
   it. Anything else goes to gen_iternext. */
DEFINEFN PyObject *
le_gen_iternext(PyObject *self)
{
    PyGenObject *gen = (PyGenObject *)self;
    le_frame_t *f =
        (le_frame_t *)((char *)gen + offsetof(PyGenObject, gi_iframe));
    PyThreadState *ts;
    le_code_extra_t *x;
    CodeBufferObject *resume;
    PyObject *result;
    if (gen->gi_frame_state >= FRAME_EXECUTING || !gen_may_resume(f, 0, &x))
        return PyGen_Type.tp_iternext(self);
    ts = PyThreadState_GET();
    resume = le_gen_to_run(ts, f, x, le_frame_stack_depth(f));
    if (resume == NULL)
        return PyGen_Type.tp_iternext(self);
    gen->gi_exc_state.previous_item = ts->exc_info;
    ts->exc_info = &gen->gi_exc_state;
    gen->gi_frame_state = FRAME_EXECUTING;
    /* the value sent, as gen_send_ex2 sends it */
    Py_INCREF(Py_None);
    result = LeGen_Run(ts, resume, f, Py_None);
#if LE_HAS_EVAL_FRAME_POPS
    if (LE_GEN_PAUSED(gen->gi_frame_state) || result == NULL)
        return result;
    if (result != Py_None)
        _PyGen_SetStopIterationValue(result);
    Py_DECREF(result);
    return NULL;
#else
    ts->exc_info = gen->gi_exc_state.previous_item;
    gen->gi_exc_state.previous_item = NULL;
    f->previous = NULL;
    if (LE_GEN_PAUSED(gen->gi_frame_state))
        return result;
    gen->gi_frame_state = FRAME_SUSPENDED;
    gen_ending_frame = f;
    gen_ending_result = result;
    return PyGen_Type.tp_iternext(self);
#endif
}

#if !LE_HAS_EVAL_FRAME_POPS
/* A direct step's run of 'gen' that returned, its frame 'f' unlinked,
   ended as gen_send_ex2 ends it, without going back through it: where the
   frame has no object, which _PyFrame_Clear would have to hand its words
   to, 1 with the frame cleared; else 0, for gen_send_ex2 to end it. */
static int
gen_returned_clear(PyGenObject *gen, le_frame_t *f)
{
    int k, n;
    if (f->frame_obj != NULL)
        return 0;
    Py_CLEAR(gen->gi_exc_state.exc_value);
    gen->gi_frame_state = FRAME_CLEARED;
    n = LE_FRAME_CODE(f)->co_nlocalsplus + le_frame_stack_depth(f);
    for (k = 0; k < n; k++) Py_XDECREF(le_frame_word_take(f, k));
    Py_CLEAR(f->f_locals);
#if LE_HAS_FRAME_FUNCOBJ
    Py_DECREF(LE_FRAME_FUNCOBJ(f));
#else
    Py_DECREF(f->f_func);
#endif
    Py_DECREF(LE_FRAME_CODE(f));
    return 1;
}
#endif

/* The rest of a step compiled code made itself (psyfunc.c:psyco_gen_step)
   whose run didn't pause at a yield in compiled code, as le_gen_iternext
   finishes a run of LeGen_Run: here the frame was never the thread's
   innermost, and the depth is counted back already. Where ceval ran it on
   (psyco_resume_run) and the evaluator pops frames, ceval has left it as
   gen_send_ex2 expects, its exception state unlinked. */
DEFINEFN PyObject *
le_gen_direct_end(PyThreadState *ts, PyObject *self, PyObject *result)
{
    PyGenObject *gen = (PyGenObject *)self;
    le_frame_t *f =
        (le_frame_t *)((char *)gen + offsetof(PyGenObject, gi_iframe));
#if LE_HAS_EVAL_FRAME_POPS
    if (gen->gi_exc_state.previous_item != NULL) {
        le_gen_frame_end(f, result);
        result = le_gen_frame_leave(ts, f, result, frame_done);
    }
    if (LE_GEN_PAUSED(gen->gi_frame_state) || result == NULL)
        return result;
    if (result != Py_None)
        _PyGen_SetStopIterationValue(result);
    Py_DECREF(result);
    return NULL;
#else
    le_gen_frame_end(f, result);
    ts->exc_info = gen->gi_exc_state.previous_item;
    gen->gi_exc_state.previous_item = NULL;
    f->previous = NULL;
    if (LE_GEN_PAUSED(gen->gi_frame_state))
        return result;
    if (result != NULL && gen_returned_clear(gen, f)) {
        if (result != Py_None)
            _PyGen_SetStopIterationValue(result);
        Py_DECREF(result);
        return NULL;
    }
    gen->gi_frame_state = FRAME_SUSPENDED;
    gen_ending_frame = f;
    gen_ending_result = result;
    return PyGen_Type.tp_iternext(self);
#endif
}

/* gen_iternext and gen_send differ only in a return of None, where
   send raises StopIteration */
DEFINEFN PyObject *
le_gen_direct_send_end(PyThreadState *ts, PyObject *self, PyObject *result)
{
    result = le_gen_direct_end(ts, self, result);
    if (result == NULL && !PyErr_Occurred())
        PyErr_SetNone(PyExc_StopIteration);
    return result;
}

DEFINEFN int
le_gen_direct_send_status(PyThreadState *ts, PyObject *self, PyObject *result,
                          PyObject *out[])
{
    PyGenObject *gen = (PyGenObject *)self;
    le_frame_t *f =
        (le_frame_t *)((char *)gen + offsetof(PyGenObject, gi_iframe));
#if LE_HAS_EVAL_FRAME_POPS
    if (gen->gi_exc_state.previous_item != NULL) {
        le_gen_frame_end(f, result);
        result = le_gen_frame_leave(ts, f, result, frame_done);
    }
    if (result == NULL)
        return -1;
    out[0] = result;
    return LE_GEN_PAUSED(gen->gi_frame_state);
#else
    PySendResult r;
    le_gen_frame_end(f, result);
    ts->exc_info = gen->gi_exc_state.previous_item;
    gen->gi_exc_state.previous_item = NULL;
    f->previous = NULL;
    if (LE_GEN_PAUSED(gen->gi_frame_state)) {
        out[0] = result;
        return 1;
    }
    if (result != NULL && gen_returned_clear(gen, f)) {
        out[0] = result;
        return 0;
    }
    /* gen_send_ex2 finishes the run, as for le_gen_direct_end, sending
       None as the hook takes it back */
    gen->gi_frame_state = FRAME_SUSPENDED;
    gen_ending_frame = f;
    gen_ending_result = result;
    r = PyIter_Send(self, Py_None, &out[0]);
    if (r == PYGEN_ERROR)
        return -1;
    return r == PYGEN_NEXT;
#endif
}
#endif

/* any other frame */
static __attribute__((noinline)) PyObject *
LeFunc_Eval(PyThreadState *tstate, le_frame_t *frame, int throwflag)
{
    PyObject *result, *entry_point;
    entry_point =
        le_code_to_run(tstate, le_frame_code(frame), le_frame_globals(frame),
                       le_frame_locals(frame), le_frame_builtins(frame));
    if (entry_point != NULL)
        result = LeCode_Run(tstate, entry_point, frame);
#if LE_HAS_EVAL_FRAME_POPS
    /* ceval links the frame to its caller, the current frame */
    else if (psyco_frame_called_from_callee(le_frame_current(tstate)))
        result = eval_from_callee(tstate, frame, throwflag);
#endif
    else {
        result = original_eval_frame(tstate, frame, throwflag);
#if LE_HAS_EVAL_FRAME_POPS
        /* ceval has popped the frame, giving one that outlives it its
           caller's frame object */
        psyco_frame_back_stands(le_frame_current(tstate));
#endif
    }
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
        le_code_extra_t *x;
#if LE_RESUMES_GENERATORS_DIRECTLY && !LE_HAS_EVAL_FRAME_POPS
        if (frame == gen_ending_frame) {
            /* the value gen_send_ex2 sent */
            LE_FRAME_WORD(frame, --frame->stacktop) = NULL;
            Py_DECREF(Py_None);
            gen_ending_frame = NULL;
            return gen_ending_result;
        }
#endif
#if LE_HAS_INTERPRETER_FRAME
        le_frame_t *back = le_frame_current(tstate);
        PyObject *result = gen_may_resume(frame, throwflag, &x)
                               ? LeGen_Eval(tstate, frame, x)
                               : original_eval_frame(tstate, frame, throwflag);
        /* a frame object that outlives its generator's ended run has its
           caller's frame object as f_back (the first run, which makes the
           generator, runs on the thread's frame) */
        if (result == NULL || (le_frame_is_gen(frame) &&
                               !LE_GEN_PAUSED(*le_frame_gen_state(frame))))
            psyco_frame_back_stands(back);
        return result;
#else
        /* gen_send_ex unlinks the frame's f_back after each run */
        if (gen_may_resume(frame, throwflag, &x))
            return LeGen_Eval(tstate, frame, x);
        return original_eval_frame(tstate, frame, throwflag);
#endif
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
    long words[LE_GEN_WORDS + 1];
    CodeBufferObject *resume = le_gen_to_run(tstate, frame, x, words);
    if (resume != NULL)
        return LeGen_Run(tstate, resume, frame, words);
    return original_eval_frame(frame, 0);
}

/* any other frame */
static __attribute__((noinline)) PyObject *
LeFunc_Eval(PyFrameObject *frame, int throwflag)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyObject *result, *entry_point;
    entry_point = le_code_to_run(tstate, frame->f_code, frame->f_globals,
                                 frame->f_locals, frame->f_builtins);
    if (entry_point != NULL)
        result = LeCode_Run(tstate, entry_point, frame);
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

/* The type of the object an async generator's yield wraps its value in,
   for asend() to tell it from what an await in the body yields: an object
   head and the value. CPython exports neither the type nor the function
   that makes one, so it is taken once, as LeJIT starts, from what the
   frame evaluator returns for such a yield: a probe of its own, installed
   for the while, runs one async generator to its end. NULL where what it
   returned isn't laid out as expected: async generators then run
   interpreted. */
DEFINEVAR PyTypeObject *psyco_async_gen_wrapper = NULL;
static PyCodeObject *wrap_code;
static PyObject *wrap_value, *wrap_seen;
static le_eval_frame_fn wrap_next;

/* the first object other than the value, and other than the generator
   itself, which the call returns where the frame runs to make it */
static void
wrap_saw(PyCodeObject *co, PyObject *r)
{
    if (co == wrap_code && r != NULL && wrap_seen == NULL && r != wrap_value &&
        !PyAsyncGen_CheckExact(r)) {
        Py_INCREF(r);
        wrap_seen = r;
    }
}

#if LE_HAS_EVAL_HOOK_TSTATE
static PyObject *
wrap_probe(PyThreadState *ts, le_frame_t *f, int throwflag)
{
    PyObject *r = wrap_next(ts, f, throwflag);
    wrap_saw(le_frame_code(f), r);
    return r;
}
#else
static PyObject *
wrap_probe(PyFrameObject *f, int throwflag)
{
    PyObject *r = wrap_next(f, throwflag);
    wrap_saw(f->f_code, r);
    return r;
}
#endif

static void
find_async_gen_wrapper(void)
{
    static int done = 0;
    PyObject *g, *w, *r;
    if (done)
        return;
    done = 1;
    g = PyDict_New();
    wrap_value = PyObject_CallObject((PyObject *)&PyBaseObject_Type, NULL);
    if (g == NULL || wrap_value == NULL ||
        PyDict_SetItemString(g, "__builtins__", PyEval_GetBuiltins()) < 0 ||
        PyDict_SetItemString(g, "v", wrap_value) < 0)
        goto done;
    r = PyRun_String("async def w():\n    yield v\n", Py_file_input, g, g);
    if (r == NULL)
        goto done;
    Py_DECREF(r);
    w = PyDict_GetItemString(g, "w");
    if (w == NULL || !PyFunction_Check(w))
        goto done;
    wrap_code = (PyCodeObject *)PyFunction_GET_CODE(w);
    wrap_next = get_eval_frame();
    set_eval_frame(wrap_probe);
    r = PyRun_String(
        "g = w()\n"
        "for _ in 0, 1:\n"
        "    try:\n"
        "        g.asend(None).send(None)\n"
        "    except (StopIteration, StopAsyncIteration):\n"
        "        pass\n",
        Py_file_input, g, g);
    set_eval_frame(wrap_next);
    Py_XDECREF(r);
    if (wrap_seen != NULL) {
        PyTypeObject *t = Py_TYPE(wrap_seen);
        if (t->tp_basicsize == sizeof(PyObject) + sizeof(PyObject *) &&
            t->tp_itemsize == 0 && PyType_HasFeature(t, Py_TPFLAGS_HAVE_GC) &&
            t->tp_dealloc != NULL &&
            *(PyObject **)(wrap_seen + 1) == wrap_value)
            psyco_async_gen_wrapper = t;
    }
done:
    PyErr_Clear();
    if (g != NULL)
        PyDict_Clear(g);
    Py_XDECREF(g);
    Py_CLEAR(wrap_seen);
    Py_CLEAR(wrap_value);
    wrap_code = NULL;
}

/* lejit.start: CPython hands every frame to LeJIT from now on. The evaluator
   that was there is kept, since LeJIT passes it the frames it doesn't
   compile, and it stays kept after lejit.stop for any call already on its
   way through LeJIT's. -1 with an exception set if it can't. */
#if LE_KEEPS_RECURSION_LIMIT
int *le_recursion_limit;
#endif

#if LE_HAS_C_STACK_SOFT_LIMIT
long le_c_entry_charge, le_c_step_charge[LE_C_WAYS];

/* The C stack a level of recursion through C takes, measured once with
   small recursions for mechanisms/recursion.h's charges, as it depends on
   how CPython and LeJIT were built: in CPython's evaluator through a C
   call, sum(), next() and send(), then in LeJIT through a C call. */
static PyObject *
stack_probe_sp(PyObject *self, PyObject *unused)
{
    return PyLong_FromVoidPtr(__builtin_frame_address(0));
}

static PyObject *
stack_probe_call(PyObject *self, PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs != 2) {
        PyErr_SetString(PyExc_TypeError, "two arguments");
        return NULL;
    }
    return PyObject_CallOneArg(args[0], args[1]);
}

static PyMethodDef stack_probes[] = {
    {"sp", stack_probe_sp, METH_NOARGS, NULL},
    {"call", (PyCFunction)(void (*)(void))stack_probe_call, METH_FASTCALL,
     NULL},
    {NULL, NULL, 0, NULL}};

static const char stack_probe_source[] =
    "def call_(n):\n"
    "    return sp() if n == 0 else call(call_, n - 1)\n"
    "def sum_(n):\n"
    "    yield sp() if n == 0 else sum(sum_(n - 1))\n"
    "def next_(n):\n"
    "    yield sp() if n == 0 else next(next_(n - 1))\n"
    "def send_(n):\n"
    "    yield sp() if n == 0 else send_(n - 1).send(None)\n"
    "class Ex_:\n"
    "    def m(self, n):\n"
    "        return sp() if n == 0 else self.m(*(n - 1,))\n"
    "def ex_(n):\n"
    "    return Ex_().m(n)\n";

/* the C stack pointer at the bottom of 'fn'(depth), a generator stepped
   once if 'gen'; 0 with an exception set */
static __attribute__((noinline)) uintptr_t
stack_probe_bottom(PyObject *fn, long depth, int gen)
{
    uintptr_t sp = 0;
    PyObject *n = PyLong_FromLong(depth), *r, *v;
    if (n == NULL)
        return 0;
    r = PyObject_CallOneArg(fn, n);
    Py_DECREF(n);
    if (r == NULL)
        return 0;
    v = gen ? PyIter_Next(r) : (Py_INCREF(r), r);
    Py_DECREF(r);
    if (v != NULL) {
        sp = (uintptr_t)PyLong_AsVoidPtr(v);
        Py_DECREF(v);
    }
    return sp;
}

/* the C stack one level of the probe 'name' takes, warm; 0 if it fails */
static long
stack_probe_level(PyObject *ns, const char *name)
{
    PyObject *fn = PyDict_GetItemString(ns, name);
    uintptr_t one = 0, two = 0;
    int gen = strcmp(name, "call_") != 0 && strcmp(name, "ex_") != 0, round;
    if (fn == NULL)
        return 0;
    for (round = 0; round < 3; round++) {
        one = stack_probe_bottom(fn, 1, gen);
        two = stack_probe_bottom(fn, 2, gen);
        if (one == 0 || two == 0 || two >= one) {
            PyErr_Clear();
            return 0;
        }
    }
    return (long)(one - two);
}

/* the probes' namespace, or NULL */
static PyObject *
stack_probes_load(void)
{
    PyObject *ns, *code, *r;
    PyMethodDef *m;
    ns = PyDict_New();
    if (ns == NULL)
        goto error;
    if (PyDict_SetItemString(ns, "__builtins__", PyEval_GetBuiltins()) < 0)
        goto error;
    for (m = stack_probes; m->ml_name != NULL; m++) {
        PyObject *f = PyCFunction_New(m, NULL);
        if (f == NULL || PyDict_SetItemString(ns, m->ml_name, f) < 0) {
            Py_XDECREF(f);
            goto error;
        }
        Py_DECREF(f);
    }
    code = Py_CompileString(stack_probe_source, "<lejit stack probe>",
                            Py_file_input);
    if (code == NULL)
        goto error;
    r = PyEval_EvalCode(code, ns, ns);
    Py_DECREF(code);
    if (r == NULL)
        goto error;
    Py_DECREF(r);
    return ns;
error:
    PyErr_Clear();
    Py_XDECREF(ns);
    return NULL;
}
#endif

DEFINEFN int
le_entry_install(void)
{
    le_eval_frame_fn now = get_eval_frame();
#if LE_HAS_C_STACK_SOFT_LIMIT
    /* the first time only, when no LeJIT level is running to hold a
       charge, and before LeJIT's hook goes in */
    static int charges_measured = 0;
    PyObject *probes = NULL;
    long plain_call = 0, plain_step[LE_C_WAYS] = {0};
    if (!charges_measured && now != LeEval_EvalFrame) {
        charges_measured = 1;
        probes = stack_probes_load();
        if (probes != NULL) {
            plain_call = stack_probe_level(probes, "call_");
            plain_step[LE_C_CONSUMER] = stack_probe_level(probes, "sum_");
            plain_step[LE_C_NEXT] = stack_probe_level(probes, "next_");
            plain_step[LE_C_SEND] = stack_probe_level(probes, "send_");
            plain_step[LE_C_EX_METHOD] = stack_probe_level(probes, "ex_");
        }
    }
#endif
#if LE_KEEPS_RECURSION_LIMIT
    le_recursion_limit = le_recursion_limit_address();
#endif
#if LE_HAS_EVAL_FRAME_POPS
    if (frame_done == NULL &&
        (frame_done = PyObject_CallNoArgs(PyExc_Exception)) == NULL)
        return -1;
#endif
    find_async_gen_wrapper();
    if (now != LeEval_EvalFrame) {
        original_eval_frame = now;
        set_eval_frame(LeEval_EvalFrame);
    }
#if LE_HAS_C_STACK_SOFT_LIMIT
    if (probes != NULL) {
        /* each compiled on its first call; LeJIT runs sum_'s level as
           compiled code alone, which is taken as what its steps take */
        long lejit_call = stack_probe_level(probes, "call_");
        long lejit_step = stack_probe_level(probes, "sum_");
        int way;
        if (lejit_call > 0 && plain_call > lejit_call)
            le_c_entry_charge = plain_call - lejit_call;
        for (way = LE_C_CONSUMER; way < LE_C_WAYS; way++)
            if (lejit_step > 0 && plain_step[way] > lejit_step)
                le_c_step_charge[way] = plain_step[way] - lejit_step;
        Py_DECREF(probes);
    }
#endif
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
