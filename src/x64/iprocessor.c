/*
 * Entry from C into compiled code. Glue generated at start-up converts the C
 * ABI to the Psyco convention: it saves the callee-saved registers, aligns the
 * stack, pushes the arguments and records RSP in the finfo. This file also
 * generates psyco_int_mul_ovf (used for compile-time overflow checks) and
 * walks machine-stack frames for frames.c.
 */
#include "../Python/frames.h"
#include "../codemanager.h"
#include "../dispatcher.h"
#include "../processor.h"
#include "iencoding.h"

typedef PyObject *(*glue_run_code_fn)(code_t *code_target, long *stack_end,
                                      long *initial_stack,
                                      struct stack_frame_info_s ***finfo);

static glue_run_code_fn glue_run_code_aligned;

static glue_run_code_fn glue_run_code_unaligned;

/* The layout above the finfo slot is fixed, for frames.c:slot_registers:
   the saved R15, R14, R13, R12, RBX and RBP, then LE_GLUE_PAD if the stack
   needed aligning, then the return address. */
static void
write_glue_run_code_fn(PsycoObject *po, bool aligned)
{
    BEGIN_CODE
    if (!aligned) {
        PUSH_I(LE_GLUE_PAD);
    }
    PUSH_R(REG_X64_RBP);
#if RBP_IS_RESERVED
    MOV_R_R(REG_X64_RBP, REG_X64_RSP);
#endif
    PUSH_R(REG_X64_RBX);
    PUSH_R(REG_X64_R12);
    PUSH_R(REG_X64_R13);
    PUSH_R(REG_X64_R14);
    PUSH_R(REG_X64_R15);
    PUSH_I(-1); /* finfo */
    MOV_A_R(REG_X64_RCX, REG_X64_RSP);
    BEGIN_SHORT_JUMP(0);
    BEGIN_REVERSE_SHORT_JUMP(1);
    SUB_R_I8(REG_X64_RSI, 8);
    PUSH_A(REG_X64_RSI);
    END_SHORT_JUMP(0);
    CMP_R_R(REG_X64_RDX, REG_X64_RSI);
    END_REVERSE_SHORT_COND_JUMP(1, CC_NE);
    CALL_R(REG_X64_RDI);
    ADD_R_I8(REG_X64_RSP, sizeof(long));
    POP_R(REG_X64_R15);
    POP_R(REG_X64_R14);
    POP_R(REG_X64_R13);
    POP_R(REG_X64_R12);
    POP_R(REG_X64_RBX);
    POP_R(REG_X64_RBP);
    if (!aligned) {
        ADD_R_I8(REG_X64_RSP, 8);
    }
    RET();
    END_CODE
}

DEFINEFN
PyObject *
psyco_processor_run(CodeBufferObject *codebuf, long initial_stack[],
                    struct stack_frame_info_s ***finfo)
{
    int argc = RUN_ARGC(codebuf);
    int regs_saved = 6;
    int finfo_pushed = 1;
    int return_address = 1;
    /* we need to work out if the call to psyco code would be made with the
     * stack 16 bytes aligned and adjust accordingly - we assume everything
     * pushed to the stack is 8 bytes wide and the call from C will be aligned
     * before the call (in the call it is off by 8 due to return address), so
     * we only need to know if an odd number of this is pushed to the stack
     * before the call.
     */
    if ((argc + regs_saved + finfo_pushed + return_address) & 1) {
        return glue_run_code_unaligned(
            codebuf->codestart, initial_stack + argc, initial_stack, finfo);
    }
    else {
        return glue_run_code_aligned(codebuf->codestart, initial_stack + argc,
                                     initial_stack, finfo);
    }
}

typedef char (*psyco_int_mul_ovf_fn)(long a, long b);

DEFINEVAR psyco_int_mul_ovf_fn psyco_int_mul_ovf;

void
write_psyco_int_mul_ovf(PsycoObject *po)
{
    BEGIN_CODE
    IMUL_R_R(REG_X64_RDI, REG_X64_RSI);
    XOR_R_R(REG_X64_RAX, REG_X64_RAX);
    SET_R_CC(REG_X64_RAX, CC_O);
    RET();
    END_CODE
}

/* The shared half of every stub (idispatcher.h:psyco_write_stub). A stub
   loads its record into R11 and jumps here; a 'call' would leave the CPU's
   return-address predictor one entry off, and every later 'ret' would
   mispredict (go lost 8% that way). Entered from compiled code (a
   conditional jump, or a promotion site's miss path), with the stack at any
   alignment: saves RAX (it may hold a result with no tracked location yet),
   calls psyco_stub_enter(record, RSI) on an aligned stack, and jumps to the
   code it returns with RSP as it was. RSI must stay untouched until the
   call: a promotion stub passes its run-time value there.
   Other registers need no saving (only callee-saved ones are allocated);
   the flags are not saved: promotion spills them first (NEED_CC) and
   psyco_coding_pause asserts none is left. */
DEFINEVAR code_t *psyco_stub_trampoline;
/* The same for a respawn's stub, which loads its record's address (just
   after the stub) and comes here to run psyco_respawn_enter. */
DEFINEVAR code_t *psyco_respawn_trampoline;

static void
write_stub_trampoline(PsycoObject *po, bool respawn)
{
    BEGIN_CODE
    PUSH_R(REG_X64_RAX);
    MOV_R_R(REG_X64_RDI, REG_X64_R11); /* the record */
    MOV_R_R(REG_X64_RAX, REG_X64_RSP);
    AND_R_I8(REG_X64_RSP, -16);
    PUSH_R(REG_X64_RAX); /* the unaligned RSP */
    SUB_R_I8(REG_X64_RSP, 8);
    if (respawn)
        CALL_I(psyco_respawn_enter);
    else
        CALL_I(psyco_stub_enter);
    ADD_R_I8(REG_X64_RSP, 8);
    POP_R(REG_X64_RSP);
    MOV_R_R(REG_X64_R11, REG_X64_RAX);
    POP_R(REG_X64_RAX);
    JMP_R(REG_X64_R11);
    END_CODE
}

/* Out-of-line slow paths that compiled code calls with its arguments
   already in the SysV registers and the stack at any alignment (the
   inline attribute store's miss and dealloc paths). Each is a matched
   call and ret, so the return-address predictor stays in step. RBP
   is saved here and holds the unaligned RSP; compiled code keeps its
   values only in callee-saved registers, which the C callee preserves. */
static void
write_aligned_call(PsycoObject *po, void *fn, bool dealloc_rax)
{
    BEGIN_CODE
    PUSH_R(REG_X64_RBP);
    MOV_R_R(REG_X64_RBP, REG_X64_RSP);
    AND_R_I8(REG_X64_RSP, -16);
    if (dealloc_rax) { /* deallocates the object in RAX, as DEC_OB_REFCNT */
        MOV_R_R(REG_X64_RDI, REG_X64_RAX);
#if defined(Py_TRACE_REFS) || LE_HAS_DEALLOC_HOOKS
        CALL_I(_Py_Dealloc);
#else
        MOV_R_O8(REG_X64_RAX, REG_X64_RDI, CPY_OFFSET(PyObject, ob_type));
        MOV_R_O8(REG_X64_RAX, REG_X64_RAX,
                 CPY_OFFSET(PyTypeObject, tp_dealloc));
        CALL_R(REG_X64_RAX);
#endif
    }
    else {
        CALL_I(fn);
    }
    MOV_R_R(REG_X64_RSP, REG_X64_RBP);
    POP_R(REG_X64_RBP);
    RET();
    END_CODE
}

static code_t *
new_aligned_call(void *fn, bool dealloc_rax)
{
    PsycoObject *po = PsycoObject_New(0);
    code_t *start = po->code = psyco_code_open(&po->codelimit);
    write_aligned_call(po, fn, dealloc_rax);
    CLOSE_CODE("glue", start, po->code);
    PsycoObject_Delete(po);
    return start;
}

DEFINEVAR code_t *psyco_dealloc_rax_glue;

DEFINEFN code_t *
psyco_aligned_call_glue(void *fn)
{
    return new_aligned_call(fn, false);
}

/* Where a compiled callee whose frame object was asked for returns: its
   return address is replaced by this (frames.c:psyco_activation), so the
   frame is let go when the call ends and costs nothing before. Entered by
   the callee's RET, with RSP at its own finfo slot and the result in RAX,
   the only register live there (the caller pushed the others); calls
   psyco_activation_returned(RSP - 8) on an aligned stack and jumps to the
   return address it gives back. */
DEFINEVAR code_t *psyco_return_barrier;

static void
write_return_barrier(PsycoObject *po)
{
    BEGIN_CODE
    PUSH_R(REG_X64_RAX);
    MOV_R_R(REG_X64_RDI, REG_X64_RSP);
    MOV_R_R(REG_X64_RAX, REG_X64_RSP);
    AND_R_I8(REG_X64_RSP, -16);
    PUSH_R(REG_X64_RAX); /* the unaligned RSP */
    SUB_R_I8(REG_X64_RSP, 8);
    CALL_I(psyco_activation_returned);
    ADD_R_I8(REG_X64_RSP, 8);
    POP_R(REG_X64_RSP);
    MOV_R_R(REG_X64_R11, REG_X64_RAX);
    POP_R(REG_X64_RAX);
    JMP_R(REG_X64_R11);
    END_CODE
}

DEFINEFN code_t *
psyco_write_stub(code_t *code, void *record)
{
    MOV_R_I(REG_X64_R11, (long)record);
    JUMP_TO(psyco_stub_trampoline);
    return code;
}

DEFINEFN code_t *
psyco_write_far_jump(code_t *code, code_t *target)
{
    code[0] = 0xFF; /* jmp qword ptr [rip] */
    code[1] = 0x25;
    *(dword_t *)(code + 2) = 0;
    *(code_t **)(code + 6) = target;
    le_trace_begin_simple_code(code);
    le_trace_jump(code, target);
    le_trace_end_simple_code(code + SIZE_OF_FAR_JUMP_ANYWHERE);
    return code + SIZE_OF_FAR_JUMP_ANYWHERE;
}

DEFINEFN code_t *
psyco_write_respawn_stub(code_t *code)
{
    LEA_R_RIP(REG_X64_R11, SIZE_OF_FAR_JUMP); /* the record, after the jump */
    JUMP_TO(psyco_respawn_trampoline);
    return code;
}

DEFINEFN struct stack_frame_info_s **
psyco_next_stack_frame(struct stack_frame_info_s **finfo)
{
    /* Hack to pick directly from the machine stack the stored
       "stack_frame_info_t*" pointers */
    return (
        struct stack_frame_info_s **)(((char *)finfo) -
                                      finfo_last(*finfo)->link_stack_depth);
}

INITIALIZATIONFN
void
psyco_processor_init(void)
{
    PsycoObject *po = PsycoObject_New(0);
    code_t *start = po->code = psyco_code_open(&po->codelimit);
    glue_run_code_aligned = (glue_run_code_fn)po->code;
    write_glue_run_code_fn(po, true);
    glue_run_code_unaligned = (glue_run_code_fn)po->code;
    write_glue_run_code_fn(po, false);
    psyco_int_mul_ovf = (psyco_int_mul_ovf_fn)po->code;
    write_psyco_int_mul_ovf(po);
    psyco_stub_trampoline = po->code;
    write_stub_trampoline(po, false);
    psyco_respawn_trampoline = po->code;
    write_stub_trampoline(po, true);
    psyco_return_barrier = po->code;
    write_return_barrier(po);
    CLOSE_CODE("glue", start, po->code);
    PsycoObject_Delete(po);
    psyco_dealloc_rax_glue = new_aligned_call(NULL, true);
}
