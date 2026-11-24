/* Built with Py_BUILD_CORE, for what the runtime and
   interpreter states (_PyRuntime, PyInterpreterState, in the internal
   headers) hold, which the extension headers don't describe. Which header
   that is, where the current thread state and eval_breaker are, and the
   copy of what ceval does when eval_breaker is set, depend on the target's
   source (the LE_HAS_ features below). */
#define Py_BUILD_CORE
#include <Python.h>
#include <stddef.h>
#include "le_features_gen.h"
/* internal/pystate.h, or where the source declares _PyRuntime */
#if LE_HAS_PYCORE_INTERP_H
/* The runtime state (_PyRuntime) in its own internal header, which does
   not include the interpreter state's (struct _is); pycore_pystate.h
   for the thread tests ceval's eval_breaker uses. */
#include "internal/pycore_runtime.h"
#include "internal/pycore_interp.h"
#include "internal/pycore_pystate.h"
#elif LE_HAS_PYCORE_PYSTATE_H
/* The runtime state (_PyRuntime) and the interpreter state, internal
   from 3.8. */
#include "internal/pycore_pystate.h"
#else
/* The runtime state (_PyRuntime) as 3.7 describes it. */
#include "internal/pystate.h"
#endif
#if LE_HAS_TSTATE_EVAL_BREAKER
/* _Py_HandlePending, _PY_EVAL_EVENTS_MASK */
#include "internal/pycore_ceval.h"
#endif
#include "frameobject.h"
#if LE_HAS_INTERPRETER_FRAME
#include "internal/pycore_frame.h"
#if LE_HAS_PYCORE_INTERPFRAME_H
#include "internal/pycore_interpframe.h"
#endif
#endif
#include "opcode.h"
#include "../bytecode.h"

/* le_tstate_current_address, where the thread state isn't a thread-local
   (mechanisms/tstate.h) */
#if !LE_HAS_THREAD_LOCAL_TSTATE
/* The running thread's state is the word _PyRuntime.gilstate.tstate_current
   (an _Py_atomic_address, which a plain load reads), at an address fixed
   for the process: what PyThreadState_GET() reads. Compiled code's load
   is src/mechanisms/tstate.h's. */
_Static_assert(
    __builtin_types_compatible_p(
        __typeof__(_PyRuntime.gilstate.tstate_current), _Py_atomic_address) &&
        sizeof(_PyRuntime.gilstate.tstate_current) == 8,
    "LeJIT: _PyRuntime.gilstate.tstate_current is loaded as a pointer");

void *
le_tstate_current_address(void)
{
    return &_PyRuntime.gilstate.tstate_current;
}
#endif
#if LE_HAS_THREAD_LOCAL_TSTATE
/* The running thread's state is the thread-local _Py_tss_tstate of the
   object that defines PyThreadState_Get (libpython, or the executable),
   a local symbol no other object can name. Where that object's TLS is
   static, the variable is at one offset from every thread's thread
   pointer (%fs:0 on x86-64), which compiled code loads in one move. The
   offset is found at start-up: the word of the object's TLS block that
   holds PyThreadState_Get(), and only if it is the only one; and a
   thread started for the test must find the object's block at the same
   offset from its own thread pointer, which dynamic TLS doesn't give. */
#include <link.h>
#include <pthread.h>

typedef struct {
    const void *in; /* an address in the object */
    void *block;    /* its TLS block in the calling thread, or NULL */
    size_t size;
} tls_find_t;

static int
tls_find_one(struct dl_phdr_info *info, size_t size, void *arg)
{
    tls_find_t *f = arg;
    const ElfW(Phdr) *tls = NULL;
    bool here = false;
    int i;
    (void)size;
    for (i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        uintptr_t lo = info->dlpi_addr + ph->p_vaddr;
        if (ph->p_type == PT_TLS)
            tls = ph;
        else if (ph->p_type == PT_LOAD && (uintptr_t)f->in >= lo &&
                 (uintptr_t)f->in < lo + ph->p_memsz)
            here = true;
    }
    if (!here)
        return 0;
    f->block = tls != NULL ? info->dlpi_tls_data : NULL;
    f->size = tls != NULL ? tls->p_memsz : 0;
    return 1;
}

static char *
thread_pointer(void)
{
    char *tp;
    __asm__("mov %%fs:0, %0" : "=r"(tp));
    return tp;
}

/* the calling thread's block, as an offset from its thread pointer */
static bool
tls_block_offset(tls_find_t *f, long *off)
{
    f->in = (const void *)PyThreadState_Get;
    f->block = NULL;
    if (dl_iterate_phdr(tls_find_one, f) != 1 || f->block == NULL)
        return false;
    *off = (char *)f->block - thread_pointer();
    return true;
}

static void *
tls_test_thread(void *arg)
{
    tls_find_t f;
    long *off = arg;
    if (!tls_block_offset(&f, off))
        *off = 1; /* never a block's offset: blocks are aligned */
    return NULL;
}

static bool
tstate_tls_find(long *offset)
{
    tls_find_t f;
    long block, other;
    PyThreadState *ts = PyThreadState_Get();
    size_t i, found = 0, at = 0;
    pthread_t t;
    if (!tls_block_offset(&f, &block))
        return false;
    for (i = 0; i + sizeof(void *) <= f.size; i += sizeof(void *))
        if (*(PyThreadState **)((char *)f.block + i) == ts) {
            found++;
            at = i;
        }
    if (found != 1 || pthread_create(&t, NULL, tls_test_thread, &other) != 0)
        return false;
    pthread_join(t, NULL);
    if (other != block)
        return false;
    *offset = block + (long)at;
    return true;
}

/* the offset of the running thread's state from the thread pointer, or
   false where it isn't one offset for every thread; found once */
bool
le_tstate_thread_offset(long *offset)
{
    static int found = 0; /* 1 found, -1 not */
    static long at;
    if (found == 0)
        found = tstate_tls_find(&at) ? 1 : -1;
    *offset = at;
    return found > 0;
}
#endif
#if LE_HAS_MONITORING
_Static_assert(8 < sizeof(((PyInterpreterState *)0)->monitors.tools) &&
                   sizeof(((PyInterpreterState *)0)->monitors.tools) <= 16,
               "LeJIT: sys.monitoring's tools are read as two words");
/* The interpreter's monitoring version, which CPython changes with every
   change to its tools table, and the last one seen in the main interpreter
   while no tool was set and no frame's locals were exposed, or -1. While
   they are equal, no tool is set (mechanisms/tracing.h:le_tracing_flag). */
#if LE_HAS_INTERP_MONITORING_VERSION
#define LE_MONITORING_VERSION(i) ((i)->monitoring_version)
#else
#define LE_MONITORING_VERSION(i) ((i)->ceval.instrumentation_version)
#endif
_Static_assert(sizeof(LE_MONITORING_VERSION((PyInterpreterState *)0)) == 8,
               "LeJIT: the monitoring version is read as one word");
uint64_t le_quiet_version = (uint64_t)-1;
#ifdef PyFrameLocalsProxy_Check
extern int psyco_frames_exposed; /* frames.c */
#define LE_FRAMES_EXPOSED psyco_frames_exposed
#else
#define LE_FRAMES_EXPOSED 0
#endif

const void *
le_monitoring_version_address(void)
{
    return &LE_MONITORING_VERSION(PyInterpreterState_Main());
}

/* sys.monitoring has an event on, for any tool, in the interpreter: its
   global monitors (sys.settrace and sys.setprofile are tools too). Events
   set on one code object (set_local_events) are not seen. */
int
le_monitoring_on(PyThreadState *ts)
{
    const uint8_t *t = ts->interp->monitors.tools;
    uint64_t a, b;
    memcpy(&a, t, 8);
    memcpy(&b, t + sizeof(ts->interp->monitors.tools) - 8, 8);
    return (a | b) != 0;
}

/* compiled code's exact test, once the version has moved, which brings
   le_quiet_version up to date */
int
le_tracing_recheck(void)
{
    PyThreadState *ts = PyThreadState_GET();
    if (le_monitoring_on(ts))
        return 1;
    if (LE_FRAMES_EXPOSED)
        return 1;
    if (ts->interp == PyInterpreterState_Main())
        le_quiet_version = LE_MONITORING_VERSION(ts->interp);
    return 0;
}
#endif
#if LE_HAS_OPCODE_LOAD_COMMON_CONSTANT
/* LOAD_COMMON_CONSTANT's object: AssertionError, tuple, all and the rest,
   held by the interpreter */
PyObject *
le_common_constant(int i)
{
    return PyInterpreterState_Get()->common_consts[i];
}
#endif
#if LE_HAS_INTERPRETER_FRAME
/* PyFrame_New makes the frame object a function of its own, which
   CPython's frames for a call don't have. Its code is made an empty one,
   so that gc.get_referrers(code) finds only the code's real functions
   (trace names a caller's class by them): the frame keeps its own code,
   and these frames run in ceval only past their first instruction, so
   the function's code isn't read. */
static void
function_without_code(PyFrameObject *f)
{
    static PyCodeObject *empty = NULL;
#if LE_HAS_FRAME_STACKREFS
    PyFunctionObject *func =
        (PyFunctionObject *)PyStackRef_AsPyObjectBorrow(f->f_frame->f_funcobj);
#elif LE_HAS_FRAME_FUNCOBJ
    PyFunctionObject *func = (PyFunctionObject *)f->f_frame->f_funcobj;
#else
    PyFunctionObject *func = f->f_frame->f_func;
#endif
    if (empty == NULL && (empty = PyCode_NewEmpty("", "", 0)) == NULL) {
        PyErr_Clear();
        return;
    }
    Py_INCREF(empty);
    Py_SETREF(func->func_code, (PyObject *)empty);
}
#endif

/* PyFrame_New, unseen by function watchers: it makes a function for the
   frame object, which CPython's own frames don't, and a watcher called
   here would run Python code while compiled code's exception is set */
PyFrameObject *
le_pyframe_new(PyThreadState *ts, PyCodeObject *co, PyObject *globals)
{
    PyFrameObject *f;
#ifdef PY_FOREACH_FUNC_EVENT
    uint8_t watchers = ts->interp->active_func_watchers;
    ts->interp->active_func_watchers = 0;
    f = PyFrame_New(ts, co, globals, NULL);
    ts->interp->active_func_watchers = watchers;
#else
    f = PyFrame_New(ts, co, globals, NULL);
#endif
#if LE_HAS_INTERPRETER_FRAME
    if (f != NULL)
        function_without_code(f);
#endif
    return f;
}
/* le_eval_breaker_address, le_eval_breaker_handler,
   le_eval_breaker_at_entry */
#if LE_HAS_TSTATE_EVAL_BREAKER
/* Each thread has its own eval_breaker, in its state: the bits under
   _PY_EVAL_EVENTS_MASK are the events (signals, pending calls, a GIL drop,
   an async exception, the GC), the bits above them the instrumentation
   version, which is not an event. ceval handles the events with
   _Py_HandlePending, which libpython exports. Compiled code loads the
   thread state and tests the events' byte (psyfunc.c:le_eval_breaker_test);
   there is no address fixed for the process. */
#if !LE_HAS_HANDLE_PENDING_LINKABLE
#error "LeJIT: a per-thread eval_breaker, but _Py_HandlePending is hidden"
#endif
_Static_assert(_PY_EVAL_EVENTS_MASK == 0xff,
               "LeJIT: the eval_breaker's events are tested as its low byte");

/* -1 with an exception set */
int
le_eval_breaker_handler(void)
{
    return _Py_HandlePending(PyThreadState_GET());
}

/* whether an event is flagged for the thread (eval_hook.c:le_gen_to_run) */
int
le_eval_breaker_pending(void)
{
    return (_Py_atomic_load_uintptr_relaxed(
                &PyThreadState_GET()->eval_breaker) &
            _PY_EVAL_EVENTS_MASK) != 0;
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:LeCode_Run). Compiled callers
   test after each call instead. -1 with the exception set, for the caller
   to give the frame a traceback entry, as ceval's error path would. */
int
le_eval_breaker_at_entry(PyCodeObject *co)
{
    PyThreadState *ts = PyThreadState_GET();
    (void)co;
    if (!(_Py_atomic_load_uintptr_relaxed(&ts->eval_breaker) &
          _PY_EVAL_EVENTS_MASK))
        return 0;
    return _Py_HandlePending(ts);
}
#elif LE_HAS_EVAL_BREAKER_GC
/* eval_breaker in the interpreter's ceval state, and what ceval does when
   it is set: the hidden _Py_HandlePending (3.12 Python/ceval_gil.c), which
   also runs the cyclic GC when the interpreter's gc_scheduled asks.
   le_eval_breaker_handler does the same with exported functions:
   _PyEval_MakePendingCalls handles the signals (in the main thread) and
   runs the pending calls, as ceval does when either is flagged;
   PyErr_CheckSignals runs the GC; PyEval_SaveThread and
   PyEval_RestoreThread are the GIL drop and take sequence; and eval_breaker
   is recomputed at the end, as ceval does (le_compute_eval_breaker, its
   COMPUTE_EVAL_BREAKER). The word's address is the compiling thread's
   interpreter's, fixed for the process as long as LeJIT runs in one
   interpreter. */
_Static_assert(
    __builtin_types_compatible_p(
        __typeof__(((PyInterpreterState *)0)->ceval.eval_breaker),
        _Py_atomic_int) &&
        sizeof(((PyInterpreterState *)0)->ceval.eval_breaker) == 4,
    "LeJIT: the interpreter's ceval.eval_breaker is tested as a 32-bit int");

static struct _ceval_state *
le_ceval2(void)
{
    return &PyThreadState_GET()->interp->ceval;
}

/* compiled code compares it with 0 as a 32-bit int, at loop heads and
   after calls */
void *
le_eval_breaker_address(void)
{
    return &le_ceval2()->eval_breaker;
}

static void
le_compute_eval_breaker(PyInterpreterState *interp)
{
    struct _ceval_runtime_state *ceval = &_PyRuntime.ceval;
    struct _ceval_state *ceval2 = &interp->ceval;
    _Py_atomic_store_relaxed(
        &ceval2->eval_breaker,
        _Py_atomic_load_relaxed(&ceval2->gil_drop_request) |
            (_Py_atomic_load_relaxed(&ceval->signals_pending) &&
             _Py_ThreadCanHandleSignals(interp)) |
            _Py_atomic_load_relaxed(&ceval2->pending.calls_to_do) |
            (_Py_IsMainThread() && _Py_IsMainInterpreter(interp) &&
             _Py_atomic_load_relaxed(&ceval->pending_mainthread.calls_to_do)) |
            ceval2->pending.async_exc |
            _Py_atomic_load_relaxed(&ceval2->gc_scheduled));
}

/* -1 with an exception set */
int
le_eval_breaker_handler(void)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;
    struct _ceval_runtime_state *ceval = &_PyRuntime.ceval;
    struct _ceval_state *ceval2 = &interp->ceval;
    if (_Py_atomic_load_relaxed(&ceval->signals_pending) ||
        _Py_atomic_load_relaxed(&ceval2->pending.calls_to_do) ||
        (_Py_IsMainThread() && _Py_IsMainInterpreter(interp) &&
         _Py_atomic_load_relaxed(&ceval->pending_mainthread.calls_to_do))) {
        if (_PyEval_MakePendingCalls(tstate) < 0)
            return -1;
    }
    if (_Py_atomic_load_relaxed(&ceval2->gc_scheduled)) {
        if (PyErr_CheckSignals() < 0)
            return -1;
    }
    if (_Py_atomic_load_relaxed(&ceval2->gil_drop_request))
        PyEval_RestoreThread(PyEval_SaveThread());
    if (tstate->async_exc != NULL) {
        PyObject *exc = tstate->async_exc;
        tstate->async_exc = NULL;
        /* UNSIGNAL_ASYNC_EXC */
        ceval2->pending.async_exc = 0;
        le_compute_eval_breaker(interp);
        PyErr_SetNone(exc);
        Py_DECREF(exc);
        return -1;
    }
    le_compute_eval_breaker(interp);
    return 0;
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:LeCode_Run). Compiled callers
   test after each call instead. -1 with the exception set, for the caller
   to give the frame a traceback entry, as ceval's error path would. */
int
le_eval_breaker_at_entry(PyCodeObject *co)
{
    (void)co;
    if (!_Py_atomic_load_relaxed(&le_ceval2()->eval_breaker))
        return 0;
    return le_eval_breaker_handler();
}
#elif LE_HAS_EVAL_FRAME_HANDLE_PENDING
/* eval_breaker in the interpreter's ceval state, and what ceval does when
   it is set: the static ceval.c:eval_frame_handle_pending, with signals
   flagged in the runtime's ceval state (signals_pending).
   le_eval_breaker_handler is a copy of that function (3.9
   Python/ceval.c): handle_signals and make_pending_calls, each when
   flagged, are what Py_MakePendingCalls runs (each returns at once in a
   thread that may not handle it); PyEval_SaveThread and
   PyEval_RestoreThread are the GIL drop and take sequence. The word's
   address is the compiling thread's interpreter's, fixed for the process
   as long as LeJIT runs in one interpreter. */
_Static_assert(
    __builtin_types_compatible_p(
        __typeof__(((PyInterpreterState *)0)->ceval.eval_breaker),
        _Py_atomic_int) &&
        sizeof(((PyInterpreterState *)0)->ceval.eval_breaker) == 4,
    "LeJIT: the interpreter's ceval.eval_breaker is tested as a 32-bit int");

static struct _ceval_state *
le_ceval2(void)
{
    return &PyThreadState_GET()->interp->ceval;
}

/* compiled code compares it with 0 as a 32-bit int, at loop heads and
   after calls */
void *
le_eval_breaker_address(void)
{
    return &le_ceval2()->eval_breaker;
}

/* -1 with an exception set */
int
le_eval_breaker_handler(void)
{
    PyThreadState *tstate = PyThreadState_GET();
    PyInterpreterState *interp = tstate->interp;
    struct _ceval_runtime_state *ceval = &_PyRuntime.ceval;
    struct _ceval_state *ceval2 = &interp->ceval;
    if (_Py_atomic_load_relaxed(&ceval->signals_pending) ||
        _Py_atomic_load_relaxed(&ceval2->pending.calls_to_do)) {
        if (Py_MakePendingCalls() < 0)
            return -1;
        /* Py_MakePendingCalls also runs make_pending_calls, which ceval
           runs only when a call is queued: it recomputes eval_breaker
           from the flags, so a signal arriving between its load and its
           store would wait for the next one. Set it again while a signal
           is pending. */
        if (_Py_atomic_load_relaxed(&ceval->signals_pending) &&
            _Py_ThreadCanHandleSignals(interp))
            _Py_atomic_store_relaxed(&ceval2->eval_breaker, 1);
    }
    if (_Py_atomic_load_relaxed(&ceval2->gil_drop_request))
        PyEval_RestoreThread(PyEval_SaveThread());
    if (tstate->async_exc != NULL) {
        PyObject *exc = tstate->async_exc;
        tstate->async_exc = NULL;
        /* UNSIGNAL_ASYNC_EXC(interp): COMPUTE_EVAL_BREAKER */
        ceval2->pending.async_exc = 0;
        _Py_atomic_store_relaxed(
            &ceval2->eval_breaker,
            _Py_atomic_load_relaxed(&ceval2->gil_drop_request) |
                (_Py_atomic_load_relaxed(&ceval->signals_pending) &&
                 _Py_ThreadCanHandleSignals(interp)) |
                (_Py_atomic_load_relaxed(&ceval2->pending.calls_to_do) &&
                 _Py_ThreadCanHandlePendingCalls()) |
                ceval2->pending.async_exc);
        PyErr_SetNone(exc);
        Py_DECREF(exc);
        return -1;
    }
    return 0;
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:LeCode_Run), skipping the
   instructions ceval's loop top skips. Compiled callers test after each
   call instead. -1 with the exception set, for the caller to give the
   frame a traceback entry, as ceval's error path would. */
int
le_eval_breaker_at_entry(PyCodeObject *co)
{
    int op;
    if (!_Py_atomic_load_relaxed(&le_ceval2()->eval_breaker))
        return 0;
    op = le_op(le_code(co), 0);
    if (op == SETUP_FINALLY || op == SETUP_WITH || op == BEFORE_ASYNC_WITH ||
        op == YIELD_FROM)
        return 0;
    return le_eval_breaker_handler();
}
#elif LE_HAS_SIGNALS_PENDING
/* eval_breaker in _PyRuntime.ceval, and what ceval does when it is set:
   inline at the top of _PyEval_EvalFrameDefault's loop, a signal flagged
   apart from the pending calls (signals_pending).
   le_eval_breaker_handler is a copy of that loop top (3.8 Python/ceval.c):
   handle_signals and make_pending_calls, each when flagged, are what
   Py_MakePendingCalls runs (both are no-ops when nothing is flagged);
   PyEval_SaveThread and PyEval_RestoreThread are the GIL drop and take
   sequence, with its exit when another thread is finalising. */
_Static_assert(
    __builtin_types_compatible_p(__typeof__(_PyRuntime.ceval.eval_breaker),
                                 _Py_atomic_int) &&
        sizeof(_PyRuntime.ceval.eval_breaker) == 4,
    "LeJIT: _PyRuntime.ceval.eval_breaker is tested as a 32-bit int");

/* compiled code compares it with 0 as a 32-bit int, at loop heads and
   after calls */
void *
le_eval_breaker_address(void)
{
    return &_PyRuntime.ceval.eval_breaker;
}

/* -1 with an exception set */
int
le_eval_breaker_handler(void)
{
    PyThreadState *tstate = PyThreadState_GET();
    struct _ceval_runtime_state *ceval = &_PyRuntime.ceval;
    if (_Py_atomic_load_relaxed(&ceval->signals_pending) ||
        _Py_atomic_load_relaxed(&ceval->pending.calls_to_do)) {
        if (Py_MakePendingCalls() < 0)
            return -1;
        /* Py_MakePendingCalls also runs make_pending_calls, which ceval
           runs only when a call is queued: it recomputes eval_breaker
           from the flags, so a signal arriving between its load and its
           store would wait for the next one. Set it again while a signal
           is pending. */
        if (_Py_atomic_load_relaxed(&ceval->signals_pending) &&
            PyThread_get_thread_ident() == _PyRuntime.main_thread &&
            tstate->interp == _PyRuntime.interpreters.main)
            _Py_atomic_store_relaxed(&ceval->eval_breaker, 1);
    }
    if (_Py_atomic_load_relaxed(&ceval->gil_drop_request))
        PyEval_RestoreThread(PyEval_SaveThread());
    if (tstate->async_exc != NULL) {
        PyObject *exc = tstate->async_exc;
        tstate->async_exc = NULL;
        /* UNSIGNAL_ASYNC_EXC(ceval): COMPUTE_EVAL_BREAKER */
        ceval->pending.async_exc = 0;
        _Py_atomic_store_relaxed(
            &ceval->eval_breaker,
            _Py_atomic_load_relaxed(&ceval->gil_drop_request) |
                _Py_atomic_load_relaxed(&ceval->signals_pending) |
                _Py_atomic_load_relaxed(&ceval->pending.calls_to_do) |
                ceval->pending.async_exc);
        PyErr_SetNone(exc);
        Py_DECREF(exc);
        return -1;
    }
    return 0;
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:LeCode_Run), skipping the
   instructions ceval's loop top skips. Compiled callers test after each
   call instead. -1 with the exception set, for the caller to give the
   frame a traceback entry, as ceval's error path would. */
int
le_eval_breaker_at_entry(PyCodeObject *co)
{
    int op;
    if (!_Py_atomic_load_relaxed(&_PyRuntime.ceval.eval_breaker))
        return 0;
    op = le_op(le_code(co), 0);
    if (op == SETUP_FINALLY || op == SETUP_WITH || op == BEFORE_ASYNC_WITH ||
        op == YIELD_FROM)
        return 0;
    return le_eval_breaker_handler();
}
#else
/* eval_breaker in _PyRuntime.ceval, and what ceval does when it is set:
   inline at the top of _PyEval_EvalFrameDefault's loop, a signal coming as
   a pending call.
   le_eval_breaker_handler is a copy of that loop top (3.7 Python/ceval.c):
   PyEval_SaveThread and PyEval_RestoreThread are its GIL drop and take
   sequence, with its exit when another thread is finalising. */
_Static_assert(
    __builtin_types_compatible_p(__typeof__(_PyRuntime.ceval.eval_breaker),
                                 _Py_atomic_int) &&
        sizeof(_PyRuntime.ceval.eval_breaker) == 4,
    "LeJIT: _PyRuntime.ceval.eval_breaker is tested as a 32-bit int");

/* compiled code compares it with 0 as a 32-bit int, at loop heads and
   after calls */
void *
le_eval_breaker_address(void)
{
    return &_PyRuntime.ceval.eval_breaker;
}

/* -1 with an exception set */
int
le_eval_breaker_handler(void)
{
    PyThreadState *tstate = PyThreadState_GET();
    if (_Py_atomic_load_relaxed(&_PyRuntime.ceval.pending.calls_to_do)) {
        if (Py_MakePendingCalls() < 0)
            return -1;
    }
    if (_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil_drop_request))
        PyEval_RestoreThread(PyEval_SaveThread());
    if (tstate->async_exc != NULL) {
        PyObject *exc = tstate->async_exc;
        tstate->async_exc = NULL;
        /* UNSIGNAL_ASYNC_EXC() */
        _PyRuntime.ceval.pending.async_exc = 0;
        _Py_atomic_store_relaxed(
            &_PyRuntime.ceval.eval_breaker,
            _Py_atomic_load_relaxed(&_PyRuntime.ceval.gil_drop_request) |
                _Py_atomic_load_relaxed(
                    &_PyRuntime.ceval.pending.calls_to_do) |
                _PyRuntime.ceval.pending.async_exc);
        PyErr_SetNone(exc);
        Py_DECREF(exc);
        return -1;
    }
    return 0;
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:LeCode_Run), skipping the
   instructions ceval's loop top skips. Compiled callers test after each
   call instead. -1 with the exception set, for the caller to give the
   frame a traceback entry, as ceval's error path would. */
int
le_eval_breaker_at_entry(PyCodeObject *co)
{
    int op;
    if (!_Py_atomic_load_relaxed(&_PyRuntime.ceval.eval_breaker))
        return 0;
    op = le_op(le_code(co), 0);
    if (op == SETUP_FINALLY || op == SETUP_WITH || op == BEFORE_ASYNC_WITH ||
        op == YIELD_FROM)
        return 0;
    return le_eval_breaker_handler();
}
#endif
/* le_recursion_limit_address, where the limit is internal */
#if LE_HAS_INTERP_RECURSION_LIMIT
/* The recursion limit is the interpreter's ceval.recursion_limit, which
   sys.setrecursionlimit sets. Compiled code compares the depth with it in
   place; its address is the compiling thread's interpreter's, fixed for
   the process as long as LeJIT runs in one interpreter. */
_Static_assert(
    __builtin_types_compatible_p(
        __typeof__(((PyInterpreterState *)0)->ceval.recursion_limit), int),
    "LeJIT: the interpreter's ceval.recursion_limit is read as an int");

int *
le_recursion_limit_address(void)
{
    return &PyThreadState_GET()->interp->ceval.recursion_limit;
}
#else
/* The recursion limit is where LeJIT's build sees it: nothing to give
   here. */
#endif

/* where the interpreter's frame-evaluation function is, which compiled
   code compares with LeJIT's before a direct call
   (mechanisms/tracing.h:le_direct_call_refused) */
_Static_assert(sizeof(((PyInterpreterState *)0)->eval_frame) == 8,
               "LeJIT: the interpreter's eval_frame is read as a word");
const long le_interp_eval_frame_at = offsetof(PyInterpreterState, eval_frame);

/* the interpreter's frame-evaluation function, PEP 523's hook
   (eval_hook.c) */
void *
le_interp_eval_frame(void)
{
    return (void *)PyThreadState_Get()->interp->eval_frame;
}

void
le_set_interp_eval_frame(void *fn)
{
    PyThreadState_Get()->interp->eval_frame = (_PyFrameEvalFunction)fn;
}

#if LE_HAS_DEALLOC_HOOKS
/* The ref tracer's function, where _Py_Dealloc reads it, or NULL. Where
   none is set, _Py_Dealloc only calls tp_dealloc (where it can defer a free
   to the trashcan, for a type the GC doesn't track), which compiled code
   then calls itself (x64/ipyencoding.c:decref_dealloc_calling). The word is
   checked once against PyRefTracer_SetTracer, the old tracer put back
   straight after. */
static int
le_tracer_probe(PyObject *op, PyRefTracerEvent event, void *data)
{
    (void)op;
    (void)event;
    (void)data;
    return 0;
}

void *
le_ref_tracer_word(void)
{
    static int found = 0; /* 1 found, -1 not */
    PyRefTracer *word = &_PyRuntime.ref_tracer.tracer_func;
    if (found == 0) {
        void *data;
        PyRefTracer old = PyRefTracer_GetTracer(&data);
        found = -1;
        if (*word == old &&
            PyRefTracer_SetTracer(le_tracer_probe, NULL) == 0) {
            if (*word == le_tracer_probe)
                found = 1;
            PyRefTracer_SetTracer(old, data);
        }
    }
    return found > 0 ? (void *)word : NULL;
}
#endif
