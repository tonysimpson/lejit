/*
 * Executes the code v_mem_mov emits and checks what it does.
 *
 * For each case: build a tiny function (save callee-saved registers, load
 * the base/index/value registers, the v_mem_mov code, move a loaded value to
 * RAX, restore, ret), run it against a patterned buffer, then check the value
 * loaded or the bytes stored, and that every other byte of the buffer is
 * untouched. Built and run by tests/encoder/mov_mem.sh against the
 * objects of an in-place LeJIT build.
 */
#include <Python.h>
#include "psyco.h"
#include "vcompiler.h"
#include "codegen.h"
#include <iencoding.h>
#include "v_mem_mov.h"
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define MEM 96
#define BASE_OFS 32
static unsigned char *mem, before[MEM]; /* mem lives next to the code */
static int far_data;                    /* 1: data out of RIP-relative range */
static code_t *exec_buf;
static int failures, cases;
static const char *REGN[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                               "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                               "r12", "r13", "r14", "r15"};
/* registers a test may assign to base/index/value: not RSP (stack), not
   R10/R11 (v_mem_mov's scratch, never allocated) */
static const int USABLE[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 12, 13, 14, 15};
#define NUSABLE ((int)(sizeof(USABLE) / sizeof(USABLE[0])))
/* smaller sets for index and value registers: the encoding edge cases
   (RBP/R13 need a displacement, RSI/RDI need REX for byte access, R8+ need
   REX bits) plus one plain register */
static const int SOME[] = {1, 5, 6, 9, 12, 13};
#define NSOME ((int)(sizeof(SOME) / sizeof(SOME[0])))

enum { LOAD, STORE_REG, STORE_IMM };
static const char *OPN[] = {"load", "store-reg", "store-imm"};

static void
fill(void)
{
    int i;
    for (i = 0; i < MEM; i++) mem[i] = (unsigned char)(0xA0 + i);
}

static long
sext(unsigned long v, int bytes)
{
    int sh = 64 - 8 * bytes;
    return (long)(v << sh) >> sh;
}
static unsigned long
zext(unsigned long v, int bytes)
{
    return bytes == 8 ? v : v & ((1UL << (8 * bytes)) - 1);
}

/* base_reg < 0: compile-time base; index_kind: 0 none, 1 run-time, 2
 * compile-time */
static void
run_case(int op, int scale, int signed_, int base_reg, int index_kind,
         int index_reg, long offset, int value_reg, long value)
{
    PsycoObject *po = PsycoObject_New(0);
    vinfo_t *vbase, *vindex = NULL, *vv = NULL, *res;
    int bytes = 1 << scale;
    long idx = 1;
    unsigned char *ea;
    code_t *code;
    long got;
    int i, bad = 0;
    char what[200];

    INIT_PROCESSOR_PSYCOOBJECT(po);
    cases++;
    fill();
    ea = mem + BASE_OFS + offset + (index_kind ? (idx << scale) : 0);
    if (op == LOAD) {
        /* put a value with the top bit set at the effective address */
        unsigned long pat = 0x8000000000000081UL | (0x55UL << 8);
        memcpy(ea, &pat, bytes);
        if (bytes > 1)
            ea[bytes - 1] = 0x80 | ea[bytes - 1];
    }
    memcpy(before, mem, MEM);

    code = exec_buf;
    /* prologue: save callee-saved registers */
    PUSH_R(REG_X64_RBX);
    PUSH_R(REG_X64_RBP);
    PUSH_R(REG_X64_R12);
    PUSH_R(REG_X64_R13);
    PUSH_R(REG_X64_R14);
    PUSH_R(REG_X64_R15);
    if (base_reg >= 0) {
        MOV_R_I(base_reg, (long)(mem + BASE_OFS));
        vbase = vinfo_new(RunTime_New(base_reg, false, false));
        REG_NUMBER(po, base_reg) = vbase;
    }
    else {
        vbase = vinfo_new(CompileTime_New((long)(mem + BASE_OFS)));
    }
    if (index_kind == 1) {
        MOV_R_I(index_reg, idx);
        vindex = vinfo_new(RunTime_New(index_reg, false, true));
        REG_NUMBER(po, index_reg) = vindex;
    }
    else if (index_kind == 2) {
        vindex = vinfo_new(CompileTime_New(idx));
    }
    if (op == STORE_REG) {
        MOV_R_I(value_reg, value);
        vv = vinfo_new(RunTime_New(value_reg, false, false));
        REG_NUMBER(po, value_reg) = vv;
    }
    else if (op == STORE_IMM) {
        vv = vinfo_new(CompileTime_New(value));
    }
    po->code = code;
    po->codelimit = exec_buf + 4000;
    res = v_mem_mov(po, vv, vbase, vindex, offset, scale, signed_);
    code = po->code;
    if (op == LOAD) {
        MOV_R_R(REG_X64_RAX, getreg(res->source));
    }
    else {
        MOV_R_I(REG_X64_RAX, 0);
    }
    POP_R(REG_X64_R15);
    POP_R(REG_X64_R14);
    POP_R(REG_X64_R13);
    POP_R(REG_X64_R12);
    POP_R(REG_X64_RBP);
    POP_R(REG_X64_RBX);
    RET();

    got = ((long (*)(void))exec_buf)();

    snprintf(what, sizeof what, "%s%s %d-byte %s base=%s index=%s off=%ld",
             far_data ? "far " : "", OPN[op], bytes,
             signed_ ? "signed" : "unsigned",
             base_reg >= 0 ? REGN[base_reg] : "ct",
             index_kind == 0   ? "none"
             : index_kind == 2 ? "ct"
                               : REGN[index_reg],
             offset);
    if (op == LOAD) {
        unsigned long raw = 0;
        long want;
        memcpy(&raw, ea, bytes);
        want = signed_ ? sext(raw, bytes) : (long)zext(raw, bytes);
        if (got != want) {
            printf("FAIL %s: loaded %#lx, want %#lx\n", what, got, want);
            bad = 1;
        }
    }
    else {
        memcpy(before + (ea - mem), &value, bytes);
        for (i = 0; i < MEM; i++)
            if (mem[i] != before[i]) {
                printf("FAIL %s value=%s%#lx: byte %d is %#x, want %#x\n",
                       what, op == STORE_REG ? REGN[value_reg] : "imm ", value,
                       i - BASE_OFS, mem[i], before[i]);
                bad = 1;
                break;
            }
    }
    failures += bad;
    PyMem_FREE(po);
}

int
main(void)
{
    int op, scale, sg, b, ik, ir, vr;
    long offs[] = {0, 8, -8};
    long imms[] = {5,           -1,           0x7f,       0x80,
                   0x1234,      -0x1234,      0x12345678, -0x12345678,
                   0x80000000L, 0x123456789aL};
    int o, k;
    setvbuf(stdout, NULL, _IOLBF, 0);
    Py_Initialize();
    exec_buf = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    /* pass 0: data near the code (RIP-relative addressing);
       pass 1: data far away (address loaded into R11) */
    for (far_data = 0; far_data < 2; far_data++) {
        static unsigned char far_mem[MEM];
        mem = far_data ? far_mem : exec_buf + (1 << 15);
        for (scale = 0; scale < 4; scale++)
            for (sg = 0; sg < 2; sg++)
                for (b = -1; b < NUSABLE; b++)
                    for (ik = 0; ik < 3; ik++)
                        for (o = 0; o < 3; o++) {
                            int base = b < 0 ? -1 : USABLE[b];
                            int iregs = (ik == 1) ? NSOME : 1;
                            for (ir = 0; ir < iregs; ir++) {
                                int index = (ik == 1) ? SOME[ir] : 0;
                                if (ik == 1 && index == base)
                                    continue;
                                run_case(LOAD, scale, sg, base, ik, index,
                                         offs[o], 0, 0);
                                for (vr = 0; vr < NSOME + 2; vr++) {
                                    int value = vr < NSOME    ? SOME[vr]
                                                : vr == NSOME ? 7
                                                              : 3;
                                    if (value == base ||
                                        (ik == 1 && value == index))
                                        continue;
                                    run_case(STORE_REG, scale, sg, base, ik,
                                             index, offs[o], value,
                                             0x0807060504030201L);
                                }
                                for (k = 0;
                                     k < (int)(sizeof imms / sizeof imms[0]);
                                     k++)
                                    run_case(STORE_IMM, scale, sg, base, ik,
                                             index, offs[o], 0, imms[k]);
                            }
                        }
    }
    printf("%d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
