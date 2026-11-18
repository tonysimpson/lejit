 /***************************************************************/
/***          Language-dependent structures                    ***/
 /***************************************************************/


#ifndef _PYCHEADER_H
#define _PYCHEADER_H


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

#define INDEX_LOC_CONTINUATION  0   /* return address in the stack */
#define INDEX_LOC_GLOBALS       1   /* globals() dict object */
#define INDEX_LOC_INLINING      2   /* for function inlining */
#define INDEX_LOC_BUILTINS      3   /* the frame's builtins dict: fixed when
                                       the frame starts, as f_builtins, so a
                                       vlocal and not a 'pr' field (the
                                       note above) */
#define INDEX_LOC_LOCALS_PLUS   4   /* start of local variables + stack */

/* LOC_INLINING's array, while a frame is inlined: the parent's code
   object, position and code serial (psyfunc.c:psyco_save_inline_po), then
   the parent's vlocals */
#define LOC_INLINE_CO           0
#define LOC_INLINE_NEXT_INSTR   1
#define LOC_INLINE_SERIAL       2
#define TOTAL_LOC_INLINE        3

/* A generator expression compiled into its consumer
   (psyfunc.c:psyco_call_genexpr) has two more slots, after its free
   variables and before the stack: what YIELD_VALUE does (GENEXPR_*, a
   compile-time value, so versions for different consumers never match at
   a merge point) and the accumulator. Its RETURN_VALUE returns the
   result. */
#define GENEXPR_LIST 1  /* list(), tuple(): acc is a list, append */
#define GENEXPR_SET  2  /* set(): acc is a set, add */
#define GENEXPR_SUM  3  /* sum(): acc = acc + value */
#define GENEXPR_ANY  4  /* any(): return True on a true value */
#define GENEXPR_ALL  5  /* all(): return False on a false value */
#define GENEXPR_MIN  6  /* min(), max(): acc is the best so far, or NULL */
#define GENEXPR_MAX  7
#define GENEXPR_SLOTS 2
#define GENEXPR_SLOT_MODE(co) (INDEX_LOC_LOCALS_PLUS + (co)->co_nlocals +     \
                               PyTuple_GET_SIZE((co)->co_freevars))
#define GENEXPR_SLOT_ACC(co) (GENEXPR_SLOT_MODE(co) + 1)

#define LOC_CONTINUATION    (po->vlocals.items[INDEX_LOC_CONTINUATION])
#define LOC_GLOBALS         (po->vlocals.items[INDEX_LOC_GLOBALS])
#define LOC_INLINING        (po->vlocals.items[INDEX_LOC_INLINING])
#define LOC_BUILTINS        (po->vlocals.items[INDEX_LOC_BUILTINS])
#define LOC_LOCALS_PLUS     (po->vlocals.items + INDEX_LOC_LOCALS_PLUS)


typedef struct {
  PyCodeObject* co;     /* code object we are compiling */
  int next_instr;       /* next instruction to compile, maybe |NEXT_INSTR_POP */
  short auto_recursion; /* # levels to auto-compile calls to Python functions */
  char is_inlining;     /* true when compiling a code inlined in a parent */
  unsigned char iblock; /* index in blockstack */
  char class_unchecked; /* a call that may assign __class__ was emitted
                           since the known types were last forgotten
                           (pobject.c:Psyco_ForgetClasses) */
  char python_call;     /* a call that may run Python code, so may have
                           started tracing, was emitted in this instruction
                           (pycompiler.c:resume_if_tracing) */
  char resume_pending;  /* test for tracing at the next instruction: after
                           an inlined frame's return, or at a handler */
  PyObject* changing_globals;  /* names of globals detected to change: saved
                                  in snapshots, so a respawn replays the same
                                  LOAD_GLOBAL decisions; shared, so never
                                  changed in place (mark_varying) */
  PyTryBlock blockstack[CO_MAXBLOCKS]; /* for try and loop blocks */
  
  /* fields after 'blockstack' are not saved in a FrozenPsycoObject */
  int stack_base;       /* number of items before the stack in LOC_LOCALS_PLUS */
  int stack_level;      /* see note below */
  PyObject* merge_points;   /* see mergepoints.h */
  vinfo_t* exc;         /* current compile-time (pseudo) exception, see below */
  vinfo_t* val;         /* exception value */
  vinfo_t* tb;          /* traceback object */
  char split_after_global; /* a guarded LOAD_GLOBAL or attribute lookup was
                              just compiled: start a new block
                              (pycompiler.c:load_global_cached,
                              pobject.c:generic_getattr) */
} pyc_data_t;

#define AUTO_RECURSION_MAX 200
#define AUTO_RECURSION(r)  ((r)>AUTO_RECURSION_MAX ? AUTO_RECURSION_MAX : (r))

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
