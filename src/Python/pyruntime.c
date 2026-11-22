/* Built with Py_BUILD_CORE, for what the runtime and
   interpreter states (_PyRuntime, PyInterpreterState, in the internal
   headers) hold, which the extension headers don't describe. Which header
   that is, where the current thread state and eval_breaker are, and the
   copy of what ceval does when eval_breaker is set, depend on the target's
   source (the LE_HAS_ features below). */
#define Py_BUILD_CORE
#include <Python.h>
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
#include "frameobject.h"
#include "opcode.h"
#include "../bytecode.h"

/* le_tstate_current_address */
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
/* le_eval_breaker_address, le_eval_breaker_handler,
   le_eval_breaker_at_entry */
#if LE_HAS_EVAL_FRAME_HANDLE_PENDING
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
