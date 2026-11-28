/*
 * Executes the code that loads and uses 64-bit constants and checks what
 * it does: MOV_R_I (an address in reach, RIP-relative; another value, from
 * its slot in the constants, dispatcher.c:psyco_const_slot; or the movabs
 * of MOV_R_IABS), MOV_R_IADDR (the same with no slot) and the group
 * instructions with a 64-bit immediate (COMMON_INSTR_IMMED,
 * COMMON_INSTR_IMMED_FROM_RT), into every register, in executable memory
 * (psyco_code_open) and in memory that isn't (no slot there); and a promotion
 * site's entry with its own slot (x64/idispatcher.h), repointed and read back.
 * Built and run by tests/encoder/mov_mem.sh against the objects of an in-place
 * LeJIT build.
 */
#include <Python.h>
#include "psyco.h"
#include "vcompiler.h"
#include "codegen.h"
#include "codemanager.h"
#include <iencoding.h>
#include <idispatcher.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int failures, cases;
static const char *REGN[16] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp",
                               "rsi", "rdi", "r8",  "r9",  "r10", "r11",
                               "r12", "r13", "r14", "r15"};
static const char *INSTRN[8] = {"add", "or",  "adc", "sbb",
                                "and", "sub", "xor", "cmp"};
#define FLAGS_MASK 0x8D5 /* OF SF ZF AF PF CF */

/* a function at 'at' run with 'reg' set to 'x', which emits 'what' and
   returns RAX: the save and restore of callee-saved registers around it */
#define PROLOGUE()       \
    PUSH_R(REG_X64_RBX); \
    PUSH_R(REG_X64_RBP); \
    PUSH_R(REG_X64_R12); \
    PUSH_R(REG_X64_R13); \
    PUSH_R(REG_X64_R14); \
    PUSH_R(REG_X64_R15)
#define EPILOGUE()      \
    POP_R(REG_X64_R15); \
    POP_R(REG_X64_R14); \
    POP_R(REG_X64_R13); \
    POP_R(REG_X64_R12); \
    POP_R(REG_X64_RBP); \
    POP_R(REG_X64_RBX); \
    RET()

/* the form MOV_R_I took at 'p': 'l' lea, 'm' a load from a slot, 'a'
   movabs, 'i' a 32-bit immediate */
static int
form_of(code_t *p)
{
    int rex = (p[0] & 0xF0) == 0x40;
    code_t op = p[rex];
    if (op == 0x8D)
        return 'l';
    if (op == 0x8B)
        return 'm';
    if (rex && (p[0] & 8) && (op & 0xF8) == 0xB8)
        return 'a';
    return 'i';
}

static void
check_mov(code_t *at, int reg, long value, int want_form, const char *where,
          bool addr)
{
    code_t *code = at, *mov;
    long got;
    int form;
    cases++;
    PROLOGUE();
    mov = code;
    if (addr)
        MOV_R_IADDR(reg, value);
    else
        MOV_R_I(reg, value);
    form = form_of(mov);
    MOV_R_R(REG_X64_RAX, reg);
    EPILOGUE();
    got = ((long (*)(void))at)();
    if (got != value || (want_form && form != want_form)) {
        printf("FAIL %s(%s, %#lx) %s: got %#lx, form '%c' want '%c'\n",
               addr ? "MOV_R_IADDR" : "MOV_R_I", REGN[reg], value, where, got,
               form, want_form);
        failures++;
    }
}

/* 'instr' of 'x' in 'reg' with 'value': RAX the result, or the flags for
   CMP; 'from_rt' through COMMON_INSTR_IMMED_FROM_RT */
static PsycoObject *po; /* for the stack depth the macros read */

static long
run_instr(code_t *at, int instr, int reg, long x, long value, bool from_rt)
{
    code_t *code = at;
    PROLOGUE();
    MOV_R_IABS(reg, x);
    if (from_rt) {
        Source src = RunTime_New(reg, false, false);
        COMMON_INSTR_IMMED_FROM_RT(instr, src, value);
    }
    else {
        COMMON_INSTR_IMMED(instr, reg, value);
    }
    if (instr == CMP) {
        WRITE_1(0x9C); /* pushfq */
        POP_R(REG_X64_RAX);
    }
    else {
        MOV_R_R(REG_X64_RAX, reg);
    }
    EPILOGUE();
    return ((long (*)(void))at)();
}

static long
expected(int instr, long x, long value)
{
    unsigned long f;
    switch (instr) {
        case ADD:
            return (long)((unsigned long)x + (unsigned long)value);
        case OR:
            return x | value;
        case AND:
            return x & value;
        case SUB:
            return (long)((unsigned long)x - (unsigned long)value);
        case XOR:
            return x ^ value;
        default:
            __asm__("cmp %2, %1; pushfq; pop %0"
                    : "=r"(f)
                    : "r"(x), "r"(value)
                    : "cc");
            return (long)f;
    }
}

static void
check_instr(code_t *at, int instr, int reg, long value, const char *where)
{
    static const long xs[] = {0, 1, -1, 0x123456789abcdef0L};
    int k, rt;
    for (rt = 0; rt < 2; rt++)
        for (k = 0; k < 4; k++) {
            long x = k < 3 ? xs[k] : value, got, want;
            cases++;
            got = run_instr(at, instr, reg, x, value, rt);
            want = expected(instr, x, value);
            if (instr == CMP) {
                got &= FLAGS_MASK;
                want &= FLAGS_MASK;
            }
            if (got != want) {
                printf(
                    "FAIL %s(%s %s, %#lx) %s: x=%#lx got %#lx want %#lx\n",
                    rt ? "COMMON_INSTR_IMMED_FROM_RT" : "COMMON_INSTR_IMMED",
                    INSTRN[instr], REGN[reg], value, where, x, got, want);
                failures++;
            }
        }
}

/* a promotion entry comparing 'reg' with its slot: 1 if 'x' is the value
   it was repointed to, else 0 */
static void
check_promotion(code_t *at, int reg, long value)
{
    static const long xs[] = {0, 1, -1, 0x123456789abcdef0L};
    int k;
    for (k = 0; k < 5; k++) {
        long x = k < 4 ? xs[k] : value, v, got;
        code_t *code = at, *entry, *slot, *miss, *hit, *t;
        cases++;
        PROLOGUE();
        MOV_R_IABS(reg, x);
        entry = code;
        slot = psyco_promotion_slot(code);
        CMP_R_RIP(reg, (long)slot - ((long)code + 7));
        JMP_CC_UI32(CC_E, code);
        miss = code;
        MOV_R_IABS(REG_X64_RAX, 0);
        EPILOGUE();
        hit = code;
        ipromotion_update_entry(entry, value, hit);
        MOV_R_IABS(REG_X64_RAX, 1);
        EPILOGUE();
        ipromotion_read_entry(entry, &v, &t);
        got = ((long (*)(void))at)();
        if (ENTRY_SIZE(entry) != PROMOTION_SLOT_ENTRY_SIZE ||
            miss - entry != PROMOTION_SLOT_ENTRY_SIZE || v != value ||
            t != hit || got != (x == value)) {
            printf(
                "FAIL promotion entry on %s for %#lx: x=%#lx got %ld, "
                "read %#lx\n",
                REGN[reg], value, x, got, v);
            failures++;
        }
    }
}

int
main(void)
{
    static const int instrs[] = {ADD, OR, AND, SUB, XOR, CMP};
    code_t *limit, *exec, *plain;
    long values[8];
    int forms[8], reg, v, n, i;
    setvbuf(stdout, NULL, _IOLBF, 0);
    Py_Initialize();
    po = PsycoObject_New(0);
    INIT_PROCESSOR_PSYCOOBJECT(po);
    exec = psyco_code_open(&limit);
    plain = mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    /* in reach, near the edge of the margin, beyond it, far, and small */
    n = 0;
    values[n] = (long)exec + 0x10000000L, forms[n++] = 'l';
    values[n] = (long)exec - 0x7e000000L, forms[n++] = 'l';
    values[n] = (long)exec + 0x7f800000L, forms[n++] = 'm';
    values[n] = (long)exec + 0x123456789aL, forms[n++] = 'm';
    values[n] = 0x123456789abcdef0L, forms[n++] = 'm';
    values[n] = -0x123456789abcL, forms[n++] = 'm';
    values[n] = 0x87654321L, forms[n++] = 'i';
    values[n] = -5, forms[n++] = 'i';
    for (reg = 0; reg < 16; reg++) {
        if (reg == REG_X64_RSP)
            continue;
        for (v = 0; v < n; v++) {
            /* outside code the same distances, and no slot: a movabs */
            long shift = v < 4 ? (long)plain - (long)exec : 0;
            check_mov(exec, reg, values[v], forms[v], "in code", false);
            check_mov(plain, reg, values[v] + shift,
                      forms[v] == 'm' ? 'a' : forms[v], "outside code", false);
            /* never from a slot */
            check_mov(exec, reg, values[v], forms[v] == 'm' ? 'a' : forms[v],
                      "in code", true);
        }
        for (v = 0; v < n; v++) check_promotion(exec, reg, values[v]);
        if (reg == REG_TRANSIENT_1)
            continue; /* the fallback's scratch */
        for (i = 0; i < 6; i++)
            for (v = 0; v < 6; v++) {
                check_instr(exec, instrs[i], reg, values[v], "in code");
                check_instr(plain, instrs[i], reg, values[v], "outside code");
            }
    }
    {
        /* a value used twice shares its slot */
        code_t *code = exec, *first;
        long d1, d2;
        cases++;
        MOV_R_I(REG_X64_RAX, values[4]);
        first = code;
        MOV_R_I(REG_X64_RCX, values[4]);
        d1 = (long)first + *(int *)(first - 4);
        d2 = (long)code + *(int *)(code - 4);
        if (d1 != d2) {
            printf("FAIL: two slots for %#lx\n", values[4]);
            failures++;
        }
    }
    printf("%d cases, %d failures\n", cases, failures);
    return failures ? 1 : 0;
}
