/*
 * Code patching used by the dispatcher. conditional_jump_to emits a rel32 jump
 * and returns a tag; change_cond_jump_target later repoints it once the target
 * has been compiled (coding pauses, respawn). A promotion site starts with a
 * patchable 'MOV R10, imm64; CMP reg, R10; JE target', which
 * fix_fast_common_case rewrites to the last value seen: an inline cache in
 * front of the C dispatcher. All jumps are rel32, so every code buffer must be
 * within +-2 GB of every other, and nothing guarantees this.
 */
 /***************************************************************/
/***       Processor-specific routines for dispatcher.c        ***/
 /***************************************************************/

#ifndef _IDISPATCHER_H
#define _IDISPATCHER_H

#include "../vcompiler.h"
#include "../codegen.h"
#include "iencoding.h"

#define NEED_STACK_FRAME_HACK    1


/***************************************************************/
 /***   Unification                                           ***/

/* idispatcher.c implements psyco_unify(),
   whose header is given in dispatcher.h.
   Conversely, dispatcher.c implements the following function
   which is declared here because it is really internal: */
typedef void (*fz_find_fn) (vinfo_t* a, RunTimeSource bsource, void* extra);
EXTERNFN void fz_find_runtimes(vinfo_array_t* aa, FrozenPsycoObject* fpo,
                               fz_find_fn callback, void* extra, bool clear);


/***************************************************************/
 /***   Promotion                                             ***/

/* Define to 1 to emit a "compare/jump-if-equal" pair of instructions
   that checks for the most common case (actually the last seen one). */
#define PROMOTION_FAST_COMMON_CASE   1


#if PROMOTION_FAST_COMMON_CASE
                                       /* for FIX_JUMP_IF_EQUAL() */
#  define INTERNAL_PROMOTION_FIELDS    code_t* jump_if_equal_code;
#else
#  define INTERNAL_PROMOTION_FIELDS    /* nothing */
#endif

struct ipromotion_s {
  INTERNAL_PROMOTION_FIELDS
  /* more stuff follows, this structure is only the head of what
     dispatch.c defines as rt_promotion_t */
};


#define UPDATABLE_JUMP_IF_EQUALS(rg, value, target) do {\
    MOV_R_UI(REG_TRANSIENT_1, value);\
    CMP_R_R(rg, REG_TRANSIENT_1);\
    JMP_CC_UI32(CC_E, (long)(target));\
} while (0)


PSY_INLINE code_t* fix_fast_common_case(void* fs, long value,
                                    code_t* codeptr)
{
#if PROMOTION_FAST_COMMON_CASE
#undef ONLY_UPDATING
#define ONLY_UPDATING 1
  code_t *code = ((struct ipromotion_s*)fs)->jump_if_equal_code;
  UPDATABLE_JUMP_IF_EQUALS(0, value, codeptr);
#undef ONLY_UPDATING
#define ONLY_UPDATING 0
#endif
  return codeptr;
}


PSY_INLINE void* ipromotion_finish(PsycoObject* po, vinfo_t* fix, void* do_promotion, size_t block_size)
{
  long xsource;
  struct ipromotion_s* fs;

#if PROMOTION_FAST_COMMON_CASE
  code_t *start_jump, *end_jump;
  BEGIN_CODE
  NEED_CC();
  RTVINFO_IN_REG(fix);
  xsource = fix->source;
  start_jump = code;
  UPDATABLE_JUMP_IF_EQUALS(getreg(xsource), 0, code);
  end_jump = code;
#undef ONLY_UPDATING
#define ONLY_UPDATING 1
  code = start_jump;
  UPDATABLE_JUMP_IF_EQUALS(0, 0, end_jump); /* rewrite with jump to end of jump */
#undef ONLY_UPDATING
#define ONLY_UPDATING 0
  END_CODE
#else
  xsource = fix->source;
#endif
  fs = (struct ipromotion_s*) psyco_call_code_builder(po, do_promotion,
                                                      PROMOTION_FAST_COMMON_CASE,
                                                      xsource, block_size);
#if PROMOTION_FAST_COMMON_CASE
  fs->jump_if_equal_code = start_jump;
#endif
  return fs;
}


/***************************************************************/
 /***   Misc.                                                 ***/

PSY_INLINE void* conditional_jump_to(PsycoObject* po, code_t* target,
                                 condition_code_t condition)
{
  BEGIN_CODE
  switch (condition) {
  case CC_ALWAYS_FALSE:          /* never jumps */
    break;
  case CC_ALWAYS_TRUE:
    JUMP_TO(target);             /* always jumps */
    break;
  default:
    FAR_COND_JUMP_TO(target, condition);
  }
  END_CODE
  return po->code;
}

PSY_INLINE void change_cond_jump_target(void* tag, code_t* newtarget)
{
  code_t* code = (code_t*) tag;
  /* safety check: do not write a JMP whose target is itself...
     would make an endless loop */
  psyco_assert(newtarget != code-5 && newtarget != code-6);
  CHANGE_JUMP_TO(newtarget);
}

PSY_INLINE code_t* resume_after_cond_jump(void* tag)
{
  return (code_t*) tag;
}

/* reserve a small buffer of code behind po->code in which conditional
   code can be stored.  See make_code_conditional(). */
PSY_INLINE void* setup_conditional_code_bounds(PsycoObject* po, PsycoObject* po2,
                                           condition_code_t condition)
{
  code_t* code2 = po->code + SIZE_OF_SHORT_CONDITIONAL_JUMP;
  po2->code = code2;
  po2->codelimit = code2 + RANGE_OF_SHORT_CONDITIONAL_JUMP;
  return NULL;
}

/* mark a small buffer reserved by setup_conditional_code_bounds() to be
   only executed if 'condition' holds. */
PSY_INLINE void make_code_conditional(PsycoObject* po, code_t* codeend,
                                  condition_code_t condition, void* extra)
{
  code_t* target;
  code_t* code2 = po->code + SIZE_OF_SHORT_CONDITIONAL_JUMP;
  extra_assert(code2 <= codeend &&
               codeend <= code2 + RANGE_OF_SHORT_CONDITIONAL_JUMP);
  BEGIN_CODE
  if (IS_A_SINGLE_JUMP(code2, codeend, target))
    FAR_COND_JUMP_TO(target, condition);  /* replace a jump with a cond jump */
  else
    { /* other cases: write a short cond jump to skip the block if !condition */
      SHORT_COND_JUMP_TO(codeend, INVERT_CC(condition));
      code = codeend;
    }
  END_CODE
  /* record the known value of 'condition' in the 'po' (non-jump) branch */
  psyco_resolved_cc(po, INVERT_CC(condition));
}


#endif /* _IDISPATCHER_H */
