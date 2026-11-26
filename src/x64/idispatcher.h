/*
 * Code patching used by the dispatcher. conditional_jump_to emits a rel32 jump
 * and returns a tag; change_cond_jump_target later repoints it once the target
 * has been compiled (coding pauses, respawn). A promotion site starts with a
 * patchable 'MOV R10, imm64; CMP reg, R10; JE target' holding the first
 * value seen, and a chain of more out of line (below): a polymorphic inline
 * cache in front of the C dispatcher. All jumps are rel32; one whose target
 * is out of reach goes through a far jump (codemanager.c:psyco_code_far_jump).
 */
/***************************************************************/
/***       Processor-specific routines for dispatcher.c        ***/
/***************************************************************/

#ifndef _IDISPATCHER_H
#define _IDISPATCHER_H

#include "../vcompiler.h"
#include "../codegen.h"
#include "iencoding.h"

#define NEED_STACK_FRAME_HACK 1

/***************************************************************/
/***   Unification                                           ***/

/* idispatcher.c implements psyco_unify(),
   whose header is given in dispatcher.h.
   Conversely, dispatcher.c implements the following function
   which is declared here because it is really internal: */
typedef void (*fz_find_fn)(vinfo_t *a, RunTimeSource bsource, void *extra);
EXTERNFN void
fz_find_runtimes(vinfo_array_t *aa, FrozenPsycoObject *fpo,
                 fz_find_fn callback, void *extra, bool clear);

/***************************************************************/
/***   Promotion                                             ***/

/* Define to 1 to emit a "compare/jump-if-equal" pair of instructions
   that checks for the first value seen, followed by a chain of more. */
#define PROMOTION_FAST_COMMON_CASE 1

/* A site's inline cache holds the first value seen. The next
   PROMOTION_CHAIN_MAX values go in a chain of the same compare/je entries,
   out of line in the stub pool, which the site's miss path is repointed to
   (the 'jmp chain' overwrites the start of the site's own stub). The chain
   ends with its own copy of the miss stub, or, once a site can compile no
   more values, a jump to its megamorphic version. Most chains hold one
   entry, so a chain starts with room for one and is copied to a chain with
   room for all when it needs more; the old one is left unused. With the
   chain full, a new value replaces an entry that goes to the megamorphic
   version, or else the last one; at the cut-off the cache is refilled with
   the site's values (dispatcher.c:promotion_cache_add,
   promotion_cache_fallback). Code is only patched from
   the C dispatcher, which the trampoline entered from this site's miss path
   (or its chain's); it returns through 'jmp rax' to a target, never back
   into the bytes it changed. */
#define PROMOTION_CHAIN_MAX 4   /* MEGAMORPHIC_MAX - 1 */
#define PROMOTION_ENTRY_SIZE 19 /* mov r10, imm64; cmp; je rel32 */
#define PROMOTION_MISS_SIZE_MAX (3 + STUB_SIZE_MAX) /* mov rsi, reg; stub */
#define PROMOTION_CHAIN_SIZE(n) \
    ((n) * PROMOTION_ENTRY_SIZE + PROMOTION_MISS_SIZE_MAX)

#if PROMOTION_FAST_COMMON_CASE
#define INTERNAL_PROMOTION_FIELDS                                    \
    code_t *jump_if_equal_code; /* the inline entry */               \
    code_t *chain;              /* NULL until the second value */    \
    unsigned char reg;          /* the register compared */          \
    unsigned char ncached;      /* values inline and in the chain */ \
    unsigned char chaincap;     /* entries the chain has room for */ \
    code_t *fallback;           /* NULL, or where the chain ends */
#else
#define INTERNAL_PROMOTION_FIELDS /* nothing */
#endif

struct ipromotion_s {
    INTERNAL_PROMOTION_FIELDS
    /* more stuff follows, this structure is only the head of what
       dispatch.c defines as rt_promotion_t */
};

#define UPDATABLE_JUMP_IF_EQUALS(rg, value, target) \
    do {                                            \
        MOV_R_UI(REG_TRANSIENT_1, value);           \
        CMP_R_R(rg, REG_TRANSIENT_1);               \
        JMP_CC_UI32(CC_E, (long)(target));          \
    } while (0)

/* write one chain entry at 'code'; returns its end */
PSY_INLINE code_t *
ipromotion_write_entry(code_t *code, int rg, long value, code_t *target)
{
    UPDATABLE_JUMP_IF_EQUALS(rg, value, target);
    return code;
}

PSY_INLINE code_t *
ipromotion_write_jump(code_t *code, code_t *target)
{
    JUMP_TO(target);
    return code;
}

/* read back an entry written by ipromotion_write_entry. The target is
   where the je ends up: past the far jump it goes through when the target
   is out of its reach (codemanager.c:psyco_code_far_jump), which a copy of
   the entry elsewhere may not reach, and which isn't the code compared
   with the megamorphic version (dispatcher.c:promotion_cache_add). */
PSY_INLINE void
ipromotion_read_entry(code_t *code, long *value, code_t **target)
{
    code_t *t = code + PROMOTION_ENTRY_SIZE +
                *(dword_t *)(code + PROMOTION_ENTRY_SIZE - 4); /* je rel32 */
    /* jmp [rip], as x64/iprocessor.c:psyco_write_far_jump writes it */
    while (t[0] == 0xFF && t[1] == 0x25 && *(dword_t *)(t + 2) == 0)
        t = *(code_t **)(t + 6);
    *value = *(long *)(code + 2); /* REX, B8+r, imm64 */
    *target = t;
}

/* repoint an existing entry (inline or in the chain) at 'code' */
PSY_INLINE void
ipromotion_update_entry(code_t *code, long value, code_t *target)
{
#undef ONLY_UPDATING
#define ONLY_UPDATING 1
    UPDATABLE_JUMP_IF_EQUALS(0, value, target);
#undef ONLY_UPDATING
#define ONLY_UPDATING 0
}

/***************************************************************/
/***   Stubs                                                 ***/

/* A stub is 15 bytes: 'mov r11, record; jmp psyco_stub_trampoline'
   (iprocessor.c). The record's first word is the C function to run,
   code_t *fn(void *closure), with the closure just after it.
   psyco_write_stub writes one at 'code' and returns its end. */
#define STUB_SIZE_MAX 15
EXTERNVAR code_t *psyco_stub_trampoline;
EXTERNFN code_t *
psyco_write_stub(code_t *code, void *record);
EXTERNFN code_t *
psyco_stub_enter(void *record, long value); /* dispatcher.c */
/* dispatcher.c: a stub that calls 'fn' with a new closure of 'size' bytes;
   returns the closure and sets *stub to the code to jump to. The record
   lives as long as the current owner (codemanager.h); one that holds
   something gets a release function (psyco_stub_set_release), called with
   the closure when the owner dies, which must do nothing for what the stub
   has used up by then. psyco_new_stub_at writes the stub at *at instead of
   in the pool, and advances *at past it. */
EXTERNFN void *
psyco_new_stub(void *fn, size_t size, code_t **stub);
EXTERNFN void *
psyco_new_stub_at(void *fn, size_t size, code_t **at);
EXTERNFN void
psyco_stub_set_release(void *closure, void (*fn)(void *));

/* A respawn's stub is 'lea r11, [rip+5]; jmp psyco_respawn_trampoline'
   (iprocessor.c), with its record just after it, which psyco_respawn_enter
   (dispatcher.c) is given; not a 'call', which would leave the CPU's
   return-address predictor a call it never returns from.
   psyco_write_respawn_stub writes one at 'code' and returns its end, the
   record's address. */
#define RESPAWN_STUB_SIZE 12
EXTERNVAR code_t *psyco_respawn_trampoline;
EXTERNFN code_t *
psyco_write_respawn_stub(code_t *code);
EXTERNFN code_t *
psyco_respawn_enter(code_t *record); /* dispatcher.c */

/* write the miss path 'mov rsi, reg; mov r11, record; jmp trampoline' */
PSY_INLINE code_t *
ipromotion_write_miss(code_t *code, int rg, void *record)
{
    MOV_R_R(REG_X64_RSI, rg);
    return psyco_write_stub(code, record);
}

/* A promotion site: the inline cache, then on a miss
   'mov rsi, reg; mov r11, record; jmp psyco_stub_trampoline', which calls
   do_promotion(fs, value) (37 bytes in all). The value is always in a
   register here; the trampoline leaves RSI alone. The record holds the
   rt_promotion_t, whose head is ipromotion_s, and lives as long as its
   owner: the site is entered again for every value the cache doesn't
   hold. */
PSY_INLINE void *
ipromotion_finish(PsycoObject *po, vinfo_t *fix, void *do_promotion,
                  size_t block_size)
{
    struct ipromotion_s *fs;
#if PROMOTION_FAST_COMMON_CASE
    code_t *start_jump, *end_jump;
#endif
    BEGIN_CODE
    NEED_CC();
    RTVINFO_IN_REG(fix);
#if PROMOTION_FAST_COMMON_CASE
    start_jump = code;
    UPDATABLE_JUMP_IF_EQUALS(getreg(fix->source), 0, code);
    end_jump = code;
    ipromotion_update_entry(start_jump, 0, end_jump); /* jump to end of jump */
#endif
    MOV_R_R(REG_X64_RSI, getreg(fix->source));
    fs = (struct ipromotion_s *)psyco_new_stub_at(do_promotion, block_size,
                                                  &code);
    END_CODE
#if PROMOTION_FAST_COMMON_CASE
    fs->jump_if_equal_code = start_jump;
    LE_RELOC_SLOT(&fs->jump_if_equal_code);
    fs->chain = NULL;
    fs->reg = getreg(fix->source);
    fs->ncached = 0;
    fs->chaincap = 0;
    fs->fallback = NULL;
#endif
    return fs;
}

/***************************************************************/
/***   Misc.                                                 ***/

PSY_INLINE void *
conditional_jump_to(PsycoObject *po, code_t *target,
                    condition_code_t condition)
{
    BEGIN_CODE
    switch (condition) {
        case CC_ALWAYS_FALSE: /* never jumps */
            break;
        case CC_ALWAYS_TRUE:
            JUMP_TO(target); /* always jumps */
            break;
        default:
            FAR_COND_JUMP_TO(target, condition);
    }
    END_CODE
    return po->code;
}

PSY_INLINE void
change_cond_jump_target(void *tag, code_t *newtarget)
{
    code_t *code = (code_t *)tag;
    /* safety check: do not write a JMP whose target is itself...
       would make an endless loop */
    psyco_assert(newtarget != code - 5 && newtarget != code - 6);
    CHANGE_JUMP_TO(newtarget);
}

PSY_INLINE code_t *
resume_after_cond_jump(void *tag)
{
    return (code_t *)tag;
}

/* reserve a small buffer of code behind po->code in which conditional
   code can be stored.  See make_code_conditional(). */
PSY_INLINE void *
setup_conditional_code_bounds(PsycoObject *po, PsycoObject *po2,
                              condition_code_t condition)
{
    code_t *code2 = po->code + SIZE_OF_SHORT_CONDITIONAL_JUMP;
    po2->code = code2;
    po2->codelimit = code2 + RANGE_OF_SHORT_CONDITIONAL_JUMP;
    return NULL;
}

/* mark a small buffer reserved by setup_conditional_code_bounds() to be
   only executed if 'condition' holds. */
PSY_INLINE void
make_code_conditional(PsycoObject *po, code_t *codeend,
                      condition_code_t condition, void *extra)
{
    code_t *target;
    code_t *code2 = po->code + SIZE_OF_SHORT_CONDITIONAL_JUMP;
    extra_assert(code2 <= codeend &&
                 codeend <= code2 + RANGE_OF_SHORT_CONDITIONAL_JUMP);
    BEGIN_CODE
    if (IS_A_SINGLE_JUMP(code2, codeend, target))
        FAR_COND_JUMP_TO(target,
                         condition); /* replace a jump with a cond jump */
    else { /* other cases: write a short cond jump to skip the block if
              !condition */
        SHORT_COND_JUMP_TO(codeend, INVERT_CC(condition));
        code = codeend;
    }
    END_CODE
    /* record the known value of 'condition' in the 'po' (non-jump) branch */
    psyco_resolved_cc(po, INVERT_CC(condition));
}

#endif /* _IDISPATCHER_H */
