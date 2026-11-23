/***************************************************************/
/***                Tables of code merge points                ***/
/***************************************************************/

#ifndef _MERGEPOINTS_H
#define _MERGEPOINTS_H

#include "psyco.h"
#include "dispatcher.h"
#include <compile.h>

/* A merge point is a point reached by two or more control paths in
   the bytecode. They are a subset of the targets found by the standard
   module function dis.findlabels(). It may be a subset because some
   targets can only be reached by jumping from a single point, e.g. 'else:'.
   Such targets are not merge points.
*/
struct mergepoint_s {
    int bytecode_position;
    int blocks; /* blocks compiled from here (vcompiler.c, the cap) */
    global_entries_t entries;
};

/* 'bytecode_ptr' gives the position of the merge point in the bytecode.
   An array of mergepoint_t structures is sorted against this field.

   'entries' is a list of snapshots of the compiler states previously
   encountered at this point.
*/

/* Get merge points (cached) */
EXTERNFN PyObject *
psyco_get_merge_points(PyCodeObject *co, int module);
/* the same where x->merge_points is NULL: analyses 'co' */
EXTERNFN PyObject *
psyco_analyse_code(PyCodeObject *co, le_code_extra_t *x, int module);
EXTERNFN int
psyco_is_fusable_generator(PyCodeObject *co);
EXTERNFN int
psyco_is_resumable_generator(PyCodeObject *co);
EXTERNFN int
psyco_handler_awaits(PyCodeObject *co, int handler);
/* the index among the free variables of '__class__', or -1 */
EXTERNFN int
psyco_class_cell(PyCodeObject *co);

/* Get a pointer to the first mergepoint_t structure in the array whose
   'bytecode_ptr' is >= position. */
EXTERNFN mergepoint_t *
psyco_next_merge_point(PyObject *mergepoints, int position);

/* Get a pointer to the very first mergepoint_t structure in the array */
EXTERNFN mergepoint_t *
psyco_first_merge_point(PyObject *mergepoints);

/* Same as psyco_next_merge_point() but returns NULL if bytecode_ptr!=position
 */
EXTERNFN mergepoint_t *
psyco_exact_merge_point(PyObject *mergepoints, int position);

/* The snapshots of the blocks started at 'position' after a split site
   (vcompiler.c:psyco_compile_join); NULL if out of memory. */
EXTERNFN global_entries_t *
psyco_join_point(PyObject *mergepoints, int position);

#define MP_FLAGS_HAS_EXCEPT 1   /* the code block has an 'except' clause */
#define MP_FLAGS_HAS_FINALLY 2  /* the code block has a 'finally' clause */
#define MP_FLAGS_INLINABLE 4    /* function could be inlined             */
#define MP_FLAGS_MODULE 8       /* can only run as module top-level code */
#define MP_FLAGS_CONTROLFLOW 16 /* can use early deletion of locals      */
#define MP_FLAGS_RESUMABLE 32   /* psyco_is_resumable_generator          */
#define MP_FLAGS_EXTRA (-256)

EXTERNFN int
psyco_mp_flags(PyObject *mergepoints);
/* a generator expression's flag, set once a type site in its fused code
   (psyfunc.c:psyco_call_genexpr) has seen more than MEGAMORPHIC_MAX
   types: from then on its consumers run it as a generator
   (psyfunc.c:psyco_genexpr_fuse), so classes made per call don't each get
   fused code that keeps them alive. Never cleared. */
EXTERNFN int *
psyco_fuse_stopped(PyObject *mergepoints);
/* whether compiled code tests eval_breaker before the instruction at
   position (a byte offset) */
EXTERNFN int
psyco_breaker_check_at(PyObject *mergepoints, int position);
/* whether ceval doesn't test eval_breaker before an instruction, but
   before the next one, as it goes on with DISPATCH(): op is the code unit
   ceval reads there, not the LeJIT opcode (bytecode.h) */
#define BREAKER_SKIPS(op) (le_op_flags(op) & LE_OPF_BREAKER_SKIPS)
#endif /* _MERGEPOINTS_H */
