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

#if LE_HAS_CTYPES_CALL_AUDITED
/* the two words PySys_Audit's should_audit (Python/sysmodule.c) tests
   for a hook: the runtime's list and the main interpreter's. False where a
   dtrace probe can also want the event, which only PySys_Audit can tell
   (Modules/pctypes.c) */
bool
le_audit_hook_words(void **runtime_head, void **interp_hooks)
{
#ifdef WITH_DTRACE
    return false;
#else
    _Static_assert(
        sizeof(_PyRuntime.audit_hooks.head) == sizeof(void *) &&
            sizeof(((PyInterpreterState *)0)->audit_hooks) == sizeof(void *),
        "LeJIT: the audit hook lists are loaded as pointers");
    *runtime_head = &_PyRuntime.audit_hooks.head;
    *interp_hooks = &PyInterpreterState_Main()->audit_hooks;
    return true;
#endif
}
#endif

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
   holds PyThreadState_Get() while the thread is attached and not while
   it is detached (PyEval_SaveThread), which tells it from a word that
   keeps the thread's state bound for PyGILState_Ensure, and only if it
   is the only one; and a thread started for the test must find the
   object's block at the same offset from its own thread pointer, which
   dynamic TLS doesn't give. */
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

/* Where CPython also keeps the running thread's interpreter in a
   thread-local (_Py_tss_interp), set on attach and cleared on detach with
   the thread state: its offset from the thread pointer, 0 if there is no
   such word, -1 if more than one word could be it */
static long le_interp_tls_offset;

static bool
tstate_tls_find(long *offset)
{
    tls_find_t f;
    long block, other;
    PyThreadState *ts = PyThreadState_Get();
    PyThreadState **attached, **words;
    size_t i, n, found = 0, at = 0, ifound = 0, iat = 0;
    pthread_t t;
    if (!tls_block_offset(&f, &block))
        return false;
    n = f.size / sizeof(void *);
    words = f.block;
    attached = PyMem_RawMalloc(n * sizeof(void *));
    if (attached == NULL)
        return false;
    memcpy(attached, words, n * sizeof(void *));
    PyEval_SaveThread();
    for (i = 0; i < n; i++) {
        if (attached[i] == ts && words[i] != ts) {
            found++;
            at = i * sizeof(void *);
        }
        if ((void *)attached[i] == (void *)ts->interp &&
            (void *)words[i] != (void *)ts->interp) {
            ifound++;
            iat = i * sizeof(void *);
        }
    }
    PyEval_RestoreThread(ts);
    PyMem_RawFree(attached);
    if (found != 1 || pthread_create(&t, NULL, tls_test_thread, &other) != 0)
        return false;
    pthread_join(t, NULL);
    if (other != block)
        return false;
    *offset = block + (long)at;
    le_interp_tls_offset = ifound == 0   ? 0
                           : ifound == 1 ? block + (long)iat
                                         : -1;
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

/* where the interpreter's tools table is (mechanisms/tracing.h) */
const long le_monitors_tools_at = offsetof(PyInterpreterState, monitors.tools);
const long le_monitors_tools_size =
    sizeof(((PyInterpreterState *)0)->monitors.tools);

/* sys.monitoring has an event on, for any tool, in the interpreter: its
   global monitors (sys.settrace and sys.setprofile are tools too) */
int
le_monitoring_on(PyThreadState *ts)
{
    const uint8_t *t = ts->interp->monitors.tools;
    uint64_t a, b;
    memcpy(&a, t, 8);
    memcpy(&b, t + sizeof(ts->interp->monitors.tools) - 8, 8);
    return (a | b) != 0;
}

_Static_assert(8 < sizeof(((_PyCoMonitoringData *)0)->local_monitors.tools) &&
                   sizeof(((_PyCoMonitoringData *)0)->local_monitors.tools) <=
                       16,
               "LeJIT: a code object's own tools are read as two words");
/* sys.monitoring has an event on, for any tool, for code co alone
   (set_local_events) */
int
le_code_monitored(PyCodeObject *co)
{
    const _PyCoMonitoringData *m = co->_co_monitoring;
    const uint8_t *t;
    uint64_t a, b;
    if (m == NULL)
        return 0;
    t = m->local_monitors.tools;
    memcpy(&a, t, 8);
    memcpy(&b, t + sizeof(m->local_monitors.tools) - 8, 8);
    return (a | b) != 0;
}

#if ALL_CHECKS
long le_stat_tracing_rechecks = 0; /* read by lejit._core._stats */
#endif

/* compiled code's exact test for code co (NULL for none), once the
   version has moved, which brings le_quiet_version up to date */
int
le_tracing_recheck(PyCodeObject *co)
{
    PyThreadState *ts = PyThreadState_GET();
#if ALL_CHECKS
    le_stat_tracing_rechecks++;
#endif
    if (le_monitoring_on(ts))
        return 1;
    if (LE_FRAMES_EXPOSED)
        return 1;
    if (ts->interp == PyInterpreterState_Main())
        le_quiet_version = LE_MONITORING_VERSION(ts->interp);
    return co != NULL && le_code_monitored(co);
}
#endif
#if LE_HAS_OPCODE_LOAD_COMMON_CONSTANT
/* LOAD_COMMON_CONSTANT's object: AssertionError, tuple, all and the rest,
   held by the interpreter */
PyObject *
le_common_constant(int i)
{
#if LE_HAS_COMMON_CONSTS_STACKREFS
    return PyStackRef_AsPyObjectBorrow(
        PyInterpreterState_Get()->common_consts[i]);
#else
    return PyInterpreterState_Get()->common_consts[i];
#endif
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

#if LE_HAS_EVAL_FRAME_POPS
/* the object the GC tracked last, or NULL: the youngest generation's
   last, which follows its GC header */
PyObject *
le_gc_youngest(PyThreadState *ts)
{
    PyGC_Head *gen0 = ts->interp->gc.generation0;
    return gen0->_gc_prev == (uintptr_t)gen0
               ? NULL
               : (PyObject *)((PyGC_Head *)gen0->_gc_prev + 1);
}
#endif

/* no function watcher is told of the function PyFrame_New makes, as a
   watcher called here would run Python code while compiled code's
   exception is set */
PyFrameObject *
le_pyframe_new_by_cpython(PyThreadState *ts, PyCodeObject *co,
                          PyObject *globals)
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
/* fn(arg) with the thread's frames as Python sees them linked and current
   (frames.c), for signal handlers, which are given the current frame */
extern int
le_with_frames_linked(int (*fn)(void *), void *arg);
#if !LE_HAS_TSTATE_EVAL_BREAKER && !LE_HAS_EVAL_BREAKER_GC
static int
le_pending_calls(void *ignored)
{
    (void)ignored;
    return Py_MakePendingCalls();
}
#endif

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
/* the events above the low byte are the free-threaded build's alone */
#ifdef _PY_EVAL_PROCESS_DELAYED_BIT
#define LE_EVENTS_FREE_THREADED_ONLY _PY_EVAL_PROCESS_DELAYED_BIT
#else
#define LE_EVENTS_FREE_THREADED_ONLY 0
#endif
_Static_assert((_PY_EVAL_EVENTS_MASK & ~LE_EVENTS_FREE_THREADED_ONLY) == 0xff,
               "LeJIT: the eval_breaker's events are tested as its low byte");

/* -1 with an exception set */
static int
le_handle_pending(void *ts)
{
    return _Py_HandlePending((PyThreadState *)ts);
}

int
le_eval_breaker_handler(void)
{
    PyThreadState *ts = PyThreadState_GET();
    if (_Py_atomic_load_uintptr_relaxed(&ts->eval_breaker) &
        _PY_SIGNALS_PENDING_BIT)
        return le_with_frames_linked(le_handle_pending, ts);
    return _Py_HandlePending(ts);
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:LeCode_Run). A direct call
   from compiled code tests before it (psyfunc.c:psyco_call_pyfunc). -1 with
   the exception set, for the caller to give the frame a traceback entry, as
   ceval's error path would. */
int
le_eval_breaker_at_entry(PyThreadState *ts, PyCodeObject *co)
{
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

static int
le_make_pending_calls(void *ts)
{
    return _PyEval_MakePendingCalls((PyThreadState *)ts);
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
        if ((_Py_atomic_load_relaxed(&ceval->signals_pending)
                 ? le_with_frames_linked(le_make_pending_calls, tstate)
                 : _PyEval_MakePendingCalls(tstate)) < 0)
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
   entered through the eval hook (eval_hook.c:LeCode_Run). A direct call
   from compiled code tests before it (psyfunc.c:psyco_call_pyfunc). -1 with
   the exception set, for the caller to give the frame a traceback entry, as
   ceval's error path would. */
int
le_eval_breaker_at_entry(PyThreadState *ts, PyCodeObject *co)
{
    (void)ts;
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

/* COMPUTE_EVAL_BREAKER */
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
            (_Py_atomic_load_relaxed(&ceval2->pending.calls_to_do) &&
             _Py_ThreadCanHandlePendingCalls()) |
            ceval2->pending.async_exc);
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
        if ((_Py_atomic_load_relaxed(&ceval->signals_pending)
                 ? le_with_frames_linked(le_pending_calls, NULL)
                 : Py_MakePendingCalls()) < 0)
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
        /* UNSIGNAL_ASYNC_EXC(interp) */
        ceval2->pending.async_exc = 0;
        le_compute_eval_breaker(interp);
        PyErr_SetNone(exc);
        Py_DECREF(exc);
        return -1;
    }
    return 0;
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:LeCode_Run), skipping the
   instructions ceval's loop top skips. A direct call from compiled code
   tests before it (psyfunc.c:psyco_call_pyfunc). -1 with the exception
   set, for the caller to give the frame a traceback entry, as ceval's error
   path would. */
int
le_eval_breaker_at_entry(PyThreadState *ts, PyCodeObject *co)
{
    int op;
    (void)ts;
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

/* COMPUTE_EVAL_BREAKER */
static void
le_compute_eval_breaker(PyInterpreterState *interp)
{
    struct _ceval_runtime_state *ceval = &_PyRuntime.ceval;
    (void)interp;
    _Py_atomic_store_relaxed(
        &ceval->eval_breaker,
        _Py_atomic_load_relaxed(&ceval->gil_drop_request) |
            _Py_atomic_load_relaxed(&ceval->signals_pending) |
            _Py_atomic_load_relaxed(&ceval->pending.calls_to_do) |
            ceval->pending.async_exc);
}

/* -1 with an exception set */
int
le_eval_breaker_handler(void)
{
    PyThreadState *tstate = PyThreadState_GET();
    struct _ceval_runtime_state *ceval = &_PyRuntime.ceval;
    if (_Py_atomic_load_relaxed(&ceval->signals_pending) ||
        _Py_atomic_load_relaxed(&ceval->pending.calls_to_do)) {
        /* handle_signals runs only in the main thread of the main
           interpreter, though eval_breaker is set in every thread */
        if ((_Py_atomic_load_relaxed(&ceval->signals_pending) &&
                     PyThread_get_thread_ident() == _PyRuntime.main_thread &&
                     tstate->interp == _PyRuntime.interpreters.main
                 ? le_with_frames_linked(le_pending_calls, NULL)
                 : Py_MakePendingCalls()) < 0)
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
        /* UNSIGNAL_ASYNC_EXC(ceval) */
        ceval->pending.async_exc = 0;
        le_compute_eval_breaker(tstate->interp);
        PyErr_SetNone(exc);
        Py_DECREF(exc);
        return -1;
    }
    return 0;
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:LeCode_Run), skipping the
   instructions ceval's loop top skips. A direct call from compiled code
   tests before it (psyfunc.c:psyco_call_pyfunc). -1 with the exception
   set, for the caller to give the frame a traceback entry, as ceval's error
   path would. */
int
le_eval_breaker_at_entry(PyThreadState *ts, PyCodeObject *co)
{
    int op;
    (void)ts;
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

/* COMPUTE_EVAL_BREAKER() */
static void
le_compute_eval_breaker(PyInterpreterState *interp)
{
    (void)interp;
    _Py_atomic_store_relaxed(
        &_PyRuntime.ceval.eval_breaker,
        _Py_atomic_load_relaxed(&_PyRuntime.ceval.gil_drop_request) |
            _Py_atomic_load_relaxed(&_PyRuntime.ceval.pending.calls_to_do) |
            _PyRuntime.ceval.pending.async_exc);
}

extern _Bool
le_pending_is_own(int (*func)(void *));

/* whether a pending call queued may be a signal's: one not LeJIT's own */
static int
le_foreign_call_queued(void)
{
    const struct _pending_calls *p = &_PyRuntime.ceval.pending;
    int i;
    for (i = p->first; i != p->last; i = (i + 1) % NPENDINGCALLS)
        if (!le_pending_is_own(p->calls[i].func))
            return 1;
    return 0;
}

/* -1 with an exception set */
int
le_eval_breaker_handler(void)
{
    PyThreadState *tstate = PyThreadState_GET();
    /* a signal is a pending call: its handler is given the frame. The
       calls run only in the main thread (once there are threads), though
       eval_breaker is set in every thread */
    if (_Py_atomic_load_relaxed(&_PyRuntime.ceval.pending.calls_to_do)) {
        unsigned long main = _PyRuntime.ceval.pending.main_thread;
        if (((main == 0 || PyThread_get_thread_ident() == main) &&
                     le_foreign_call_queued()
                 ? le_with_frames_linked(le_pending_calls, NULL)
                 : Py_MakePendingCalls()) < 0)
            return -1;
    }
    if (_Py_atomic_load_relaxed(&_PyRuntime.ceval.gil_drop_request))
        PyEval_RestoreThread(PyEval_SaveThread());
    if (tstate->async_exc != NULL) {
        PyObject *exc = tstate->async_exc;
        tstate->async_exc = NULL;
        /* UNSIGNAL_ASYNC_EXC() */
        _PyRuntime.ceval.pending.async_exc = 0;
        le_compute_eval_breaker(tstate->interp);
        PyErr_SetNone(exc);
        Py_DECREF(exc);
        return -1;
    }
    return 0;
}

/* The same test before a frame's first instruction, for compiled code
   entered through the eval hook (eval_hook.c:LeCode_Run), skipping the
   instructions ceval's loop top skips. A direct call from compiled code
   tests before it (psyfunc.c:psyco_call_pyfunc). -1 with the exception
   set, for the caller to give the frame a traceback entry, as ceval's error
   path would. */
int
le_eval_breaker_at_entry(PyThreadState *ts, PyCodeObject *co)
{
    int op;
    (void)ts;
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

#if LE_HAS_C_STACK_SOFT_LIMIT
#include "internal/pycore_tstate.h"
/* where a thread's C stack limit is (mechanisms/recursion.h) */
const long le_c_stack_soft_limit_at =
    offsetof(_PyThreadStateImpl, c_stack_soft_limit);

#include "internal/pycore_pythonrun.h"
/* A thread's C stack limits set for the stack from 'base' up to 'top', as
   CPython sets them for a thread's own (_Py_InitializeRecursionLimits),
   but with no more than 'room' above the soft limit, with the old ones kept
   in 'saved'; and put back from there (segments.c) */
void
le_c_stack_limits_set(PyThreadState *ts, uintptr_t base, uintptr_t top,
                      long room, uintptr_t saved[3])
{
    _PyThreadStateImpl *t = (_PyThreadStateImpl *)ts;
    uintptr_t soft = base + _PyOS_STACK_MARGIN_BYTES * 2;
    saved[0] = t->c_stack_top;
    saved[1] = t->c_stack_soft_limit;
    saved[2] = t->c_stack_hard_limit;
    if (room < 0)
        room = 0;
    if ((uintptr_t)room < top - soft)
        soft = top - room;
    t->c_stack_top = top;
    t->c_stack_soft_limit = soft;
    t->c_stack_hard_limit = soft - _PyOS_STACK_MARGIN_BYTES;
}

void
le_c_stack_limits_put_back(PyThreadState *ts, const uintptr_t saved[3])
{
    _PyThreadStateImpl *t = (_PyThreadStateImpl *)ts;
    t->c_stack_top = saved[0];
    t->c_stack_soft_limit = saved[1];
    t->c_stack_hard_limit = saved[2];
}
#endif

/* le_gil_release, le_gil_take: what Py_BEGIN_ALLOW_THREADS and
   Py_END_ALLOW_THREADS do (PyEval_SaveThread, PyEval_RestoreThread), for
   compiled code, which has the running thread's state already
   (mechanisms/gil.h). The common case is CPython's own code, copied: the
   thread state's detach and attach (Python/pystate.c), drop_gil and
   take_gil (Python/ceval_gil.c, or ceval_gil.h) and the eval_breaker
   update take_gil ends with. Where each word is comes from features of the
   GIL's layout (tools/features/features.py:gil_per_interpreter and the
   detectors after it) and of eval_breaker's. Everything else is left to
   CPython's own function: the GIL held by another thread, a finalising
   runtime or interpreter, a thread state not as the copy expects it, and
   the builds whose versions of these functions differ (debug,
   free-threaded, statistics, isolated subinterpreters).

   One saving beyond the copy, with the same behaviour: when the GIL is
   free and this thread was its last holder, no thread waits on
   switch_cond (drop_gil waits there only while it is itself the last
   holder, and the take that ended that signalled it), so the switch mutex
   and its signal are skipped. */
#include <errno.h>
#include <pthread.h>
#if defined(Py_DEBUG) || defined(Py_GIL_DISABLED) || defined(Py_STATS) || \
    defined(EXPERIMENTAL_ISOLATED_SUBINTERPRETERS) ||                     \
    !defined(FORCE_SWITCHING)
#define LE_GIL_COPY 0
#else
#define LE_GIL_COPY 1
#endif

#if LE_GIL_COPY
/* relaxed atomics, as CPython's _Py_atomic_*_relaxed: the GIL's words are
   a _Py_atomic_int or _Py_atomic_address (the value alone in a struct), or
   a plain int and pointer */
#define LE_LOAD(p) __atomic_load_n((p), __ATOMIC_RELAXED)
#define LE_STORE(p, v) __atomic_store_n((p), (v), __ATOMIC_RELAXED)
#define LE_INT(field) ((int *)&(field))
#define LE_TSTATE(field) ((PyThreadState **)&(field))
_Static_assert(
    sizeof(((struct _gil_runtime_state *)0)->locked) == sizeof(int) &&
        sizeof(((struct _gil_runtime_state *)0)->last_holder) ==
            sizeof(void *) &&
        __builtin_types_compatible_p(
            __typeof__(((struct _gil_runtime_state *)0)->mutex),
            pthread_mutex_t) &&
        __builtin_types_compatible_p(
            __typeof__(((struct _gil_runtime_state *)0)->cond),
            pthread_cond_t),
    "LeJIT: the GIL is an int, a pointer and pthread's mutex and cond");

#define LE_MUTEX_LOCK(m)                                  \
    do {                                                  \
        if (pthread_mutex_lock(&(m)))                     \
            Py_FatalError("PyMUTEX_LOCK(" #m ") failed"); \
    } while (0)
#define LE_MUTEX_UNLOCK(m)                                  \
    do {                                                    \
        if (pthread_mutex_unlock(&(m)))                     \
            Py_FatalError("PyMUTEX_UNLOCK(" #m ") failed"); \
    } while (0)
#define LE_COND_SIGNAL(c)                                  \
    do {                                                   \
        if (pthread_cond_signal(&(c)))                     \
            Py_FatalError("PyCOND_SIGNAL(" #c ") failed"); \
    } while (0)

#if LE_HAS_GIL_PER_INTERPRETER
#define LE_GIL(ts) ((ts)->interp->ceval.gil)
#else
#define LE_GIL(ts) (&_PyRuntime.ceval.gil)
#endif

/* the word that holds the running thread's state */
#if LE_HAS_THREAD_LOCAL_TSTATE
static long le_gil_tls_offset;
#define LE_CURRENT() ((PyThreadState **)(thread_pointer() + le_gil_tls_offset))
#define LE_INTERP_CURRENT() \
    ((PyInterpreterState **)(thread_pointer() + le_interp_tls_offset))
#else
#define LE_CURRENT() LE_TSTATE(_PyRuntime.gilstate.tstate_current)
#endif

/* tstate_must_exit or _PyThreadState_MustExit can be true: CPython's
   take_gil then ends the thread. Read before the thread state, which can
   be freed memory then where the runtime's finalising thread is the test
   (a daemon thread after Py_Finalize). */
#if LE_HAS_GET_FINALIZING
#define LE_RUNTIME_FINALIZING() \
    (_PyRuntimeState_GetFinalizing(&_PyRuntime) != NULL)
#else
#define LE_RUNTIME_FINALIZING() (_PyRuntime.finalizing != NULL)
#endif
#if LE_HAS_INTERP_GET_FINALIZING
#define LE_FINALIZING(ts)       \
    (LE_RUNTIME_FINALIZING() || \
     _PyInterpreterState_GetFinalizing((ts)->interp))
#else
#define LE_FINALIZING(ts) LE_RUNTIME_FINALIZING()
#endif

/* a GIL drop request, which drop_gil waits on and take_gil resets */
#if LE_HAS_TSTATE_EVAL_BREAKER
#if LE_HAS_TSTATE_HOLDS_GIL
#define LE_HOLDS_GIL(ts) ((ts)->holds_gil)
#else
#define LE_HOLDS_GIL(ts) ((ts)->_status.holds_gil)
#endif
#define le_drop_requested(ts) \
    _Py_eval_breaker_bit_is_set(ts, _PY_GIL_DROP_REQUEST_BIT)
#define le_reset_drop_request(ts) \
    _Py_unset_eval_breaker_bit(ts, _PY_GIL_DROP_REQUEST_BIT)

/* the end of take_gil, update_eval_breaker_for_thread and
   copy_eval_breaker_bits; the main-thread test is _Py_IsMainThread's
   (PyThread_get_thread_ident is pthread_self) */
static inline void
le_gil_taken(PyThreadState *ts)
{
    PyInterpreterState *interp = ts->interp;
    const uintptr_t mask = ~(uintptr_t)_PY_EVAL_EVENTS_MASK;
    uintptr_t from, old;
#if LE_HAS_TSTATE_GIL_REQUESTED
    ts->gil_requested = 0;
#endif
    LE_HOLDS_GIL(ts) = 1;
    le_reset_drop_request(ts);
    if (_Py_atomic_load_int32_relaxed(&interp->ceval.pending.npending))
        _Py_set_eval_breaker_bit(ts, _PY_CALLS_TO_DO_BIT);
    else if ((unsigned long)pthread_self() == _PyRuntime.main_thread &&
             _Py_atomic_load_int32_relaxed(
                 &_PyRuntime.ceval.pending_mainthread.npending))
        _Py_set_eval_breaker_bit(ts, _PY_CALLS_TO_DO_BIT);
    from = _Py_atomic_load_uintptr_relaxed(
               &interp->ceval.instrumentation_version) &
           mask;
    old = _Py_atomic_load_uintptr_relaxed(&ts->eval_breaker);
    if (from != (old & mask)) {
        uintptr_t new_value;
        do {
            new_value = (old & ~mask) | from;
        } while (!_Py_atomic_compare_exchange_uintptr(&ts->eval_breaker, &old,
                                                      new_value));
    }
}
#else
#if LE_HAS_EVAL_BREAKER_GC || LE_HAS_EVAL_FRAME_HANDLE_PENDING
#define LE_CEVAL(ts) (&(ts)->interp->ceval)
#else
#define LE_CEVAL(ts) (&_PyRuntime.ceval)
#endif
#define le_drop_requested(ts) LE_LOAD(LE_INT(LE_CEVAL(ts)->gil_drop_request))

/* RESET_GIL_DROP_REQUEST */
static inline void
le_reset_drop_request(PyThreadState *ts)
{
    LE_STORE(LE_INT(LE_CEVAL(ts)->gil_drop_request), 0);
    le_compute_eval_breaker(ts->interp);
}

/* the end of take_gil */
static inline void
le_gil_taken(PyThreadState *ts)
{
    if (le_drop_requested(ts))
        le_reset_drop_request(ts);
#if LE_HAS_EVAL_BREAKER_GC || LE_HAS_EVAL_FRAME_HANDLE_PENDING
    /* where eval_breaker is the interpreter's, take_gil recomputes it
       always, for a signal another thread couldn't handle */
    else
        le_compute_eval_breaker(ts->interp);
#endif
    if (ts->async_exc != NULL) {
        /* SIGNAL_ASYNC_EXC */
        LE_CEVAL(ts)->pending.async_exc = 1;
        LE_STORE(LE_INT(LE_CEVAL(ts)->eval_breaker), 1);
    }
}
#endif

/* PyEval_SaveThread, for the running thread's state */
static void
le_gil_release(PyThreadState *ts)
{
    struct _gil_runtime_state *gil = LE_GIL(ts);
#if LE_HAS_TSTATE_ATTACH
    if (ts->critical_section != 0) {
        PyEval_SaveThread();
        return;
    }
#endif
    /* the detach: tstate_deactivate, tstate_set_detached,
       current_fast_clear, or _PyThreadState_Swap(NULL) */
#if LE_HAS_TSTATE_STATUS
    ts->_status.active = 0;
#endif
#if LE_HAS_TSTATE_ATTACH
    ts->state = _Py_THREAD_DETACHED;
#endif
    LE_STORE(LE_CURRENT(), NULL);
#if LE_HAS_THREAD_LOCAL_TSTATE
    if (le_interp_tls_offset != 0)
        LE_STORE(LE_INTERP_CURRENT(), NULL);
#endif
    /* drop_gil */
    if (!LE_LOAD(LE_INT(gil->locked)))
        Py_FatalError("drop_gil: GIL is not locked");
    LE_STORE(LE_TSTATE(gil->last_holder), ts);
    LE_MUTEX_LOCK(gil->mutex);
    LE_STORE(LE_INT(gil->locked), 0);
#if LE_HAS_TSTATE_EVAL_BREAKER
    LE_HOLDS_GIL(ts) = 0;
#endif
#if LE_HAS_TSTATE_GIL_REQUESTED
    ts->gil_requested = 0;
#endif
    LE_COND_SIGNAL(gil->cond);
    LE_MUTEX_UNLOCK(gil->mutex);
    if (le_drop_requested(ts)) {
        LE_MUTEX_LOCK(gil->switch_mutex);
        if (LE_LOAD(LE_TSTATE(gil->last_holder)) == ts) {
            le_reset_drop_request(ts);
            if (pthread_cond_wait(&gil->switch_cond, &gil->switch_mutex))
                Py_FatalError("PyCOND_WAIT(gil->switch_cond) failed");
        }
        LE_MUTEX_UNLOCK(gil->switch_mutex);
    }
}

/* PyEval_RestoreThread of the running thread's state */
static void
le_gil_take(PyThreadState *ts)
{
    int err = errno;
    struct _gil_runtime_state *gil;
    if (LE_FINALIZING(ts) || LE_LOAD(LE_CURRENT()) != NULL)
        goto cpython;
#if LE_HAS_TSTATE_STATUS
    if (!ts->_status.bound_gilstate)
        goto cpython;
#endif
#if LE_HAS_TSTATE_ATTACH
    if (LE_LOAD(&ts->state) != _Py_THREAD_DETACHED ||
        ts->critical_section != 0)
        goto cpython;
#endif
#if LE_HAS_C_STACK_SOFT_LIMIT
    /* _PyThreadState_Attach sets the limits up the first time */
    if (((_PyThreadStateImpl *)ts)->c_stack_hard_limit == 0)
        goto cpython;
#endif
    /* take_gil, when the GIL is free */
    gil = LE_GIL(ts);
    LE_MUTEX_LOCK(gil->mutex);
    if (LE_LOAD(LE_INT(gil->locked))) {
        LE_MUTEX_UNLOCK(gil->mutex);
        goto cpython;
    }
#if LE_HAS_TSTATE_GIL_REQUESTED
    ts->gil_requested = 1;
#endif
    /* the GIL is free and its mutex held, so last_holder is stable */
    if (LE_LOAD(LE_TSTATE(gil->last_holder)) == ts)
        LE_STORE(LE_INT(gil->locked), 1);
    else {
        LE_MUTEX_LOCK(gil->switch_mutex);
        LE_STORE(LE_INT(gil->locked), 1);
        LE_STORE(LE_TSTATE(gil->last_holder), ts);
        ++gil->switch_number;
        LE_COND_SIGNAL(gil->switch_cond);
        LE_MUTEX_UNLOCK(gil->switch_mutex);
    }
    if (LE_FINALIZING(ts)) {
        /* finalisation began meanwhile: the GIL given back, CPython's
           take_gil ends the thread */
        LE_STORE(LE_INT(gil->locked), 0);
#if LE_HAS_TSTATE_GIL_REQUESTED
        ts->gil_requested = 0;
#endif
        LE_COND_SIGNAL(gil->cond);
        LE_MUTEX_UNLOCK(gil->mutex);
        goto cpython;
    }
    le_gil_taken(ts);
    LE_MUTEX_UNLOCK(gil->mutex);
    errno = err;
    /* the attach: current_fast_set, tstate_activate, tstate_try_attach, or
       _PyThreadState_Swap(ts) */
    LE_STORE(LE_CURRENT(), ts);
#if LE_HAS_THREAD_LOCAL_TSTATE
    if (le_interp_tls_offset != 0)
        LE_STORE(LE_INTERP_CURRENT(), ts->interp);
#endif
#if LE_HAS_TSTATE_STATUS
    ts->_status.active = 1;
#endif
#if LE_HAS_TSTATE_ATTACH
    ts->state = _Py_THREAD_ATTACHED;
#endif
    return;
cpython:
    errno = err;
    PyEval_RestoreThread(ts);
}
#endif

static void
le_gil_release_cpython(PyThreadState *ts)
{
    (void)ts;
    PyEval_SaveThread();
}

/* the functions compiled code calls to release the GIL and take it back,
   each given the running thread's state (mechanisms/gil.h) */
void
le_gil_calls(void **release, void **take)
{
#if LE_GIL_COPY
#if LE_HAS_THREAD_LOCAL_TSTATE
    if (le_tstate_thread_offset(&le_gil_tls_offset) &&
        le_interp_tls_offset != -1)
#endif
    {
        *release = (void *)le_gil_release;
        *take = (void *)le_gil_take;
        return;
    }
#endif
    *release = (void *)le_gil_release_cpython;
    *take = (void *)PyEval_RestoreThread;
}
