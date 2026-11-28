/***************************************************************/
/***          Language-dependent structures                    ***/
/***************************************************************/

#ifndef _PYCHEADER_H
#define _PYCHEADER_H

#include "../mechanisms/code.h"
#include "../psyco.h"
#include <compile.h>
#include <frameobject.h>

/* "Frames": the Python compiler may be interrupted between the
   compilation of two opcodes. Any local data it needs that should
   persist across such interruptions must be stored in the PsycoObject
   driving compilation. The PsycoObject plays the same role as the
   FrameObject in normal interpretation. The usual fields of
   FrameObjects are split into two groups:

   1) in the 'vlocals' array, as 'vinfo_t' structures. This makes
      dynamically-staged variables, that is, whose value may be known
      at compile-time, run-time or virtual-time only. Use the LOC_xxx
      macros to access these variables.

   2) in the 'pr' field whose structure is defined below. For
      compile-time-only data.

   IMPORTANT: the dispatcher only looks at LOC_xxx variables to
   determine if the needed code has already be compiled. If two
   PsycoObjects differ only by their 'pr' field, the code that
   the compiler would emit in each case must be interchangeable
   (but not necessary identical, as different optimizations
   could be done).
*/

#define INDEX_LOC_CONTINUATION 0 /* return address in the stack */
#define INDEX_LOC_GLOBALS 1      /* globals() dict object */
#define INDEX_LOC_INLINING 2     /* for function inlining */
#define INDEX_LOC_BUILTINS                                               \
    3                           /* the frame's builtins dict: fixed when \
                                   the frame starts, as f_builtins, so a \
                                   vlocal and not a 'pr' field (the      \
                                   note above) */
#define INDEX_LOC_LOCALS_PLUS 4 /* start of local variables + stack */

/* LOC_INLINING's array, while a frame is inlined: the parent's code
   object, position and code serial (psyfunc.c:psyco_save_inline_po), the
   inlined frame's function where the call knows it, else (i << 1) | 1 if
   it is the parent's run-time vlocal i, else 0, then the
   parent's vlocals */
#define LOC_INLINE_CO 0
#define LOC_INLINE_NEXT_INSTR 1
#define LOC_INLINE_SERIAL 2
#define LOC_INLINE_FUNC 3
#define TOTAL_LOC_INLINE 4

/* A generator compiled into its consumer
   (psyfunc.c:psyco_call_genexpr) has two more slots, after its free
   variables and before the stack: what YIELD_VALUE does (GENEXPR_*, a
   compile-time value, so versions for different consumers never match at
   a merge point) and the accumulator; and where sum() compensates float
   sums, a third, the compensation (pbltinmodule.c:psyco_sum_step). Its
   RETURN_VALUE returns the result. */
#define GENEXPR_LIST 1 /* list(), tuple(): acc is a list, append */
#define GENEXPR_SET 2  /* set(): acc is a set, add */
#define GENEXPR_SUM 3  /* sum(): acc = acc + value */
#define GENEXPR_ANY 4  /* any(): return True on a true value */
#define GENEXPR_ALL 5  /* all(): return False on a false value */
#define GENEXPR_MIN 6  /* min(), max(): acc is the best so far, or NULL */
#define GENEXPR_MAX 7
/* Not fused: the generator's own body, resumed on its frame
   (eval_hook.c:compile_resume). acc is its frame, borrowed: YIELD_VALUE
   saves the locals, stack and blocks into the frame, as ceval leaves a
   paused generator's frame, and returns the value. */
#define GENEXPR_RESUMABLE 8
/* A coroutine awaited as soon as it is made (await f(x)), run as a call
   (psyfunc.c:psyco_coro_start): its RETURN_VALUE returns the await's
   value, and an await inside that suspends makes the coroutine and its
   frame, as they would be, and returns le_coro_suspended
   (pycompiler.c:yield_direct). acc is unused. */
#define GENEXPR_DIRECT 9
/* sum() where it compensates float sums, in the phase of builtin_sum_impl's
   loop its result is in (pbltinmodule.c:psyco_sum_step): GENEXPR_SUM is
   its int loop, then these */
#define GENEXPR_SUM_FLOAT 10   /* acc a float, the compensation a double */
#define GENEXPR_SUM_COMPLEX 11 /* acc the complex sum's state */
#define GENEXPR_SUM_GENERIC 12 /* plain +, for good */
/* A generator compiled into the for loop that steps it
   (psyfunc.c:psyco_gen_inline_next), inlined there from where it paused to
   its yield, which leaves it paused in the loop's virtual generator and
   gives the loop's FOR_ITER the value; its return ends it. acc is
   unused. */
#define GENEXPR_INLINED 13
/* pr.inline_left for a generator compiled into its loop left at its yield */
#define INLINE_LEFT_PAUSED 2
#define GENEXPR_SLOTS (2 + LE_HAS_SUM_COMPENSATED)
/* the code whose body may run compiled on its own frame, with the slots
   above (psyco_generator_mode) */
#define LE_BODY_FLAGS (CO_GENERATOR | CO_COROUTINE | CO_ASYNC_GENERATOR)
#define GENEXPR_SLOT_MODE(co) (INDEX_LOC_LOCALS_PLUS + le_code_nlocalsplus(co))
#define GENEXPR_SLOT_ACC(co) (GENEXPR_SLOT_MODE(co) + 1)

#define LOC_CONTINUATION (po->vlocals.items[INDEX_LOC_CONTINUATION])
#define LOC_GLOBALS (po->vlocals.items[INDEX_LOC_GLOBALS])
#define LOC_INLINING (po->vlocals.items[INDEX_LOC_INLINING])
#define LOC_BUILTINS (po->vlocals.items[INDEX_LOC_BUILTINS])
#define LOC_LOCALS_PLUS (po->vlocals.items + INDEX_LOC_LOCALS_PLUS)

/* a try or loop block of the code being compiled */
typedef struct {
    int b_type;    /* the SETUP_* opcode that opened it */
    int b_handler; /* where it goes, in bytes */
    int b_level;   /* the value stack's depth in it */
} le_block_t;

typedef struct {
    PyCodeObject *co; /* code object we are compiling */
    int next_instr;   /* next instruction to compile, maybe |NEXT_INSTR_POP */
    short auto_recursion; /* # levels to auto-compile calls to Python functions
                           */
    char is_inlining;     /* true when compiling a code inlined in a parent */
    unsigned char iblock; /* index in blockstack */
    char class_unchecked; /* a call that may assign __class__ was emitted
                             since the known types were last forgotten
                             (pobject.c:Psyco_ForgetClasses) */
    char python_call;     /* a call that may run Python code, so may have
                             started tracing, was emitted in this instruction
                             (pycompiler.c:resume_if_tracing) */
    char proxy_write;     /* this instruction stores or deletes an item of
                             what may be a frame-locals proxy */
    char resume_pending;  /* test for tracing at the next instruction: after
                             an inlined frame's return, or at a handler */
    char inline_ran_python; /* the inlined frame made a call that may run
                               Python, which may have written its caller's
                               locals through a proxy */
    char inline_left;       /* an inlined frame that ran Python returned, or
                               a generator compiled into a loop paused
                               (INLINE_LEFT_PAUSED): the instruction that
                               called it lets its kept frame go
                               (pycompiler.c:let_inline_frame_go) */
    char go_interpreted;    /* the frame goes on in ceval from the next
                               instruction: a type-promotion site's
                               megamorphic version (Psyco_NeedType) */
    char ex_call;           /* the call being compiled is an f(*a, **kw)
                               call of a Python function, whose frame is
                               counted without the recursion limit's test
                               (psyfunc.c:psyco_call_pyfunc) */
    char ex_keywords;       /* the call being compiled is an f(*a, **d)
                               call whose keywords are a run-time exact dict
                               the binder may read (psyco_dict_keywords) */
    char ex_method;         /* the call being compiled is an f(*a, **kw)
                               of a bound method, which CPython runs from C
                               (PyObject_Call) */
    int call_site; /* 1 + the CALL_* instruction being compiled, or 0 */
    struct locals_map_s *pending_map; /* noted for the C call being emitted,
                                         which gives it its return address */
    PyObject *changing_globals; /* names of globals detected to change: saved
                                   in snapshots, so a respawn replays the same
                                   LOAD_GLOBAL decisions; shared, so never
                                   changed in place (mark_varying) */
    le_block_t blockstack[CO_MAXBLOCKS]; /* for try and loop blocks */

    /* fields after 'blockstack' are not saved in a FrozenPsycoObject */
    int stack_base;  /* number of items before the stack in LOC_LOCALS_PLUS */
    int stack_level; /* see note below */
    PyObject *merge_points; /* see mergepoints.h */
    vinfo_t *exc; /* current compile-time (pseudo) exception, see below */
    vinfo_t *val; /* exception value */
    vinfo_t *tb;  /* traceback object */
    char split_after_global; /* a guarded LOAD_GLOBAL or attribute lookup was
                                just compiled: start a new block
                                (pycompiler.c:load_global_cached,
                                pobject.c:generic_getattr) */
    char pieces;             /* a pieces list may be live (plistobject.c:
                                PsycoList_ForcePieces); set when unsure */
} pyc_data_t;

#define AUTO_RECURSION_MAX 200
#define AUTO_RECURSION(r) ((r) > AUTO_RECURSION_MAX ? AUTO_RECURSION_MAX : (r))

/* Note.
   'stack_level' is stored in 'pr'. To do so we must assume that the level
   of the interpreter value stack is always the same when execution reaches
   the same position in the bytecode. This is normally the case in Python,
   with a few exceptions that need to be worked around ('finally:' clauses
   in the original interpreter are invoked with a variable number of objects
   pushed on the stack; the present interpreter always only pushes one,
   grouping objects in a tuple if necessary (remember that tuples are
   abstracted to virtual-time, so this doesn't mean we loose time building
   and deconstructing Python tuples)).

   The same assumption holds for the other fields of pyc_data_t but only
   'stack_level' needs special care.
*/

#include "../cf_flags.h"
#endif /* _PYCHEADER_H */
