/*
 * psyco_unify: given a compatible snapshot, emit the stack and register moves
 * (and reference fix-ups) that turn the current run-time layout into the
 * snapshot's, then jump to its code. References are settled first: those
 * the snapshot's values own are moved or made, and the values it doesn't
 * have are decref'd from where they are, before any stack move can
 * overwrite their slots. If the moves overflow the current buffer they are
 * copied to a new one (data_new_buffer). A value wanted in several
 * registers (shared in po, distinct in the snapshot) is moved to one and
 * copied to the others last. With ALL_CHECKS the generated code verifies
 * every target register after the moves and traps (UD2, SIGILL) on a
 * mismatch; whether a given loop reaches the case depends on register
 * allocation.
 */
#include "idispatcher.h"
#include "../dispatcher.h"
#include "../codemanager.h"
#include "ipyencoding.h"

/***************************************************************/
/***   the hard processor-dependent part of dispatching:     ***/
/***   Unification.                                          ***/

struct dmove_s {
    PsycoObject *po;
    int original_stack_depth;
    char *usages; /* buffer: array of vinfo_t*, see ORIGINAL_VINFO() below */
    int usages_size;
    vinfo_t *copy_regs[REG_TOTAL];
    code_t *code_origin;
    code_t *code_limit;
    code_t *code; /* only used by data_update_stack() */
    CodeBufferObject *private_codebuf;
};

static code_t *
data_new_buffer(code_t *code, struct dmove_s *dm)
{
    /* creates a new buffer containing a copy of the already-written code */
    CodeBufferObject *codebuf;
    int codesize;
    if (dm->private_codebuf != NULL) {
        /* overflowing the regular (large) code buffer */
        psyco_code_continue(&code, &dm->code_limit);
        return code;
    }
    else {
        /* overflowing the small buffer, start a new (regular) one */
        codebuf = psyco_new_code_buffer(NULL, NULL, &dm->code_limit);
        codebuf->snapshot.fz_stuff.fz_stack_depth = dm->original_stack_depth;
        /* the new buffer should be at least as large as the old one */
        codesize = code - dm->code_origin;
        if ((code_t *)codebuf->codestart + codesize > dm->code_limit)
            Py_FatalError("psyco: unexpected unify buffer overflow");
        /* copy old code to new buffer */
        memcpy(codebuf->codestart, dm->code_origin, codesize);
        dm->private_codebuf = codebuf;
        return ((code_t *)codebuf->codestart) + codesize;
    }
}

#define ORIGINAL_VINFO(spos)                                               \
    (*(vinfo_t **)(dm->usages +                                            \
                   (extra_assert(0 <= (spos) && (spos) < dm->usages_size), \
                    (spos))))

static void
data_original_table(vinfo_t *a, RunTimeSource bsource, struct dmove_s *dm)
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

static void
data_transfer_ref(vinfo_t *a, RunTimeSource bsource, struct dmove_s *dm)
{
    PsycoObject *po = dm->po;
    code_t *code = dm->code;
    char rg;
    /* check for values passing from no-reference to reference */
    if (has_rtref(bsource)) {     /* destination has ref */
        if (has_rtref(a->source)) /* source has ref too */
        {
            /* remove the reference from 'a' because it now belongs
               to 'b' ('b->source' itself is in the frozen snapshot
               and must not be modified!) */
            a->source = remove_rtref(a->source);
        }
        else {
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
    if (code > dm->code_limit)
        /* oops, buffer overflow. Start a new buffer */
        code = data_new_buffer(code, dm);
    dm->code = code;
}

static void
data_update_stack(vinfo_t *a, RunTimeSource bsource, struct dmove_s *dm)
{
    PsycoObject *po = dm->po;
    code_t *code = dm->code;
    long dststack = getstack(bsource);
    long srcstack = getstack(a->source);
    char rg, rgb;
    vinfo_t *overridden;

    /* The operation below is: copy the value currently held by 'a'
       into the stack position 'dststack'. */
    rgb = getreg(bsource);
    if (rgb != REG_NONE)
        dm->copy_regs[(int)rgb] = a;
    if (dststack == RUNTIME_STACK_NONE || dststack == srcstack)
        ; /* nothing to do */
    else {
        rg = RUNTIME_REG(a);
        if (rg == REG_NONE) /* load 'a' into a register before it can be */
        {                   /* stored back in the stack                  */
            NEED_FREE_REG(rg);
            LOAD_REG_FROM_EBP_BASE(rg, srcstack);
            REG_NUMBER(po, rg) = a;
            /*SET_RUNTIME_REG_TO(a, rg); ignored*/
        }
        /* is there already a pending value at 'dststack'? */
        overridden = ORIGINAL_VINFO(dststack);
        if (overridden == NULL || RUNTIME_STACK(overridden) != dststack)
            goto can_save_only; /* no -- just save the new value to 'dststack'.
                                   The case RUNTIME_STACK(overridden) !=
                                   dststack corresponds to a vinfo_t which has
                                   been moved elsewhere in the mean time. */

        /* yes -- careful! We might have to save the current value of
           'dststack' before we can overwrite it. */
        SET_RUNTIME_STACK_TO_NONE(overridden);

        if (!RUNTIME_REG_IS_NONE(overridden)) {
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
        else {
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

static code_t *
data_free_unused(code_t *code, struct dmove_s *dm, vinfo_array_t *aa)
{
    /* decref any object that would be present in 'po' but not at all in
       the snapshot. Note that it is uncommon that this function actually
       finds any unused object at all. */
    int i = aa->count;
    while (i--) {
        vinfo_t *a = aa->items[i];
        if (a != NULL) {
            if (has_rtref(a->source)) {
                PsycoObject *po = dm->po;
                code_t *saved_code;
                a->source = remove_rtref(a->source);

                saved_code = po->code;
                po->code = code;
                if (is_maybe_null(a->source))
                    psyco_xdecref_rt(po, a);
                else
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

#if ALL_CHECKS
/* nothing in 'po' owns a reference any more (data_transfer_ref,
   data_free_unused): the stack moves may overwrite any slot that the
   snapshot doesn't use */
static void
check_no_rtref(vinfo_array_t *aa)
{
    int i = aa->count;
    while (i--)
        if (aa->items[i] != NULL) {
            extra_assert(!is_runtime(aa->items[i]->source) ||
                         !has_rtref(aa->items[i]->source));
            if (aa->items[i]->array != NullArray)
                check_no_rtref(aa->items[i]->array);
        }
}
#endif

#if ALL_CHECKS
/* Debug check of the register moves below: before them,
   each value wanted in a register is copied from where it is now to
   unify_check_buf; after them, the generated code compares every target
   register with its copy and traps (UD2) on a mismatch. Each piece is
   small and position-independent, as data_new_buffer may move the code
   written so far. */
static long unify_check_buf[REG_TOTAL];

static code_t *
unify_check_save(code_t *code, struct dmove_s *dm, vinfo_t *wanted[])
{
    int i;
    for (i = 0; i < REG_TOTAL; i++) {
        if (wanted[i] != NULL && RUNTIME_REG(wanted[i]) != REG_NONE) {
            le_trace_begin_simple_code(code);
            MOV_R_IABS(REG_TRANSIENT_2, (long)&unify_check_buf[i]);
            MOV_O_R(REG_TRANSIENT_2, 0, RUNTIME_REG(wanted[i]));
            le_trace_end_simple_code(code);
            if (code > dm->code_limit)
                code = data_new_buffer(code, dm);
        }
    }
    return code;
}

static code_t *
unify_check_verify(code_t *code, struct dmove_s *dm, vinfo_t *wanted[],
                   bool saved[])
{
    int i;
    for (i = 0; i < REG_TOTAL; i++) {
        if (saved[i]) {
            le_trace_begin_simple_code(code);
            WRITE_1(0x9C); /* PUSHFQ */
            MOV_R_IABS(REG_TRANSIENT_2, (long)&unify_check_buf[i]);
            MOV_R_O(REG_TRANSIENT_1, REG_TRANSIENT_2, 0);
            CMP_R_R(REG_TRANSIENT_1, i);
            {
                BEGIN_SHORT_COND_JUMP(0, CC_E);
                WRITE_2(0x0F, 0x0B); /* UD2 */
                END_SHORT_JUMP(0);
            }
            WRITE_1(0x9D); /* POPFQ */
            le_trace_end_simple_code(code);
            if (code > dm->code_limit)
                code = data_new_buffer(code, dm);
        }
    }
    return code;
}
#endif

DEFINEFN
code_t *
psyco_unify(PsycoObject *po, vcompatible_t *lastmatch,
            CodeBufferObject **target)
{
    /* Update 'po' to match 'lastmatch', then jump to 'lastmatch'. */

    int i;
    struct dmove_s dm;
    vinfo_t *wanted[REG_TOTAL]; /* the targets, before (1) clears them */
    reg_t copy_from[REG_TOTAL]; /* (1b): copy this register to i */
#if ALL_CHECKS
    bool check_saved[REG_TOTAL];
#endif
    code_t *code = po->code;
    code_t *backpointer;
    CodeBufferObject *target_codebuf = lastmatch->matching;
    int sdepth = get_stack_depth(&target_codebuf->snapshot);
    le_trace_unify(po, target_codebuf);
    psyco_owner_depend(target_codebuf);
    extra_assert(lastmatch->diff ==
                 NullArray); /* unify with exact match only */
    psyco_assert_coherent(po);
    extra_assert(!psyco_xmm_live(po)); /* snapshots hold no XMM register */
    dm.usages_size = sdepth + sizeof(vinfo_t **);
    dm.usages = (char *)PyMem_MALLOC(dm.usages_size);
    if (dm.usages == NULL)
        OUT_OF_MEMORY();
    memset(dm.usages, 0, dm.usages_size); /* set to all NULL */
    memset(dm.copy_regs, 0, sizeof(dm.copy_regs));
    fz_find_runtimes(&po->vlocals, &target_codebuf->snapshot,
                     (fz_find_fn)&data_original_table, &dm, false);

    dm.po = po;
    dm.original_stack_depth = po->stack_depth;
    dm.code_origin = code;
    dm.code_limit = po->codelimit == NULL ? code : po->codelimit;
    dm.private_codebuf = NULL;

    if (sdepth > po->stack_depth) {
        /* more items in the target stack (uncommon case).
           Let the stack grow. */
        le_trace_begin_simple_code(code);
        STACK_CORRECTION(sdepth - po->stack_depth);
        le_trace_end_simple_code(code);
        po->stack_depth = sdepth;
    }

    /* the references the snapshot's values own: moved from the value in
       'po', or new */
    dm.code = code;
    fz_find_runtimes(&po->vlocals, &target_codebuf->snapshot,
                     (fz_find_fn)&data_transfer_ref, &dm, false);
    code = dm.code;

    /* decref any object that would be present in 'po' but not at all in
       the snapshot (data_transfer_ref() has removed the 'ref' tag of all
       vinfo_ts it actually used from 'po'). Before the stack moves: they
       know only the values the snapshot has, and may overwrite the slot
       of one it hasn't. */
    code = data_free_unused(code, &dm, &po->vlocals);
#if ALL_CHECKS
    check_no_rtref(&po->vlocals);
#endif

    /* update the stack */
    dm.code = code;
    fz_find_runtimes(&po->vlocals, &target_codebuf->snapshot,
                     (fz_find_fn)&data_update_stack, &dm, true);
    code = dm.code;

    /* update the registers (1): reg-to-reg moves and exchanges. A value
       can be wanted in several registers (shared in 'po', distinct in the
       snapshot): it is moved to one target, and copied to the others in
       (1b), once every register holds its final value. */
    memcpy(wanted, dm.copy_regs, sizeof(wanted));
    for (i = 0; i < REG_TOTAL; i++) copy_from[i] = REG_NONE;
#if ALL_CHECKS
    for (i = 0; i < REG_TOTAL; i++)
        check_saved[i] =
            wanted[i] != NULL && RUNTIME_REG(wanted[i]) != REG_NONE;
    code = unify_check_save(code, &dm, wanted);
#endif
    for (i = 0; i < REG_TOTAL; i++) {
        vinfo_t *v = dm.copy_regs[i];
        if (v != NULL) {
            reg_t rg = RUNTIME_REG(v);
            if (rg != REG_NONE) {
                if (rg != i && wanted[(int)rg] == v) {
                    /* v must also stay in rg: copy it in (1b) */
                    copy_from[i] = rg;
                }
                else if (rg != i) {
                    /* Value v is in rg but needs to be in register i */
                    vinfo_t *v2 = REG_NUMBER(po, i);
                    if (v2 != NULL) {
                        /* Theres something in register i exchange and update
                         */
                        le_trace_begin_simple_code(code);
                        XCHG_R_R(rg, i);
                        REG_NUMBER(po, i) = v;
                        SET_RUNTIME_REG_TO(v, i);
                        REG_NUMBER(po, rg) = v2;
                        SET_RUNTIME_REG_TO(v2, rg);
                        le_trace_end_simple_code(code);
                    }
                    else {
                        /* Register i is free just move rg to i */
                        le_trace_begin_simple_code(code);
                        MOV_R_R(i, rg);
                        REG_NUMBER(po, rg) = NULL;
                        REG_NUMBER(po, i) = v;
                        SET_RUNTIME_REG_TO(v, i);
                        le_trace_end_simple_code(code);
                    }
                }
                dm.copy_regs[i] = NULL;
            }
        }
    }
    /* (1b): the extra copies. A register written here is a target of v, so
       whatever it held is not wanted in any register (that value has
       already been moved to its own target, or is wanted only on the
       stack, which is up to date). */
    for (i = 0; i < REG_TOTAL; i++) {
        if (copy_from[i] != REG_NONE) {
            vinfo_t *w = REG_NUMBER(po, i);
            extra_assert(RUNTIME_REG(wanted[i]) == copy_from[i]);
            le_trace_begin_simple_code(code);
            MOV_R_R(i, copy_from[i]);
            le_trace_end_simple_code(code);
            if (w != NULL && w != wanted[i]) {
                SET_RUNTIME_REG_TO_NONE(w);
                REG_NUMBER(po, i) = NULL;
            }
        }
    }
    if (code > dm.code_limit) {
        /* start a new buffer if we wrote past the end */
        code = data_new_buffer(code, &dm);
    }
#if ALL_CHECKS
    code = unify_check_verify(code, &dm, wanted, check_saved);
#endif
    /* update the registers (2): stack-to-register loads */
    for (i = 0; i < REG_TOTAL; i++) {
        vinfo_t *v = dm.copy_regs[i];
        if (v != NULL) {
            le_trace_begin_simple_code(code);
            RTVINFO_FROM_STACK_TO_REG(v, i);
            le_trace_end_simple_code(code);
        }
    }

    /* done */
    le_trace_begin_simple_code(code);
    STACK_CORRECTION(sdepth - po->stack_depth);
    le_trace_end_simple_code(code);

    if (code > dm.code_limit) {
        /* start a new buffer if we wrote past the end */
        code = data_new_buffer(code, &dm);
    }
    backpointer = code;
    /* psyco_unify_code() passes a throwaway stack buffer with no room
       (codelimit NULL): if nothing needed a new buffer, the jump would be
       discarded, and it can't reach the code from the stack anyway */
    if (po->codelimit != NULL || dm.private_codebuf != NULL) {
        le_trace_begin_simple_code(code);
        JUMP_TO((code_t *)target_codebuf->codestart);
        le_trace_end_simple_code(code);
    }

    /* start a new buffer if the last JUMP_TO overflowed,
       but not if we had no room at all in the first place. */
    if (code > dm.code_limit && po->codelimit != NULL) {
        /* the JMP instruction emitted by JUMP_TO() is not position-
           independent; we must emit it again at the new position */
        code = data_new_buffer(backpointer, &dm);
        le_trace_begin_simple_code(code);
        JUMP_TO((code_t *)target_codebuf->codestart);
        le_trace_end_simple_code(code);
        psyco_assert(code <= dm.code_limit);
    }

    PyMem_FREE(dm.usages);
    if (dm.private_codebuf == NULL) {
        /* no new buffer created */
        *target = target_codebuf;
    }
    else {
        CLOSE_CODE("unify", dm.private_codebuf->codestart, code);
        *target = dm.private_codebuf;
        /* add a jump from the original code buffer to the new one, unless
           that is psyco_unify_code()'s throwaway stack buffer (codelimit
           NULL): the jump could not reach from there, and nothing runs it */
        code = po->code;
        if (po->codelimit != NULL) {
            le_trace_begin_simple_code(code);
            JUMP_TO((code_t *)dm.private_codebuf->codestart);
            le_trace_end_simple_code(code);
        }
    }
    PsycoObject_Delete(po);
    return code;
}
