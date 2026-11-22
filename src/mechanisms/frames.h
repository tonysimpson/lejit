#ifndef LE_MECHANISMS_FRAMES_H
#define LE_MECHANISMS_FRAMES_H
#if LE_HAS_INTERPRETER_FRAME
/* Frames are interpreter frames (_PyInterpreterFrame): the thread's
   innermost is tstate->cframe->current_frame, each links to its caller by
   previous, and a frame's arguments, locals, cells and free variables are
   its localsplus, its value stack after them. A frame is incomplete until
   ceval has run its first instructions (the copies of the free variables,
   the cells): Python never sees one, so walks skip it. A frame object
   (PyFrameObject) is made for a frame when something asks for it, and is
   a view of it (f_frame) while the thread runs it; one made by PyFrame_New
   owns its frame. le_frame_t is the frame entry hands to
   eval_hook.c:LeCode_Run and frames.c walks. Its position is the
   instruction before the next (prev_instr), its value stack's depth
   stacktop, counted from localsplus. A frame the thread runs is running,
   as frame.clear() sees it, by its owner; a frame object's own frame is
   never running. */
#define Py_BUILD_CORE
#include "internal/pycore_frame.h"
#undef Py_BUILD_CORE
/* its <stdbool.h> makes bool a macro for _Bool: LeJIT's bool is psyco.h's
   int */
#undef bool
typedef _PyInterpreterFrame le_frame_t;

static inline le_frame_t *
le_frame_current(PyThreadState *ts)
{
    return ts->cframe->current_frame;
}
static inline void
le_frame_set_current(PyThreadState *ts, le_frame_t *f)
{
    ts->cframe->current_frame = f;
}
/* the frame runs as the thread's innermost, called from the innermost
   before; ceval's prologue does this for a frame it runs */
static inline void
le_frame_push(PyThreadState *ts, le_frame_t *f)
{
    f->previous = ts->cframe->current_frame;
    ts->cframe->current_frame = f;
}
static inline le_frame_t *
le_frame_back(le_frame_t *f)
{
    return f->previous;
}
/* Python sees the frame (sys._getframe, frame.f_back) */
static inline int
le_frame_complete(le_frame_t *f)
{
    return !_PyFrame_IsIncomplete(f);
}
/* the frame's code starts running compiled: placed as ceval places it
   once past the instructions before the first traceable one (RESUME), so
   that it is complete */
static inline void
le_frame_start(le_frame_t *f)
{
    f->prev_instr = _PyCode_CODE(f->f_code) + f->f_code->_co_firsttraceable;
}
/* the frame object Python sees (borrowed), made if it has none:
   PyEval_GetFrame makes the current frame's, so the frame is made current
   for it. NULL with no exception set if it can't be made. */
static inline PyFrameObject *
le_frame_object(le_frame_t *f)
{
    PyThreadState *ts;
    le_frame_t *cur;
    PyFrameObject *o;
    if (f->frame_obj != NULL)
        return f->frame_obj;
    ts = PyThreadState_GET();
    cur = le_frame_current(ts);
    le_frame_set_current(ts, f);
    o = PyEval_GetFrame();
    le_frame_set_current(ts, cur);
    return o;
}
/* the frame a frame object is (its own, or the one it is a view of) */
static inline le_frame_t *
le_frame_of(PyFrameObject *f)
{
    return f->f_frame;
}
/* the frame object's caller as frame.f_back gives it (borrowed, NULL for
   none): the one it was linked to (f_back), else its frame's caller that
   Python sees */
static inline PyFrameObject *
le_frame_object_back(PyFrameObject *f)
{
    le_frame_t *p;
    if (f->f_back != NULL)
        return f->f_back;
    for (p = f->f_frame->previous; p != NULL && _PyFrame_IsIncomplete(p);)
        p = p->previous;
    return p == NULL ? NULL : le_frame_object(p);
}
static inline PyCodeObject *
le_frame_code(le_frame_t *f)
{
    return f->f_code;
}
static inline PyObject *
le_frame_globals(le_frame_t *f)
{
    return f->f_globals;
}
static inline PyObject *
le_frame_builtins(le_frame_t *f)
{
    return f->f_builtins;
}
/* the frame borrows its builtins (its function's), so the reference 'b'
   given is never released */
static inline void
le_frame_set_builtins(le_frame_t *f, PyObject *b)
{
    f->f_builtins = b;
}
/* the mapping its locals are run in, NULL for a function's */
static inline PyObject *
le_frame_locals(le_frame_t *f)
{
    return f->f_locals;
}
/* the argument words compiled code runs on, owned by the frame */
static inline PyObject **
le_frame_args(le_frame_t *f)
{
    return f->localsplus;
}
/* its value stack */
static inline PyObject **
le_frame_stack(le_frame_t *f)
{
    return f->localsplus + f->f_code->co_nlocalsplus;
}
/* f_lasti as CPython counts it (instructions), -1 before the first */
static inline int
le_frame_lasti(le_frame_t *f)
{
    return _PyInterpreterFrame_LASTI(f);
}
static inline void
le_frame_set_lasti(le_frame_t *f, int lasti)
{
    f->prev_instr = _PyCode_CODE(f->f_code) + lasti;
}
static inline void
le_frame_set_executing(le_frame_t *f, int on)
{
    (void)f;
    (void)on;
}
static inline int
le_frame_is_executing(PyFrameObject *f)
{
    return f->f_frame->owner == FRAME_OWNED_BY_THREAD;
}
/* nothing on the value stack, and ceval not running the frame: frames.c
   may keep its notes in the stack's first slots */
static inline int
le_frame_stack_empty(PyFrameObject *f)
{
    return !le_frame_is_executing(f) &&
           f->f_frame->stacktop == f->f_frame->f_code->co_nlocalsplus;
}
/* the stack ceval resumes the frame with: its first n slots */
static inline void
le_frame_set_stack_depth(PyFrameObject *f, int n)
{
    f->f_frame->stacktop = f->f_frame->f_code->co_nlocalsplus + n;
}
#else
/* Frames as Python sees them are PyFrameObjects: the thread's
   innermost is tstate->frame, each links to its caller by f_back, and a
   running frame's arguments and locals are its f_localsplus. le_frame_t is
   the frame entry hands to eval_hook.c:LeCode_Run and frames.c walks; here
   it is the frame object itself. Whether ceval runs a frame is its f_state
   (FRAME_EXECUTING) or f_executing, and its value stack's depth is
   f_stackdepth (-1 while ceval runs it) or f_stacktop (NULL then). */
typedef PyFrameObject le_frame_t;

static inline le_frame_t *
le_frame_current(PyThreadState *ts)
{
    return ts->frame;
}
static inline void
le_frame_set_current(PyThreadState *ts, le_frame_t *f)
{
    ts->frame = f;
}
/* the frame runs as the thread's innermost; its f_back is already the
   innermost before */
static inline void
le_frame_push(PyThreadState *ts, le_frame_t *f)
{
    ts->frame = f;
}
static inline le_frame_t *
le_frame_back(le_frame_t *f)
{
    return f->f_back;
}
/* the frame object's caller as frame.f_back gives it (borrowed, NULL for
   none) */
static inline PyFrameObject *
le_frame_object_back(PyFrameObject *f)
{
    return f->f_back;
}
/* Python sees the frame */
static inline int
le_frame_complete(le_frame_t *f)
{
    (void)f;
    return 1;
}
/* the frame's code starts running compiled */
static inline void
le_frame_start(le_frame_t *f)
{
    (void)f;
}
/* the frame object Python sees (borrowed) */
static inline PyFrameObject *
le_frame_object(le_frame_t *f)
{
    return f;
}
/* the frame a frame object is */
static inline le_frame_t *
le_frame_of(PyFrameObject *f)
{
    return f;
}
static inline PyCodeObject *
le_frame_code(le_frame_t *f)
{
    return f->f_code;
}
static inline PyObject *
le_frame_globals(le_frame_t *f)
{
    return f->f_globals;
}
static inline PyObject *
le_frame_builtins(le_frame_t *f)
{
    return f->f_builtins;
}
/* the frame holds a reference to its builtins: the old one goes, 'b' is
   stolen */
static inline void
le_frame_set_builtins(le_frame_t *f, PyObject *b)
{
    Py_XSETREF(f->f_builtins, b);
}
/* the mapping its locals are run in, NULL for a function's */
static inline PyObject *
le_frame_locals(le_frame_t *f)
{
    return f->f_locals;
}
/* the argument words compiled code runs on, owned by the frame */
static inline PyObject **
le_frame_args(le_frame_t *f)
{
    return f->f_localsplus;
}
/* its value stack */
static inline PyObject **
le_frame_stack(le_frame_t *f)
{
    return f->f_valuestack;
}
/* f_lasti as CPython counts it, -1 before the first instruction */
static inline int
le_frame_lasti(le_frame_t *f)
{
    return f->f_lasti;
}
static inline void
le_frame_set_lasti(le_frame_t *f, int lasti)
{
    f->f_lasti = lasti;
}
#if LE_HAS_FRAME_STATE
/* while set, frame.clear() refuses the frame; once unset the frame has
   returned, which frame.clear() accepts */
static inline void
le_frame_set_executing(le_frame_t *f, int on)
{
    f->f_state = on ? FRAME_EXECUTING : FRAME_RETURNED;
}
static inline int
le_frame_is_executing(PyFrameObject *f)
{
    return f->f_state == FRAME_EXECUTING;
}
/* nothing on the value stack, and ceval not running the frame: frames.c
   may keep its notes in the stack's first slots */
static inline int
le_frame_stack_empty(PyFrameObject *f)
{
    return f->f_stackdepth == 0;
}
/* the stack ceval resumes the frame with: its first n slots */
static inline void
le_frame_set_stack_depth(PyFrameObject *f, int n)
{
    f->f_stackdepth = n;
}
#else
/* while set, frame.clear() refuses the frame */
static inline void
le_frame_set_executing(le_frame_t *f, int on)
{
    f->f_executing = on;
}
static inline int
le_frame_is_executing(PyFrameObject *f)
{
    return f->f_executing;
}
/* nothing on the value stack, and ceval not running the frame (its
   f_stacktop is NULL then): frames.c may keep its notes in the stack's
   first slots */
static inline int
le_frame_stack_empty(PyFrameObject *f)
{
    return f->f_stacktop == f->f_valuestack;
}
/* the stack ceval resumes the frame with: its first n slots */
static inline void
le_frame_set_stack_depth(PyFrameObject *f, int n)
{
    f->f_stacktop = f->f_valuestack + n;
}
#endif
#endif

/* a frame object's code, and its frame's value stack */
static inline PyCodeObject *
le_pyframe_code(PyFrameObject *f)
{
    return le_frame_code(le_frame_of(f));
}
static inline PyObject **
le_pyframe_stack(PyFrameObject *f)
{
    return le_frame_stack(le_frame_of(f));
}
#endif /* LE_MECHANISMS_FRAMES_H */
