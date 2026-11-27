/***************************************************************/
/***            Python frames and virtual frames               ***/
/***************************************************************/

#ifndef _FRAMES_H
#define _FRAMES_H

#include "../psyco.h"
#include "pyver.h"
#include <compile.h>
#include <frameobject.h>
#include <idispatcher.h>
/* le_frame_t */
#include "../mechanisms/frames.h"
#include "../segments.h"

/* the real frame, if the compiled code 'co' making the current C call is
   the one LeCode_Run runs (not a compiled callee); borrowed, or NULL */
EXTERNFN PyFrameObject *
psyco_running_real_frame(PyCodeObject *co);
/* ceval evaluating a frame itself, past the hook (eval_hook.c) */
EXTERNFN PyObject *
le_eval_frame_default(le_frame_t *f);
#if LE_HAS_EVAL_FRAME_POPS
/* ceval instruments the code of frame 'f' for sys.monitoring now; -1 with
   an exception set if it can't (eval_hook.c) */
EXTERNFN int
le_code_instrument(le_frame_t *f);
#endif

EXTERNFN PyObject *
psyco_get_globals(void);

/* called by psyco_return_barrier as a compiled callee whose frame object
   was kept returns, 'sp' one word below its slot: lets the frame go and
   gives the call's real return address */
EXTERNFN code_t *
psyco_activation_returned(long *sp);
/* the frame object kept for the innermost compiled frame, or the frame
   inlined in it, if it is of code 'co' (borrowed), else NULL */
EXTERNFN PyFrameObject *
psyco_innermost_activation(PyCodeObject *co);
/* the function of the innermost compiled frame, if it is of code 'co' and
   LeJIT knows it (borrowed), else NULL */
EXTERNFN PyFunctionObject *
psyco_innermost_function(PyCodeObject *co);
/* how many frame objects of inlined frames are kept: compiled code
   leaving an inlined frame that ran Python then calls
   psyco_inline_frame_left, which lets its own go */
EXTERNVAR int psyco_inline_frames_kept;
EXTERNFN int
psyco_inline_frame_left(void);

/* A compiled frame going on interpreted: psyco_resume_begin, then each of
   its f_localsplus and value stack items in order (psyco_resume_value) and
   its blocks (psyco_resume_block); psyco_resume_run runs it in ceval from
   the instruction after 'lasti' and returns its result. Called from
   compiled code (pycompiler.c:resume_if_tracing). 'frameless': no frame
   object is made where none stands for the call's frame yet, for
   psyco_resume_suspend to make the coroutine's frame itself. */
struct resume_s;
EXTERNFN struct resume_s *
psyco_resume_begin(PyCodeObject *co, PyObject *globals, PyObject *builtins,
                   long nvalues, PyObject **word, long frameless);
EXTERNFN void
psyco_resume_value(struct resume_s *st, long i, PyObject *v);
#if !LE_HAS_EXCEPTION_TABLE
EXTERNFN void
psyco_resume_block(struct resume_s *st, long type, long handler, long level);
#endif
/* A compiled frame with handlers has a catcher word, which holds a
   reference to its traceback frame from when it catches an exception
   until its call ends (pycompiler.c:catcher_word). psyco_frame_catch puts
   'f' in it as the frame catches, the innermost compiled frame, whose own
   slot is 'own'. As the call ends, _shared lets it go if nothing else
   holds it, and says whether something does; _end lets it go once given
   its locals. */
EXTERNFN void
psyco_frame_catch(PyFrameObject *f, PyObject **word, stack_frame_info_t **own);
EXTERNFN long
psyco_frame_catcher_shared(PyObject **word);
EXTERNFN void
psyco_frame_catcher_end(PyObject **word);
EXTERNFN void
psyco_frame_link_callee(PyFrameObject *callee, PyFrameObject *f);
/* A new generator, coroutine or async generator (by the code's flags) of
   the function 'func', whose frame is the frame object 'f''s own frame,
   with its words, stack and place: f's reference is taken. Its state is
   FRAME_CREATED where it has one, and it has no origin recorded. NULL with
   an exception. */
EXTERNFN PyObject *
le_gen_of_frame(PyFrameObject *f, PyFunctionObject *func);
#if LE_HAS_INTERPRETER_FRAME
/* the generator or coroutine for func, written directly, its frame for the
   caller to fill and the caller to track */
EXTERNFN PyObject *
le_gen_make(PyFunctionObject *func);
/* the type of a generator of the code 'co': generator, coroutine or async
   generator, by its flags */
EXTERNFN PyTypeObject *
le_gen_type(PyCodeObject *co);
/* gen's header but its exception state and state, as le_gen_make
   writes it, for the function 'func' */
EXTERNFN void
le_gen_init_names(PyGenObject *gen, PyFunctionObject *func);
#endif
/* A new frame object for a call running 'co' in 'globals', of the function
   'func' where it is known (NULL: not known). Where frames are
   interpreter frames, one with the function owns its frame, as
   PyFrame_New's does, and is all that is made, as CPython's frame for the
   call holds its function; one without holds a function of its own, which
   PyFrame_New makes. NULL with an exception. */
EXTERNFN PyFrameObject *
le_pyframe_new(PyThreadState *ts, PyCodeObject *co, PyObject *globals,
               PyFunctionObject *func);
/* PyFrame_New, with no function watcher told of the function it makes */
EXTERNFN PyFrameObject *
le_pyframe_new_by_cpython(PyThreadState *ts, PyCodeObject *co,
                          PyObject *globals); /* pyruntime.c */
/* a frame's place, as CPython gives it, while the instruction at 'pos'
   in its code 'co' calls: the instruction's last unit, past its caches */
EXTERNFN int
psyco_call_lasti(PyCodeObject *co, int pos);
/* the frame 'f' has just run and outlives its call (le_frame_outlives) */
EXTERNFN void
psyco_frame_returned(le_frame_t *f);
/* the real frame object 'f', if compiled code runs it, keeps the locals
   its run ends with */
EXTERNFN void
psyco_frame_stands(PyFrameObject *f);
/* the same for 'back', if it has a frame object something else holds too:
   a frame that called from it may have outlived its call */
EXTERNFN void
psyco_frame_back_stands(le_frame_t *back);
/* a frame whose caller is 'back' is called from a compiled callee or
   inlined frame of the real frame 'back' */
EXTERNFN int
psyco_frame_called_from_callee(le_frame_t *back);
/* that callee, emulated and linked to the frames out of it (a new
   reference, or NULL) */
EXTERNFN PyFrameObject *
psyco_frame_callee_back(void);
EXTERNFN PyObject *
psyco_resume_run(struct resume_s *st, long lasti, long nstack);
/* PyObject* psyco_get_locals(void); this one implemented in psyco.c */

/* Where a compiled frame's locals (co_nlocals, then cells and frees)
   are during a call it makes that may run Python: in its slot, as a note
   ((index in psyco_locals_maps << 1) | 1) for a C call, or as the
   callee's record's back_locals for a compiled one. A note stays after
   its call (no store to clear it): its return address tells. Each value is
   three longs, (LM_STACK, offset from data, 0), (LM_REG, register, 0) for a
   callee-saved register at a C call, (LM_CONST, value, 0), (LM_NONE, 0, 0) or
   (LM_VIRTUAL, source_virtual_t *, n) followed by its n items; data is the
   slot's address minus 'below' (frames.c:slot_locals). */
enum { LM_NONE, LM_STACK, LM_REG, LM_CONST, LM_VIRTUAL };
struct locals_map_s {
    code_t *retaddr; /* a C call's return address, to tell a stale note */
    int lasti;       /* the call's instruction */
    int nvalues;
    long below;
    long ret_below; /* a note's: where the frame keeps its return address */
    /* the frame's catcher word (pycompiler.c:catcher_word) as an offset
       from data, or -1 if it has none */
    long word;
    long items[1];
};

/* what the glue from C into compiled code pushes to align the stack */
#define LE_GLUE_PAD 0x4e49474cL

/* to keep a trace of the Psyco stack frames */
struct stack_frame_info_s {
    long link_stack_depth; /* -1 if there is an inline frame following */
    PyCodeObject *co;
    PyObject *globals; /* NULL if not compile-time */
    int back_lasti;    /* the instruction of the next frame out that called
                          this one, or -1 */
    int nargs;         /* the words a compiled call pushes between this
                          frame's own slot and its return address, or -1
                          for an inlined frame (frames.c:kept_frame) */
    struct locals_map_s *back_locals; /* that frame's locals then, or NULL */
    code_t *ret_addr;                 /* a compiled call's return address */
    /* an inlined frame's own instruction, at a call through C
       (codegen.c:psyco_note_call_site), or -1 */
    int own_lasti;
    /* an inlined frame's own locals then, or NULL */
    struct locals_map_s *own_locals;
    /* the function whose coroutine's body this call runs, started by its
       await (psyfunc.c:psyco_coro_start), or NULL */
    PyFunctionObject *coro_func;
    /* the frame's function, where the code making the call knows it as it
       compiles (a constant its owner holds), else NULL: the frame object
       made for the frame holds it (le_pyframe_new) */
    PyFunctionObject *func;
    /* this call runs a generator's body on the generator's own frame,
       whose address is the word above the callee's own slot
       (psyfunc.c:psyco_gen_step) */
    int on_gen_frame;
    /* CPython makes this call from C, taking C stack, where compiled code
       makes it directly (psyco_call_psyco's 'through_c') */
    int through_c;
    /* where the frame that makes the call keeps its own return address
       meanwhile (codegen.h:psyco_ret_below); in a record's first entry */
    long ret_below;
};

/* the map of po's locals now, with lasti; while inlining, of the
   inlined frame's parent's, whose slot the call uses, unless 'own'. A
   local only in a register is found there (LM_REG), or with reg_pos at
   the position its register is pushed at for this call
   (psyco_call_psyco). Nothing is emitted. */
EXTERNFN struct locals_map_s *
psyco_locals_map(PsycoObject *po, int lasti, const long *reg_pos, int own);
EXTERNVAR struct locals_map_s **psyco_locals_maps;
EXTERNFN bool
psyco_locals_map_reads(vinfo_t *vi, int level);
PSY_INLINE struct locals_map_s *
psyco_note_map(long note)
{
    return psyco_locals_maps[note >> 1];
}

PSY_INLINE stack_frame_info_t *
finfo_last(stack_frame_info_t *finfo)
{
    if (finfo->link_stack_depth < 0) {
        finfo -= finfo->link_stack_depth;
    }
    return finfo;
}

EXTERNFN stack_frame_info_t *
psyco_finfo(PsycoObject *caller, PsycoObject *callee);
#if LE_RESUMES_GENERATORS_DIRECTLY
/* the record of a call from 'caller' that runs a body of the generator
   code 'co' on the generator's own frame (on_gen_frame) */
EXTERNFN stack_frame_info_t *
psyco_finfo_gen(PsycoObject *caller, PyCodeObject *co);
#endif

/* A real frame LeCode_Run runs: on its C stack, linked into this
   thread's list from start to return (frames.c:frame_runs) */
typedef struct frame_runtime_s *FrameRuntime_list_t;
typedef struct frame_runtime_s {
    le_frame_t *frame; /* LeCode_Run's caller holds it */
    stack_frame_info_t ***psy_frames_start;
    PyCodeObject *psy_code;
    PyObject *psy_globals;
    PyObject **entry_locals; /* see psyco_frame_write_locals, or NULL */
    int n_entry_locals;
    /* ceval now runs the frame (psyco_resume_run): its locals and line are
       ceval's, and the compiled code under it waits for ceval's result */
    int resumed;
    int exposed;                   /* frames.c:expose */
    struct walked_calls_s *walked; /* frames.c:innermost_slot, or NULL */
    PyObject **word; /* the run's catcher word (frames.c:psyco_frame_stands),
                        or NULL if not found yet */
    struct frame_runtime_s *next; /* the next outer one */
    /* the next run of any thread whose frame hashes alike (frames.c:
       psyco_frame_start_get) */
    struct frame_runtime_s *same_hash;
    FrameRuntime_list_t *list; /* its thread's frame_runs */
} FrameRuntime;

#if LE_RESUMES_ON_FRAME
/* The locals of a resumable generator's frame 'f' that its compiled run
   holds, listed, at a pause or end (pycompiler.c:gen_take_written): what
   Python wrote in them meanwhile (psyco_frame_write_locals) is taken out
   of the frame, before the compiled code stores its own. */
typedef struct {
    int ntaken;
    int taken[1];
} le_gen_taken_t;
EXTERNFN void
psyco_gen_take_written(le_frame_t *f, const le_gen_taken_t *t);
#else
/* The same for the frame of the run 'fr', all of whose locals the
   compiled run holds */
EXTERNFN void
psyco_gen_take_written(FrameRuntime *fr);
#endif
EXTERNVAR PyObject *psyco_coro_suspended;
EXTERNFN PyObject *
psyco_resume_suspend(struct resume_s *st, long at, long nstack, long level,
                     PyFunctionObject *func, PyObject *value);
EXTERNFN void
psyco_coro_taken(PyObject *out[]);
EXTERNFN void
psyco_coros_realise(void);
EXTERNFN PyObject *
psyco_resume_run_coro(struct resume_s *st, long lasti, long nstack, long level,
                      PyFunctionObject *func);

EXTERNFN void
psyco_frame_start_register(FrameRuntime *fr, le_frame_t *f,
                           stack_frame_info_t ***psy_frames_start);
EXTERNFN void
psyco_frame_start_unregister(FrameRuntime *fr);
#ifdef PyFrameLocalsProxy_Check
/* how many compiled frames' locals are their frame objects' now
   (frames.c:expose); compiled code tests it with tracing */
EXTERNVAR int psyco_frames_exposed;
/* the compiled frame running code 'co' goes on in ceval at its next
   resume test: tracing is on, or its locals are exposed */
EXTERNFN int
psyco_frame_goes_interpreted(PyCodeObject *co);
#endif
/* f's record if LeCode_Run runs it on this thread, else NULL */
EXTERNFN FrameRuntime *
psyco_frame_start_get(le_frame_t *f);
#if LE_STACK_SEGMENTS
/* the C stack that this thread's calls on its stack from 'lo' up to 'hi'
   take, of those CPython makes with none: compiled calls not through_c,
   and the calls in 'e' (segments.c) down to the run each entered. The
   runs on it, innermost first, are walked. */
EXTERNFN long
psyco_direct_calls_size(const char *lo, const char *hi,
                        const le_direct_entry_t *e);
#endif

/* f_localsplus[start...] = values (up to count, within the frame's
   locals), each with a new reference: the locals of a compiled frame, put
   into the frame of its traceback entry. NULL clears a slot (an unbound
   local); LE_LOCAL_UNKNOWN leaves it. */
#define LE_LOCAL_UNKNOWN ((PyObject *)1)
EXTERNFN void
psyco_frame_write_locals(PyFrameObject *f, int start, PyObject **values,
                         int count);

/* _warnings.warn's C function, and a call of 'warn', one of its function
   objects, with the compiled frames current as warn walks them */
EXTERNVAR PyCFunction le_c_warn;
EXTERNFN PyObject *
le_warn_with_frames(PyObject *warn, PyObject *args, PyObject *kwds);

#endif /* _FRAMES_H */
