/* What Python/pyruntime.c gives the rest of LeJIT, and the few things it
   takes from it. pyruntime.c is built with Py_BUILD_CORE, so it can't
   include psyco.h: this header uses only CPython's public headers. */
#ifndef _PYRUNTIME_H
#define _PYRUNTIME_H

#include <Python.h>
#include <frameobject.h>
#include <stdbool.h>
#include <stdint.h>
#include "le_features_gen.h"

#if LE_HAS_CTYPES_CALL_AUDITED
/* the two words PySys_Audit tests for a hook (Modules/pctypes.c) */
extern bool
le_audit_hook_words(void **runtime_head, void **interp_hooks);
#endif

#if LE_HAS_THREAD_LOCAL_TSTATE
/* the running thread's state's offset from the thread pointer, where it is
   one for every thread (mechanisms/tstate.h) */
extern bool
le_tstate_thread_offset(long *offset);
#else
/* the word that holds the running thread's state (mechanisms/tstate.h) */
extern void *
le_tstate_current_address(void);
#endif

#if LE_HAS_MONITORING
/* sys.monitoring's state, as compiled code tests it (mechanisms/tracing.h) */
extern uint64_t le_quiet_version;
extern const void *
le_monitoring_version_address(void);
extern const long le_monitors_tools_at, le_monitors_tools_size;
extern int
le_monitoring_on(PyThreadState *ts);
extern int
le_code_monitored(PyCodeObject *co);
extern int
le_tracing_recheck(PyCodeObject *co);
#if ALL_CHECKS
extern long le_stat_tracing_rechecks;
#endif
#endif

#if LE_HAS_OPCODE_LOAD_COMMON_CONSTANT
extern PyObject *
le_common_constant(int i);
#endif

#if LE_HAS_EVAL_FRAME_POPS
/* the object the GC tracked last, or NULL */
extern PyObject *
le_gc_youngest(PyThreadState *ts);
#endif
/* PyFrame_New, with no function watcher told of the function it makes */
extern PyFrameObject *
le_pyframe_new_by_cpython(PyThreadState *ts, PyCodeObject *co,
                          PyObject *globals);

/* ceval's eval_breaker: the handler compiled code calls when it is set, the
   test at a frame's entry, and where it is when that is fixed for the
   process */
extern int
le_eval_breaker_handler(void);
extern int
le_eval_breaker_at_entry(PyThreadState *ts, PyCodeObject *co);
#if !LE_HAS_TSTATE_EVAL_BREAKER
extern void *
le_eval_breaker_address(void);
#endif

#if LE_HAS_INTERP_RECURSION_LIMIT
extern int *
le_recursion_limit_address(void);
#endif

/* the interpreter's frame-evaluation function, PEP 523's hook */
extern const long le_interp_eval_frame_at;
extern void *
le_interp_eval_frame(void);
extern void
le_set_interp_eval_frame(void *fn);

#if LE_HAS_DEALLOC_HOOKS
extern void *
le_ref_tracer_word(void);
#endif

#if LE_HAS_C_STACK_SOFT_LIMIT
extern const long le_c_stack_soft_limit_at;
extern void
le_c_stack_limits_set(PyThreadState *ts, uintptr_t base, uintptr_t top,
                      long room, uintptr_t saved[3]);
extern void
le_c_stack_limits_put_back(PyThreadState *ts, const uintptr_t saved[3]);
#endif

#if LE_HAS_DEALLOC_DEFERS_TO_TRASHCAN
extern bool
le_trashcan_words(long *tstate_at, long *soft_at, long *chain_at, long *room);
#endif

/* the functions compiled code calls to release the GIL and take it back
   (mechanisms/gil.h) */
extern void
le_gil_calls(void **release, void **take);

/* What pyruntime.c takes from the rest of LeJIT. */
#ifdef PyFrameLocalsProxy_Check
/* how many compiled frames' locals are their frame objects' now
   (frames.c:expose); compiled code tests it with tracing */
extern int psyco_frames_exposed;
#endif
/* fn(arg) with the thread's frames as Python sees them linked and current
   (frames.c), for signal handlers, which are given the current frame */
extern int
le_with_frames_linked(int (*fn)(void *), void *arg);
/* whether a pending call is one LeJIT queued itself (codemanager.c) */
extern bool
le_pending_is_own(int (*func)(void *));

#endif /* _PYRUNTIME_H */
