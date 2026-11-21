 /***************************************************************/
/***          Structures used by the dispatcher part           ***/
 /***************************************************************/

#ifndef _DISPATCHER_H
#define _DISPATCHER_H


#include "psyco.h"
#include "processor.h"
#include "codegen.h"

/* a frozen PsycoObject is a snapshot of an actual PsycoObject,
   capturing the state of the compiler in a form that can be used
   later to compare live states to it.
   The FrozenPsycoObject's secondary goal is to capture enough
   information to rebuild a "live" PsycoObject, close enough to
   the original to let the next few Python instructions produce
   exactly the same machine code as the original. See
   psyco_prepare_respawn(). Be careful, there are a lot of such
   snapshots around in memory; keep them as small as possible.

   The first implementation (with VLOCALS_OPC=0) stores a copy
   of the PsycoObject vlocals. So does the second implementation
   (VLOCALS_OPC=1), but it compresses the copy into a very compact
   pseudo-code. */
#define VLOCALS_OPC  1

struct FrozenPsycoObject_s {
  union {
    int fz_stack_depth;
    struct respawn_s* respawning;
  } fz_stuff;
#if VLOCALS_OPC
  Source* fz_vlocals_opc;     /* compact pseudo-code copy */
#else
  vinfo_array_t* fz_vlocals;       /* verbatim copy */
#endif
  PROCESSOR_FROZENOBJECT_FIELDS
  short fz_respawned_cnt;
  int fz_next_instr;        /* pr.next_instr, which fz_pyc_data leaves out */
  CodeBufferObject* fz_respawned_from;
  pyc_data_t* fz_pyc_data;  /* partially allocated, and shared by the
                               snapshots with the same (dispatcher.c,
                               pyc_share) */
};


/* construction */
PSY_INLINE void fpo_mark_new(FrozenPsycoObject* fpo) {
	fpo->fz_respawned_cnt = 0;
	fpo->fz_respawned_from = NULL;
}
PSY_INLINE void fpo_mark_unused(FrozenPsycoObject* fpo) {
#if VLOCALS_OPC
	fpo->fz_vlocals_opc = NULL;
#else
	fpo->fz_vlocals = NullArray;
#endif
	fpo->fz_pyc_data = NULL;
}
EXTERNFN void fpo_build(FrozenPsycoObject* fpo, PsycoObject* po);
EXTERNFN void fpo_release(FrozenPsycoObject* fpo);


/* build a 'live' PsycoObject from frozen snapshot */
EXTERNFN PsycoObject* fpo_unfreeze(FrozenPsycoObject* fpo);

/* A compiler state kept until run time reaches it: a coding pause's, or a
   promotion site's for each new value, with a variable in it ('fix', the
   promoted one). Most are never used, and a live PsycoObject is about four
   times the size of its frozen form, but freezing one and unfreezing it
   costs more than copying it. So a parked state stays live until
   PARK_LIVE_MAX newer ones are, then is frozen: a short program never
   freezes one. A frozen state has the fields a snapshot rebuilds or
   leaves out saved as they were, and 'fix' as the indices leading to it;
   unlike fpo_build, freezing keeps what is known of constants' classes,
   since the code compiled from it runs only after the checks before it.
   A state with a pending pseudo-exception, or whose 'fix' is too deep or
   under a constant, stays live. What only a live state uses and what only
   a frozen one does share their bytes; 'live' says which is there. */
#ifndef PARK_LIVE_MAX
#define PARK_LIVE_MAX 4096
#endif
#define FIXPATH_MAX 6
typedef struct parked_state_s {
  PsycoObject* live;   /* the live state, or NULL once frozen or released */
  union {
    struct {
      vinfo_t* fix;    /* in 'live' */
      struct parked_state_s *older, *newer;  /* the live ones not frozen yet */
    };
    struct {
      FrozenPsycoObject fz;     /* unused (fpo_mark_unused) once released */
      CodeBufferObject* respawn_proxy;
      PyObject* merge_points;
      unsigned long mp_serial;
      int respawn_cnt, stack_base, stack_level;
      short fixpath[FIXPATH_MAX];   /* ended by -1 if shorter */
      char split_after_global;
    };
  };
} parked_state_t;
/* parks 'po', which it takes; 'fix' may be NULL. 'ps' is zeroed */
EXTERNFN void psyco_park(parked_state_t* ps, PsycoObject* po, vinfo_t* fix);
/* a state to compile from, and 'fix' in it. The parked one stays, for a
   compile that runs out of memory to start again from: a frozen one is
   unfrozen afresh, a live one is returned and replaced by a copy
   (psyco_duplicate) */
EXTERNFN PsycoObject* psyco_unpark(parked_state_t* ps, vinfo_t** fix);
/* releases the parked state; it may be released again */
EXTERNFN void psyco_park_release(parked_state_t* ps);
/* states parked live, held to PARK_LIVE_MAX, and frozen:
   lejit._core._stats()['parked'] */
EXTERNVAR long ni_parked_live;
#if ALL_CHECKS
EXTERNVAR long psyco_stat_parked_frozen;
#endif

/* inspection */
PSY_INLINE int get_stack_depth(FrozenPsycoObject* fpo) {
	return fpo->fz_stuff.fz_stack_depth;
}

/* psyco_compatible():
   search in the given global_entries_t for a match to the live PsycoObject.
   Return NULL if no match is found, or a vcompatible_t structure otherwise.
   The returned 'diff' is an array of the 'vinfo_t*' of 'po' which are
   compile-time but should be un-promoted to run-time. In particular, the
   array is empty (==NullArray) if an exact match is found.

   The current implementation is not re-entrent. The returned vcompatible_t
   structure must be released by psyco_stabilize() or one of the psyco_unify
   functions below, before another call to psyco_compatible() can be made.
*/
typedef struct {
  CodeBufferObject* matching;   /* best match */
  vinfo_array_t* diff;          /* array of differences */
} vcompatible_t;
EXTERNFN vcompatible_t* psyco_compatible(PsycoObject* po,
                                         global_entries_t* pattern);
/* set while a promotion's continuation is compared at its own instruction:
   no known fact is dropped there (dispatcher.c, compatible_unused_fact) */
EXTERNVAR bool psyco_compatible_keep_facts;
#if ALL_CHECKS
/* merge-point matches that dropped unused facts
   (lejit._core._stats()['fact_drops']) */
EXTERNVAR long psyco_stat_fact_drops;
#endif
/* the same, at a join point (vcompiler.c:psyco_compile_join): with
   'drop_fields' the known fields of a run-time value that the snapshot
   hasn't are dropped at any depth, not only below the first two
   (compatible_array, CHECK_FOR_NULL) */
EXTERNFN vcompatible_t* psyco_compatible_join(PsycoObject* po,
                                              global_entries_t* pattern,
                                              bool drop_fields);

/* What psyco_compatible() can reject a snapshot by without reading it:
   for the first 63 vlocals, two bits each of what the item must be in a
   state that matches (b1:b0 = 00 anything, 01 run-time, 10 compile-time,
   11 virtual); in an inlined frame, a hash of the fixed constants
   LOC_INLINING starts with (its code object, position and serial; 0 if
   none), which a state that matches has the same, and the same two bits
   for the first 64 items of the parent frame after them. Made when a
   buffer is registered (codemanager.c), from the state its snapshot is
   taken of. */
typedef struct {
  unsigned long b1, b0; /* SUMMARY_REST in b0 */
} fz_summary_head_t;
typedef struct {
  fz_summary_head_t head;
  unsigned long inline_key;
  unsigned long p1, p0;
} fz_summary_t;
/* A state with no inlined frame has a summary whose rest after 'head' is
   0 and rejects nothing: most states. Their buffers keep 'head' alone
   (codemanager.h); the others have SUMMARY_REST in head.b0, and so has
   every state's summary. */
#define SUMMARY_REST (1UL << 63)
EXTERNFN bool psyco_summary_short(vinfo_array_t* aa);
EXTERNFN void psyco_summary_build(fz_summary_t* s, vinfo_array_t* aa);
EXTERNFN void psyco_summary_build_short(fz_summary_head_t* s, vinfo_array_t* aa);

EXTERNFN void psyco_stabilize(vcompatible_t* lastmatch);
EXTERNFN bool psyco_forget_values(PsycoObject* po, global_entries_t* ge);
/* a block left 'ge', or 'ge' is freed: what psyco_forget_values kept
   about it is dropped */
EXTERNFN void psyco_ge_changed(global_entries_t* ge);
EXTERNFN bool psyco_unpromote_large_ints(PsycoObject* po);
#if NI_CODESTATS
EXTERNVAR int psyco_cs_align_rejects;
#endif


/*****************************************************************/
 /***   "Global Entries"                                        ***/

/* global entry points for the compiler. One global entry point holds a list
   of already-compiled code buffers corresponding to the various states
   in which the compiler has already be seen at this point. See
   psyco_compatible().

   The dispatcher saves all CodeBufferObjects (with their copy of the
   compiler state) in a list for each 'entry point' of the compiler
   (the main entry point is spec_main_loop, the start of the main loop).
   When coming back to this entry point later, the list let us determine
   whether we already encountered the same state.
   
   The details of this structure are private.
   XXX implemented as a list object holding CodeBufferObjects in no
   XXX particular order. Must be optimized for reasonably fast searches
   XXX if the lists become large (more than just a few items).

   The list starts with PyLongObjects whose numbers tell which local variables
   may safely be deleted at that point.
*/
struct global_entries_s {
	PyObject* fatlist;      /* list of PyLongObjects then CodeBufferObjects */
	struct varies_s* varies; /* psyco_forget_values's, or NULL */
};

/* initialize a global_entries_t structure */
PSY_INLINE void psyco_ge_init(global_entries_t* ge) {
	ge->varies = NULL;
	ge->fatlist = PyList_New(0);
	if (ge->fatlist == NULL)
		OUT_OF_MEMORY();
}

/* register the code buffer; it will be found by future calls to
   psyco_compatible(). */
PSY_INLINE int register_codebuf(global_entries_t* ge, CodeBufferObject* codebuf) {
	return PyList_Append(ge->fatlist, (PyObject*) codebuf);
}

/* for mergepoints.c */
PSY_INLINE void psyco_ge_unused_var(global_entries_t* ge, int num)
{
	PyObject* o = PyLong_FromLong(num);
	if (o == NULL || PyList_Append(ge->fatlist, o))
		OUT_OF_MEMORY();
}

EXTERNFN void psyco_forget_dead_vars(PsycoObject* po, global_entries_t* ge);


/*****************************************************************/
 /***   Unification                                             ***/

/* Update 'po' to match 'lastmatch', then jump to 'lastmatch'.
   For the conversion we might have to emit some code.
   If po->code == NULL or there is not enough room between code and
   po->codelimit, a new code buffer is created. The new or target code
   buffer is returned in 'target', borrowed (its owner holds it).
   If po->code != NULL, the return value points at the end of the
   code that has been written there; otherwise, the return value is
   undefined (but not NULL).
   'po' and 'lastmatch' are released.
*/
EXTERNFN code_t* psyco_unify(PsycoObject* po, vcompatible_t* lastmatch,
                             CodeBufferObject** target);

/* Simplified interface to psyco_unify() without using a previously
   existing code buffer (i.e. 'po->code' is uninitialized). If needed,
   return a new buffer with the necessary code followed by a JMP to
   'lastmatch'. If no code is needed, just return 'lastmatch->matching'
   (borrowed).
*/
EXTERNFN CodeBufferObject* psyco_unify_code(PsycoObject* po,
                                            vcompatible_t* lastmatch);

/* To "simplify" recursively a vinfo_array_t. The simplification done
   is to replace run-time values inside a sub-array of a non-virtual
   value with NULL, and to remove sub-arrays of constant-time values.
   We assume that these can still be reloaded later if necessary.
   Returns the number of run-time values left.
   This assumes that all 'tmp' marks are cleared in 'array'. */
EXTERNFN int psyco_simplify_array(vinfo_array_t* array,
                                  PsycoObject* po);  /* 'po' may be NULL */

/* Emit the code to prepare for Psyco code calling Psyco code in
   a compiled function call */
PSY_INLINE bool psyco_forking(PsycoObject* po, vinfo_array_t* array) {
	/* Some virtual-time objects cannot remain virtualized across calls,
	   because if the called function pulls them out of virtual-time,
	   the caller will not know it.  This is unacceptable for
	   mutable Python objects.  We hope it does not hurt in other cases,
	   but could be defeated by the "is" operator. */
	return psyco_limit_nested_weight(po, array, NWI_FUNCALL,
                                         NESTED_WEIGHT_END);
}

/*****************************************************************/
 /***   Promotion                                               ***/

/* Promotion of a run-time variable into a fixed compile-time one.
   Finish the code block with a jump to the dispatcher that
   promotes the run-time variable 'fix' to compile-time. This
   usually means the compiler will be called back again, at the
   given entry point.
   Note: Releases 'po'.
*/
EXTERNFN code_t* psyco_finish_promotion(PsycoObject* po, vinfo_t* fix,
                                        int pflags);


/* Un-Promotion from non-fixed compile-time into run-time.
   Note: this does not release 'po'. Un-promoting is easy and
   don't require encoding calls to the dispatcher.
*/
EXTERNFN void psyco_unfix(PsycoObject* po, vinfo_t* vi);


/*****************************************************************/
 /***   Respawning                                              ***/

/* internal use */
EXTERNFN void* psyco_prepare_respawn_ex(PsycoObject* po,
                                        condition_code_t jmpcondition,
                                        void* fn, size_t extrasize);
EXTERNFN bool psyco_prepare_respawn(PsycoObject* po,
                                    condition_code_t jmpcondition);
EXTERNFN code_t* psyco_do_respawn(void* arg, int extrasize);
EXTERNFN code_t* psyco_dont_respawn(void* arg, int extrasize);
EXTERNFN void psyco_respawn_detected(PsycoObject* po);
#define detect_respawn_ex(po)  (!++(po)->respawn_cnt)
PSY_INLINE bool detect_respawn(PsycoObject* po) {
	if (detect_respawn_ex(po)) {
		psyco_respawn_detected(po);
		return true;
	}
	else
		return false;
}
PSY_INLINE bool is_respawning(PsycoObject* po) { return po->respawn_cnt < 0; }

/* the following powerful function stands for 'if the processor flag (cond) is
   set at run-time, then...'. Of course we do not know yet if this will be
   the case or not, but the macro takes care of preparing the required
   respawns if needed. 'cond' may be CC_ALWAYS_xxx or a real processor flag.
   runtime_condition_f() assumes the outcome is generally false,
   runtime_condition_t() assumes the outcome is generally true. */
PSY_INLINE bool runtime_condition_f(PsycoObject* po, condition_code_t cond) {
	extra_assert(cond != CC_ERROR);
	if (cond == CC_ALWAYS_FALSE) return false;
	if (cond == CC_ALWAYS_TRUE) return true;
        return psyco_prepare_respawn(po, cond);
}
PSY_INLINE bool runtime_condition_t(PsycoObject* po, condition_code_t cond) {
	extra_assert(cond != CC_ERROR);
	if (cond == CC_ALWAYS_TRUE) return true;
	if (cond == CC_ALWAYS_FALSE) return false;
        return !psyco_prepare_respawn(po, INVERT_CC(cond));
}
/* extreme care is needed when using the runtime_condition_x() functions:
   they must *always* get called in a sequence that doesn't depend on external
   factors that may change. If a call is in one branch on an if/else, for
   example, then you must make sure that the "if" condition cannot give a
   different result during later respawning. See load_global for a what to do
   if this is not the case. */

/* the following functions let you test at compile-time the fact that a
   value is 0 or not. Returns 0 or 1, or -1 in case of error.
   Uses integer_NON_NULL() and so accepts 'vi==NULL' and !!consumes a ref!!
   *Do not use* for values that might hold a reference to a PyObject;
   use object_non_null() in this case. */
EXTERNFN int runtime_NON_NULL_f(PsycoObject* po, vinfo_t* vi);
EXTERNFN int runtime_NON_NULL_t(PsycoObject* po, vinfo_t* vi);

/* check if an integer is between the given *inclusive* bounds.
   Same return values as above.  Does not consume a ref. */
EXTERNFN int runtime_in_bounds(PsycoObject* po, vinfo_t* vi,
                               long lowbound, long highbound);

#if ALL_CHECKS
/* the replay check (dispatcher.c) records where each respawn point was made:
   the outermost of these calls, as file and line */
EXTERNVAR const char* psyco_respawn_file;
EXTERNVAR int psyco_respawn_line;
#define NI_RESPAWN_SITE(call) ({                                    \
        const char* _f = psyco_respawn_file;                        \
        int _l = psyco_respawn_line;                                \
        __typeof__(call) _r;                                        \
        if (_f == NULL) {                                           \
                psyco_respawn_file = __FILE__;                      \
                psyco_respawn_line = __LINE__;                      \
        }                                                           \
        _r = (call);                                                \
        psyco_respawn_file = _f;                                    \
        psyco_respawn_line = _l;                                    \
        _r; })
#define runtime_condition_f(po, c) NI_RESPAWN_SITE(runtime_condition_f(po, c))
#define runtime_condition_t(po, c) NI_RESPAWN_SITE(runtime_condition_t(po, c))
#define runtime_NON_NULL_f(po, v)  NI_RESPAWN_SITE(runtime_NON_NULL_f(po, v))
#define runtime_NON_NULL_t(po, v)  NI_RESPAWN_SITE(runtime_NON_NULL_t(po, v))
#define runtime_in_bounds(po, v, lo, hi)                                  \
        NI_RESPAWN_SITE(runtime_in_bounds(po, v, lo, hi))
#endif


#endif /* _DISPATCHER_H */
