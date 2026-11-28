/*
 * v_mem_mov emits one sized load or store, [base + index*size + offset], where
 * the base, index and value may each be run-time or compile-time (constants
 * are folded into the displacement or the immediate). It is the code behind
 * every psyco_get_field/psyco_put_field on a run-time object.
 *
 * x86-64 traps handled here:
 *  - MOVSX needs REX.W to sign-extend to 64 bits;
 *  - the 0x66 operand-size prefix must come before REX, so it is written
 *    separately, after anything (R11 address loads) emitted ahead of it;
 *  - byte stores from registers 4-7 without REX would mean AH/CH/DH/BH, so
 *    they get one (REX_BYTE);
 *  - a RIP-relative displacement is relative to the end of the instruction,
 *    including a trailing immediate (ADDRESS_ENCODING's imm_len).
 * Tested by executing the emitted code: tests/encoder/mov_mem.sh.
 * Callers must compute a virtual base or index first (checked by
 * extra_assert): a virtual one would be treated as compile-time.
 */
#ifndef _V_MEM_MOV_H
#define _V_MEM_MOV_H

#include "../psyco.h"
#include "iencoding.h"
#define V_IS_RUNTIME(v) ((v) != NULL && is_runtime(((vinfo_t *)(v))->source))
#define V_GET_REG(v) \
    (V_IS_RUNTIME(v) ? getreg(((vinfo_t *)(v))->source) : REG_NONE)
#define V_MEM_MOV_SCALE_BYTE 0
#define V_MEM_MOV_SCALE_WORD 1
#define V_MEM_MOV_SCALE_DWORD 2
#define V_MEM_MOV_SCALE_QWORD 3
#define V_MEM_MOV_SCALE_POINTER V_MEM_MOV_SCALE_QWORD
#define V_MEM_MOV_SCALE_TO_BYTES(scale) (1 << (scale))
#define V_CT_GET_VALUE(v) CompileTime_Get(((vinfo_t *)(v))->source)->value

static vinfo_t *
v_mem_mov(PsycoObject *po, vinfo_t *v, vinfo_t *base, vinfo_t *index,
          long offset, unsigned char scale, bool signed_)
{
    reg_t v_reg, base_reg, index_reg;
    reg_t r;
    long value = 0;
    long offset_or_address = (offset);
    unsigned char op_len = 0, b1 = 0, b2 = 0, b3 = 0;
    int rex_w = false; /* or REX_BYTE */
    bool read = false;

    /* a virtual base or index would be read as a compile-time value
       (V_CT_GET_VALUE); callers must compute it first */
    extra_assert(base == NULL || !is_virtualtime(base->source));
    extra_assert(index == NULL || !is_virtualtime(index->source));
    if (v != NULL) {
        /* may emit code */
        if (!compute_vinfo(v, po)) {
            return NULL;
        }
    }

    BEGIN_CODE
    if (v == NULL) {
        NEED_FREE_REG_COND(
            v_reg, (v_reg != V_GET_REG(base) && v_reg != V_GET_REG(index)));
        /* an unsigned field narrower than 8 bytes is known non-negative */
        v = vinfo_new(RunTime_New(
            v_reg, false, !(signed_) && scale < V_MEM_MOV_SCALE_QWORD));
        REG_NUMBER(po, v_reg) = v;
        read = true;
    }
    else {
        if (V_IS_RUNTIME(v)) {
            v_reg = V_GET_REG(v);
            if (v_reg != REG_NONE && REG_IS_XMM(v_reg)) {
                /* a double: stored from a general register */
                code = psyco_xmm_to_reg(po, code, v);
                v_reg = V_GET_REG(v);
            }
            if (v_reg == REG_NONE) {
                NEED_FREE_REG_COND(v_reg, (v_reg != V_GET_REG(base) &&
                                           v_reg != V_GET_REG(index)));
                RTVINFO_FROM_STACK_TO_REG(v, v_reg);
            }
        }
        else {
            v_reg = REG_NONE;
            value = V_CT_GET_VALUE(v);
            /* a store only keeps the low bytes, so narrow stores can always
               use an immediate (truncated here). A qword store sign-extends
               an imm32, so other values must go through a register. */
            switch (scale) {
                case V_MEM_MOV_SCALE_BYTE:
                    value = (signed char)value;
                    break;
                case V_MEM_MOV_SCALE_WORD:
                    value = (short)value;
                    break;
                case V_MEM_MOV_SCALE_DWORD:
                    value = (int)value;
                    break;
                default:
                    if (!FITS_IN_32BITS(value)) {
                        v_reg = REG_TRANSIENT_1;
                        MOV_R_I(v_reg, value);
                    }
            }
        }
    }
    if (V_IS_RUNTIME(base)) {
        base_reg = V_GET_REG(base);
        if (base_reg == REG_NONE) {
            NEED_FREE_REG_COND(base_reg, (base_reg != V_GET_REG(index) &&
                                          base_reg != V_GET_REG(v)));
            RTVINFO_FROM_STACK_TO_REG(base, base_reg);
        }
    }
    else {
        base_reg = REG_NONE;
        offset_or_address += V_CT_GET_VALUE(base);
    }
    if (index == NULL) {
        index_reg = REG_NONE;
    }
    else {
        if (V_IS_RUNTIME(index)) {
            index_reg = V_GET_REG(index);
            if (index_reg == REG_NONE) {
                NEED_FREE_REG_COND(index_reg, (index_reg != V_GET_REG(base) &&
                                               index_reg != V_GET_REG(v)));
                RTVINFO_FROM_STACK_TO_REG(index, index_reg);
            }
        }
        else {
            long i = V_CT_GET_VALUE(index);
            if (base_reg != REG_NONE && (i < -(1L << 26) || i >= (1L << 26))) {
                /* scaled, it may not fit the displacement (in bounds only
                   for an array of 64 MB or more): the index goes in R11,
                   scaled by the SIB byte. With a constant base it folds
                   into the absolute address, which goes in R11 below. */
                index_reg = REG_TRANSIENT_2;
                MOV_R_I(index_reg, i);
            }
            else {
                index_reg = REG_NONE;
                offset_or_address += (long)((unsigned long)i << scale);
            }
        }
    }
    if (read) {
        /* MOVSX needs REX.W to sign-extend to 64 bits; MOVZX r32 already
           zero-extends to 64 */
        if (scale == V_MEM_MOV_SCALE_BYTE) {
            rex_w = (signed_);
            op_len = 2;
            b1 = 0x0F;
            b2 = (signed_) ? 0xBE : 0xB6;
        }
        else if (scale == V_MEM_MOV_SCALE_WORD) {
            rex_w = (signed_);
            op_len = 2;
            b1 = 0x0F;
            b2 = (signed_) ? 0xBF : 0xB7;
        }
        else if (scale == V_MEM_MOV_SCALE_DWORD) {
            if (signed_) {
                rex_w = true;
                op_len = 1;
                b1 = 0x63;
            }
            else {
                op_len = 1;
                b1 = 0x8B;
            }
        }
        else {
            rex_w = true;
            op_len = 1;
            b1 = 0x8B;
        }
    }
    else {
        if (v_reg != REG_NONE) {
            /* from register */
            if (scale == V_MEM_MOV_SCALE_BYTE) {
                rex_w = REX_BYTE;
                op_len = 1;
                b1 = 0x88;
            }
            else if (scale == V_MEM_MOV_SCALE_WORD) {
                op_len = 1; /* 0x66 prefix written below, before REX */
                b1 = 0x89;
            }
            else if (scale == V_MEM_MOV_SCALE_DWORD) {
                op_len = 1;
                b1 = 0x89;
            }
            else {
                rex_w = true;
                op_len = 1;
                b1 = 0x89;
            }
        }
        else {
            /* from immediate */
            if (scale == V_MEM_MOV_SCALE_BYTE) {
                op_len = 1;
                b1 = 0xC6;
            }
            else if (scale == V_MEM_MOV_SCALE_WORD) {
                op_len = 1; /* 0x66 prefix written below, before REX */
                b1 = 0xC7;
            }
            else if (scale == V_MEM_MOV_SCALE_DWORD) {
                op_len = 1;
                b1 = 0xC7;
            }
            else {
                rex_w = true;
                op_len = 1;
                b1 = 0xC7;
            }
        }
    }
    if (base_reg == REG_NONE &&
        (index_reg != REG_NONE ||
         !FITS_IN_32BITS(offset_or_address - (long)code - 64) ||
         !FITS_IN_32BITS(offset_or_address - (long)code + 64))) {
        /* absolute address not reachable RIP-relative, or combined with an
           index: put it in R11 now, before any prefix of the access itself */
        MOV_R_IADDR(REG_TRANSIENT_2, offset_or_address);
        base_reg = REG_TRANSIENT_2;
        offset_or_address = 0;
    }
    if (!read && scale == V_MEM_MOV_SCALE_WORD) {
        WRITE_1(0x66); /* operand-size prefix: must precede REX */
    }
    r = (v_reg != REG_NONE ? v_reg : 0);
    if (base_reg == REG_NONE) {
        /* RIP-relative; the target is relative to the end of the
           instruction, after any immediate */
        int imm_len = 0;
        if (!read && v_reg == REG_NONE)
            imm_len = scale == V_MEM_MOV_SCALE_BYTE   ? 1
                      : scale == V_MEM_MOV_SCALE_WORD ? 2
                                                      : 4;
        ADDRESS_ENCODING(rex_w, op_len, b1, b2, b3, 0, r, offset_or_address,
                         imm_len);
    }
    else if (index_reg == REG_NONE) {
        OFFSET_ENCODING(false, rex_w, op_len, b1, b2, b3, 0, 0, r, base_reg,
                        offset_or_address);
    }
    else {
        SIB_ENCODING(false, rex_w, op_len, b1, b2, b3, 0, 0, r, base_reg,
                     offset_or_address, index_reg,
                     V_MEM_MOV_SCALE_TO_BYTES(scale));
    }
    if ((!read) && v_reg == REG_NONE) {
        switch (scale) {
            case V_MEM_MOV_SCALE_BYTE:
                WRITE_8BIT(value);
                break;
            case V_MEM_MOV_SCALE_WORD:
                WRITE_16BIT(value);
                break;
            default: /* both dword and qwords in 32bit range */
                WRITE_32BIT(value);
        }
    }
    END_CODE
    return v;
}

#endif /* _V_MEM_MOV_H */
