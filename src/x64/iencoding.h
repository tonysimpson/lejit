/*
 * The x86-64 backend's macro layer: instruction encoders, the register and
 * condition-code model stored in PsycoObject, spill/reload, SysV C-call macros
 * and jumps. Conventions to know before editing:
 *  - Only callee-saved registers (R13, R14, R15, RBX, and RBP where it is
 * not the frame pointer: see RegistersLoop in iencoding.c) hold tracked
 * values, which is why C calls need no spilling,
 *    and XMM8-XMM14, which hold only doubles and are stored at every call
 *    (iencoding.c:psyco_xmm_store). R10/R11 and XMM15 are encoder scratch
 *    (REG_TRANSIENT_1/2, XMM_SCRATCH); RAX is the return value.
 *  - Values are addressed relative to RSP using po->stack_depth. The
 * *_EBP_BASE names are i386 leftovers.
 *  - Any emitter that clobbers the flags must call NEED_CC* first.
 *  - ONLY_UPDATING=1 re-emits an instruction in place, writing only the
 *    UPDATABLE_* fields (this is how code is patched).
 *  - Many macros are bare statement sequences (no do/while), and the asserts
 *    here only compile because CPython builds with -DNDEBUG.
 * Emitters marked '@asm:' are checked against GNU as by
 * tests/encoder/check.py.
 */
/***************************************************************/
/***       Processor-specific code-producing macros            ***/
/***************************************************************/

#ifndef _IENCODING_H
#define _IENCODING_H

#include "../Python/pycheader.h"
#include "../psyco.h"

/* everything before args, including finfo which is the last thing before args
 */
#define INITIAL_STACK_DEPTH 8

/* Set to 0 to limit registers to RAX>RDI */

#define REG_TOTAL 23
typedef enum {
    REG_X64_RAX = 0,  /* reserved for return values */
    REG_X64_RCX = 1,  /* 4th ARG */
    REG_X64_RDX = 2,  /* 3rd ARG */
    REG_X64_RBX = 3,  /* CALLEE Saved */
    REG_X64_RSP = 4,  /* stack pointer */
    REG_X64_RBP = 5,  /* CALLEE Saved; the frame pointer, never allocated,
                         where LE_KEEPS_FRAME_POINTERS */
    REG_X64_RSI = 6,  /* 2nd ARG */
    REG_X64_RDI = 7,  /* 1st ARG */
    REG_X64_R8 = 8,   /* 5th ARG */
    REG_X64_R9 = 9,   /* 6th ARG */
    REG_X64_R10 = 10, /* CALLER Saved */
    REG_X64_R11 = 11, /* CALLER Saved */
    REG_X64_R12 = 12, /* CALLEE Saved */
    REG_X64_R13 = 13, /* CALLEE Saved */
    REG_X64_R14 = 14, /* CALLEE Saved */
    REG_X64_R15 = 15, /* CALLEE Saved */

    /* Float registers */
    REG_X64_XMM0 = 0,
    REG_X64_XMM1 = 1,
    REG_X64_XMM2 = 2,
    REG_X64_XMM3 = 3,
    REG_X64_XMM4 = 4,
    REG_X64_XMM5 = 5,
    REG_X64_XMM6 = 6,
    REG_X64_XMM7 = 7,

    /* XMM8-XMM14 as tracked registers, holding run-time doubles: these are
       their numbers in a Source and reg_array, not the encoders' (XMM_HW).
       Call arguments are XMM0-XMM7, so setting them up never overwrites a
       tracked value */
    REG_X64_XMM8 = 16,
    REG_X64_XMM14 = 22,

    REG_NONE = -1
} reg_t;

#define REG_LOOP_START REG_X64_R13
#define REG_IS_XMM(rg) ((rg) >= REG_X64_XMM8)
#define XMM_HW(rg) ((rg) - 8) /* a tracked register's number for encoders */
#define XMM_SCRATCH 15        /* XMM15, an encoder number */
#define XMM_SCRATCH_2 0       /* XMM0: free between calls */
/* po->xmm_call: no XMM value may be live at a call; they are stored to
   stack slots (the call may clobber the registers); the callee keeps
   them (glue that saves them) */
#define XMM_CALL_NONE 0
#define XMM_CALL_STORED 1
#define XMM_CALL_KEPT 2

#define REG_ANY_CALLEE_SAVED REG_X64_RBX /* saved by C functions */
#define REG_FUNCTIONS_RETURN REG_X64_RAX
#define REG_TRANSIENT_1 REG_X64_R10
#define REG_TRANSIENT_2 REG_X64_R11

/* the registers we want Psyco to use in compiled code,
 *    as a circular linked list */
EXTERNVAR reg_t RegistersLoop[REG_TOTAL];

/* returns the next register that should be used */
#define next_free_reg(po) \
    ((po)->last_used_reg = RegistersLoop[(int)((po)->last_used_reg)])

typedef enum {
    CC_O = 0, /* overflow */
    CC_NO = 1,
    CC_B = 2, /* below (unsigned) */
    CC_NB = 3,
    CC_E = 4, /* equal */
    CC_NE = 5,
    CC_BE = 6, /* below or equal (unsigned) */
    CC_NBE = 7,
    CC_S = 8, /* sign (i.e. negative) */
    CC_NS = 9,
    CC_L = 12, /* lower than */
    CC_NL = 13,
    CC_LE = 14, /* lower or equal */
    CC_NLE = 15,
#define CC_TOTAL 16
    /* synonyms */
    CC_NGE = CC_L,        /* not greater or equal */
    CC_GE = CC_NL,        /* greater or equal */
    CC_NG = CC_LE,        /* not greater than */
    CC_G = CC_NLE,        /* greater than */
    CC_uL = CC_B,         /* unsigned test */
    CC_uNL = CC_NB,       /* unsigned test */
    CC_uLE = CC_BE,       /* unsigned test */
    CC_uNLE = CC_NBE,     /* unsigned test */
    CC_uNGE = CC_uL,      /* unsigned test */
    CC_uGE = CC_uNL,      /* unsigned test */
    CC_uNG = CC_uLE,      /* unsigned test */
    CC_uG = CC_uNLE,      /* unsigned test */
    CC_ALWAYS_FALSE = 16, /* pseudo condition codes for known outcomes */
    CC_ALWAYS_TRUE = 17,
    CC_ERROR = -1
} condition_code_t;

#define INVERT_CC(cc) ((condition_code_t)((int)(cc) ^ 1))
#define HAVE_CCREG 2
#define INDEX_CC(cc) ((int)(cc) & 1)
#define HAS_CCREG(po) ((po)->ccregs[0] != NULL || (po)->ccregs[1] != NULL)

/* processor-depend part of PsycoObject */
#define PROCESSOR_PSYCOOBJECT_FIELDS                                          \
    int stack_depth; /* the size of data currently pushed in the stack */     \
    vinfo_t                                                                   \
        *reg_array[REG_TOTAL]; /* the 'vinfo_t' currently stored in regs */   \
    vinfo_t *ccregs[2];        /* processor condition codes (aka flags)  */   \
                               /* ccregs[0] is for "positive" conditions */   \
                               /* and ccregs[1] for "negative" ones      */   \
    reg_t last_used_reg;       /* the most recently used register        */   \
    reg_t last_used_xmm;       /* and XMM register                       */   \
    char xmm_call;    /* XMM_CALL_*: how live XMM values survive a call    */ \
    code_t *call_end; /* just after the last END_CALL_I's call   */
#define INIT_PROCESSOR_PSYCOOBJECT(po) \
    (po->last_used_reg = REG_LOOP_START, po->last_used_xmm = REG_X64_XMM14)

/* a respawn replays the compilation, which must choose the same registers:
   both round-robin positions, the XMM one in the high byte */
#define PROCESSOR_FROZENOBJECT_FIELDS short fz_last_used_reg;
#define SAVE_PROCESSOR_FROZENOBJECT(fpo, po) \
    (fpo->fz_last_used_reg =                 \
         (short)((int)po->last_used_reg | (int)po->last_used_xmm << 8))
#define RESTORE_PROCESSOR_FROZENOBJECT(fpo, po)                 \
    (po->last_used_reg = (reg_t)(fpo->fz_last_used_reg & 0xFF), \
     po->last_used_xmm = (reg_t)(fpo->fz_last_used_reg >> 8))

#define INIT_CODE_EMISSION(code) \
    do { /* nothing */           \
    } while (0)
#define insn_code_label(code) ((code_t *)(code))

#define CHECK_STACK_SPACE() \
    do { /* nothing */      \
    } while (0)

/*****************************************************************/
/***   Production of code (common instruction encodings)       ***/

/* Most of the following macros implicitely use and update the
 * local variable 'code'. Some also use 'po'. No macro outside the
 * present header file must implicitely use or modify 'code'.
 */

/* Written as a large set of macro. */

#define RSOURCE_REG(src) getreg(src)
#define RSOURCE_REG_IS_NONE(src) is_reg_none(src)
#define RSOURCE_STACK(src) getstack(src)

#define RUNTIME_STACK_NONE RunTime_StackNone
#define RUNTIME_REG(vi) RSOURCE_REG(((vinfo_t *)(vi))->source)
#define RUNTIME_REG_IS_NONE(vi) RSOURCE_REG_IS_NONE(((vinfo_t *)(vi))->source)
#define RUNTIME_STACK(vi) RSOURCE_STACK(((vinfo_t *)(vi))->source)

#define SET_RUNTIME_REG_TO(vi, rg) \
    (((vinfo_t *)(vi))->source = set_rtreg_to(((vinfo_t *)(vi))->source, rg))
#define SET_RUNTIME_REG_TO_NONE(vi) \
    (((vinfo_t *)(vi))->source = set_rtreg_to_none(((vinfo_t *)(vi))->source))
#define SET_RUNTIME_STACK_TO(vi, s) \
    (((vinfo_t *)(vi))->source =    \
         set_rtstack_to(((vinfo_t *)(vi))->source, (s)))
#define SET_RUNTIME_STACK_TO_NONE(vi) \
    (((vinfo_t *)(vi))->source =      \
         set_rtstack_to_none(((vinfo_t *)(vi))->source))

#define KSOURCE_SOURCE(src) CompileTime_Get(src)

#define NEXT_FREE_REG() next_free_reg(po)
#define REG_NUMBER(po, rg) ((po)->reg_array[(int)(rg)])

/* release a run-time vinfo_t */
#define RTVINFO_RELEASE(rtsource)                         \
    do {                                                  \
        if (!RSOURCE_REG_IS_NONE(rtsource))               \
            REG_NUMBER(po, RSOURCE_REG(rtsource)) = NULL; \
    } while (0)

/* move a run-time vinfo_t */
#define RTVINFO_MOVE(rtsource, vtarget)                        \
    do {                                                       \
        if (!RSOURCE_REG_IS_NONE(rtsource))                    \
            REG_NUMBER(po, RSOURCE_REG(rtsource)) = (vtarget); \
    } while (0)

/* for PsycoObject_Duplicate() */
#define DUPLICATE_PROCESSOR(result, po)                         \
    do {                                                        \
        int i;                                                  \
        for (i = 0; i < REG_TOTAL; i++)                         \
            if (REG_NUMBER(po, i) != NULL)                      \
                REG_NUMBER(result, i) = REG_NUMBER(po, i)->tmp; \
        for (i = 0; i < 2; i++)                                 \
            if (po->ccregs[i] != NULL)                          \
                result->ccregs[i] = po->ccregs[i]->tmp;         \
                                                                \
        result->stack_depth = po->stack_depth;                  \
        result->last_used_reg = po->last_used_reg;              \
        result->last_used_xmm = po->last_used_xmm;              \
    } while (0)

#define RTVINFO_CHECK(po, vsource, found)                                 \
    do {                                                                  \
        RunTimeSource _src = (vsource)->source;                           \
        if (!RSOURCE_REG_IS_NONE(_src)) {                                 \
            extra_assert(REG_NUMBER(po, RSOURCE_REG(_src)) == (vsource)); \
            found[(int)RSOURCE_REG(_src)] = 1;                            \
        }                                                                 \
    } while (0)
#define RTVINFO_CHECKED(po, found)                       \
    do {                                                 \
        int i;                                           \
        for (i = 0; i < REG_TOTAL; i++)                  \
            if (!found[i])                               \
                extra_assert(REG_NUMBER(po, i) == NULL); \
    } while (0)

/*****************************************************************
 * Low level code producing macros
 *
 * All codegen should be done via these
 *
 * REX will include the REX byte before opcode
 * MODRM will include the MODRM byte after opcode
 * U   - Prefix for updatable (use #define UPDATE_CODE ... #undef UPDATE_CODE
 * REG - MODRM REG or just reg encoded in opcode (REX.R)
 * RM  - MODRM RM or just reg encoded in opcode (REX.B)
 * A   - MODRM RM with indirect addressing (reg is pointer to address)
 * O8  - MODRM RM with indirect addressing and 8bit offset
 * O32 - MODRM RM with indirect addressing and 32bit offset
 * I8  - Immediate 8bit
 * I32 - Immediate 32bit
 * I64 - Immediate 64bit
 */
#define FITS_IN_8BITS(v) (((long)(v) < 128 && (long)(v) >= -128))
#define FITS_IN_32BITS(v) \
    (((long)(v) < 2147483648 && (long)(v) >= -2147483648))
/* 'addr' is in reach of RIP-relative code at 'at', with room for the code
   to grow by 16 MB: code emitted again a little further on decides the
   same */
#define LE_RIP_NEAR(at, addr) \
    ((unsigned long)(addr) - (unsigned long)(at) + 0x7f000000UL < 0xfe000000UL)
/* the address of a slot of executable memory holding 'value', in reach of
   code at 'at' (LE_RIP_NEAR), or NULL (dispatcher.c) */
EXTERNFN code_t *
psyco_const_slot(code_t *at, long value);
/* the rel32 of a jump ending at 'end' to 'target': to the target, or else
   to a far jump to it within reach (codemanager.c:psyco_code_far_jump) */
EXTERNFN code_t *
psyco_code_far_jump(code_t *end, code_t *target);
#define REL32(end, target)                                                 \
    ((dword_t)le_reloc_jump(                                               \
        (code_t *)(end),                                                   \
        (FITS_IN_32BITS((code_t *)(target) - (code_t *)(end))              \
             ? (code_t *)(target)                                          \
             : psyco_code_far_jump((code_t *)(end), (code_t *)(target))) - \
            (code_t *)(end)))
/* jmp [rip]; the 8-byte target (x64/iprocessor.c:psyco_write_far_jump) */
#define SIZE_OF_FAR_JUMP_ANYWHERE 14
EXTERNFN code_t *
psyco_write_far_jump(code_t *code, code_t *target);
#define ONLY_UPDATING 0 /* redefine to 1 when updating code */
#define REX_64_REG_RM(reg, rm) (0x48 | (reg > 7 ? 4 : 0) | (rm > 7 ? 1 : 0))
#define MODRM_GENERAL(type_code, reg, rm) \
    (type_code | ((HW_REG(reg) & 0x7) << 3) | (HW_REG(rm) & 0x7))
#define MODRM_REG_RM(reg, rm) MODRM_GENERAL(0xC0, reg, rm)
#define WRITE_8BIT(immed)                                                    \
    do {                                                                     \
        if (((long)(immed) < -128) || ((long)(immed) > 127)) {               \
            fprintf(stderr, "Bad value for 8bit %ld %s:%d\n", (long)(immed), \
                    __FILE__, __LINE__);                                     \
        }                                                                    \
        if (!ONLY_UPDATING) {                                                \
            *code = (code_t)(immed);                                         \
        }                                                                    \
        code += 1;                                                           \
    } while (0)
#define WRITE_16BIT(immed)                                                    \
    do {                                                                      \
        if (((long)(immed) < -32768) || ((long)(immed) > 32767)) {            \
            fprintf(stderr, "Bad value for 16bit %ld %s:%d\n", (long)(immed), \
                    __FILE__, __LINE__);                                      \
        }                                                                     \
        if (!ONLY_UPDATING) {                                                 \
            *(word_t *)code = (word_t)(immed);                                \
        }                                                                     \
        code += sizeof(word_t);                                               \
    } while (0)
#define WRITE_32BIT(immed)                                                    \
    do {                                                                      \
        if (((long)(immed) < -2147483648) || ((long)(immed) > 2147483647)) {  \
            fprintf(stderr, "Bad value for 32bit %ld %s:%d\n", (long)(immed), \
                    __FILE__, __LINE__);                                      \
        }                                                                     \
        if (!ONLY_UPDATING) {                                                 \
            *(dword_t *)code = (dword_t)(immed);                              \
        }                                                                     \
        code += sizeof(dword_t);                                              \
    } while (0)
#define WRITE_64BIT(immed)                       \
    do {                                         \
        if (!ONLY_UPDATING) {                    \
            *(qword_t *)code = (qword_t)(immed); \
        }                                        \
        code += sizeof(qword_t);                 \
    } while (0)
#define UPDATABLE_WRITE_8BIT(immed) \
    do {                            \
        *code = (code_t)(immed);    \
        code += 1;                  \
    } while (0)
#define UPDATABLE_WRITE_32BIT(immed)         \
    do {                                     \
        *(dword_t *)code = (dword_t)(immed); \
        code += sizeof(dword_t);             \
    } while (0)
#define UPDATABLE_WRITE_64BIT(immed)         \
    do {                                     \
        *(qword_t *)code = (qword_t)(immed); \
        code += sizeof(qword_t);             \
    } while (0)
#define WRITE_1(b1)           \
    do {                      \
        if (!ONLY_UPDATING) { \
            code[0] = b1;     \
        }                     \
        code += 1;            \
    } while (0)
#define WRITE_2(b1, b2)       \
    do {                      \
        if (!ONLY_UPDATING) { \
            code[0] = b1;     \
            code[1] = b2;     \
        }                     \
        code += 2;            \
    } while (0)
#define WRITE_4(b1, b2, b3, b4) \
    do {                        \
        if (!ONLY_UPDATING) {   \
            code[0] = b1;       \
            code[1] = b2;       \
            code[2] = b3;       \
            code[3] = b4;       \
        }                       \
        code += 4;              \
    } while (0)
/* Common Instructions:
 * U - Prefix for updatable fields
 * R - Register direct
 * A - Address in register
 * O8 - Address is register + 8 bit offset
 * O32 - Address is register + 32 bit offset
 * I - Immediat
 * X - base + (index * scale) + offset
 */
/* rex_w: true, false, or REX_BYTE for a byte register 'r': no REX.W, but a
   REX for SPL, BPL, SIL and DIL, which without one would be AH to BH */
#define REX_BYTE 2
#define REX_NEEDED(rex_w, r, i, rm)                                    \
    ((rex_w) == true || ((rex_w) == REX_BYTE && (r) > 3) || (r) > 7 || \
     (i) > 7 || (rm) > 7)
/* an encoder's register: 0-15. A tracked XMM register's number (16 and up)
   must be turned into XMM_HW first, and REG_NONE never reaches one */
#define HW_REG(r) (extra_assert((unsigned long)(r) < 16), (r))
#define REX_ENCODING(rex_w, r, i, rm)                                      \
    do {                                                                   \
        if (REX_NEEDED(rex_w, HW_REG(r), i, HW_REG(rm))) {                 \
            WRITE_1(0x40 | ((rex_w) == true ? 8 : 0) | ((r) > 7 ? 4 : 0) | \
                    ((i) > 7 ? 2 : 0) | ((rm) > 7 ? 1 : 0));               \
        }                                                                  \
    } while (0)
#define MODRM_ENCODING(mod, r, rm)                                \
    do {                                                          \
        WRITE_1(mod | ((HW_REG(r) & 7) << 3) | (HW_REG(rm) & 7)); \
    } while (0)
#define DECODE_SIZE(size, offset) \
    (((size) == 0) ? (FITS_IN_8BITS(offset) ? 8 : 32) : (size))
#define ENCODE_OFFSET(updatable, size, value) \
    do {                                      \
        if (updatable) {                      \
            if (size == 8) {                  \
                UPDATABLE_WRITE_8BIT(value);  \
            }                                 \
            else {                            \
                UPDATABLE_WRITE_32BIT(value); \
            }                                 \
        }                                     \
        else {                                \
            if (size == 8) {                  \
                WRITE_8BIT(value);            \
            }                                 \
            else {                            \
                WRITE_32BIT(value);           \
            }                                 \
        }                                     \
    } while (0)
#define WRITE_OPCODES(op_len, b1, b2, b3) \
    do {                                  \
        if (op_len > 0) {                 \
            WRITE_1(b1);                  \
        }                                 \
        if (op_len > 1) {                 \
            WRITE_1(b2);                  \
        }                                 \
        if (op_len > 2) {                 \
            WRITE_1(b3);                  \
        }                                 \
    } while (0)
#define REQUIRES_OFFSET(rm) ((((rm) & 7) == 4 || ((rm) & 7) == 5))
#define SIB_ENCODING(updatable, rex_w, op_len, b1, b2, b3, mod, size, r,  \
                     base, offset, index, scale)                          \
    do {                                                                  \
        REX_ENCODING(rex_w, r, index, base);                              \
        WRITE_OPCODES(op_len, b1, b2, b3);                                \
        MODRM_ENCODING(                                                   \
            mod | ((offset == 0 && !REQUIRES_OFFSET(base))                \
                       ? 0x04                                             \
                       : (DECODE_SIZE(size, offset) == 8 ? 0x44 : 0x84)), \
            r, 0);                                                        \
        WRITE_1((scale == 8   ? 0xC0                                      \
                 : scale == 4 ? 0x80                                      \
                 : scale == 2 ? 0x40                                      \
                              : 0x00) |                                   \
                (base & 7) | ((index & 7) << 3));                         \
        if (offset != 0 || REQUIRES_OFFSET(base)) {                       \
            ENCODE_OFFSET(updatable, DECODE_SIZE(size, offset), offset);  \
        }                                                                 \
    } while (0)
#define OFFSET_ENCODING(updatable, rex_w, op_len, b1, b2, b3, mod, size, r,   \
                        rm, offset)                                           \
    do {                                                                      \
        if ((rm & 7) == 4) {                                                  \
            /* RSP/R12 as base need a SIB byte; index 4 without REX.X = none  \
             */                                                               \
            SIB_ENCODING(updatable, rex_w, op_len, b1, b2, b3, mod, size, r,  \
                         rm, offset, REG_X64_RSP, 0);                         \
        }                                                                     \
        else {                                                                \
            REX_ENCODING(rex_w, r, 0, rm);                                    \
            WRITE_OPCODES(op_len, b1, b2, b3);                                \
            MODRM_ENCODING(                                                   \
                mod | (DECODE_SIZE(size, offset) == 8 ? 0x40 : 0x80), r, rm); \
            ENCODE_OFFSET(updatable, DECODE_SIZE(size, offset), offset);      \
        }                                                                     \
    } while (0)
#define INDIRECT_ENCODING(rex_w, op_len, b1, b2, b3, mod, r, rm)             \
    do {                                                                     \
        if ((rm & 7) == 4 || (rm & 7) == 5) {                                \
            OFFSET_ENCODING(false, rex_w, op_len, b1, b2, b3, mod, 8, r, rm, \
                            0);                                              \
        }                                                                    \
        else {                                                               \
            REX_ENCODING(rex_w, r, 0, rm);                                   \
            WRITE_OPCODES(op_len, b1, b2, b3);                               \
            MODRM_ENCODING(mod, r, rm);                                      \
        }                                                                    \
    } while (0)
#define REX_ENCODING2(rex, rex_w, r, i, rm)                    \
    do {                                                       \
        if (rex || rex_w || r > 7 || rm > 7) {                 \
            WRITE_1(0x40 | (rex_w ? 8 : 0) | (r > 7 ? 4 : 0) | \
                    (i > 7 ? 2 : 0) | (rm > 7 ? 1 : 0));       \
        }                                                      \
    } while (0)
#define DIRECT_ENCODING2(rex, rex_w, op_len, b1, b2, b3, mod, r, rm) \
    do {                                                             \
        REX_ENCODING2(rex, rex_w, r, 0, rm);                         \
        WRITE_OPCODES(op_len, b1, b2, b3);                           \
        MODRM_ENCODING(mod | 0xC0, r, rm);                           \
    } while (0)
#define DIRECT_ENCODING(rex_w, op_len, b1, b2, b3, mod, r, rm) \
    do {                                                       \
        REX_ENCODING(rex_w, r, 0, rm);                         \
        WRITE_OPCODES(op_len, b1, b2, b3);                     \
        MODRM_ENCODING(mod | 0xC0, r, rm);                     \
    } while (0)
#define BASE_ENCODING(rex_w, op_len, b1, b2, b3, mod, r, rm) \
    do {                                                     \
        REX_ENCODING(rex_w, r, 0, rm);                       \
        WRITE_OPCODES(op_len, b1, b2, b3);                   \
        MODRM_ENCODING(mod, r, rm);                          \
    } while (0)
/* imm_len: bytes of immediate that follow the displacement; RIP-relative
   addresses are relative to the end of the whole instruction */
#define ADDRESS_ENCODING(rex_w, op_len, b1, b2, b3, mod, r, address, imm_len) \
    do {                                                                      \
        long rip_offset;                                                      \
        rip_offset = (long)address -                                          \
                     ((long)code + 5 + op_len +                               \
                      (REX_NEEDED(rex_w, r, 0, 0) ? 1 : 0) + (imm_len));      \
        if (FITS_IN_32BITS(rip_offset)) {                                     \
            REX_ENCODING(rex_w, r, 0, 0);                                     \
            WRITE_OPCODES(op_len, b1, b2, b3);                                \
            MODRM_ENCODING(mod, r, 5);                                        \
            WRITE_32BIT(rip_offset);                                          \
            le_reloc_note(code - 4, LE_RELOC_DATA, 4 + (imm_len));            \
        }                                                                     \
        else {                                                                \
            MOV_R_I(REG_TRANSIENT_2, address);                                \
            INDIRECT_ENCODING(rex_w, op_len, b1, b2, b3, mod, r,              \
                              REG_TRANSIENT_2);                               \
        }                                                                     \
    } while (0)
/* @asm: mov {r}, {rm} */
#define MOV_R_R(r, rm) DIRECT_ENCODING(true, 1, 0x8B, 0, 0, 0, r, rm)
/* @asm: mov {r:d}, {rm:d} */
#define MOVZX_QR_DR(r, rm) DIRECT_ENCODING(false, 1, 0x8B, 0, 0, 0, r, rm)
/* @asm: movsxd {r}, {rm:d} */
#define MOVSX_QR_DR(r, rm) DIRECT_ENCODING(true, 1, 0x63, 0, 0, 0, r, rm)
/* @asm: mov {r}, qword ptr [{rm}] */
#define MOV_R_A(r, rm) INDIRECT_ENCODING(true, 1, 0x8B, 0, 0, 0, r, rm)
/* @asm: mov qword ptr [{rm}], {r} */
#define MOV_A_R(rm, r) INDIRECT_ENCODING(true, 1, 0x89, 0, 0, 0, r, rm)
/* the shortest form that doesn't depend on where the code is: a 32-bit
   MOV zero-extends, C7 /0 sign-extends an imm32; otherwise the 10-byte
   movabs. For code written aside and measured or copied, and code emitted
   twice that must have the same length both times. Not for code that is
   patched later: that is MOV_R_UI. */
/* @asm: mov {rm}, {i:i64} */
#define MOV_R_IABS(rm, i)                                        \
    do {                                                         \
        long _mov_i = (long)(i);                                 \
        if (_mov_i >= 0 && _mov_i <= 0xFFFFFFFFL) {              \
            BASE_ENCODING(false, 0, 0, 0, 0, 0xB8, 0, rm);       \
            *(dword_t *)code =                                   \
                (dword_t)_mov_i; /* unsigned: not WRITE_32BIT */ \
            code += 4;                                           \
        }                                                        \
        else if (FITS_IN_32BITS(_mov_i)) {                       \
            DIRECT_ENCODING(true, 1, 0xC7, 0, 0, 0, 0, rm);      \
            WRITE_32BIT(_mov_i);                                 \
        }                                                        \
        else {                                                   \
            BASE_ENCODING(true, 0, 0, 0, 0, 0xB8, 0, rm);        \
            WRITE_64BIT(_mov_i);                                 \
        }                                                        \
    } while (0)
/* @asm: mov {r}, qword ptr [rip{o:d32}] */
#define MOV_R_RIP(r, o)                            \
    do {                                           \
        long _rip_o = (o);                         \
        REX_ENCODING(true, r, 0, 0);               \
        WRITE_1(0x8B);                             \
        MODRM_ENCODING(0, r, 5);                   \
        WRITE_32BIT(_rip_o);                       \
        le_reloc_note(code - 4, LE_RELOC_DATA, 4); \
    } while (0)
/* a constant in a register, in 7 bytes where MOV_R_IABS would take 10: the
   address of something within reach, or a load from its slot in the
   owner's constants (dispatcher.c:psyco_const_slot) */
#define MOV_R_I(rm, i)                                            \
    do {                                                          \
        long _k = (long)(i);                                      \
        code_t *_kslot;                                           \
        if (FITS_IN_32BITS(_k) || (_k >= 0 && _k <= 0xFFFFFFFFL)) \
            MOV_R_IABS(rm, _k);                                   \
        else if (LE_RIP_NEAR(code, _k))                           \
            LEA_R_RIP(rm, _k - ((long)code + 7));                 \
        else if ((_kslot = psyco_const_slot(code, _k)) != NULL)   \
            MOV_R_RIP(rm, (long)_kslot - ((long)code + 7));       \
        else                                                      \
            MOV_R_IABS(rm, _k);                                   \
    } while (0)
/* @asm: movabs {rm}, {i:i64} */
#define MOV_R_UI(rm, i)                           \
    BASE_ENCODING(true, 0, 0, 0, 0, 0xB8, 0, rm); \
    UPDATABLE_WRITE_64BIT(i)
/* @asm: mov {r}, qword ptr [{rm}{o:d8}] */
#define MOV_R_O8(r, rm, o) \
    OFFSET_ENCODING(false, true, 1, 0x8B, 0, 0, 0, 8, r, rm, o)
/* @asm: mov {r}, qword ptr [{rm}{o:d32}] */
#define MOV_R_O32(r, rm, o) \
    OFFSET_ENCODING(false, true, 1, 0x8B, 0, 0, 0, 32, r, rm, o)
/* @asm: mov {r}, qword ptr [{rm}{o}] */
#define MOV_R_O(r, rm, o) \
    OFFSET_ENCODING(false, true, 1, 0x8B, 0, 0, 0, 0, r, rm, o)
/* @asm: mov qword ptr [{rm}{o}], {r} */
#define MOV_O_R(rm, o, r) \
    OFFSET_ENCODING(false, true, 1, 0x89, 0, 0, 0, 0, r, rm, o)
/* @asm: lea {r}, [{rm}{o}] */
#define LEA_R_O(r, rm, o) \
    OFFSET_ENCODING(false, true, 1, 0x8D, 0, 0, 0, 0, r, rm, o)
/* @asm: mov {r:d}, dword ptr [{rm}{o}] */
#define MOV_DR_O(r, rm, o) \
    OFFSET_ENCODING(false, false, 1, 0x8B, 0, 0, 0, 0, r, rm, o)
/* @asm: movzx {r:d}, word ptr [{rm}{o}] */
#define MOVZX_DR_WO(r, rm, o) \
    OFFSET_ENCODING(false, false, 2, 0x0F, 0xB7, 0, 0, 0, r, rm, o)
/* @asm: movzx {r:d}, byte ptr [{rm}{o}] */
#define MOVZX_DR_BO(r, rm, o) \
    OFFSET_ENCODING(false, false, 2, 0x0F, 0xB6, 0, 0, 0, r, rm, o)
/* @asm: mov byte ptr [{rm}{o}], {r:b} */
#define MOV_BO_R(rm, o, r) \
    OFFSET_ENCODING(false, REX_BYTE, 1, 0x88, 0, 0, 0, 0, r, rm, o)
/* RIP-relative: 'o' is from the end of the instruction, which is 7 bytes.
   It is evaluated before anything is written, so it may use 'code'. */
/* @asm: lea {r}, [rip{o:d32}] */
#define LEA_R_RIP(r, o)                            \
    do {                                           \
        long _rip_o = (o);                         \
        REX_ENCODING(true, r, 0, 0);               \
        WRITE_1(0x8D);                             \
        MODRM_ENCODING(0, r, 5);                   \
        WRITE_32BIT(_rip_o);                       \
        le_reloc_note(code - 4, LE_RELOC_DATA, 4); \
    } while (0)
/* compares with the word at 'o' from the thread pointer */
/* @asm: cmp {r}, qword ptr fs:[0{o:d32}] */
#define CMP_R_FS(r, o)               \
    do {                             \
        long _fs_o = (o);            \
        WRITE_1(0x64);               \
        REX_ENCODING(true, r, 0, 0); \
        WRITE_1(0x3B);               \
        MODRM_ENCODING(0, r, 4);     \
        WRITE_1(0x25);               \
        WRITE_32BIT(_fs_o);          \
    } while (0)
/* the word at 'o' from the thread pointer (%fs base) */
/* @asm: mov {r}, qword ptr fs:[0{o:d32}] */
#define MOV_R_FS(r, o)               \
    do {                             \
        long _fs_o = (o);            \
        WRITE_1(0x64);               \
        REX_ENCODING(true, r, 0, 0); \
        WRITE_1(0x8B);               \
        MODRM_ENCODING(0, r, 4);     \
        WRITE_1(0x25);               \
        WRITE_32BIT(_fs_o);          \
    } while (0)
/* @asm: xor {r}, qword ptr [rip{o:d32}] */
#define XOR_R_RIP(r, o)                            \
    do {                                           \
        long _rip_o = (o);                         \
        REX_ENCODING(true, r, 0, 0);               \
        WRITE_1(0x33);                             \
        MODRM_ENCODING(0, r, 5);                   \
        WRITE_32BIT(_rip_o);                       \
        le_reloc_note(code - 4, LE_RELOC_DATA, 4); \
    } while (0)
/* @asm: xor {r}, qword ptr [{rm}{o}] */
#define XOR_R_O(r, rm, o) \
    OFFSET_ENCODING(false, true, 1, 0x33, 0, 0, 0, 0, r, rm, o)
/* @asm: mov {r}, qword ptr [{base}+{index}*{scale}{offset:d8}] */
#define MOV_R_X8(r, base, offset, index, scale)                            \
    SIB_ENCODING(false, true, 1, 0x8B, 0, 0, 0, 8, r, base, offset, index, \
                 scale)
/* @asm: mov qword ptr [{base}+{index}*{scale}{offset:d8}], {r} */
#define MOV_X8_R(base, offset, index, scale, r)                            \
    SIB_ENCODING(false, true, 1, 0x89, 0, 0, 0, 8, r, base, offset, index, \
                 scale)
/* @asm: mov byte ptr [{base}+{index}*{scale}{offset:d8}], {r:b} */
#define MOV_BX8_R(base, offset, index, scale, r)                        \
    SIB_ENCODING(false, REX_BYTE, 1, 0x88, 0, 0, 0, 8, r, base, offset, \
                 index, scale)
/* @asm: movsx {r}, byte ptr [{base}+{index}*{scale}{offset:d8}] */
#define MOVSX_R_BX8(r, base, offset, index, scale)                            \
    SIB_ENCODING(false, true, 2, 0x0F, 0xBE, 0, 0, 8, r, base, offset, index, \
                 scale)
/* @asm: lea {r}, [{base}+{index}*{scale}{offset:d8}] */
#define LEA_R_X8(r, base, offset, index, scale)                            \
    SIB_ENCODING(false, true, 1, 0x8D, 0, 0, 0, 8, r, base, offset, index, \
                 scale)
/* @asm: cmp qword ptr [{base}+{index}*{scale}{offset:d8}], {r} */
#define CMP_R_X8(r, base, offset, index, scale)                            \
    SIB_ENCODING(false, true, 1, 0x39, 0, 0, 0, 8, r, base, offset, index, \
                 scale)
/* @asm: movabs r10, {i:i64}; mov qword ptr [{rm}{o}], r10 */
#define MOV_O_I(rm, o, i)                \
    do {                                 \
        MOV_R_I(REG_TRANSIENT_1, i);     \
        MOV_O_R(rm, o, REG_TRANSIENT_1); \
    } while (0)
/* @asm: mov qword ptr [{rm}{o}], {i:i32} */
#define MOV_O_I32(rm, o, i)                                          \
    do {                                                             \
        OFFSET_ENCODING(false, true, 1, 0xC7, 0, 0, 0, 0, 0, rm, o); \
        WRITE_32BIT(i);                                              \
    } while (0)
/* @asm: xchg {rg1}, {rg2} */
#define XCHG_R_R(rg1, rg2)                                     \
    do {                                                       \
        if (rg1 == REG_X64_RAX) {                              \
            BASE_ENCODING(true, 0, 0, 0, 0, 0x90, 0, rg2);     \
        }                                                      \
        else if (rg2 == REG_X64_RAX) {                         \
            BASE_ENCODING(true, 0, 0, 0, 0, 0x90, 0, rg1);     \
        }                                                      \
        else {                                                 \
            DIRECT_ENCODING(true, 1, 0x87, 0, 0, 0, rg1, rg2); \
        }                                                      \
    } while (0)
/* @asm: xchg qword ptr [{rm}{o}], {r} */
#define XCHG_R_O(r, rm, o) \
    OFFSET_ENCODING(false, true, 1, 0x87, 0, 0, 0, 0, r, rm, o)
/* @asm: test {rm}, {r} */
#define TEST_R_R(r, rm) DIRECT_ENCODING(true, 1, 0x85, 0, 0, 0, r, rm)
/* @asm: test {rm:d}, {r:d} */
#define TEST_DR_DR(r, rm) DIRECT_ENCODING(false, 1, 0x85, 0, 0, 0, r, rm)
/* @asm: test {r}, {i:i32} */
#define TEST_R_I(r, i)                             \
    DIRECT_ENCODING(true, 1, 0xF7, 0, 0, 0, 0, r); \
    WRITE_32BIT(i)
/* @asm: cmp {rm}, {r} */
#define CMP_R_R(r, rm) DIRECT_ENCODING(true, 1, 0x39, 0, 0, 0, r, rm)
/* @asm: cmp qword ptr [{rm}], {r} */
#define CMP_R_A(r, rm) INDIRECT_ENCODING(true, 1, 0x39, 0, 0, 0, r, rm)
/* @asm: cmp qword ptr [{rm}{o:d8}], {r} */
#define CMP_R_O8(r, rm, o) \
    OFFSET_ENCODING(false, true, 1, 0x39, 0, 0, 0, 8, r, rm, o)
/* @asm: cmp qword ptr [{rm}{o:d32}], {r} */
#define CMP_R_O32(r, rm, o) \
    OFFSET_ENCODING(false, true, 1, 0x39, 0, 0, 0, 32, r, rm, o)
/* @asm: cmp qword ptr [{rm}{o}], {i:i32} */
#define CMP_I32_O(i, rm, o)                                         \
    OFFSET_ENCODING(false, true, 1, 0x81, 0, 0, 0x38, 0, 0, rm, o); \
    WRITE_32BIT(i)
/* @asm: cmp qword ptr [{rm}{o}], {i:i8} */
#define CMP_I8_O(i, rm, o)                                          \
    OFFSET_ENCODING(false, true, 1, 0x83, 0, 0, 0x38, 0, 0, rm, o); \
    WRITE_8BIT(i)
/* @asm: cmp byte ptr [{rm}{o}], {i:i8} */
#define CMP_I8_BO(i, rm, o)                                              \
    do {                                                                 \
        OFFSET_ENCODING(false, false, 1, 0x80, 0, 0, 0x38, 0, 0, rm, o); \
        WRITE_8BIT(i);                                                   \
    } while (0)
/* @asm: cmp word ptr [{rm}{o}], {i:i16} */
#define CMP_I16_WO(i, rm, o)                                             \
    do {                                                                 \
        WRITE_1(0x66);                                                   \
        OFFSET_ENCODING(false, false, 1, 0x81, 0, 0, 0x38, 0, 0, rm, o); \
        WRITE_16BIT(i);                                                  \
    } while (0)
/* @asm: cmp dword ptr [{rm}{o}], {i:i32} */
#define CMP_I32_DO(i, rm, o)                                             \
    do {                                                                 \
        OFFSET_ENCODING(false, false, 1, 0x81, 0, 0, 0x38, 0, 0, rm, o); \
        WRITE_32BIT(i);                                                  \
    } while (0)
/* @asm: cmp dword ptr [{rm}], {i:i32} */
#define CMP_I32_DA(i, rm)                                 \
    INDIRECT_ENCODING(false, 1, 0x81, 0, 0, 0x38, 0, rm); \
    WRITE_32BIT(i)
/* @asm: cmp qword ptr [{rm}], {i:i8} */
#define CMP_I8_A(i, rm)                                  \
    INDIRECT_ENCODING(true, 1, 0x83, 0, 0, 0x38, 0, rm); \
    WRITE_8BIT(i)
/* @asm: cmp dword ptr [{rm}], {i:i8} */
#define CMP_I8_DA(i, rm)                                  \
    INDIRECT_ENCODING(false, 1, 0x83, 0, 0, 0x38, 0, rm); \
    WRITE_8BIT(i)
/* @asm: cmp dword ptr [{rm}{o}], {i:i8} */
#define CMP_I8_DO(i, rm, o)                                          \
    OFFSET_ENCODING(false, false, 1, 0x83, 0, 0, 0x38, 0, 0, rm, o); \
    WRITE_8BIT(i)
/* @asm: cmp dword ptr [rip{o:d32}], {i:i8} */
#define CMP_I8_DRIP(i, o)                          \
    do {                                           \
        WRITE_1(0x83);                             \
        WRITE_1(0x3D);                             \
        WRITE_32BIT(o);                            \
        WRITE_8BIT(i);                             \
        le_reloc_note(code - 5, LE_RELOC_DATA, 5); \
    } while (0)
/* cmp dword ptr [address], i: RIP-relative when in reach (7 bytes), else
   through REG_TRANSIENT_2 */
#define CMP_I8_DABS(i, address)                             \
    do {                                                    \
        long _rip_off = (long)(address) - ((long)code + 7); \
        if (FITS_IN_32BITS(_rip_off)) {                     \
            CMP_I8_DRIP(i, _rip_off);                       \
        }                                                   \
        else {                                              \
            MOV_R_I(REG_TRANSIENT_2, (long)(address));      \
            CMP_I8_DA(i, REG_TRANSIENT_2);                  \
        }                                                   \
    } while (0)
/* @asm: cmp {rm}, {i:i8} */
#define CMP_I8_R(i, rm)                                \
    DIRECT_ENCODING(true, 1, 0x83, 0, 0, 0xF8, 0, rm); \
    WRITE_8BIT(i)
/* @asm: cmp {rm:d}, {i:i8} */
#define CMP_I8_DR(i, rm)                                \
    DIRECT_ENCODING(false, 1, 0x83, 0, 0, 0xF8, 0, rm); \
    WRITE_8BIT(i)
#define CMP_I_R(i1, r2)                                        \
    do {                                                       \
        if (FITS_IN_8BITS(i1)) {                               \
            DIRECT_ENCODING(true, 1, 0x83, 0, 0, 0xF8, 0, r2); \
            WRITE_8BIT(i1);                                    \
        }                                                      \
        else {                                                 \
            MOV_R_I(REG_TRANSIENT_1, (i1));                    \
            CMP_R_R(REG_TRANSIENT_1, (r2));                    \
        }                                                      \
    } while (0)
/* @asm: jmp {r} */
#define JMP_R(r)                                           \
    do {                                                   \
        le_trace_jump_reg((code_t *)(code), (int)(r));     \
        DIRECT_ENCODING(false, 1, 0xFF, 0, 0, 0xE0, 0, r); \
    } while (0)
#if LE_TRACE
#define CAPTURE_CODE_LOCATION code_t *location = code;
#else
#define CAPTURE_CODE_LOCATION /* Nothing */
#endif
#define JMP_CC_UI32(cc, address)                                   \
    do {                                                           \
        CAPTURE_CODE_LOCATION                                      \
        BASE_ENCODING(false, 1, 0x0F, 0, 0, 0x80 | (cc), 0, 0);    \
        le_trace_jump_cond((code_t *)(location), (code_t *)(code), \
                           (code_t *)(address));                   \
        UPDATABLE_WRITE_32BIT(REL32(code + 4, address));           \
    } while (0)
/* a checked build clobbers XMM8-XMM14 after a call that XMM values are
   live across in their stack slots, as any C callee may, so that a value
   wrongly read from its register after the call shows in tests; code that
   saves them around a call does the same before loading them back.
   po->call_end, the return address END_CALL_I records, is then taken
   before that code */
#if ALL_CHECKS
EXTERNFN code_t *
psyco_clobber_xmm(code_t *code);
#define CLOBBER_XMM_AFTER_CALL()                                   \
    do {                                                           \
        po->call_end = code;                                       \
        if (po->xmm_call == XMM_CALL_STORED && psyco_xmm_live(po)) \
            code = psyco_clobber_xmm(code);                        \
    } while (0)
#define CALL_END_HERE() ((void)0)
#else
#define CLOBBER_XMM_AFTER_CALL() \
    do {                         \
    } while (0)
#define CALL_END_HERE() (po->call_end = code)
#endif
/* @asm: call {r} */
#define CALL_R(r)                                          \
    do {                                                   \
        extra_assert(psyco_xmm_may_call(po));              \
        le_trace_call_reg((code_t *)(code), r);            \
        DIRECT_ENCODING(false, 1, 0xFF, 0, 0, 0xD0, 0, r); \
        CLOBBER_XMM_AFTER_CALL();                          \
    } while (0)
#define CALL_I(i)                                          \
    do {                                                   \
        long jump_amount;                                  \
        extra_assert(psyco_xmm_may_call(po));              \
        le_trace_call((code_t *)(code), (code_t *)(i));    \
        jump_amount = (long)(i) - ((long)code + 5);        \
        if (FITS_IN_32BITS(jump_amount)) {                 \
            WRITE_1(0xE8);                                 \
            WRITE_32BIT(jump_amount);                      \
            le_reloc_note(code - 4, LE_RELOC_JUMP, 4);     \
            CLOBBER_XMM_AFTER_CALL();                      \
        }                                                  \
        else {                                             \
            code_t *_call_mov = code;                      \
            MOV_R_I(REG_TRANSIENT_1, i);                   \
            if (code - _call_mov == 10)                    \
                le_reloc_note(code - 8, LE_RELOC_WORD, 0); \
            CALL_R(REG_TRANSIENT_1);                       \
        }                                                  \
    } while (0)
/* @asm: add qword ptr [{rm}], {i:i8} */
#define ADD_A_I8(rm, i)                               \
    INDIRECT_ENCODING(true, 1, 0x83, 0, 0, 0, 0, rm); \
    WRITE_8BIT(i)
/* @asm: add qword ptr [{rm}{o:d8}], {i:i8} */
#define ADD_O8_I8(rm, o, i)                                      \
    OFFSET_ENCODING(false, true, 1, 0x83, 0, 0, 0, 8, 0, rm, o); \
    WRITE_8BIT(i)
/* @asm: sub qword ptr [{rm}], {i:i8} */
#define SUB_A_I8(rm, i)                                  \
    INDIRECT_ENCODING(true, 1, 0x83, 0, 0, 0x28, 0, rm); \
    WRITE_8BIT(i)
/* @asm: sub dword ptr [{rm}], {i:i8} */
#define SUB_DA_I8(rm, i)                                  \
    INDIRECT_ENCODING(false, 1, 0x83, 0, 0, 0x28, 0, rm); \
    WRITE_8BIT(i)
/* @asm: add dword ptr [{rm}], {i:i8} */
#define ADD_DA_I8(rm, i)                               \
    INDIRECT_ENCODING(false, 1, 0x83, 0, 0, 0, 0, rm); \
    WRITE_8BIT(i)
/* @asm: sbb dword ptr [{rm}], {i:i8} */
#define SBB_DA_I8(rm, i)                                  \
    INDIRECT_ENCODING(false, 1, 0x83, 0, 0, 0x18, 0, rm); \
    WRITE_8BIT(i)
/* inc and dec leave CF alone, so only ZF/SF/OF tests may follow */
/* @asm: inc qword ptr [{rm}] */
#define INC_A(rm) INDIRECT_ENCODING(true, 1, 0xFF, 0, 0, 0, 0, rm)
/* @asm: dec qword ptr [{rm}] */
#define DEC_A(rm) INDIRECT_ENCODING(true, 1, 0xFF, 0, 0, 0x08, 0, rm)
/* @asm: inc qword ptr [{rm}{o:d8}] */
#define INC_O8(rm, o) \
    OFFSET_ENCODING(false, true, 1, 0xFF, 0, 0, 0, 8, 0, rm, o)
/* @asm: test byte ptr [{rm}{o}], {i:i8} */
#define TEST_I8_BO(rm, o, i)                                      \
    OFFSET_ENCODING(false, false, 1, 0xF6, 0, 0, 0, 0, 0, rm, o); \
    WRITE_8BIT(i)
/* @asm: add {r}, {rm} */
#define ADD_R_R(r, rm) DIRECT_ENCODING(true, 1, 0x03, 0, 0, 0, r, rm);
/* @asm: xor {r}, {rm} */
#define XOR_R_R(r, rm) DIRECT_ENCODING(true, 1, 0x33, 0, 0, 0, r, rm);
/* @asm: or {r}, {rm} */
#define OR_R_R(r, rm) DIRECT_ENCODING(true, 1, 0x0B, 0, 0, 0, r, rm)
/* @asm: and {r}, {rm} */
#define AND_R_R(r, rm) DIRECT_ENCODING(true, 1, 0x23, 0, 0, 0, r, rm)

/* @asm: sub {rm}, {i:i8} */
#define SUB_R_I8(rm, i)                                \
    DIRECT_ENCODING(true, 1, 0x83, 0, 0, 0x28, 0, rm); \
    WRITE_8BIT(i)
/* @asm: sub {rm}, {i:i32} */
#define SUB_R_I32(rm, i)                               \
    DIRECT_ENCODING(true, 1, 0x81, 0, 0, 0x28, 0, rm); \
    WRITE_32BIT(i)
/* @asm: and {rm}, {i:i8} */
#define AND_R_I8(rm, i)                                \
    DIRECT_ENCODING(true, 1, 0x83, 0, 0, 0x20, 0, rm); \
    WRITE_8BIT(i)
/* @asm: add {rm}, {i:i8} */
#define ADD_R_I8(rm, i)                               \
    DIRECT_ENCODING(true, 1, 0x83, 0, 0, 0x0, 0, rm); \
    WRITE_8BIT(i)
/* @asm: add {rm}, {i:i32} */
#define ADD_R_I32(rm, i)                              \
    DIRECT_ENCODING(true, 1, 0x81, 0, 0, 0x0, 0, rm); \
    WRITE_32BIT(i)
/* @asm: imul {r1}, {r2} */
#define IMUL_R_R(r1, r2) \
    WRITE_4(REX_64_REG_RM(r1, r2), 0x0F, 0xAF, MODRM_REG_RM(r1, r2))
/* @asm: set{cc} {r:b} */
#define SET_R_CC(r, cc) \
    DIRECT_ENCODING2((r > 3), false, 2, 0x0F, 0x90 | cc, 0, 0, 0, r)
/* @asm: push qword ptr [{rm}] */
#define PUSH_A(rm) INDIRECT_ENCODING(false, 1, 0xFF, 0, 0, 0x30, 0, rm)
/* @asm: push {r} */
#define PUSH_R(r) BASE_ENCODING(false, 0, 0, 0, 0, 0x50, 0, r);
/* @asm: movabs r10, {immed:i64}; push r10 */
#define PUSH_I(immed)                    \
    do {                                 \
        MOV_R_I(REG_TRANSIENT_1, immed); \
        PUSH_R(REG_TRANSIENT_1);         \
    } while (0)
/* @asm: push qword ptr [{rm}{o}] */
#define PUSH_O(rm, o) \
    OFFSET_ENCODING(false, false, 1, 0xFF, 0, 0, 0x30, 0, 0, rm, o)
/* @asm: pop {r} */
#define POP_R(r) BASE_ENCODING(false, 0, 0, 0, 0, 0x58, 0, r);
/* A counted PUSH that a later TEMP_POP_R must undo. 'mark' (an int) records
   the depth: nothing else may change it in between, e.g. a register spill
   by the allocator, which would make the POP restore the spilled value
   instead. Calls in between are fine: END_CALL restores
   the depth. */
#define TEMP_PUSH_R(r, mark)      \
    do {                          \
        PUSH_R(r);                \
        psyco_inc_stackdepth(po); \
        (mark) = po->stack_depth; \
    } while (0)
#define TEMP_POP_R(r, mark)                      \
    do {                                         \
        psyco_assert(po->stack_depth == (mark)); \
        POP_R(r);                                \
        psyco_dec_stackdepth(po);                \
    } while (0)
#define RET()                                 \
    do {                                      \
        le_trace_return((code_t *)(code), 0); \
        WRITE_1(0xC3);                        \
    } while (0)
/* @asm: ret {n:u16} */
#define RET_N(n)                                  \
    do {                                          \
        if (n == 0) {                             \
            RET();                                \
        }                                         \
        else {                                    \
            le_trace_return((code_t *)(code), n); \
            WRITE_1(0xC2);                        \
            WRITE_16BIT(n);                       \
        }                                         \
    } while (0)
#define PUSH_CC()                 \
    do {                          \
        WRITE_1(0x9C);            \
        psyco_inc_stackdepth(po); \
    } while (0)
#define POP_CC()                  \
    do {                          \
        WRITE_1(0x9D);            \
        psyco_dec_stackdepth(po); \
    } while (0)
#define SHIFT_BY_COUNT_ENCODING(r, mod, count)               \
    do {                                                     \
        if (count == 1) {                                    \
            DIRECT_ENCODING(true, 1, 0xD1, 0, 0, mod, 0, r); \
        }                                                    \
        else {                                               \
            DIRECT_ENCODING(true, 1, 0xC1, 0, 0, mod, 0, r); \
            WRITE_8BIT(count);                               \
        }                                                    \
    } while (0)
#define SHIFT_COUNTER REG_X64_RCX
#define SHIFT_BY_RCX_ENCODING(r, mod) \
    DIRECT_ENCODING(true, 1, 0xD3, 0, 0, mod, 0, r)

/* @asm: shl {rg}, {cnt:sh} */
#define SHIFT_LEFT_BY(rg, cnt) SHIFT_BY_COUNT_ENCODING(rg, 0x20, cnt)
/* @asm: shr {rg}, {cnt:sh} */
#define SHIFT_RIGHT_BY(rg, cnt) SHIFT_BY_COUNT_ENCODING(rg, 0x28, cnt)
/* @asm: sar {rg}, {cnt:sh} */
#define SHIFT_SIGNED_RIGHT_BY(rg, cnt) SHIFT_BY_COUNT_ENCODING(rg, 0x38, cnt)

/* XXX get rid of this ?? */
#define SHIFT_GENERICCL(rg, group) SHIFT_BY_RCX_ENCODING(rg, (group << 3))

#define STACK_POS_OFFSET(stack_pos) (po->stack_depth - (stack_pos))

typedef enum {
    ADD = 0,
    OR = 1,
    ADC = 2,
    SBB = 3,
    AND = 4,
    SUB = 5,
    XOR = 6,
    CMP = 7
} common_instr;

/* The "common instructions" groups: there are 8 arithmetic instructions
   whose encodings are identical. Here they are, with their 'group' value:
     ADD    (group 0)
     OR     (group 1)
     ADC    (group 2)
     SBB    (group 3)
     AND    (group 4)
     SUB    (group 5)
     XOR    (group 6)
     CMP    (group 7)  */

#define _ENCODE_COMMON_INSTR(instr) ((instr) << 3)
/* The following macro encodes  "INSTR register, immediate"  */

#define COMMON_INSTR_IMMED(instr, rm, i)                                      \
    do {                                                                      \
        if (FITS_IN_8BITS(i)) {                                               \
            DIRECT_ENCODING(true, 1, 0x83, 0, 0, _ENCODE_COMMON_INSTR(instr), \
                            0, rm);                                           \
            WRITE_8BIT(i);                                                    \
        }                                                                     \
        else if (FITS_IN_32BITS(i)) {                                         \
            DIRECT_ENCODING(true, 1, 0x81, 0, 0, _ENCODE_COMMON_INSTR(instr), \
                            0, rm);                                           \
            WRITE_32BIT(i);                                                   \
        }                                                                     \
        else {                                                                \
            COMMON_INSTR_I64(instr, rm, i);                                   \
        }                                                                     \
    } while (0)
/* "INSTR rm, i" for an 'i' beyond 32 bits: with its slot in the owner's
   constants (MOV_R_I), "INSTR rm, qword ptr [rip+slot]", which computes
   the same; else through REG_TRANSIENT_1 */
#define COMMON_INSTR_I64(instr, rm, i)                                      \
    do {                                                                    \
        long _ci = (long)(i);                                               \
        code_t *_cslot = psyco_const_slot(code, _ci);                       \
        if (_cslot != NULL) {                                               \
            COMMON_INSTR_R_RIP(instr, rm, (long)_cslot - ((long)code + 7)); \
        }                                                                   \
        else {                                                              \
            MOV_R_I(REG_TRANSIENT_1, _ci);                                  \
            DIRECT_ENCODING(true, 1, 0x01 | _ENCODE_COMMON_INSTR(instr), 0, \
                            0, 0, REG_TRANSIENT_1, rm);                     \
        }                                                                   \
    } while (0)
/* "INSTR r, qword ptr [rip+o]"; 'o' is from the end of its 7 bytes */
#define COMMON_INSTR_R_RIP(instr, r, o)              \
    do {                                             \
        long _rip_o = (o);                           \
        REX_ENCODING(true, r, 0, 0);                 \
        WRITE_1(0x03 | _ENCODE_COMMON_INSTR(instr)); \
        MODRM_ENCODING(0, r, 5);                     \
        WRITE_32BIT(_rip_o);                         \
        le_reloc_note(code - 4, LE_RELOC_DATA, 4);   \
    } while (0)
/* @asm: cmp {r}, qword ptr [rip{o:d32}] */
#define CMP_R_RIP(r, o) COMMON_INSTR_R_RIP(CMP, r, o)

#define DIRECT_OR_RSP_OFFSET_ENCODING(rex_w, op_len, b1, b2, b3, mod, r, \
                                      source)                            \
    do {                                                                 \
        if (getreg(source) == REG_NONE) {                                \
            long offset = STACK_POS_OFFSET(getstack(source));            \
            OFFSET_ENCODING(false, rex_w, op_len, b1, b2, b3, mod, 0, r, \
                            REG_X64_RSP, offset);                        \
        }                                                                \
        else {                                                           \
            DIRECT_ENCODING(rex_w, op_len, b1, b2, b3, mod, r,           \
                            getreg(source));                             \
        }                                                                \
    } while (0)

#define COMMON_INSTR_FROM_RT(instr, r, source) \
    DIRECT_OR_RSP_OFFSET_ENCODING(             \
        true, 1, 0x03 | _ENCODE_COMMON_INSTR(instr), 0, 0, 0, r, source)
#define COMMON_INSTR_IMMED_FROM_RT(instr, source, i)                          \
    do {                                                                      \
        if (FITS_IN_8BITS(i)) {                                               \
            DIRECT_OR_RSP_OFFSET_ENCODING(                                    \
                true, 1, 0x83, 0, 0, _ENCODE_COMMON_INSTR(instr), 0, source); \
            WRITE_8BIT(i);                                                    \
        }                                                                     \
        else if (FITS_IN_32BITS(i)) {                                         \
            DIRECT_OR_RSP_OFFSET_ENCODING(                                    \
                true, 1, 0x81, 0, 0, _ENCODE_COMMON_INSTR(instr), 0, source); \
            WRITE_32BIT(i);                                                   \
        }                                                                     \
        else if (getreg(source) != REG_NONE) {                                \
            COMMON_INSTR_I64(instr, getreg(source), i);                       \
        }                                                                     \
        else {                                                                \
            MOV_R_I(REG_TRANSIENT_1, i);                                      \
            DIRECT_OR_RSP_OFFSET_ENCODING(true, 1,                            \
                                          0x01 | _ENCODE_COMMON_INSTR(instr), \
                                          0, 0, 0, REG_TRANSIENT_1, source);  \
        }                                                                     \
    } while (0)

/* Encodes  "INSTR register, source"  for a run-time or compile-time vinfo_t */
#define COMMON_INSTR_FROM(group, rg, source)                              \
    do {                                                                  \
        if (((source) & TimeMask) == RunTime)                             \
            COMMON_INSTR_FROM_RT(group, rg, source);                      \
        else                                                              \
            COMMON_INSTR_IMMED(group, rg, KSOURCE_SOURCE(source)->value); \
    } while (0)

/* Encodes "INSTR reg" for the following instructions:
     NOT    (group 2)
     NEG    (group 3)
*/
#define UNARY_INSTR_ON_REG(instr, rm) \
    DIRECT_ENCODING(true, 1, 0xF7, 0, 0, _ENCODE_COMMON_INSTR(instr), 0, rm)

/* Encodes a check (zero/non-zero) on the given 'source' */
#define CHECK_NONZERO_FROM_RT(source, rcc)                     \
    do {                                                       \
        NEED_CC_SRC(source);                                   \
        if (RSOURCE_REG_IS_NONE(source)) {                     \
            CMP_I8_O(0, REG_X64_RSP,                           \
                     STACK_POS_OFFSET(RSOURCE_STACK(source))); \
        }                                                      \
        else {                                                 \
            CHECK_NONZERO_REG(RSOURCE_REG(source));            \
        }                                                      \
        rcc = CC_NE; /* a.k.a. NZ flag */                      \
    } while (0)
#define CHECK_NONZERO_REG(rg) TEST_R_R(rg, rg)

#define COMPARE_IMMED_FROM_RT(source, immed) \
    COMMON_INSTR_IMMED_FROM_RT(CMP, source, immed)

/* Signed integer multiplications */
#define IMUL_REG_FROM_RT(source, rg) \
    DIRECT_OR_RSP_OFFSET_ENCODING(true, 2, 0x0F, 0xAF, 0, 0, rg, source)

#define IMUL_IMMED_FROM_RT(source, immed, dstrg)                         \
    do {                                                                 \
        if (FITS_IN_8BITS(immed)) {                                      \
            DIRECT_OR_RSP_OFFSET_ENCODING(true, 1, 0x6B, 0, 0, 0, dstrg, \
                                          source);                       \
            WRITE_8BIT(immed);                                           \
        }                                                                \
        else if (FITS_IN_32BITS(immed)) {                                \
            DIRECT_OR_RSP_OFFSET_ENCODING(true, 1, 0x69, 0, 0, 0, dstrg, \
                                          source);                       \
            WRITE_32BIT(immed);                                          \
        }                                                                \
        else {                                                           \
            MOV_R_I(REG_TRANSIENT_1, immed);                             \
            IMUL_REG_FROM_RT(source, REG_TRANSIENT_1);                   \
            MOV_R_R(dstrg, REG_TRANSIENT_1);                             \
        }                                                                \
    } while (0)

/* a call of 'c_function' made now, at compile time, or emitted to run
   (codegen.h:psyco_generic_call) */
EXTERNFN vinfo_t *
compile_time_call(PsycoObject *po, void *c_function, int flags, int arg_count,
                  char argtags[], long args[]);
EXTERNFN vinfo_t *
run_time_call(PsycoObject *po, void *c_function, vinfo_t *vtarget, int flags,
              int arg_count, char argtags[], long args[],
              int total_output_array_size);

EXTERNFN vinfo_t *
bininstrgrp(PsycoObject *po, int group, bool ovf, bool nonneg, vinfo_t *v1,
            vinfo_t *v2);
EXTERNFN vinfo_t *
bininstrmul(PsycoObject *po, bool ovf, bool nonneg, vinfo_t *v1, vinfo_t *v2);
EXTERNFN vinfo_t *
bininstrshift(PsycoObject *po, int group, bool nonneg, vinfo_t *v1,
              vinfo_t *v2);
EXTERNFN condition_code_t
bininstrcmp(PsycoObject *po, int base_py_op, vinfo_t *v1, vinfo_t *v2);
EXTERNFN vinfo_t *
unaryinstrgrp(PsycoObject *po, int group, bool ovf, bool nonneg, vinfo_t *v1);

#define BINARY_INSTR_ADD(ovf, nonneg) bininstrgrp(po, 0, ovf, nonneg, v1, v2)
#define BINARY_INSTR_OR(ovf, nonneg) bininstrgrp(po, 1, ovf, nonneg, v1, v2)
#define BINARY_INSTR_AND(ovf, nonneg) bininstrgrp(po, 4, ovf, nonneg, v1, v2)
#define BINARY_INSTR_SUB(ovf, nonneg) bininstrgrp(po, 5, ovf, nonneg, v1, v2)
#define BINARY_INSTR_XOR(ovf, nonneg) bininstrgrp(po, 6, ovf, nonneg, v1, v2)
#define BINARY_INSTR_MUL(ovf, nonneg) bininstrmul(po, ovf, nonneg, v1, v2)
#define BINARY_INSTR_LSHIFT(nonneg) bininstrshift(po, 4, nonneg, v1, v2)
#define BINARY_INSTR_RSHIFT(nonneg) bininstrshift(po, 7, nonneg, v1, v2)
#define BINARY_INSTR_CMP(base_py_op) bininstrcmp(po, base_py_op, v1, v2)
#define UNARY_INSTR_NEG(ovf, nonneg) unaryinstrgrp(po, 3, ovf, nonneg, v1)

EXTERNFN vinfo_t *
bint_add_i(PsycoObject *po, vinfo_t *rt1, long value2, bool unsafe);
EXTERNFN vinfo_t *
bint_mul_i(PsycoObject *po, vinfo_t *v1, long value2, bool ovf);
EXTERNFN vinfo_t *
bint_lshift_i(PsycoObject *po, vinfo_t *v1, int counter);
EXTERNFN vinfo_t *
bint_rshift_i(PsycoObject *po, vinfo_t *v1, int counter);
EXTERNFN vinfo_t *
bint_urshift_i(PsycoObject *po, vinfo_t *v1, int counter);
EXTERNFN condition_code_t
bint_cmp_i(PsycoObject *po, int base_py_op, vinfo_t *rt1, long immed2);

/*****************************************************************/
/***   Some basic management instructions...                   ***/

#define LOAD_REG_FROM_IMMED(dst, immed) MOV_R_I(dst, immed)
#define LOAD_REG_FROM_EBP_BASE(dst, stack_pos) \
    MOV_R_O(dst, REG_X64_RSP, STACK_POS_OFFSET(stack_pos))
#define LOAD_REG_FROM_RT(source, dst) \
    DIRECT_OR_RSP_OFFSET_ENCODING(true, 1, 0x8B, 0, 0, 0, dst, source)
#define SAVE_REG_TO_EBP_BASE(src, stack_pos)                        \
    do {                                                            \
        if (REG_IS_XMM(src))                                        \
            MOVSD_O_XMM(REG_X64_RSP, STACK_POS_OFFSET(stack_pos),   \
                        XMM_HW(src));                               \
        else                                                        \
            MOV_O_R(REG_X64_RSP, STACK_POS_OFFSET(stack_pos), src); \
    } while (0)
/* through REG_TRANSIENT_1, as xchg with memory is locked: tens of cycles */
#define XCHG_REG_AND_EBP_BASE(src, stack_pos)                               \
    do {                                                                    \
        MOV_R_O(REG_TRANSIENT_1, REG_X64_RSP, STACK_POS_OFFSET(stack_pos)); \
        MOV_O_R(REG_X64_RSP, STACK_POS_OFFSET(stack_pos), src);             \
        MOV_R_R(src, REG_TRANSIENT_1);                                      \
    } while (0)
#define SAVE_IMMED_TO_EBP_BASE(immed, stack_pos)                          \
    do {                                                                  \
        long _sv_i = (long)(immed);                                       \
        if (FITS_IN_32BITS(_sv_i)) {                                      \
            MOV_O_I32(REG_X64_RSP, (STACK_POS_OFFSET(stack_pos)), _sv_i); \
        }                                                                 \
        else {                                                            \
            MOV_O_I(REG_X64_RSP, (STACK_POS_OFFSET(stack_pos)), _sv_i);   \
        }                                                                 \
    } while (0)
#define SAVE_IMM8_TO_EBP_BASE(imm8, stack_pos) \
    MOV_O_I32(REG_X64_RSP, (STACK_POS_OFFSET(stack_pos)), imm8)
#define PUSH_IMMED(immed) PUSH_I(immed)

/******************************************************************/
/*** Double Percision Floating Point                            ***/

/* XXX should refactor SIB_ENCODING and OFFSET_ENCODING too       */
#define SIB_ENCODING2(updatable, pre_rex, rex_w, post_rex, mod, size, r,  \
                      base, offset, index, scale)                         \
    do {                                                                  \
        pre_rex;                                                          \
        REX_ENCODING(rex_w, r, index, base);                              \
        post_rex;                                                         \
        MODRM_ENCODING(                                                   \
            mod | ((offset == 0 && !REQUIRES_OFFSET(base))                \
                       ? 0x04                                             \
                       : (DECODE_SIZE(size, offset) == 8 ? 0x44 : 0x84)), \
            r, 0);                                                        \
        WRITE_1((scale == 8   ? 0xC0                                      \
                 : scale == 4 ? 0x80                                      \
                 : scale == 2 ? 0x40                                      \
                              : 0x00) |                                   \
                (base & 7) | ((index & 7) << 3));                         \
        if (offset != 0 || REQUIRES_OFFSET(base)) {                       \
            ENCODE_OFFSET(updatable, DECODE_SIZE(size, offset), offset);  \
        }                                                                 \
    } while (0)
#define OFFSET_ENCODING2(updatable, pre_rex, rex_w, post_rex, mod, size, r,   \
                         rm, offset)                                          \
    do {                                                                      \
        if ((rm & 7) == 4) {                                                  \
            /* as OFFSET_ENCODING: index RSP without REX.X is none */         \
            SIB_ENCODING2(updatable, pre_rex, rex_w, post_rex, mod, size, r,  \
                          rm, offset, REG_X64_RSP, 0);                        \
        }                                                                     \
        else {                                                                \
            pre_rex;                                                          \
            REX_ENCODING(rex_w, r, 0, rm);                                    \
            post_rex;                                                         \
            MODRM_ENCODING(                                                   \
                mod | (DECODE_SIZE(size, offset) == 8 ? 0x40 : 0x80), r, rm); \
            ENCODE_OFFSET(updatable, DECODE_SIZE(size, offset), offset);      \
        }                                                                     \
    } while (0)

/* SSE2 scalar double instructions: a mandatory prefix, then REX, then
   0F and the opcode. x, x1 and x2 are XMM register numbers 0-15; x1 is
   the destination. None of these changes the flags, except UCOMISD */
#define SSE_DIRECT(prefix, rex_w, op, r, rm)              \
    do {                                                  \
        WRITE_1(prefix);                                  \
        DIRECT_ENCODING(rex_w, 2, 0x0F, op, 0, 0, r, rm); \
    } while (0)
#define SSE_OFFSET(prefix, rex_w, op, r, rm, offset)                         \
    OFFSET_ENCODING2(false, WRITE_1(prefix), rex_w, WRITE_2(0x0F, op), 0, 0, \
                     r, rm, offset)

/* @asm: movq {r}, {x} */
#define MOVQ_QR_XMM(r, x) SSE_DIRECT(0x66, true, 0x7E, x, r)
/* @asm: movq {x}, {r} */
#define MOVQ_XMM_QR(x, r) SSE_DIRECT(0x66, true, 0x6E, x, r)
/* @asm: movq {x}, qword ptr [{rm}{o}] */
#define MOVQ_XMM_O(x, rm, o) SSE_OFFSET(0x66, true, 0x6E, x, rm, o)
/* @asm: movsd {x}, qword ptr [{rm}{o}] */
#define MOVSD_XMM_O(x, rm, o) SSE_OFFSET(0xF2, false, 0x10, x, rm, o)
/* @asm: movsd qword ptr [{rm}{o}], {x} */
#define MOVSD_O_XMM(rm, o, x) SSE_OFFSET(0xF2, false, 0x11, x, rm, o)
/* a whole-register copy: MOVSD between registers keeps x1's high half, so
   it would wait for x1's last writer */
/* @asm: movapd {x1}, {x2} */
#define MOVAPD_XMM_XMM(x1, x2) SSE_DIRECT(0x66, false, 0x28, x1, x2)
/* @asm: addsd {x1}, {x2} */
#define ADDSD_XMM_XMM(x1, x2) SSE_DIRECT(0xF2, false, 0x58, x1, x2)
/* @asm: subsd {x1}, {x2} */
#define SUBSD_XMM_XMM(x1, x2) SSE_DIRECT(0xF2, false, 0x5C, x1, x2)
/* @asm: mulsd {x1}, {x2} */
#define MULSD_XMM_XMM(x1, x2) SSE_DIRECT(0xF2, false, 0x59, x1, x2)
/* @asm: divsd {x1}, {x2} */
#define DIVSD_XMM_XMM(x1, x2) SSE_DIRECT(0xF2, false, 0x5E, x1, x2)
/* sets ZF, PF and CF as an unsigned compare of x1 with x2; all three
   when either is a NaN. Call NEED_CC first */
/* @asm: ucomisd {x1}, {x2} */
#define UCOMISD_XMM_XMM(x1, x2) SSE_DIRECT(0x66, false, 0x2E, x1, x2)
/* @asm: xorpd {x1}, {x2} */
#define XORPD_XMM_XMM(x1, x2) SSE_DIRECT(0x66, false, 0x57, x1, x2)
/* @asm: andpd {x1}, {x2} */
#define ANDPD_XMM_XMM(x1, x2) SSE_DIRECT(0x66, false, 0x54, x1, x2)
/* x1's low half becomes all ones if predicate 'i' holds of x1 and x2, else
   zero (0 equal, false if unordered; 4 not equal, true if unordered) */
/* @asm: cmpsd {x1}, {x2}, {i:u8} */
#define CMPSD_XMM_XMM_U8(x1, x2, i)            \
    do {                                       \
        SSE_DIRECT(0xF2, false, 0xC2, x1, x2); \
        WRITE_8BIT(i);                         \
    } while (0)
/* writes only x's low half: zero x first (XORPD) so that it doesn't wait
   for x's last writer */
/* @asm: cvtsi2sd {x}, {r} */
#define CVTSI2SD_XMM_QR(x, r) SSE_DIRECT(0xF2, true, 0x2A, x, r)

/*******************************************************************
 * Call a C function.
 * Use BEGIN_CALL() then CALL_SET_ARG_* in reverse arg order
 * then END_CALL_R(r) or END_CALL_I(immediate)
 *******************************************************************/
static const int argument_reg_table_len = 6;
static const reg_t argument_reg_table[] = {
    REG_X64_RDI, /* First ARG in RDI etc. */
    REG_X64_RSI, REG_X64_RDX, REG_X64_RCX, REG_X64_R8, REG_X64_R9};

static const bool callee_saved_reg_table[REG_TOTAL] = {
    false, /* RAX */
    false, /* RCX */
    false, /* RDX */
    true,  /* RBX */
    false, /* RSP*/
    true,  /* RBP */
    false, /* RSI */
    false, /* RDI */
    false, /* R8 */
    false, /* R9 */
    false, /* R10 */
    false, /* R11 */
    true,  /* R12 */
    true,  /* R13 */
    true,  /* R14 */
    true,  /* R15 */
};

/*
 * Return the stack depth after and including the return address pushed by the
 * call to this function when inlining the return address is saved in a funny
 * place
 */
#define STACK_DEPTH_SINCE_CALL()                                     \
    (po->pr.is_inlining                                              \
         ? (LOC_INLINING == NULL                                     \
                ? (LOC_CONTINUATION == NULL                          \
                       ? (extra_assert(!"no frame to align from"),   \
                          (size_t)-1)                                \
                       : ((po->stack_depth -                         \
                           getstack(LOC_CONTINUATION->source)) +     \
                          sizeof(long)))                             \
                : ((po->stack_depth -                                \
                    getstack(LOC_INLINING->array                     \
                                 ->items[TOTAL_LOC_INLINE +          \
                                         INDEX_LOC_CONTINUATION]     \
                                 ->source)) +                        \
                   sizeof(long)))                                    \
         : ((po->stack_depth - getstack(LOC_CONTINUATION->source)) + \
            sizeof(long)))

#define BEGIN_CALL(_num_args)                                            \
    do {                                                                 \
        int _last_arg_index = (_num_args); /* args are set last first */ \
        int _initial_stack_depth = po->stack_depth;                      \
        /* align stack to 16 byte */                                     \
        int _stack_correction = 0;                                       \
        int _args_depth = 0;                                             \
        if (_num_args > argument_reg_table_len) {                        \
            _args_depth =                                                \
                (((int)(_num_args) - (int)(argument_reg_table_len)) *    \
                 (int)sizeof(long));                                     \
        }                                                                \
        _stack_correction =                                              \
            (16 - ((STACK_DEPTH_SINCE_CALL() + _args_depth) % 16)) % 16; \
        STACK_CORRECTION(_stack_correction);                             \
        po->stack_depth += _stack_correction;                            \
        do {                                                             \
    } while (0)
#define _CHECK_IS_NEXT_ARG(index)                                             \
    do {                                                                      \
        /* each integer argument once, last first: stack arguments are pushed \
         */                                                                   \
        extra_assert((index) == _last_arg_index - 1);                         \
        _last_arg_index = (index);                                            \
    } while (0)
#define END_CALL_R(r)                                                    \
    do {                                                                 \
        extra_assert(_last_arg_index == 0); /* every argument was set */ \
        CALL_R(r);                                                       \
        STACK_CORRECTION(_initial_stack_depth - po->stack_depth);        \
        po->stack_depth = _initial_stack_depth;                          \
        (void)_last_arg_index; /* read only by extra_assert */           \
    } while (0);                                                         \
    }                                                                    \
    while (0)
#define END_CALL_I(immed)                                                \
    do {                                                                 \
        extra_assert(_last_arg_index == 0); /* every argument was set */ \
        CALL_I(immed);                                                   \
        CALL_END_HERE();                                                 \
        STACK_CORRECTION(_initial_stack_depth - po->stack_depth);        \
        po->stack_depth = _initial_stack_depth;                          \
        (void)_last_arg_index; /* read only by extra_assert */           \
    } while (0);                                                         \
    }                                                                    \
    while (0)
/* ends a BEGIN_CALL whose calls were written in it, each at its depth */
#define END_CALLS()                                                      \
    do {                                                                 \
        extra_assert(_last_arg_index == 0); /* every argument was set */ \
        STACK_CORRECTION(_initial_stack_depth - po->stack_depth);        \
        po->stack_depth = _initial_stack_depth;                          \
        (void)_last_arg_index; /* read only by extra_assert */           \
    } while (0);                                                         \
    }                                                                    \
    while (0)
/* END_CALL_I, or END_CALL_R(r) where immed is NULL */
#define END_CALL_I_OR_R(immed, r)                                        \
    do {                                                                 \
        extra_assert(_last_arg_index == 0); /* every argument was set */ \
        if ((immed) != NULL)                                             \
            CALL_I(immed);                                               \
        else                                                             \
            CALL_R(r);                                                   \
        po->call_end = code;                                             \
        STACK_CORRECTION(_initial_stack_depth - po->stack_depth);        \
        po->stack_depth = _initial_stack_depth;                          \
        (void)_last_arg_index; /* read only by extra_assert */           \
    } while (0);                                                         \
    }                                                                    \
    while (0)
#define CHECK_AND_UPDATE_TARGET_REG(reg)                           \
    do {                                                           \
        vinfo_t *vi = REG_NUMBER(po, reg);                         \
        if (vi != NULL) {                                          \
            extra_assert(RUNTIME_STACK(vi) != RUNTIME_STACK_NONE); \
            SET_RUNTIME_REG_TO_NONE(vi);                           \
            REG_NUMBER(po, (reg)) = NULL;                          \
        }                                                          \
    } while (0)
#define CALL_SET_ARG_IMMED(immed, arg_index)               \
    do {                                                   \
        _CHECK_IS_NEXT_ARG(arg_index);                     \
        if (arg_index < argument_reg_table_len) {          \
            reg_t dst_reg = argument_reg_table[arg_index]; \
            CHECK_AND_UPDATE_TARGET_REG(dst_reg);          \
            MOV_R_I(dst_reg, immed);                       \
        }                                                  \
        else {                                             \
            PUSH_I(immed);                                 \
            psyco_inc_stackdepth(po);                      \
        }                                                  \
    } while (0)
#define CHECK_AND_UPDATE_SOURCE_REG(reg)                               \
    do {                                                               \
        vinfo_t *vi = REG_NUMBER(po, reg);                             \
        if (vi != NULL) {                                              \
            if (!callee_saved_reg_table[RUNTIME_REG(vi)]) {            \
                extra_assert(RUNTIME_STACK(vi) != RUNTIME_STACK_NONE); \
                SET_RUNTIME_REG_TO_NONE(vi);                           \
                REG_NUMBER(po, (reg)) = NULL;                          \
            }                                                          \
        }                                                              \
    } while (0)
#define CALL_SET_ARG_FROM_REG(src_reg, arg_index)          \
    do {                                                   \
        _CHECK_IS_NEXT_ARG(arg_index);                     \
        if (arg_index < argument_reg_table_len) {          \
            reg_t dst_reg = argument_reg_table[arg_index]; \
            if (dst_reg != src_reg) {                      \
                CHECK_AND_UPDATE_TARGET_REG(dst_reg);      \
                MOV_R_R(dst_reg, src_reg);                 \
            }                                              \
            CHECK_AND_UPDATE_SOURCE_REG(src_reg);          \
        }                                                  \
        else {                                             \
            PUSH_R(src_reg);                               \
            psyco_inc_stackdepth(po);                      \
        }                                                  \
    } while (0)
#define CALL_SET_ARG_FROM_RT(source, arg_index)                               \
    do {                                                                      \
        if (RSOURCE_REG_IS_NONE(source)) {                                    \
            _CHECK_IS_NEXT_ARG(arg_index);                                    \
            extra_assert(getstack(source) != RUNTIME_STACK_NONE);             \
            if (arg_index < argument_reg_table_len) {                         \
                reg_t dst_reg = argument_reg_table[arg_index];                \
                CHECK_AND_UPDATE_TARGET_REG(dst_reg);                         \
                MOV_R_O(dst_reg, REG_X64_RSP,                                 \
                        STACK_POS_OFFSET(RSOURCE_STACK(source)));             \
            }                                                                 \
            else {                                                            \
                PUSH_O(REG_X64_RSP, STACK_POS_OFFSET(RSOURCE_STACK(source))); \
                psyco_inc_stackdepth(po);                                     \
            }                                                                 \
        }                                                                     \
        else {                                                                \
            CALL_SET_ARG_FROM_REG(getreg(source), arg_index);                 \
        }                                                                     \
    } while (0)
#define CALL_SET_ARG_FROM_RT_DOUBLE(source, double_arg_index)         \
    do {                                                              \
        if (RSOURCE_REG_IS_NONE(source)) {                            \
            extra_assert(getstack(source) != RUNTIME_STACK_NONE);     \
            /* we assume all doubles are passed in xmm registers */   \
            MOVQ_XMM_O(double_arg_index, REG_X64_RSP,                 \
                       STACK_POS_OFFSET(RSOURCE_STACK(source)));      \
        }                                                             \
        else if (REG_IS_XMM(getreg(source))) {                        \
            MOVAPD_XMM_XMM(double_arg_index, XMM_HW(getreg(source))); \
        }                                                             \
        else {                                                        \
            MOVQ_XMM_QR(double_arg_index, getreg(source));            \
        }                                                             \
    } while (0)
#define CALL_SET_ARG_IMMED_DOUBLE(immed, double_arg_index)      \
    do {                                                        \
        /* we assume all doubles are passed in xmm registers */ \
        MOV_R_I(REG_TRANSIENT_1, immed);                        \
        MOVQ_XMM_QR(double_arg_index, REG_TRANSIENT_1);         \
    } while (0)
#define CALL_SET_ARG_FROM_STACK_REF(source, arg_index)        \
    do {                                                      \
        _CHECK_IS_NEXT_ARG(arg_index);                        \
        extra_assert(RSOURCE_REG_IS_NONE(source));            \
        extra_assert(getstack(source) != RUNTIME_STACK_NONE); \
        if (arg_index < argument_reg_table_len) {             \
            reg_t dst_reg = argument_reg_table[arg_index];    \
            CHECK_AND_UPDATE_TARGET_REG(dst_reg);             \
            LEA_R_O(dst_reg, REG_X64_RSP,                     \
                    STACK_POS_OFFSET(RSOURCE_STACK(source))); \
        }                                                     \
        else {                                                \
            LEA_R_O(REG_TRANSIENT_1, REG_X64_RSP,             \
                    STACK_POS_OFFSET(RSOURCE_STACK(source))); \
            PUSH_R(REG_TRANSIENT_1);                          \
            psyco_inc_stackdepth(po);                         \
        }                                                     \
    } while (0)

/***************************************************************/
/***************************************************************/
#define LOAD_REG_FROM_REG_PLUS_IMMED(dst, rg1, immed) LEA_R_O(dst, rg1, immed)

/*****************************************************************/
/***   vinfo_t saving                                          ***/

/* save 'vi', which is currently in register 'rg', to a new stack slot;
   LEA keeps the flags, as PUSH does */
#define SAVE_REG_VINFO(vi, rg)                       \
    do {                                             \
        if (REG_IS_XMM(rg)) {                        \
            LEA_R_O(REG_X64_RSP, REG_X64_RSP, -8);   \
            MOVSD_O_XMM(REG_X64_RSP, 0, XMM_HW(rg)); \
        }                                            \
        else                                         \
            PUSH_R(rg);                              \
        psyco_inc_stackdepth(po);                    \
        SET_RUNTIME_STACK_TO(vi, po->stack_depth);   \
    } while (0)

/* ensure that the register 'rg' is free */
#define NEED_REGISTER(rg)                                      \
    do {                                                       \
        vinfo_t *_content = REG_NUMBER(po, (rg));              \
        if (_content != NULL) {                                \
            if (RUNTIME_STACK(_content) == RUNTIME_STACK_NONE) \
                SAVE_REG_VINFO(_content, rg);                  \
            SET_RUNTIME_REG_TO_NONE(_content);                 \
            REG_NUMBER(po, (rg)) = NULL;                       \
        }                                                      \
    } while (0)

/* ensure that the condition code flags of the processor no
   longer contain any useful information */

#define NEED_CC() NEED_CC_REG(REG_NONE)
/* same as NEED_CC() but don't overwrite rg */
#define NEED_CC_REG(rg)                              \
    do {                                             \
        if (HAS_CCREG(po))                           \
            code = psyco_compute_cc(po, code, (rg)); \
    } while (0)
/* same as NEED_CC() but don't overwrite the given source */
#define NEED_CC_SRC(src) \
    NEED_CC_REG(is_runtime(src) ? RSOURCE_REG(src) : REG_NONE)
/* internal */
EXTERNFN code_t *
psyco_compute_cc(PsycoObject *po, code_t *code, reg_t reserved);

#define LOAD_REG_FROM_CONDITION(rg, cc) SET_R_CC(rg, cc)

#define NEED_FREE_REG_COND(targ, cond)                 \
    do {                                               \
        targ = po->last_used_reg;                      \
        if (!(cond) || REG_NUMBER(po, targ) != NULL) { \
            do {                                       \
                targ = NEXT_FREE_REG();                \
            } while (!(cond));                         \
            NEED_REGISTER(targ);                       \
        }                                              \
    } while (0)

/* like NEED_REGISTER but 'targ' is an output argument which will
   receive the number of a now-free register */
#define NEED_FREE_REG(targ) NEED_FREE_REG_COND(targ, 1)
#define NEED_FREE_BYTE_REG(targ, resrv1, resrv2)                        \
    do {                                                                \
        NEED_FREE_REG_COND(targ, targ != (resrv1) && targ != (resrv2)); \
        MOV_R_I(targ, 0); /* zero before use; MOV keeps the flags */    \
    } while (0)

/* XMM registers (iencoding.c). A run-time double may be in one;
   nothing else is. Code that needs it in a general register uses
   RTVINFO_IN_REG, which moves it there. */
/* a free tracked XMM register in 'targ', its value stored if none is free */
#define NEED_FREE_XMM(targ) (code = psyco_need_free_xmm(po, code, &(targ)))
/* every XMM value to its stack slot, the registers forgotten: before a C
   call, and at merge points, whose snapshots hold no XMM register */
#define XMM_SPILL()                       \
    do {                                  \
        code = psyco_xmm_store(po, code); \
        psyco_xmm_forget(po);             \
    } while (0)
EXTERNFN code_t *
psyco_need_free_xmm(PsycoObject *po, code_t *code, reg_t *targ);
/* 'vi', in an XMM register, into a general one */
EXTERNFN code_t *
psyco_xmm_to_reg(PsycoObject *po, code_t *code, vinfo_t *vi);
/* a C call clobbers every XMM register. Straight-line code stores each XMM
   value that has no stack slot to a new one before the call
   (psyco_xmm_store), and forgets the registers after it
   (psyco_xmm_forget). A call on a conditional path, which must leave the
   state as it was, saves the live ones around it
   (ipyencoding.c:decref_dealloc_calling) or goes through the copy of its
   glue that keeps them (processor.h:psyco_glue_keeping_xmm). */
EXTERNFN code_t *
psyco_xmm_store(PsycoObject *po, code_t *code);
EXTERNFN void
psyco_xmm_forget(PsycoObject *po);
EXTERNFN bool
psyco_xmm_live(PsycoObject *po);

/* float operations inline, on run-time or known doubles (not both known):
   a new run-time double in an XMM register. With two NaNs, a + b and
   a * b give a's, as CPython's specialised float instructions do */
#define FLOAT_OP_ADD 0
#define FLOAT_OP_SUB 1
#define FLOAT_OP_MUL 2
#define FLOAT_OP_DIV 3 /* b must not be zero: psyco_float_is_zero */
EXTERNFN vinfo_t *
psyco_float_op(PsycoObject *po, int op, vinfo_t *a, vinfo_t *b);
/* -a, or abs(a) if not 'neg' (only the sign bit changes, NaNs too) */
EXTERNFN vinfo_t *
psyco_float_neg_abs(PsycoObject *po, bool neg, vinfo_t *a);
/* the condition that 'a <py_op> b' holds (Py_LT...), as C's double
   compare: a NaN makes all but Py_NE false */
EXTERNFN condition_code_t
psyco_float_cmp(PsycoObject *po, int py_op, vinfo_t *a, vinfo_t *b);
/* the condition that run-time double 'vi' is +0.0 or -0.0 */
EXTERNFN condition_code_t
psyco_float_is_zero(PsycoObject *po, vinfo_t *vi);
/* (double) of a run-time word, rounded to nearest even as C's cast */
EXTERNFN vinfo_t *
psyco_float_from_word(PsycoObject *po, vinfo_t *w);
#if ALL_CHECKS
/* whether a call may be emitted now: no XMM value is live, or they are
   stored or saved */
EXTERNFN bool
psyco_xmm_may_call(PsycoObject *po);
#endif

/* make sure that the register 'reg' will not
   be returned by the next call to NEED_FREE_REG() */
#define DELAY_USE_OF(reg)                                   \
    do {                                                    \
        if (RegistersLoop[(int)po->last_used_reg] == (reg)) \
            po->last_used_reg = (reg);                      \
        if (po->last_used_reg == (reg))                     \
            NEXT_FREE_REG();                                \
    } while (0)

/*****************************************************************/
/***   vinfo_t restoring                                       ***/

#define RTVINFO_FROM_STACK_TO_REG(v, r)              \
    do {                                             \
        LOAD_REG_FROM_EBP_BASE(r, RUNTIME_STACK(v)); \
        REG_NUMBER(po, r) = (v);                     \
        SET_RUNTIME_REG_TO(v, r);                    \
    } while (0)

/* ensure that a run-time vinfo is in a general register */
#define RTVINFO_IN_REG(vi)                                           \
    do {                                                             \
        if (!RUNTIME_REG_IS_NONE(vi) && REG_IS_XMM(RUNTIME_REG(vi))) \
            code = psyco_xmm_to_reg(po, code, vi);                   \
        else if (RUNTIME_REG_IS_NONE(vi)) {                          \
            /* reload the vinfo from the stack */                    \
            reg_t _rg;                                               \
            long _stack;                                             \
            NEED_FREE_REG(_rg);                                      \
            REG_NUMBER(po, _rg) = (vi);                              \
            _stack = RUNTIME_STACK(vi);                              \
            SET_RUNTIME_REG_TO(vi, _rg);                             \
            LOAD_REG_FROM_EBP_BASE(_rg, _stack);                     \
        }                                                            \
    } while (0)

/* load register 'dst' from the given non-virtual source vinfo */
#define LOAD_REG_FROM(source, dst)                                   \
    do {                                                             \
        if (((source) & CompileTime) != 0) {                         \
            LOAD_REG_FROM_IMMED(dst, KSOURCE_SOURCE(source)->value); \
        }                                                            \
        else if (RSOURCE_REG(source) != dst) {                       \
            LOAD_REG_FROM_RT(source, dst);                           \
        }                                                            \
    } while (0)

/*****************************************************************/
/***   conditional jumps                                       ***/

#define JUMP_TO(addr)                                      \
    do {                                                   \
        le_trace_jump((code_t *)(code), (code_t *)(addr)); \
        *code++ = 0xE9; /* JMP rel32 */                    \
        *(dword_t *)(code) = REL32(code + 4, addr);        \
        code += 4;                                         \
    } while (0)

/* decodes what JUMP_TO wrote (checked by tests/encoder/check.py) */
#define IS_A_JUMP(code, targetaddr) \
    (code[0] == (code_t)0xE9 &&     \
     (targetaddr = code + 5 + *(dword_t *)(code + 1), 1))

#define IS_A_SINGLE_JUMP(code, codeend, targetaddr) \
    ((codeend) - (code) == SIZE_OF_FAR_JUMP && IS_A_JUMP(code, targetaddr))

#define FAR_COND_JUMP_TO(addr, condition)                          \
    do {                                                           \
        CAPTURE_CODE_LOCATION                                      \
        *code++ = 0x0F;                                            \
        *code++ = 0x80 | (code_t)(condition);                      \
        *(dword_t *)(code) = REL32(code + 4, addr);                \
        code += 4;                                                 \
        le_trace_jump_cond((code_t *)(location), (code_t *)(code), \
                           (code_t *)(addr));                      \
    } while (0)

#define CHANGE_JUMP_TO(addr)                                              \
    do {                                                                  \
        le_trace_jump_cond_update((code_t *)(code - 6), (code_t *)(code), \
                                  (code_t *)(addr));                      \
        *(dword_t *)(code - 4) = REL32(code, addr);                       \
    } while (0)

#define SIZE_OF_FAR_JUMP 5
#define MAXIMUM_SIZE_OF_FAR_JUMP SIZE_OF_FAR_JUMP
#define SIZE_OF_SHORT_CONDITIONAL_JUMP 2    /* Jcond rel8 */
#define RANGE_OF_SHORT_CONDITIONAL_JUMP 127 /* max. positive offset */

#define BEGIN_SHORT_JUMP(id)                \
    code[0] = 0xEB;                         \
    code += 2;                              \
    code_t *_short_jump_op_end_##id = code; \
    int _conditional_##id = 0;              \
    do {                                    \
    } while (0)
#define BEGIN_SHORT_COND_JUMP(id, condition) \
    code[0] = 0x70 | (condition);            \
    code += SIZE_OF_SHORT_CONDITIONAL_JUMP;  \
    code_t *_short_jump_op_end_##id = code;  \
    int _conditional_##id = 1;               \
    do {                                     \
    } while (0)
#define END_SHORT_JUMP(id)                                              \
    do {                                                                \
        long _jump_amount = (long)code - (long)_short_jump_op_end_##id; \
        extra_assert(-128 <= _jump_amount && _jump_amount < 128);       \
        if (_conditional_##id) {                                        \
            le_trace_jump_cond((code_t *)(_short_jump_op_end_##id - 2), \
                               (code_t *)(_short_jump_op_end_##id),     \
                               (code_t *)(code));                       \
        }                                                               \
        else {                                                          \
            le_trace_jump((code_t *)(_short_jump_op_end_##id - 2),      \
                          (code_t *)(code));                            \
        }                                                               \
        *(_short_jump_op_end_##id - 1) = (code_t)_jump_amount;          \
    } while (0)

/* a forward conditional jump, 8-bit or (if 'near') 32-bit */
#define BEGIN_COND_JUMP_OF(id, condition, near) \
    int _near_##id = (near);                    \
    if (_near_##id) {                           \
        code[0] = 0x0F;                         \
        code[1] = 0x80 | (condition);           \
        code += 6;                              \
    }                                           \
    else {                                      \
        code[0] = 0x70 | (condition);           \
        code += 2;                              \
    }                                           \
    code_t *_jump_op_end_##id = code;           \
    do {                                        \
    } while (0)
#define END_JUMP_OF(id)                                                  \
    do {                                                                 \
        long _jump_amount = (long)code - (long)_jump_op_end_##id;        \
        le_trace_jump_cond(_jump_op_end_##id - (_near_##id ? 6 : 2),     \
                           _jump_op_end_##id, (code_t *)(code));         \
        if (_near_##id)                                                  \
            *(dword_t *)(_jump_op_end_##id - 4) = (dword_t)_jump_amount; \
        else {                                                           \
            extra_assert(_jump_amount < 128);                            \
            *(_jump_op_end_##id - 1) = (code_t)_jump_amount;             \
        }                                                                \
    } while (0)

#define BEGIN_REVERSE_SHORT_JUMP(id) \
    code_t *_reverse_short_jump_target_##id = code;
#define END_REVERSE_SHORT_COND_JUMP(id, cond)                            \
    do {                                                                 \
        long _jump_amount =                                              \
            (long)_reverse_short_jump_target_##id - (long)code - 2;      \
        extra_assert(-128 <= _jump_amount && _jump_amount < 128);        \
        code[0] = 0x70 | (cond);                                         \
        code[1] = _jump_amount;                                          \
        le_trace_jump_cond((code_t *)(code), (code_t *)(code + 2),       \
                           (code_t *)(_reverse_short_jump_target_##id)); \
        code += 2;                                                       \
    } while (0)

#define SHORT_COND_JUMP_TO(addr, condition)                        \
    do {                                                           \
        long _ofs = (addr) - (code + 2);                           \
        extra_assert(-128 <= _ofs && _ofs < 128);                  \
        code[0] = 0x70 | (code_t)(condition);                      \
        code[1] = (code_t)_ofs;                                    \
        le_trace_jump_cond((code_t *)(code), (code_t *)(code + 2), \
                           (code_t *)(addr));                      \
        code += 2;                                                 \
    } while (0)

/* correct the stack pointer. With a condition live in the flags this must
   not touch them, so it is a LEA, as in Psyco */
#define STACK_CORRECTION(stack_correction)                           \
    do {                                                             \
        long _sc = (stack_correction); /* > 0 grows the stack */     \
        if (_sc != 0 && HAS_CCREG(po)) {                             \
            LEA_R_O(REG_X64_RSP, REG_X64_RSP, -_sc);                 \
        }                                                            \
        else if (_sc > 0) {                                          \
            if (FITS_IN_8BITS(_sc)) {                                \
                SUB_R_I8(REG_X64_RSP, _sc);                          \
            }                                                        \
            else {                                                   \
                SUB_R_I32(REG_X64_RSP, _sc);                         \
            }                                                        \
        }                                                            \
        else if (_sc < 0) { /* add rsp, n rather than sub rsp, -n */ \
            if (FITS_IN_8BITS(-_sc)) {                               \
                ADD_R_I8(REG_X64_RSP, -_sc);                         \
            }                                                        \
            else {                                                   \
                ADD_R_I32(REG_X64_RSP, -_sc);                        \
            }                                                        \
        }                                                            \
    } while (0)

/* convenience macros */
#define COPY_IN_REG(vi, rg)                                    \
    do {                                                       \
        NEED_FREE_REG(rg);                                     \
        if (((vi)->source & (TimeMask | RunTime_StackMask)) == \
            (RunTime | RunTime_StackNone)) {                   \
            reg_t _rg2 = rg;                                   \
            rg = RUNTIME_REG(vi);                              \
            extra_assert(rg != _rg2);                          \
            MOV_R_R(_rg2, rg);                                 \
            SET_RUNTIME_REG_TO(vi, _rg2);                      \
            REG_NUMBER(po, _rg2) = vi;                         \
            REG_NUMBER(po, rg) = NULL;                         \
        }                                                      \
        else {                                                 \
            LOAD_REG_FROM(vi->source, rg);                     \
        }                                                      \
    } while (0)

#define ALIGN_PAD_CODE_PTR()                                               \
    do {                                                                   \
        if ((((long)code) & 15) > 8) /* directive .p2align 4,,7 of 'as' */ \
            code = (code_t *)((((long)code) & ~15) + 16);                  \
    } while (0)

#define ALIGN_WITH_BYTE(byte)                                              \
    do {                                                                   \
        if ((((long)code) & 15) > 8) /* directive .p2align 4,,7 of 'as' */ \
            do {                                                           \
                *code++ = byte;                                            \
            } while ((((long)code) & 15) != 0);                            \
    } while (0)

#if ALL_CHECKS
#define ALIGN_NO_FILL() ALIGN_WITH_BYTE(0xCC) /* INT 03  (debugging) */
#else
#define ALIGN_NO_FILL() ALIGN_PAD_CODE_PTR()
#endif

#define ABOUT_TO_CALL_SUBFUNCTION(finfo) \
    SAVE_IMMED_TO_EBP_BASE((long)(finfo), INITIAL_STACK_DEPTH)
#define RETURNED_FROM_SUBFUNCTION() \
    SAVE_IMM8_TO_EBP_BASE(-1, INITIAL_STACK_DEPTH)
/* the frame's own slot while it clears its locals on the way out: what
   their finalisers run no longer counts it (frames.c:pvisitframes) */
#define FINFO_RETURNING (-3)
#define ABOUT_TO_RETURN() \
    SAVE_IMM8_TO_EBP_BASE(FINFO_RETURNING, INITIAL_STACK_DEPTH)

#endif /* _IENCODING_H */
