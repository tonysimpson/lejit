/***************************************************************/
/***            Processor-specific code generation             ***/
/***************************************************************/

#ifndef _CODEGEN_H
#define _CODEGEN_H

#include "vcompiler.h"
#include <iencoding.h>

#if SIZEOF_LONG == 4
#define SIZE_OF_LONG_BITS 2
#else
#define SIZE_OF_LONG_BITS 2
#endif

/***************************************************************/
/*** Condition Codes (a.k.a. the processor 'flags' register) ***/

/* return a new vinfo_t* meaning `in the processor flags, true if <cc>',
   as an integer 0 or 1.  The source of the vinfo_t* is compile-time
   if cc is CC_ALWAYS_TRUE/FALSE, and virtual-time otherwise. */
EXTERNFN vinfo_t *
psyco_vinfo_condition(PsycoObject *po, condition_code_t cc);
EXTERNFN VirtualTimeSource
psyco_source_condition(condition_code_t cc);

/* if 'source' comes from psyco_vinfo_condition(), return its <cc>;
   otherwise return CC_ALWAYS_FALSE. */
#if HAVE_CCREG
EXTERNFN condition_code_t
psyco_vsource_cc(Source source);
EXTERNFN void
psyco_resolved_cc(PsycoObject *po, condition_code_t cc_known_true);
#else
#define psyco_vsource_cc(src) CC_ALWAYS_FALSE
#endif

/*****************************************************************/
/***   Calling C functions                                     ***/

/* A generic way of emitting the call to a C function.
   For maximal performance you can also directly use the macros
   CALL_C_FUNCTION()&co. in encoding.h.

   'arguments' is a string describing the following arguments
   of psyco_generic_call(). Each argument to the C function to call
   comes from a 'vinfo_t*' structure, excepted when it is known to be
   compile-time, in which case a 'long' or 'PyObject*' can be passed
   instead. In 'arguments' the characters are interpreted as follows:

      l      an immediate 'long' or 'PyObject*' value
      v      a 'vinfo_t*' value
      r      a run-time 'vinfo_t*' value passed as a reference
      a      a 'vinfo_array_t*' used by the C function as output buffer
      A      same as 'a', but the C function creates new references

   'r' means that the C function wants to get a reference to a
   single-word value (typically it is an output argument). The
   run-time value is pushed in the stack if it is just in a
   register. Incompatible with CfPure.

   'a' means that the C function gets a pointer to a buffer capable of
   containing as many words as specified by the array count.
   psyco_generic_call() fills the array with run-time vinfo_ts
   representing the output values.
*/
EXTERNFN vinfo_t *
psyco_generic_call(PsycoObject *po, void *c_function, int flags,
                   const char *arguments, ...);

/* compile_time_call's result for a fresh object (psyco_shared_constant):
   psyco_generic_call then emits the call */
#define VINFO_FRESH ((vinfo_t *)2)

/* psyco_generic_call reads every argument with va_arg(long), but a vararg
   narrower than a word (an int, an enum, 0) arrives with its upper 32 bits
   unspecified. This wrapper makes such an argument a compile
   error ("size of unnamed array is negative"): cast it to (long). _Generic
   and sizeof don't evaluate 'x', so each argument is still evaluated once. */
#define LE_WORD_VARG(x)                        \
    ((void)sizeof(char[_Generic((x),           \
                           _Bool: -1,          \
                           char: -1,           \
                           signed char: -1,    \
                           unsigned char: -1,  \
                           short: -1,          \
                           unsigned short: -1, \
                           int: -1,            \
                           unsigned int: -1,   \
                           float: -1,          \
                           double: -1,         \
                           default: 1)]),      \
     (x))
#define LE_WORD_VARGS1(a) LE_WORD_VARG(a)
#define LE_WORD_VARGS2(a, ...) LE_WORD_VARG(a), LE_WORD_VARGS1(__VA_ARGS__)
#define LE_WORD_VARGS3(a, ...) LE_WORD_VARG(a), LE_WORD_VARGS2(__VA_ARGS__)
#define LE_WORD_VARGS4(a, ...) LE_WORD_VARG(a), LE_WORD_VARGS3(__VA_ARGS__)
#define LE_WORD_VARGS5(a, ...) LE_WORD_VARG(a), LE_WORD_VARGS4(__VA_ARGS__)
#define LE_WORD_VARGS6(a, ...) LE_WORD_VARG(a), LE_WORD_VARGS5(__VA_ARGS__)
#define LE_WORD_VARGS7(a, ...) LE_WORD_VARG(a), LE_WORD_VARGS6(__VA_ARGS__)
#define LE_WORD_VARGS8(a, ...) LE_WORD_VARG(a), LE_WORD_VARGS7(__VA_ARGS__)
#define LE_GC_FIXED(po, fn, fl, args) po, fn, fl, args
#define LE_GC_VARGS(n, po, fn, fl, args, ...) \
    po, fn, fl, args, LE_WORD_VARGS##n(__VA_ARGS__)
#define LE_GC4(...) LE_GC_FIXED(__VA_ARGS__)
#define LE_GC5(...) LE_GC_VARGS(1, __VA_ARGS__)
#define LE_GC6(...) LE_GC_VARGS(2, __VA_ARGS__)
#define LE_GC7(...) LE_GC_VARGS(3, __VA_ARGS__)
#define LE_GC8(...) LE_GC_VARGS(4, __VA_ARGS__)
#define LE_GC9(...) LE_GC_VARGS(5, __VA_ARGS__)
#define LE_GC10(...) LE_GC_VARGS(6, __VA_ARGS__)
#define LE_GC11(...) LE_GC_VARGS(7, __VA_ARGS__)
#define LE_GC12(...) LE_GC_VARGS(8, __VA_ARGS__)
#define LE_GC_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, N, ...) N
#define psyco_generic_call(...)                                             \
    (psyco_generic_call)(LE_GC_PICK(__VA_ARGS__, LE_GC12, LE_GC11, LE_GC10, \
                                    LE_GC9, LE_GC8, LE_GC7, LE_GC6, LE_GC5, \
                                    LE_GC4, , , )(__VA_ARGS__))

#include "cf_flags.h"

/* To emit the call to other code blocks emitted by Psyco. 'argsources' gives
   the run-time sources for the arguments, as specified by
   psyco_build_frame(). */
/* Py_LeaveRecursiveCall after a compiled call (psyfunc.c); false with an
   exception set */
EXTERNFN bool
psyco_leave_recursive_call(PsycoObject *po);
/* tests the 32-bit int at 'address' at run time: CC_NE if it isn't 0 */
EXTERNFN condition_code_t
psyco_int32_nonzero_at(PsycoObject *po, void *address);
/* tests the 32-bit int at 'offset' in the run-time pointer 'vbase': CC_NE
   if it isn't 0 */
EXTERNFN condition_code_t
psyco_int32_nonzero_in(PsycoObject *po, vinfo_t *vbase, long offset);
/* tests the byte at 'offset' in the structure whose address is the word at
   'at' from the thread pointer, through a scratch register that holds
   nothing after: CC_NE if it isn't 0 */
EXTERNFN condition_code_t
psyco_thread_byte_nonzero(PsycoObject *po, long at, long offset);
/* ceval's eval_breaker test at a loop head or after a call (psyfunc.c);
   false with an exception set */
EXTERNFN bool
psyco_check_eval_breaker(PsycoObject *po);
/* whether a C function is one after which a call needs no eval_breaker
   test (psyfunc.c) */
EXTERNFN bool
psyco_cannot_block(void *c_function);
EXTERNFN vinfo_t *
psyco_call_psyco(PsycoObject *po, CodeBufferObject *codebuf,
                 Source argsources[], int argcount,
                 struct stack_frame_info_s *finfo);

PSY_INLINE int
get_arguments_count(vinfo_array_t *vlocals)
{
    int retpos = getstack(vlocals->items[INDEX_LOC_CONTINUATION]->source);
    extra_assert(retpos != RunTime_StackNone);
    return (retpos - (INITIAL_STACK_DEPTH + sizeof(long))) / sizeof(long);
}

/* Emit the code to mark the presence of an inlined frame */
EXTERNFN void
psyco_inline_enter(PsycoObject *po);
EXTERNFN void
psyco_inline_exit(PsycoObject *po);
EXTERNFN void
psyco_note_call_site(PsycoObject *po, int lasti);
/* give back the psyco_locals_maps indices of a dead owner's call sites */
EXTERNFN void
psyco_release_notes(int *notes, int n);
/* indices made, and free for reuse */
EXTERNVAR long le_notes_made, le_notes_free;

/*****************************************************************/
/***   Emit common instructions                                ***/

/* Returns a condition code for: "'vi' is not null". Warning, when a
   function returns a condition code it must be used immediately, before
   there is any chance for new code to be emitted. If you are unsure, use
   psyco_vinfo_condition() to turn the condition code into a 0 or 1 integer. */
EXTERNFN condition_code_t
integer_non_null(PsycoObject *po, vinfo_t *vi);

/* Same as above, but consumes the reference on 'vi'. Also checks if
   'vi==NULL' and returns CC_ERROR in this case. */
EXTERNFN condition_code_t
integer_NON_NULL(PsycoObject *po, vinfo_t *vi);

/* Instructions with an 'ovf' parameter will check for overflow
   if 'ovf' is true. They return NULL if an overflow is detected. */
EXTERNFN
vinfo_t *
integer_add(PsycoObject *po, vinfo_t *v1, vinfo_t *v2, bool ovf);
EXTERNFN /* 'unsafe': optimize by silently assuming no overflow is possible */
    vinfo_t *
    integer_add_i(PsycoObject *po, vinfo_t *v1, long value2, bool unsafe);
EXTERNFN
vinfo_t *
integer_sub(PsycoObject *po, vinfo_t *v1, vinfo_t *v2, bool ovf);
/*EXTERNFN XXX implement me
  vinfo_t* integer_sub_i(PsycoObject* po, vinfo_t* v1, long value2);*/
EXTERNFN
vinfo_t *
integer_mul(PsycoObject *po, vinfo_t *v1, vinfo_t *v2, bool ovf);
EXTERNFN
vinfo_t *
integer_mul_i(PsycoObject *po, vinfo_t *v1, long value2);
EXTERNFN
vinfo_t *
integer_or(PsycoObject *po, vinfo_t *v1, vinfo_t *v2);
EXTERNFN
vinfo_t *
integer_xor(PsycoObject *po, vinfo_t *v1, vinfo_t *v2);
EXTERNFN
vinfo_t *
integer_and(PsycoObject *po, vinfo_t *v1, vinfo_t *v2);
EXTERNFN
vinfo_t *
integer_lshift(PsycoObject *po, vinfo_t *v1, vinfo_t *v2);
EXTERNFN /* signed */
    vinfo_t *
    integer_rshift(PsycoObject *po, vinfo_t *v1, vinfo_t *v2);
/*EXTERNFN              unsigned
  vinfo_t* integer_urshift  (PsycoObject* po, vinfo_t* v1, vinfo_t* v2);*/
EXTERNFN
vinfo_t *
integer_lshift_i(PsycoObject *po, vinfo_t *v1, long counter);
EXTERNFN /* signed */
    vinfo_t *
    integer_rshift_i(PsycoObject *po, vinfo_t *v1, long counter);
EXTERNFN /* unsigned */
    vinfo_t *
    integer_urshift_i(PsycoObject *po, vinfo_t *v1, long counter);
EXTERNFN
vinfo_t *
integer_inv(PsycoObject *po, vinfo_t *v1);
EXTERNFN
vinfo_t *
integer_neg(PsycoObject *po, vinfo_t *v1, bool ovf);
EXTERNFN
vinfo_t *
integer_abs(PsycoObject *po, vinfo_t *v1, bool ovf);
/* Comparison: 'py_op' is one of Python's rich comparison numbers
   Py_LT, Py_LE, Py_EQ, Py_NE, Py_GT, Py_GE
   optionally together with COMPARE_UNSIGNED, CHEAT_MAXINT. */
EXTERNFN condition_code_t
integer_cmp(PsycoObject *po, vinfo_t *v1, vinfo_t *v2, int py_op);
EXTERNFN condition_code_t
integer_cmp_i(PsycoObject *po, vinfo_t *v1, long value2, int py_op);
#define COMPARE_UNSIGNED 8
#define CHEAT_MAXINT                            \
    16 /* assume only a constant can be exactly \
          LONG_MIN or LONG_MAX */
#define COMPARE_BASE_MASK 7
#define COMPARE_OP_MASK 15

/* Return one of two constants, depending on the condition code */
EXTERNFN vinfo_t *
integer_conditional(PsycoObject *po, condition_code_t cc, long immed_true,
                    long immed_false);

/* make a run-time copy of a vinfo_t */
EXTERNFN vinfo_t *
make_runtime_copy(PsycoObject *po, vinfo_t *v);

/* store 'v' in the local '*slot', releasing what it held */
EXTERNFN void
psyco_rebind_local(PsycoObject *po, vinfo_t **slot, vinfo_t *v);

PSY_INLINE int
intlog2(long value)
{
    int counter = 0;
    while ((1 << counter) < value) counter++;
    return counter;
}

/*****************************************************************/
/***   code termination                                        ***/

/* write a function header reserving 'nframelocal' machine words for
   local storage. These are put in an array under LOC_CONTINUATION as
   run-time values. Do not use this with nframelocal==0; no header is
   needed (so far) in this case. */
EXTERNFN void
psyco_emit_header(PsycoObject *po, int nframelocal);

/* write a return, clearing the stack as necessary, and releases 'po'.
   'retval' may not be virtual-time in the current implementation. */
EXTERNFN code_t *
psyco_finish_return(PsycoObject *po, Source retval);

/*****************************************************************/
/***   run-time switches                                       ***/

typedef struct c_promotion_s {
    source_virtual_t header;
    int pflags;
} c_promotion_t;

#define PFlagPyObj 1
#define PFlagMegamorphic 2
#define PFlagStaticType 4 /* with PFlagPyObj|PFlagMegamorphic */
#define PFlagInterpret 8  /* with PFlagPyObj|PFlagMegamorphic */
#define PFlagFused 16     /* with PFlagPyObj */

/* The pseudo-exceptions meaning 'promote me' but against no particular
   fixed_switch_t. The second one has the SkFlagPyObj flag. */
EXTERNVAR c_promotion_t psyco_nonfixed_promotion;
EXTERNVAR c_promotion_t psyco_nonfixed_pyobj_promotion;

/* The same, but detecting megamorphic sites, where many different run-time
   values keep showing up.  Promotion stop after MEGAMORPHIC_MAX different
   values. */
/*EXTERNVAR c_promotion_t psyco_nonfixed_promotion_mega;*/
EXTERNVAR c_promotion_t psyco_nonfixed_pyobj_promotion_mega;

/* The same, but a value that is not a static (non-heap) type is never made
   a constant: it goes to the site's megamorphic version at once, and that
   version is not one of the MEGAMORPHIC_MAX values. A constant is
   referenced by the compiled code for good, which is free for a static type
   and keeps anything else alive. */
EXTERNVAR c_promotion_t psyco_static_type_promotion;

/* The same, but the megamorphic version has no generic path: from the
   instruction that promotes, the frame goes on in ceval
   (pycompiler.c:psyco_go_interpreted). Psyco_NeedType's, whose callers
   need a type. */
EXTERNVAR c_promotion_t psyco_interp_pyobj_promotion;

/* Psyco_NeedType's in a generator expression's fused code, which can't go
   on in ceval: every value gets its version, but past MEGAMORPHIC_MAX
   values the generator expression stops being fused (mergepoints.h:
   psyco_fuse_stopped), so calls after this one run it as a generator. */
EXTERNVAR c_promotion_t psyco_fused_pyobj_promotion;

#define MEGAMORPHIC_MAX 5

/* an input array that a translated case passes to a C function
   (BUILD_STRING) goes as this many words at most, the
   rest NULL; a longer one as a tuple of its items */
#define ARRAY_ARGS_MAX 4

/* Check if the given virtual source is a promotion exception */
EXTERNFN bool
psyco_vsource_is_promotion(VirtualTimeSource source);

#endif /* _CODEGEN_H */
