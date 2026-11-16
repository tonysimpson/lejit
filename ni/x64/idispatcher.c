/*
 * psyco_unify: given a compatible snapshot, emit the stack and register moves
 * (and reference fix-ups) that turn the current run-time layout into the
 * snapshot's, then jump to its code. If the moves overflow the current buffer
 * they are copied to a new one (data_new_buffer). The register-move phase was
 * rewritten for x86-64 and may mishandle a value that must end up in two
 * registers at once (the XCHG case). Untested.
 */
#include "idispatcher.h"
#include "../dispatcher.h"
#include "../codemanager.h"
#include "ipyencoding.h"


/***************************************************************/
 /***   the hard processor-dependent part of dispatching:     ***/
  /***   Unification.                                          ***/


struct dmove_s {
  PsycoObject* po;
  int original_stack_depth;
  char* usages;   /* buffer: array of vinfo_t*, see ORIGINAL_VINFO() below */
  int usages_size;
  vinfo_t* copy_regs[REG_TOTAL];
  code_t* code_origin;
  code_t* code_limit;
  code_t* code;   /* only used by data_update_stack() */
  CodeBufferObject* private_codebuf;
};

static code_t* data_new_buffer(code_t* code, struct dmove_s* dm)
{
  /* creates a new buffer containing a copy of the already-written code */
  CodeBufferObject* codebuf;
  int codesize;
  if (dm->private_codebuf != NULL)
    {
      /* overflowing the regular (large) code buffer */
      psyco_emergency_enlarge_buffer(&code, &dm->code_limit);
      return code;
    }
  else
    {
      /* overflowing the small buffer, start a new (regular) one */
      codebuf = psyco_new_code_buffer(NULL, NULL, &dm->code_limit);
      codebuf->snapshot.fz_stuff.fz_stack_depth = dm->original_stack_depth;
      /* the new buffer should be at least as large as the old one */
      codesize = code - dm->code_origin;
      if ((code_t*) codebuf->codestart + codesize > dm->code_limit)
        Py_FatalError("psyco: unexpected unify buffer overflow");
      /* copy old code to new buffer */
      memcpy(codebuf->codestart, dm->code_origin, codesize);
      dm->private_codebuf = codebuf;
      return ((code_t*) codebuf->codestart) + codesize;
    }
}

#define ORIGINAL_VINFO(spos)    (*(vinfo_t**)(dm->usages + (            \
		extra_assert(0 <= (spos) && (spos) < dm->usages_size),  \
                (spos))))

static void data_original_table(vinfo_t* a, RunTimeSource bsource,
                                struct dmove_s* dm)
{
  /* called on each run-time vinfo_t in the FrozenPsycoObject.
     Record in the array dm->usages which vinfo_t is found at what position
     in the stack. Ignore the ones after dm->usages_size: they correspond to
     stack positions which will soon be deleted (because the stack will
     shrink). Note: this uses the fact that RUNTIME_STACK_NONE is zero
     and uses the 0th item of dm->usages_size as general trash. */
  if (RUNTIME_STACK(a) < dm->usages_size)
    ORIGINAL_VINFO(RUNTIME_STACK(a)) = a;
}

static void data_update_stack(vinfo_t* a, RunTimeSource bsource,
                              struct dmove_s* dm)
{
  PsycoObject* po = dm->po;
  code_t* code = dm->code;
  long dststack = getstack(bsource);
  long srcstack = getstack(a->source);
  char rg, rgb;
  vinfo_t* overridden;
  /* check for values passing from no-reference to reference */
  if (has_rtref(bsource)) {  /* destination has ref */
    if (has_rtref(a->source))   /* source has ref too */
      {
        /* remove the reference from 'a' because it now belongs
           to 'b' ('b->source' itself is in the frozen snapshot
           and must not be modified!) */
        a->source = remove_rtref(a->source);
      }
    else
      {
        /* create a new reference for 'b'. Note that if the same
           'a' is copied to several 'b's during data_update_stack()
           as is allowed by graph quotient detection in
           psyco_compatible(), then only the first copy will get
           the original reference owned by 'a' (if any) and for
           the following copies the following increfing code is
           executed as well. */
        RTVINFO_IN_REG(a);
        rg = RUNTIME_REG(a);
        INC_OB_REFCNT_CC(rg);
      }
  }
  /* 'a' must no longer own a reference at this point.
     The case of 'b' wanting no reference but 'a' having one
     is forbidden by psyco_compatible() because decrefing 'a'
     would potentially leave a freed pointer in 'b'. */
  extra_assert(!has_rtref(a->source));

  /* The operation below is: copy the value currently held by 'a'
     into the stack position 'dststack'. */
  rgb = getreg(bsource);
  if (rgb != REG_NONE)
    dm->copy_regs[(int)rgb] = a;
  if (dststack == RUNTIME_STACK_NONE || dststack == srcstack)
    ;  /* nothing to do */
  else
    {
      rg = RUNTIME_REG(a);
      if (rg == REG_NONE)  /* load 'a' into a register before it can be */
        {                  /* stored back in the stack                  */
          NEED_FREE_REG(rg);
          LOAD_REG_FROM_EBP_BASE(rg, srcstack);
          REG_NUMBER(po, rg) = a;
          /*SET_RUNTIME_REG_TO(a, rg); ignored*/
        }
      /* is there already a pending value at 'dststack'? */
      overridden = ORIGINAL_VINFO(dststack);
      if (overridden == NULL || RUNTIME_STACK(overridden) != dststack)
        goto can_save_only; /* no -- just save the new value to 'dststack'.
                               The case RUNTIME_STACK(overridden) != dststack
                               corresponds to a vinfo_t which has been moved
                               elsewhere in the mean time. */
      
      /* yes -- careful! We might have to save the current value of
         'dststack' before we can overwrite it. */
      SET_RUNTIME_STACK_TO_NONE(overridden);
  
      if (!RUNTIME_REG_IS_NONE(overridden))
        {
          /* no need to save the value, it is already in a register too */
        can_save_only:
          /* copy 'a' to 'dststack' */
          SAVE_REG_TO_EBP_BASE(rg, dststack);
          /*if (rgb == REG_NONE)
             {
              REG_NUMBER(po, rg) = NULL;
              rg = REG_NONE;
             }*/
        }
      else
        {
          /* save 'a' to 'dststack' and load the previous value of 'dststack'
             back into the register 'rg' */
          XCHG_REG_AND_EBP_BASE(rg, dststack);
          SET_RUNTIME_REG_TO(overridden, rg);
          REG_NUMBER(po, rg) = overridden;
          rg = REG_NONE;
        }
      /* Now 'a' is at 'dststack', but might still be in 'rg' too */
      a->source = RunTime_New1(dststack, rg, false, false);
      ORIGINAL_VINFO(dststack) = a; /* 'a' is now there */
      
      if (code > dm->code_limit)
        /* oops, buffer overflow. Start a new buffer */
        code = data_new_buffer(code, dm);
      
    }
  dm->code = code;
}

static code_t* data_free_unused(code_t* code, struct dmove_s* dm,
                                vinfo_array_t* aa)
{
  /* decref any object that would be present in 'po' but not at all in
     the snapshot. Note that it is uncommon that this function actually
     finds any unused object at all. */
  int i = aa->count;
  while (i--)
    {
      vinfo_t* a = aa->items[i];
      if (a != NULL)
        {
          if (has_rtref(a->source))
            {
              PsycoObject* po = dm->po;
              code_t* saved_code;
              a->source = remove_rtref(a->source);
              
              saved_code = po->code;
              po->code = code;
              psyco_decref_rt(po, a);
              code = po->code;
              po->code = saved_code;

              if (code > dm->code_limit)
                /* oops, buffer overflow. Start a new buffer */
                code = data_new_buffer(code, dm);
            }
          if (a->array != NullArray)
            code = data_free_unused(code, dm, a->array);
        }
    }
  return code;
}

DEFINEFN
code_t* psyco_unify(PsycoObject* po, vcompatible_t* lastmatch,
                    CodeBufferObject** target)
{
  /* Update 'po' to match 'lastmatch', then jump to 'lastmatch'. */

  int i;
  struct dmove_s dm;
  code_t* code = po->code;
  code_t* backpointer;
  CodeBufferObject* target_codebuf = lastmatch->matching;
  int sdepth = get_stack_depth(&target_codebuf->snapshot);
  ni_trace_unify(po, target_codebuf); 
  extra_assert(lastmatch->diff == NullArray);  /* unify with exact match only */
  psyco_assert_coherent(po);
  dm.usages_size = sdepth + sizeof(vinfo_t**);
  dm.usages = (char*) PyMem_MALLOC(dm.usages_size);
  if (dm.usages == NULL)
    OUT_OF_MEMORY();
  memset(dm.usages, 0, dm.usages_size);   /* set to all NULL */
  memset(dm.copy_regs, 0, sizeof(dm.copy_regs));
  fz_find_runtimes(&po->vlocals, &target_codebuf->snapshot,
                   (fz_find_fn) &data_original_table,
                   &dm, false);

  dm.po = po;
  dm.original_stack_depth = po->stack_depth;
  dm.code_origin = code;
  dm.code_limit = po->codelimit == NULL ? code : po->codelimit;
  dm.private_codebuf = NULL;

  if (sdepth > po->stack_depth)
    {
      /* more items in the target stack (uncommon case).
         Let the stack grow. */
      ni_trace_begin_simple_code(code);
      STACK_CORRECTION(sdepth - po->stack_depth);
      ni_trace_end_simple_code(code);
      po->stack_depth = sdepth;
    }

  /* update the stack */
  dm.code = code;
  fz_find_runtimes(&po->vlocals, &target_codebuf->snapshot,
                   (fz_find_fn) &data_update_stack,
                   &dm, true);
  code = dm.code;

  /* decref any object that would be present in 'po' but not at all in
     the snapshot (data_update_stack() has removed the 'ref' tag of all
     vinfo_ts it actually used from 'po') */
  code = data_free_unused(code, &dm, &po->vlocals);

  /* update the registers (1): reg-to-reg moves and exchanges */
    for (i=0; i<REG_TOTAL; i++) {
        vinfo_t* v = dm.copy_regs[i];
        if (v != NULL) {
            reg_t rg = RUNTIME_REG(v);
            if (rg != REG_NONE) {
                if (rg != i) {
                    /* Value v is in rg but needs to be in register i */
                    vinfo_t *v2 = REG_NUMBER(po, i);
                    if (v2 != NULL) {
                        /* Theres something in register i exchange and update */
                        ni_trace_begin_simple_code(code);
                        XCHG_R_R(rg, i);
                        REG_NUMBER(po, i) = v;
                        SET_RUNTIME_REG_TO(v, i);
                        REG_NUMBER(po, rg) = v2;
                        SET_RUNTIME_REG_TO(v2, rg);
                        ni_trace_end_simple_code(code);
                    }
                    else {
                        /* Register i is free just move rg to i */
                        ni_trace_begin_simple_code(code);
                        MOV_R_R(i, rg);
                        REG_NUMBER(po, rg) = NULL;
                        REG_NUMBER(po, i) = v;
                        SET_RUNTIME_REG_TO(v, i);
                        ni_trace_end_simple_code(code);
                    }
                }
                dm.copy_regs[i] = NULL;
            } 
        }
    }
    if (code > dm.code_limit) {
        /* start a new buffer if we wrote past the end */
        code = data_new_buffer(code, &dm);
    }
    /* update the registers (2): stack-to-register loads */
    for (i=0; i<REG_TOTAL; i++) {
        vinfo_t* v = dm.copy_regs[i];
        if (v != NULL) {
            ni_trace_begin_simple_code(code);
            RTVINFO_FROM_STACK_TO_REG(v, i);
            ni_trace_end_simple_code(code);

        }
    }

    /* done */
    ni_trace_begin_simple_code(code);
    STACK_CORRECTION(sdepth - po->stack_depth);
    ni_trace_end_simple_code(code);

  if (code > dm.code_limit) {
    /* start a new buffer if we wrote past the end */
    code = data_new_buffer(code, &dm);
  }
  backpointer = code;
  /* psyco_unify_code() passes a throwaway stack buffer with no room
     (codelimit NULL): if nothing needed a new buffer, the jump would be
     discarded, and it can't reach the code from the stack anyway (B5) */
  if (po->codelimit != NULL || dm.private_codebuf != NULL) {
    ni_trace_begin_simple_code(code);
    JUMP_TO((code_t*) target_codebuf->codestart);
    ni_trace_end_simple_code(code);
  }
  
  /* start a new buffer if the last JUMP_TO overflowed,
     but not if we had no room at all in the first place. */
  if (code > dm.code_limit && po->codelimit != NULL)
    {
      /* the JMP instruction emitted by JUMP_TO() is not position-
         independent; we must emit it again at the new position */
      code = data_new_buffer(backpointer, &dm);
      ni_trace_begin_simple_code(code);
      JUMP_TO((code_t*) target_codebuf->codestart);
      ni_trace_end_simple_code(code);
      psyco_assert(code <= dm.code_limit);
    }
  
  PyMem_FREE(dm.usages);
  if (dm.private_codebuf == NULL)
    {
      Py_INCREF(target_codebuf);      /* no new buffer created */
      *target = target_codebuf;
    }
  else
    {
      SHRINK_CODE_BUFFER(dm.private_codebuf, code, "unify");
      *target = dm.private_codebuf;
      /* add a jump from the original code buffer to the new one */
      code = po->code;
      ni_trace_begin_simple_code(code);
      JUMP_TO((code_t*) dm.private_codebuf->codestart);
      ni_trace_end_simple_code(code);
    }
  PsycoObject_Delete(po);
  return code;
}
