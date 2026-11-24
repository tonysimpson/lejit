#ifndef LE_MECHANISMS_FRAMES_H
#define LE_MECHANISMS_FRAMES_H
#if LE_HAS_INTERPRETER_FRAME
/* Frames are interpreter frames (_PyInterpreterFrame): the thread's
   innermost is LE_CURRENT_FRAME below, each links to its caller by
   previous, and a frame's arguments, locals, cells and free variables are
   its localsplus, its value stack after them. A frame is incomplete until
   ceval has run its first instructions (the copies of the free variables,
   the cells): Python never sees one, so walks skip it. A frame object
   (PyFrameObject) is made for a frame when something asks for it, and is
   a view of it (f_frame) while the thread runs it; one made by PyFrame_New
   owns its frame. le_frame_t is the frame entry hands to
   eval_hook.c:LeCode_Run and frames.c walks. Its position is the
   instruction at f_lasti (LE_FRAME_PLACE), its value stack's depth
   stacktop, counted from localsplus. A frame the thread runs is running,
   as frame.clear() sees it, by its owner; a frame object's own frame is
   never running. */
#include "code.h"
#define Py_BUILD_CORE
#include "internal/pycore_frame.h"
#if LE_HAS_PYCORE_INTERPFRAME_H
#include "internal/pycore_interpframe.h"
#endif
#if LE_HAS_FRAME_CLEAR_AND_POP_LINKABLE
/* _PyEval_FrameClearAndPop */
#include "internal/pycore_ceval.h"
#endif
#if LE_HAS_GENERATOR_RETURN_KIND
/* _PyThreadStateImpl */
#include "internal/pycore_tstate.h"
#endif
#undef Py_BUILD_CORE
typedef _PyInterpreterFrame le_frame_t;

/* the frame's code object, its place (the instruction at f_lasti) and
   the thread's innermost frame, by their fields */
#if LE_HAS_FRAME_STACKREFS
#define LE_FRAME_CODE(f) \
    ((PyCodeObject *)PyStackRef_AsPyObjectBorrow((f)->f_executable))
#elif LE_HAS_FRAME_EXECUTABLE
#define LE_FRAME_CODE(f) ((PyCodeObject *)(f)->f_executable)
#else
#define LE_FRAME_CODE(f) ((f)->f_code)
#endif
#if LE_HAS_FRAME_INSTR_PTR
#define LE_FRAME_PLACE_FIELD instr_ptr
#else
#define LE_FRAME_PLACE_FIELD prev_instr
#endif
#define LE_FRAME_PLACE(f) ((f)->LE_FRAME_PLACE_FIELD)
#if LE_HAS_TSTATE_CURRENT_FRAME
#define LE_CURRENT_FRAME(ts) ((ts)->current_frame)
#else
#define LE_CURRENT_FRAME(ts) ((ts)->cframe->current_frame)
#endif
/* its function (borrowed) where LE_HAS_FRAME_FUNCOBJ, and its word k
   (its locals, then its value stack): LE_FRAME_WORD the object (borrowed,
   NULL for none), le_frame_word_take the reference it holds (NULL for
   none), leaving none, and le_frame_word_put one it takes (NULL for none)
   where it holds none. Where the words are _PyStackRefs, one may be a
   reference it doesn't own (tagged), which take makes a new one of. */
#if LE_HAS_FRAME_STACKREFS
#define LE_FRAME_FUNCOBJ(f) PyStackRef_AsPyObjectBorrow((f)->f_funcobj)
#define LE_FRAME_WORD(f, k) PyStackRef_AsPyObjectBorrow((f)->localsplus[k])
static inline PyObject *
le_frame_word_take(le_frame_t *f, int k)
{
    _PyStackRef r = f->localsplus[k];
    f->localsplus[k] = PyStackRef_NULL;
    return PyStackRef_IsNull(r) ? NULL : PyStackRef_AsPyObjectSteal(r);
}
static inline void
le_frame_word_put(le_frame_t *f, int k, PyObject *o)
{
    f->localsplus[k] =
        o == NULL ? PyStackRef_NULL : PyStackRef_FromPyObjectSteal(o);
}
#else
#define LE_FRAME_FUNCOBJ(f) ((f)->f_funcobj)
#define LE_FRAME_WORD(f, k) ((f)->localsplus[k])
static inline PyObject *
le_frame_word_take(le_frame_t *f, int k)
{
    PyObject *o = f->localsplus[k];
    f->localsplus[k] = NULL;
    return o;
}
static inline void
le_frame_word_put(le_frame_t *f, int k, PyObject *o)
{
    f->localsplus[k] = o;
}
#endif
/* word k becomes 'o' (a reference it takes, NULL for none), and the one
   before goes */
static inline void
le_frame_word_set(le_frame_t *f, int k, PyObject *o)
{
    PyObject *old = le_frame_word_take(f, k);
    le_frame_word_put(f, k, o);
    Py_XDECREF(old);
}

static inline le_frame_t *
le_frame_current(PyThreadState *ts)
{
    return LE_CURRENT_FRAME(ts);
}
static inline void
le_frame_set_current(PyThreadState *ts, le_frame_t *f)
{
    LE_CURRENT_FRAME(ts) = f;
}
/* the frame runs as the thread's innermost, called from the innermost
   before; ceval's prologue does this for a frame it runs */
static inline void
le_frame_push(PyThreadState *ts, le_frame_t *f)
{
    f->previous = LE_CURRENT_FRAME(ts);
    LE_CURRENT_FRAME(ts) = f;
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
/* the closure of the frame's function */
static inline PyObject *
le_frame_closure(le_frame_t *f)
{
#if LE_HAS_FRAME_FUNCOBJ
    return PyFunction_GET_CLOSURE(LE_FRAME_FUNCOBJ(f));
#else
    return f->f_func->func_closure;
#endif
}
/* the frame's code starts running compiled: what the instructions before
   the first traceable one (RESUME) do, done here as ceval's MAKE_CELL and
   COPY_FREE_VARS do it, and the frame placed past them, so that it is
   complete; compiled code runs them as no-ops. -1 with an exception set
   if a cell can't be made. */
static inline int
le_frame_start(le_frame_t *f)
{
    PyCodeObject *co = LE_FRAME_CODE(f);
    int k, nfrees = co->co_nfreevars, n = co->co_nlocalsplus - nfrees;
    LE_FRAME_PLACE(f) = _PyCode_CODE(co) + co->_co_firsttraceable;
    for (k = 0; k < n; k++)
        if (_PyLocals_GetKind(co->co_localspluskinds, k) & CO_FAST_CELL) {
            PyObject *cell = PyCell_New(LE_FRAME_WORD(f, k));
            if (cell == NULL)
                return -1;
            le_frame_word_set(f, k, cell);
        }
    for (k = 0; k < nfrees; k++) {
        PyObject *o = PyTuple_GET_ITEM(le_frame_closure(f), k);
        le_frame_word_set(f, n + k, Py_NewRef(o));
    }
    return 0;
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
    return LE_FRAME_CODE(f);
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
/* the frame borrows its builtins from its function, which holds them;
   the frames given others are PyFrame_New's, whose function is their own,
   so the function's are replaced */
static inline void
le_frame_set_builtins(le_frame_t *f, PyObject *b)
{
    PyFunctionObject *func;
    if (f->f_builtins == b)
        return;
#if LE_HAS_FRAME_FUNCOBJ
    func = (PyFunctionObject *)LE_FRAME_FUNCOBJ(f);
#else
    func = f->f_func;
#endif
    Py_INCREF(b);
    Py_SETREF(func->func_builtins, b);
    f->f_builtins = b;
}
/* the mapping its locals are run in, NULL for a function's */
static inline PyObject *
le_frame_locals(le_frame_t *f)
{
    return f->f_locals;
}
/* the argument words compiled code runs on, owned by the frame, where
   they are plain objects; where they are _PyStackRefs, its words to write
   as objects, which le_frame_words_own then makes the frame's */
static inline PyObject **
le_frame_args(le_frame_t *f)
{
    return (PyObject **)f->localsplus;
}
/* its first n words, written through le_frame_args as objects (each a
   reference it holds, NULL for none), are the frame's own */
static inline void
le_frame_words_own(le_frame_t *f, int n)
{
#if LE_HAS_FRAME_STACKREFS
    int k;
    for (k = 0; k < n; k++)
        le_frame_word_put(f, k, ((PyObject **)f->localsplus)[k]);
#else
    (void)f;
    (void)n;
#endif
}
/* its value stack */
static inline PyObject **
le_frame_stack(le_frame_t *f)
{
    return (PyObject **)f->localsplus + LE_FRAME_CODE(f)->co_nlocalsplus;
}
/* f_lasti as CPython counts it (instructions), -1 before the first */
static inline int
le_frame_lasti(le_frame_t *f)
{
    return _PyInterpreterFrame_LASTI(f);
}
/* a place before the first traceable instruction (-1: before the first) is
   the first, as for a frame ceval has started, so the frame is complete */
static inline void
le_frame_set_lasti(le_frame_t *f, int lasti)
{
    if (lasti < LE_FRAME_CODE(f)->_co_firsttraceable)
        lasti = LE_FRAME_CODE(f)->_co_firsttraceable;
    LE_FRAME_PLACE(f) = _PyCode_CODE(LE_FRAME_CODE(f)) + lasti;
}
static inline void
le_frame_set_executing(le_frame_t *f, int on)
{
    (void)f;
    (void)on;
}
/* the frame is running, as frame.clear() sees it: the thread's, or a
   generator's while it runs */
static inline int
le_frame_executing(le_frame_t *f)
{
    if (f->owner == FRAME_OWNED_BY_GENERATOR)
        return ((PyGenObject *)((char *)f - offsetof(PyGenObject, gi_iframe)))
                   ->gi_frame_state == FRAME_EXECUTING;
    return f->owner == FRAME_OWNED_BY_THREAD;
}
static inline int
le_frame_is_executing(PyFrameObject *f)
{
    return le_frame_executing(f->f_frame);
}
/* the frame object's frame is its own (PyFrame_New's, or a frame's that
   has returned), not a view of the thread's or a generator's */
static inline int
le_pyframe_owns(PyFrameObject *f)
{
    return f->f_frame == (le_frame_t *)f->_f_frame_data;
}
/* A frame object made for a call compiled code runs (frames.c:kept_frame)
   is running while 'on', as the thread's frames are: frame.clear()
   refuses it, the GC leaves its locals to the call, and it is its own
   frame's frame object, which CPython looks for when it reaches the frame
   in the thread's chain. Off, it is a frame object's own frame again. */
static inline void
le_pyframe_set_executing(PyFrameObject *f, int on)
{
    if (!le_pyframe_owns(f))
        return;
    f->f_frame->owner =
        on ? FRAME_OWNED_BY_THREAD : FRAME_OWNED_BY_FRAME_OBJECT;
    f->f_frame->frame_obj = on ? f : NULL;
}
/* marks a frame object running only for frame.clear(), which then
   refuses it; le_pyframe_unmark_running puts back what this returns */
static inline int
le_pyframe_mark_running(PyFrameObject *f)
{
    int old = f->f_frame->owner;
    if (le_pyframe_owns(f))
        f->f_frame->owner = FRAME_OWNED_BY_THREAD;
    return old;
}
static inline void
le_pyframe_unmark_running(PyFrameObject *f, int old)
{
    f->f_frame->owner = (char)old;
}
/* ceval is to run the frame object's frame itself, as the thread's
   innermost (frames.c:psyco_resume_run): ceval links it to the thread's
   innermost, so the frame compiled code runs, if it is that one, is
   unlinked first; a frame object's own frame runs as kept frames do
   (le_pyframe_set_executing). What this returns goes to
   le_pyframe_eval_end. */
static inline int
le_pyframe_eval_begin(PyThreadState *ts, PyFrameObject *f)
{
    if (LE_CURRENT_FRAME(ts) == f->f_frame)
        LE_CURRENT_FRAME(ts) = f->f_frame->previous;
    if (!le_pyframe_owns(f) || le_frame_is_executing(f))
        return 0;
    le_pyframe_set_executing(f, 1);
    return 1;
}
/* ceval has run it: a frame object's own frame keeps the frame it was
   called from as f_back (if it has none), as a frame that outlives its
   call does, and is no longer in the thread's chain */
static inline void
le_pyframe_eval_end(PyFrameObject *f, int marked)
{
    le_frame_t *p;
    if (!le_pyframe_owns(f))
        return;
    for (p = f->f_frame->previous; p != NULL && _PyFrame_IsIncomplete(p);)
        p = p->previous;
    if (f->f_back == NULL && p != NULL) {
        f->f_back = le_frame_object(p);
        Py_XINCREF(f->f_back);
    }
    f->f_frame->previous = NULL;
    if (marked)
        le_pyframe_set_executing(f, 0);
}
/* the frame object of a frame that has just run, if something besides
   the frame holds it, so that it outlives the call */
static inline PyFrameObject *
le_frame_outlives(le_frame_t *f)
{
    if (f->owner != FRAME_OWNED_BY_THREAD || f->frame_obj == NULL ||
        Py_REFCNT(f->frame_obj) < 2)
        return NULL;
    return f->frame_obj;
}
/* 'back' (a reference it takes) is its caller from now on, in place of
   the frame it was called from, which CPython gives it as it clears it */
static inline void
le_frame_set_back_at_exit(le_frame_t *f, PyFrameObject *back)
{
    Py_XSETREF(f->frame_obj->f_back, back);
    f->previous = NULL;
}
/* the frame object is the thread's innermost, for C code that reads it
   (frames.c:le_warn): a frame object's own frame is its frame object
   until le_pyframe_current_end, which it holds a reference to meanwhile,
   as the GC sees it does */
static inline void
le_pyframe_make_current(PyThreadState *ts, PyFrameObject *f)
{
    if (le_pyframe_owns(f) && f->f_frame->frame_obj == NULL) {
        Py_INCREF(f);
        f->f_frame->frame_obj = f;
    }
    LE_CURRENT_FRAME(ts) = f->f_frame;
}
static inline void
le_pyframe_current_end(PyFrameObject *f)
{
    if (le_pyframe_owns(f) && !le_frame_is_executing(f) &&
        f->f_frame->frame_obj == f) {
        f->f_frame->frame_obj = NULL;
        Py_DECREF(f);
    }
}
/* nothing on the value stack, and ceval not running the frame: frames.c
   may keep its notes in the stack's first slots */
static inline int
le_frame_stack_empty(PyFrameObject *f)
{
    le_frame_t *frame = f->f_frame;
    int n = LE_FRAME_CODE(frame)->co_nlocalsplus;
#if LE_HAS_FRAME_STACKREFS
    return !le_frame_is_executing(f) &&
           frame->stackpointer == frame->localsplus + n;
#else
    return !le_frame_is_executing(f) && frame->stacktop == n;
#endif
}
/* the stack ceval resumes the frame with: its first n slots */
static inline void
le_frame_set_stack_depth(PyFrameObject *f, int n)
{
#if LE_HAS_FRAME_STACKREFS
    f->f_frame->stackpointer =
        f->f_frame->localsplus + LE_FRAME_CODE(f->f_frame)->co_nlocalsplus + n;
#else
    f->f_frame->stacktop = LE_FRAME_CODE(f->f_frame)->co_nlocalsplus + n;
#endif
}
/* A generator's frame as ceval leaves it at a yield and resumes it
   (eval_hook.c:LeGen_Run, pycompiler.c): its locals and its value stack,
   which follows them, with the stack's depth (le_frame_stack_depth), its
   place at the yield and its generator's state. Compiled code stores into
   localsplus by its offset. */
#define LE_OFS_le_frame_t__localsplus offsetof(le_frame_t, localsplus)
#define LE_SIZE2_le_frame_t__localsplus 3
#define LE_UNSIGNED_le_frame_t__localsplus 0
_Static_assert(sizeof(((le_frame_t *)0)->localsplus[0]) == sizeof(void *),
               "LeJIT: a frame's localsplus words are not pointers");
/* the frame is a generator's, resumed: the call of a generator function
   runs its frame first to RETURN_GENERATOR, which makes the generator */
static inline int
le_frame_is_gen(le_frame_t *f)
{
    return f->owner == FRAME_OWNED_BY_GENERATOR;
}
/* the generator whose frame f is */
static inline PyGenObject *
le_frame_gen(le_frame_t *f)
{
    return (PyGenObject *)((char *)f - offsetof(PyGenObject, gi_iframe));
}
/* Where compiled code that steps a generator itself (psyfunc.c:
   psyco_gen_step) finds what it reads and writes: the generator's fields,
   its frame's (counted from the generator, whose frame is inside it), and
   the thread state's exception state and innermost frame. The code word
   is a _PyStackRef where the frame's words are (le_gen_word_code). */
#define LE_GEN_FRAME_OFS offsetof(PyGenObject, gi_iframe)
#if LE_HAS_FRAME_STACKREFS || LE_HAS_FRAME_EXECUTABLE
#define LE_OFS_PyGenObject__code \
    (LE_GEN_FRAME_OFS + offsetof(le_frame_t, f_executable))
#else
#define LE_OFS_PyGenObject__code \
    (LE_GEN_FRAME_OFS + offsetof(le_frame_t, f_code))
#endif
#define LE_SIZE2_PyGenObject__code 3
#define LE_UNSIGNED_PyGenObject__code 0
#define LE_OFS_PyGenObject__globals \
    (LE_GEN_FRAME_OFS + offsetof(le_frame_t, f_globals))
#define LE_SIZE2_PyGenObject__globals 3
#define LE_UNSIGNED_PyGenObject__globals 0
#define LE_OFS_PyGenObject__builtins \
    (LE_GEN_FRAME_OFS + offsetof(le_frame_t, f_builtins))
#define LE_SIZE2_PyGenObject__builtins 3
#define LE_UNSIGNED_PyGenObject__builtins 0
#define LE_OFS_PyGenObject__locals \
    (LE_GEN_FRAME_OFS + offsetof(le_frame_t, f_locals))
#define LE_SIZE2_PyGenObject__locals 3
#define LE_UNSIGNED_PyGenObject__locals 0
#define LE_OFS_PyGenObject__place \
    (LE_GEN_FRAME_OFS + offsetof(le_frame_t, LE_FRAME_PLACE_FIELD))
#define LE_SIZE2_PyGenObject__place 3
#define LE_UNSIGNED_PyGenObject__place 0
#define LE_OFS_PyGenObject__previous \
    (LE_GEN_FRAME_OFS + offsetof(le_frame_t, previous))
#define LE_SIZE2_PyGenObject__previous 3
#define LE_UNSIGNED_PyGenObject__previous 0
#define LE_OFS_PyGenObject__frame_state offsetof(PyGenObject, gi_frame_state)
#define LE_SIZE2_PyGenObject__frame_state 0
#define LE_UNSIGNED_PyGenObject__frame_state 0
#define LE_OFS_PyGenObject__exc_previous \
    offsetof(PyGenObject, gi_exc_state.previous_item)
#define LE_SIZE2_PyGenObject__exc_previous 3
#define LE_UNSIGNED_PyGenObject__exc_previous 0
#if LE_HAS_FRAME_STACKREFS
#define LE_OFS_PyGenObject__stackpointer \
    (LE_GEN_FRAME_OFS + offsetof(le_frame_t, stackpointer))
#define LE_SIZE2_PyGenObject__stackpointer 3
#define LE_UNSIGNED_PyGenObject__stackpointer 0
#else
#define LE_OFS_PyGenObject__stacktop \
    (LE_GEN_FRAME_OFS + offsetof(le_frame_t, stacktop))
#define LE_SIZE2_PyGenObject__stacktop 2
#define LE_UNSIGNED_PyGenObject__stacktop 0
_Static_assert(sizeof(((le_frame_t *)0)->stacktop) == 4,
               "LeJIT: a frame's stack top is not 32 bits");
#endif
#define LE_OFS_PyThreadState__exc_info offsetof(PyThreadState, exc_info)
#define LE_SIZE2_PyThreadState__exc_info 3
#define LE_UNSIGNED_PyThreadState__exc_info 0
_Static_assert(sizeof(((PyGenObject *)0)->gi_frame_state) == 1,
               "LeJIT: a generator's frame state is not a byte");
_Static_assert(sizeof(((le_frame_t *)0)->LE_FRAME_PLACE_FIELD) ==
                       sizeof(void *) &&
                   sizeof(((le_frame_t *)0)->f_locals) == sizeof(void *),
               "LeJIT: a frame's place or locals is not a pointer");
/* the code object of a generator's code word */
static inline PyCodeObject *
le_gen_word_code(long w)
{
#if LE_HAS_FRAME_STACKREFS
    _PyStackRef r;
    r.bits = (uintptr_t)w;
    return (PyCodeObject *)PyStackRef_AsPyObjectBorrow(r);
#else
    return (PyCodeObject *)w;
#endif
}
/* A generator's frame's place, new or paused, as an index into its code's
   table of direct entries (psyco.h:le_gen_direct_t): its code units from
   the first, plus one, since a new frame's place may be one before it. The
   table has a slot for each unit and two more. */
static inline long
le_gen_direct_index(le_frame_t *f)
{
    return (LE_FRAME_PLACE(f) - _PyCode_CODE(LE_FRAME_CODE(f))) + 1;
}

/* the depth of a paused frame's stack */
static inline int
le_frame_stack_depth(le_frame_t *f)
{
#if LE_HAS_FRAME_STACKREFS
    return (int)(f->stackpointer - f->localsplus) -
           LE_FRAME_CODE(f)->co_nlocalsplus;
#else
    return f->stacktop - LE_FRAME_CODE(f)->co_nlocalsplus;
#endif
}
static inline void
le_gen_frame_set_depth(le_frame_t *f, int n)
{
#if LE_HAS_FRAME_STACKREFS
    f->stackpointer = f->localsplus + LE_FRAME_CODE(f)->co_nlocalsplus + n;
#else
    f->stacktop = LE_FRAME_CODE(f)->co_nlocalsplus + n;
#endif
}
/* the generator's frame runs, its stack held by compiled code (and its
   locals, but where LE_RESUMES_ON_FRAME those the code hasn't taken):
   gen_send_ex has marked it as running. Its place moves off the yield
   (LE_GEN_RUN_STEP), as ceval's would: gen.close() and gi_yieldfrom
   read a frame at a yield as paused there. */
#define LE_GEN_RUN_STEP ((long)sizeof(_Py_CODEUNIT))
static inline void
le_gen_frame_run(le_frame_t *f)
{
    le_gen_frame_set_depth(f, 0);
    LE_FRAME_PLACE(f) =
        (_Py_CODEUNIT *)((char *)LE_FRAME_PLACE(f) + LE_GEN_RUN_STEP);
}
/* paused at the yield at 'lasti' with 'nstack' items under the value, as
   ceval's YIELD_VALUE leaves it: its place is the yield, or the instruction
   after it where the place is the one to go on with */
static inline void
le_gen_frame_pause(le_frame_t *f, int lasti, int nstack)
{
    LE_FRAME_PLACE(f) =
        _PyCode_CODE(LE_FRAME_CODE(f)) + lasti + LE_HAS_FRAME_INSTR_PTR;
    le_gen_frame_set_depth(f, nstack);
    le_frame_gen(f)->gi_frame_state = FRAME_SUSPENDED;
}
/* the run that gave 'result' has ended: paused (le_gen_frame_pause, or
   ceval's own YIELD_VALUE), else returned or raised with its stack empty,
   which gen_send_ex then marks and clears, or le_gen_frame_leave where
   the evaluator does */
static inline void
le_gen_frame_end(le_frame_t *f, PyObject *result)
{
    (void)f;
    (void)result;
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
static inline int
le_frame_start(le_frame_t *f)
{
    (void)f;
    return 0;
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
/* the frame holds a reference to its builtins: the old one goes */
static inline void
le_frame_set_builtins(le_frame_t *f, PyObject *b)
{
    Py_INCREF(b);
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
/* its word k, its locals then its value stack, as above */
#define LE_FRAME_WORD(f, k) ((f)->f_localsplus[k])
static inline PyObject *
le_frame_word_take(le_frame_t *f, int k)
{
    PyObject *o = f->f_localsplus[k];
    f->f_localsplus[k] = NULL;
    return o;
}
static inline void
le_frame_word_put(le_frame_t *f, int k, PyObject *o)
{
    f->f_localsplus[k] = o;
}
static inline void
le_frame_word_set(le_frame_t *f, int k, PyObject *o)
{
    Py_XSETREF(f->f_localsplus[k], o);
}
static inline void
le_frame_words_own(le_frame_t *f, int n)
{
    (void)f;
    (void)n;
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
/* marks the frame running only for frame.clear(), which then refuses it;
   le_pyframe_unmark_running puts back what this returns */
static inline int
le_pyframe_mark_running(PyFrameObject *f)
{
    int old = f->f_state;
    f->f_state = FRAME_EXECUTING;
    return old;
}
static inline void
le_pyframe_unmark_running(PyFrameObject *f, int old)
{
    f->f_state = (PyFrameState)old;
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
/* marks the frame running only for frame.clear(), which then refuses it;
   le_pyframe_unmark_running puts back what this returns */
static inline int
le_pyframe_mark_running(PyFrameObject *f)
{
    int old = f->f_executing;
    f->f_executing = 1;
    return old;
}
static inline void
le_pyframe_unmark_running(PyFrameObject *f, int old)
{
    f->f_executing = (char)old;
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
/* A generator's frame as ceval leaves it at a yield and resumes it
   (eval_hook.c:LeGen_Run, pycompiler.c): its locals and its value stack,
   which follows them, with the stack's depth (le_frame_stack_depth),
   f_lasti at the yield and its blocks. Compiled code stores into
   f_localsplus by its offset. */
#define LE_OFS_PyFrameObject__f_localsplus \
    offsetof(PyFrameObject, f_localsplus)
#define LE_SIZE2_PyFrameObject__f_localsplus 3
#define LE_UNSIGNED_PyFrameObject__f_localsplus 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFrameObject *)0)->f_localsplus),
                   PyObject *[1]),
               "LeJIT: PyFrameObject.f_localsplus is not PyObject *[1]");
/* the depth of a paused frame's stack */
static inline int
le_frame_stack_depth(PyFrameObject *f)
{
#if LE_HAS_FRAME_STATE
    return f->f_stackdepth;
#else
    return (int)(f->f_stacktop - f->f_valuestack);
#endif
}
/* the frame is a generator's, resumed: a generator function's frame is
   made when the generator is */
static inline int
le_frame_is_gen(le_frame_t *f)
{
    (void)f;
    return 1;
}
/* the generator's frame runs, its stack held by compiled code: as ceval's
   prologue leaves it */
static inline void
le_gen_frame_run(PyFrameObject *f)
{
#if LE_HAS_FRAME_STATE
    f->f_stackdepth = -1;
    f->f_state = FRAME_EXECUTING;
#else
    f->f_stacktop = NULL;
    f->f_executing = 1;
#endif
}
/* compiled code pauses it at a yield, before storing its locals and stack
   items */
static inline void
le_gen_frame_pause(PyFrameObject *f, int lasti, int nstack, int iblock,
                   const PyTryBlock *blocks)
{
    int i;
    f->f_lasti = lasti;
    le_frame_set_stack_depth(f, nstack);
    f->f_iblock = iblock;
    for (i = 0; i < iblock; i++) f->f_blockstack[i] = blocks[i];
#if LE_HAS_FRAME_STATE
    f->f_state = FRAME_SUSPENDED;
#endif
}
/* the run that gave 'result' has ended: paused (le_gen_frame_pause, or
   ceval's own YIELD_VALUE), else returned or raised, as ceval leaves it */
static inline void
le_gen_frame_end(PyFrameObject *f, PyObject *result)
{
#if LE_HAS_FRAME_STATE
    if (f->f_state == FRAME_EXECUTING) {
        f->f_stackdepth = 0;
        f->f_state = result != NULL ? FRAME_RETURNED : FRAME_RAISED;
    }
#else
    (void)result;
    f->f_executing = 0;
#endif
}
/* the frame is running, as frame.clear() sees it */
static inline int
le_frame_executing(le_frame_t *f)
{
    return le_frame_is_executing(f);
}
/* a frame object made for a call compiled code runs (frames.c:kept_frame)
   is running while 'on', as the thread's frames are */
static inline void
le_pyframe_set_executing(PyFrameObject *f, int on)
{
    le_frame_set_executing(f, on);
}
/* ceval is to run the frame object itself (frames.c:psyco_resume_run),
   which needs nothing first: ceval links it and puts back the thread's
   innermost as its f_back */
static inline int
le_pyframe_eval_begin(PyThreadState *ts, PyFrameObject *f)
{
    (void)ts;
    (void)f;
    return 0;
}
static inline void
le_pyframe_eval_end(PyFrameObject *f, int marked)
{
    (void)f;
    (void)marked;
}
/* the frame of a call that has just run (not a generator's), if
   something besides the call holds it, so that it outlives the call */
static inline PyFrameObject *
le_frame_outlives(le_frame_t *f)
{
    return Py_REFCNT(f) < 2 ? NULL : f;
}
/* 'back' (a reference it takes) is its caller from now on */
static inline void
le_frame_set_back_at_exit(le_frame_t *f, PyFrameObject *back)
{
    Py_XSETREF(f->f_back, back);
}
/* the frame object is the thread's innermost, for C code that reads it
   (frames.c:le_warn), until le_pyframe_current_end */
static inline void
le_pyframe_make_current(PyThreadState *ts, PyFrameObject *f)
{
    le_frame_set_current(ts, f);
}
static inline void
le_pyframe_current_end(PyFrameObject *f)
{
    (void)f;
}
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

#if LE_HAS_EVAL_FRAME_POPS
extern int
le_monitoring_on(PyThreadState *ts); /* pyruntime.c */
/* ceval ends frame 'f', the thread's innermost, with the exception set:
   run from before its first instruction (incomplete, so ceval adds no
   traceback entry and finds no handler) with throwflag, it unwinds it at
   once, clearing and popping it as ceval does a frame that raises. That
   start instruments the code for sys.monitoring, as RESUME would;
   sys.monitoring's events for the unwind are not given (tstate->tracing).
   The exception is still set after, or another in its place. */
static inline void
le_frame_throw_out(PyThreadState *ts, le_frame_t *f)
{
    int quiet = le_monitoring_on(ts);
    PyObject *r;
    LE_FRAME_PLACE(f) = _PyCode_CODE(LE_FRAME_CODE(f)) - 1;
    if (quiet)
        ts->tracing++;
    r = _PyEval_EvalFrameDefault(ts, f, 1);
    if (quiet)
        ts->tracing--;
    if (r != NULL)
        Py_FatalError("LeJIT: ceval ended a frame with a result");
}

/* room on the thread's data stack for a frame of 'size' words, which
   ceval's pop gives back, as pystate.c:_PyThreadState_PushFrame makes it;
   its new chunk (push_chunk) is hidden, so is copied here. NULL if there
   is no memory. */
static inline le_frame_t *
le_frame_push_room(PyThreadState *ts, int size)
{
    PyObjectArenaAllocator arena;
    _PyStackChunk *chunk;
    PyObject **res;
    int bytes = 16 * 1024;
    if (_PyThreadState_HasStackSpace(ts, size)) {
        res = ts->datastack_top;
        ts->datastack_top += size;
        return (le_frame_t *)res;
    }
    while (bytes < (int)sizeof(PyObject *) * (size + 1000)) bytes *= 2;
    PyObject_GetArenaAllocator(&arena);
    chunk = arena.alloc(arena.ctx, bytes);
    if (chunk == NULL)
        return NULL;
    chunk->previous = ts->datastack_chunk;
    chunk->size = bytes;
    chunk->top = 0;
    if (ts->datastack_chunk != NULL)
        ts->datastack_chunk->top =
            ts->datastack_top - &ts->datastack_chunk->data[0];
    ts->datastack_chunk = chunk;
    ts->datastack_limit = (PyObject **)((char *)chunk + bytes);
    /* the root chunk's first word is skipped, so that no pop frees it */
    res = &chunk->data[chunk->previous == NULL];
    ts->datastack_top = res + size;
    return (le_frame_t *)res;
}

/* ceval runs frame object f's frame on from where it is to its end and
   clears and pops it (frames.c:psyco_resume_run). The thread's frame (the
   real frame LeCode_Run runs) runs as it is. A frame object's own frame,
   which ceval would end as a generator's, moves for the run to a frame on
   the thread's data stack: ceval's clear gives it back to the frame
   object, as for any frame that outlives its call (frameobject.c:
   take_ownership). The frame object keeps the f_back it has, which
   take_ownership would replace. The caller holds a reference to f. */
static inline PyObject *
le_pyframe_run_to_end(PyThreadState *ts, PyFrameObject *f)
{
    le_frame_t *frame = f->f_frame;
    PyFrameObject *back = f->f_back;
    PyObject *r;
    if (le_pyframe_owns(f)) {
        int size = LE_FRAME_CODE(frame)->co_framesize;
        le_frame_t *t = le_frame_push_room(ts, size);
        if (t == NULL)
            return PyErr_NoMemory();
        memcpy(t, frame, size * sizeof(PyObject *));
#if LE_HAS_FRAME_STACKREFS
        /* the stack's top is a pointer into the frame copied */
        t->stackpointer =
            t->localsplus + (frame->stackpointer - frame->localsplus);
#endif
        t->owner = FRAME_OWNED_BY_THREAD;
        t->frame_obj = (PyFrameObject *)Py_NewRef(f);
        f->f_frame = t;
        frame = t;
    }
    if (le_frame_current(ts) == frame)
        le_frame_set_current(ts, frame->previous);
    f->f_back = NULL;
    r = _PyEval_EvalFrameDefault(ts, frame, 0);
    if (back != NULL)
        Py_XSETREF(f->f_back, back);
    return r;
}
#endif

/* The frame entry handed to compiled code (eval_hook.c:LeCode_Run) ends:
   'result' (a new reference, or NULL with the exception set) is what it
   returns, after le_frame_finish. */
#if LE_HAS_EVAL_FRAME_POPS && LE_HAS_FRAME_CLEAR_AND_POP_LINKABLE
/* Whoever runs the frame clears and pops it, here with the function ceval
   uses ('done' is not needed). A frame object that outlives the frame
   keeps the f_back psyco_frame_returned gave it, which the pop would
   replace with the frame it was called from. */
static inline PyObject *
le_frame_finish(PyThreadState *ts, le_frame_t *f, PyObject *result,
                PyObject *done)
{
    PyFrameObject *fo = f->frame_obj, *back = NULL;
    (void)done;
    if (fo != NULL && Py_REFCNT(fo) > 1) {
        back = fo->f_back;
        fo->f_back = NULL;
    }
    _PyEval_FrameClearAndPop(ts, f);
    if (back != NULL)
        Py_XSETREF(fo->f_back, back);
    return result;
}
#elif LE_HAS_EVAL_FRAME_POPS
/* Whoever runs the frame clears and pops it, with functions CPython hides.
   A frame of the thread's with no frame object, not the first in its
   data-stack chunk, is cleared and popped here as ceval's
   clear_thread_frame does it. Any other is given to ceval to end
   (le_frame_throw_out): for a result, the exception is 'done', which is
   taken back. A frame object that outlives the frame keeps its place,
   which ceval would show as the first line, and the f_back
   psyco_frame_returned gave it, which ceval would replace with the frame
   it was called from. */
static inline PyObject *
le_frame_finish(PyThreadState *ts, le_frame_t *f, PyObject *result,
                PyObject *done)
{
    PyFrameObject *fo = f->frame_obj, *back = NULL;
    _Py_CODEUNIT *at = LE_FRAME_PLACE(f);
    if (fo == NULL && f->owner == FRAME_OWNED_BY_THREAD &&
        (PyObject **)f != &ts->datastack_chunk->data[0]) {
        PyCodeObject *co = LE_FRAME_CODE(f);
        int k, n = co->co_nlocalsplus + le_frame_stack_depth(f);
#if LE_HAS_C_RECURSION_REMAINING
        ts->c_recursion_remaining--;
#endif
        for (k = 0; k < n; k++) Py_XDECREF(le_frame_word_take(f, k));
        Py_XDECREF(f->f_locals);
        Py_DECREF(LE_FRAME_FUNCOBJ(f));
        Py_DECREF(co);
#if LE_HAS_C_RECURSION_REMAINING
        ts->c_recursion_remaining++;
#endif
        ts->datastack_top = (PyObject **)f;
        return result;
    }
    if (fo != NULL) {
        Py_INCREF(fo);
        back = fo->f_back;
        fo->f_back = NULL;
    }
    if (result != NULL)
        PyErr_SetRaisedException(Py_NewRef(done));
    le_frame_throw_out(ts, f);
    if (result != NULL) {
        /* ceval raised another exception (RecursionError on entering it)
           in place of 'done', or a finaliser the clear ran dropped it */
        PyObject *e = PyErr_GetRaisedException();
        if (e == done)
            Py_DECREF(e);
        else if (e != NULL) {
            PyErr_SetRaisedException(e);
            Py_CLEAR(result);
        }
    }
    if (fo != NULL) {
        /* its own copy now, since this holds a reference */
        le_frame_t *own = fo->f_frame;
        PyCodeObject *co = LE_FRAME_CODE(own);
        if (at >= _PyCode_CODE(co) + co->_co_firsttraceable)
            LE_FRAME_PLACE(own) = at;
        if (back != NULL)
            Py_XSETREF(fo->f_back, back);
        Py_DECREF(fo);
    }
    return result;
}
#else
/* the frame's caller pops it */
#define le_frame_finish(ts, f, result, done) (result)
#endif

#if LE_HAS_EVAL_FRAME_POPS
/* A generator's frame 'f', run by compiled code (eval_hook.c:LeGen_Run) to
   give 'result', is left as ceval leaves it, which gen_send_ex expects:
   paused at a yield, the thread's exception state is its caller's again
   and the frame is unlinked; else, ended, ceval clears it as a generator's
   frame (le_frame_finish). ceval would give a generator's frame the
   exception it unwinds with a traceback entry, as it does for any frame
   it has started (_PyFrame_IsIncomplete), so for the unwind the frame
   isn't the generator's; ceval tells the two apart only by the owner. */
static inline PyObject *
le_gen_frame_leave(PyThreadState *ts, le_frame_t *f, PyObject *result,
                   PyObject *done)
{
    PyGenObject *gen = le_frame_gen(f);
    if (result != NULL && gen->gi_frame_state == FRAME_SUSPENDED) {
        ts->exc_info = gen->gi_exc_state.previous_item;
        gen->gi_exc_state.previous_item = NULL;
#if LE_HAS_LAST_PROFILED_FRAME
        _PyThreadState_UpdateLastProfiledFrame(ts, f, f->previous);
#endif
        f->previous = NULL;
#if LE_HAS_GENERATOR_RETURN_KIND
        /* last: gen_send_ex2 reads it as the run's */
        ((_PyThreadStateImpl *)ts)->generator_return_kind = GENERATOR_YIELD;
#endif
        return result;
    }
    f->owner = FRAME_OWNED_BY_FRAME_OBJECT;
    result = le_frame_finish(ts, f, result, done);
    f->owner = FRAME_OWNED_BY_GENERATOR;
#if LE_HAS_GENERATOR_RETURN_KIND
    /* as ceval's clear of a generator's frame says, which a frame owned
       by its object doesn't get */
    ((_PyThreadStateImpl *)ts)->generator_return_kind = GENERATOR_RETURN;
#endif
    return result;
}
#endif
#endif /* LE_MECHANISMS_FRAMES_H */
