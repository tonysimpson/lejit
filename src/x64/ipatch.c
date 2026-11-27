/*
 * Replacing a C function at its entry (ipatch.h).
 *
 * The entry's first five bytes become 'jmp rel32' to a stub mapped within
 * 2 GB of it, which goes on to the replacement through le_patch_call. Five
 * bytes, not a 14-byte absolute jump, because compilers put a function's cold
 * part elsewhere and jump back into it from there, as near as 9 bytes from the
 * entry (PyFrame_GetBack, PyThreadState_GetFrame); and some functions are
 * shorter than 14 bytes. Nothing jumps into an entry's first five bytes.
 *
 * The instructions those five bytes held are copied into the stub page in
 * front of a jump back to the first instruction after them, so the copy
 * runs the original. Only instructions found in function entries are known
 * here: each is moved as it is, with a RIP-relative displacement or rel32
 * target refitted to its new address (the page is near), and a short jump
 * widened to rel32. A call is moved as a push of the original's own return
 * address and a jump, so that the stub is never a return address. Anything
 * else, an indirect call, and a branch into the bytes moved refuse the
 * patch.
 *
 * The entry's page is made writable for the write, and executable
 * throughout, since other threads may be running other code in it. The
 * five bytes go in as one 8-byte store where they share an aligned word.
 */
#include "ipatch.h"
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define ENTRY_JUMP 5 /* jmp rel32 */
/* the moved code's largest size: five bytes of instructions of at most 15
   bytes each, a short jump among them made a 6-byte one, the jump back */
#define MOVED_MAX (2 * 15 + 6 + 5)
/* a stub: push [rip+6] (the replacement's address), jmp [rip+8]
   (le_patch_call's), those two addresses, or jmp [rip] and the
   replacement's address (le_patch_entry_jump), then the original's moved first
   instructions after an endbr64, so that it can be called indirectly where
   indirect branch tracking is on */
#define STUB_JUMP 28
#define STUB_SIZE ((STUB_JUMP + 4 + MOVED_MAX + 15) & ~15)

/* Calls the replacement whose address the stub pushed, keeping every
   register but rax. Callers inside a library built with link-time
   optimisation may know which call-clobbered registers the original leaves
   alone and keep values in them across the call, where the replacement, an
   ordinary C function, would change them. It is here rather than in the
   stub so that unwinding finds it in a loaded object, with its frame
   described: the stub is never a return address. */
extern const char le_patch_call[] __attribute__((visibility("hidden")));
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl le_patch_call\n"
    ".hidden le_patch_call\n"
    ".type le_patch_call, @function\n"
    "le_patch_call:\n"
    ".cfi_startproc\n"
    ".cfi_def_cfa %rsp, 16\n" /* the replacement's address on top */
    "endbr64\n"
    "push %rbp\n"
    ".cfi_def_cfa_offset 24\n"
    ".cfi_offset %rbp, -24\n"
    "mov %rsp, %rbp\n"
    ".cfi_def_cfa_register %rbp\n"
    "push %rdi\n push %rsi\n push %rdx\n push %rcx\n"
    "push %r8\n push %r9\n push %r10\n push %r11\n"
    "and $-16, %rsp\n"
    "sub $256, %rsp\n"
    "movdqu %xmm0, 0(%rsp)\n movdqu %xmm1, 16(%rsp)\n"
    "movdqu %xmm2, 32(%rsp)\n movdqu %xmm3, 48(%rsp)\n"
    "movdqu %xmm4, 64(%rsp)\n movdqu %xmm5, 80(%rsp)\n"
    "movdqu %xmm6, 96(%rsp)\n movdqu %xmm7, 112(%rsp)\n"
    "movdqu %xmm8, 128(%rsp)\n movdqu %xmm9, 144(%rsp)\n"
    "movdqu %xmm10, 160(%rsp)\n movdqu %xmm11, 176(%rsp)\n"
    "movdqu %xmm12, 192(%rsp)\n movdqu %xmm13, 208(%rsp)\n"
    "movdqu %xmm14, 224(%rsp)\n movdqu %xmm15, 240(%rsp)\n"
    "call *8(%rbp)\n"
    "movdqu 0(%rsp), %xmm0\n movdqu 16(%rsp), %xmm1\n"
    "movdqu 32(%rsp), %xmm2\n movdqu 48(%rsp), %xmm3\n"
    "movdqu 64(%rsp), %xmm4\n movdqu 80(%rsp), %xmm5\n"
    "movdqu 96(%rsp), %xmm6\n movdqu 112(%rsp), %xmm7\n"
    "movdqu 128(%rsp), %xmm8\n movdqu 144(%rsp), %xmm9\n"
    "movdqu 160(%rsp), %xmm10\n movdqu 176(%rsp), %xmm11\n"
    "movdqu 192(%rsp), %xmm12\n movdqu 208(%rsp), %xmm13\n"
    "movdqu 224(%rsp), %xmm14\n movdqu 240(%rsp), %xmm15\n"
    "lea -64(%rbp), %rsp\n"
    "pop %r11\n pop %r10\n pop %r9\n pop %r8\n"
    "pop %rcx\n pop %rdx\n pop %rsi\n pop %rdi\n"
    "pop %rbp\n"
    ".cfi_def_cfa %rsp, 16\n"
    ".cfi_restore %rbp\n"
    "lea 8(%rsp), %rsp\n"
    ".cfi_def_cfa_offset 8\n"
    "ret\n"
    ".cfi_endproc\n"
    ".size le_patch_call, .-le_patch_call\n");

typedef struct {
    int len;
    int rel32_at;   /* where a rel32 or RIP-relative disp32 is, or -1 */
    int branch;     /* rel32_at is a branch's target, not data's */
    int short_jump; /* a jmp or jcc rel8, its opcode the last byte but one */
    int ends;       /* the next instruction isn't run after it */
    int call;
} insn_t;

/* the instruction at 'p', if one this file can move */
static bool
decode(const unsigned char *p, insn_t *in)
{
    int i = 0, z = 4, rexw = 0, modrm = 0, imm = 0;
    unsigned char op;
    in->rel32_at = -1;
    in->branch = in->short_jump = in->ends = in->call = 0;
    for (;; i++) {
        if (p[i] == 0x66)
            z = 2;
        else if (p[i] != 0xf2 && p[i] != 0xf3 && p[i] != 0x64 && p[i] != 0x65)
            break;
        if (i == 4)
            return false;
    }
    if ((p[i] & 0xf0) == 0x40)
        rexw = (p[i++] & 8) != 0;
    op = p[i++];
    if (op == 0x0f) {
        unsigned char op2 = p[i++];
        if (op2 >= 0x80 && op2 <= 0x8f) { /* jcc rel32 */
            in->rel32_at = i;
            in->branch = 1;
            i += 4;
        }
        else if (op2 == 0x1e || op2 == 0x1f || (op2 >= 0x40 && op2 <= 0x4f) ||
                 (op2 >= 0x90 && op2 <= 0x9f) ||
                 (op2 >= 0x10 && op2 <= 0x17) ||
                 (op2 >= 0x28 && op2 <= 0x2f) || op2 == 0x57 || op2 == 0x6e ||
                 op2 == 0x6f || op2 == 0x7e || op2 == 0x7f || op2 == 0xaf ||
                 op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf ||
                 op2 == 0xd6 || op2 == 0xef)
            modrm = 1; /* endbr64, nop, cmov, set, SSE moves, imul, movzx */
        else if (op2 == 0xba) { /* bt, bts, btr, btc r/m, imm8 */
            modrm = 1;
            imm = 1;
        }
        else
            return false;
    }
    else if ((op & 0xf0) == 0x50 || op == 0x90) /* push, pop, nop */
        ;
    else if (op == 0xc3)
        in->ends = 1;
    else if (op == 0xe8 || op == 0xe9) { /* call, jmp rel32 */
        in->rel32_at = i;
        in->branch = 1;
        in->ends = op == 0xe9;
        in->call = op == 0xe8;
        i += 4;
    }
    else if (op == 0xeb || (op >= 0x70 && op <= 0x7f)) {
        if (i != 1)
            return false; /* prefixed */
        in->short_jump = 1;
        in->ends = op == 0xeb;
        i += 1;
    }
    else if ((op & 0xf8) == 0xb8) /* mov reg, imm */
        imm = rexw ? 8 : z;
    else if (op < 0x40 && (op & 7) <= 3) /* add ... cmp, r/m forms */
        modrm = 1;
    else if (op < 0x40 && (op & 7) == 4)
        imm = 1;
    else if (op < 0x40 && (op & 7) == 5)
        imm = z;
    else if (op == 0x63 || (op >= 0x84 && op <= 0x8b) || op == 0x8d ||
             op == 0xd1 || op == 0xd3)
        modrm = 1; /* movsxd, test, xchg, mov, lea, shifts */
    else if (op == 0x80 || op == 0x83 || op == 0xc0 || op == 0xc1 ||
             op == 0xc6 || op == 0x6b) {
        modrm = 1;
        imm = 1;
    }
    else if (op == 0x81 || op == 0xc7 || op == 0x69) {
        modrm = 1;
        imm = z;
    }
    else if (op == 0xa8)
        imm = 1;
    else if (op == 0xa9)
        imm = z;
    else if (op == 0xf6 || op == 0xf7) { /* test r/m, imm, or not/neg... */
        modrm = 1;
        if (((p[i] >> 3) & 7) <= 1)
            imm = op == 0xf6 ? 1 : z;
    }
    else if (op == 0xff) { /* inc, dec, push, or an indirect call or jump */
        modrm = 1;
        in->ends = ((p[i] >> 3) & 7) == 4 || ((p[i] >> 3) & 7) == 5;
        if (((p[i] >> 3) & 7) == 2 || ((p[i] >> 3) & 7) == 3)
            return false; /* an indirect call */
    }
    else
        return false;
    if (modrm) {
        unsigned char m = p[i++];
        int mod = m >> 6, rm = m & 7;
        if (mod != 3 && rm == 4) {
            if (mod == 0 && (p[i] & 7) == 5)
                i += 4;
            i++; /* the SIB byte */
        }
        else if (mod == 0 && rm == 5) {
            in->rel32_at = i; /* RIP-relative */
            i += 4;
        }
        if (mod == 1)
            i += 1;
        else if (mod == 2)
            i += 4;
    }
    in->len = i + imm;
    return true;
}

static int32_t
read32(const unsigned char *p)
{
    int32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* writes rel32 at 'at' for 'target' from the instruction ending at 'end' */
static bool
put_rel32(unsigned char *at, const unsigned char *end, intptr_t target)
{
    intptr_t d = target - (intptr_t)end;
    int32_t v = (int32_t)d;
    if (v != d)
        return false;
    memcpy(at, &v, 4);
    return true;
}

/* the first instructions of 'fn', at least ENTRY_JUMP bytes of them,
   moved to 'out', with a jump back to the rest: the number of bytes moved,
   or 0. A function that returns or jumps away sooner may be shorter than
   the jump, so it is refused. */
static int
move_entry(const unsigned char *fn, unsigned char *out)
{
    intptr_t targets[ENTRY_JUMP];
    int ntargets = 0, n = 0, o = 0, k;
    insn_t in;
    while (n < ENTRY_JUMP) {
        const unsigned char *p = fn + n;
        if (!decode(p, &in) ||
            ((in.ends || in.call) && n + in.len < ENTRY_JUMP))
            return 0;
        if (in.call) {
            /* pushes the original's own return address and jumps, so that
               the callee returns into the original past the bytes moved and
               the stub is never a return address: push imm32 (sign
               extended), mov dword [rsp+4], imm32, jmp rel32 */
            uint64_t back = (uint64_t)(uintptr_t)(p + in.len);
            uint32_t lo = (uint32_t)back, hi = (uint32_t)(back >> 32);
            out[o++] = 0x68;
            memcpy(out + o, &lo, 4);
            o += 4;
            memcpy(out + o, "\xc7\x44\x24\x04", 4);
            o += 4;
            memcpy(out + o, &hi, 4);
            o += 4;
            out[o++] = 0xe9;
            if (!put_rel32(out + o, out + o + 4,
                           (intptr_t)(p + in.len) + read32(p + in.rel32_at)))
                return 0;
            o += 4;
            n += in.len;
            break;
        }
        if (in.short_jump) {
            unsigned char op = p[0];
            intptr_t target = (intptr_t)(p + 2) + (signed char)p[1];
            if (op == 0xeb)
                out[o++] = 0xe9;
            else {
                out[o++] = 0x0f;
                out[o++] = 0x80 | (op & 0x0f);
            }
            if (!put_rel32(out + o, out + o + 4, target))
                return 0;
            o += 4;
            targets[ntargets++] = target;
        }
        else {
            memcpy(out + o, p, in.len);
            if (in.rel32_at >= 0) {
                intptr_t target =
                    (intptr_t)(p + in.len) + read32(p + in.rel32_at);
                if (!put_rel32(out + o + in.rel32_at, out + o + in.len,
                               target))
                    return 0;
                if (in.branch)
                    targets[ntargets++] = target;
            }
            o += in.len;
        }
        n += in.len;
        if (in.ends)
            break;
    }
    for (k = 0; k < ntargets; k++)
        if (targets[k] > (intptr_t)fn && targets[k] < (intptr_t)fn + n)
            return 0;
    if (!in.ends && !in.call) {
        out[o] = 0xe9;
        if (!put_rel32(out + o + 1, out + o + 5, (intptr_t)fn + n))
            return 0;
    }
    return n;
}

static bool
near(const void *a, const void *b)
{
    intptr_t d = (intptr_t)a - (intptr_t)b;
    return d > -(1L << 30) && d < (1L << 30);
}

/* one stub's room, within 1 GB of 'fn', or NULL */
static unsigned char *
stub_alloc(const void *fn)
{
    static unsigned char *page = NULL;
    static long left = 0;
    long ps = sysconf(_SC_PAGESIZE), step;
    uintptr_t base = (uintptr_t)fn & ~(uintptr_t)(ps - 1);
    if (page == NULL || left < STUB_SIZE || !near(page, fn)) {
        page = NULL;
        for (step = 0; page == NULL && step < (1L << 30); step += 1L << 24) {
            int side;
            for (side = -1; side <= 1; side += 2) {
                void *hint = (void *)(base + side * step);
                unsigned char *p =
                    mmap(hint, ps, PROT_READ | PROT_WRITE | PROT_EXEC,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (p == MAP_FAILED)
                    continue;
                if (near(p, fn) && near(p + ps, fn)) {
                    page = p;
                    left = ps;
                    break;
                }
                munmap(p, ps);
            }
        }
        if (page == NULL)
            return NULL;
    }
    left -= STUB_SIZE;
    return page + left;
}

static bool
write_entry(unsigned char *fn, const unsigned char jump[ENTRY_JUMP])
{
    long ps = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)fn & ~(uintptr_t)(ps - 1);
    uintptr_t end =
        ((uintptr_t)fn + ENTRY_JUMP + ps - 1) & ~(uintptr_t)(ps - 1);
    uintptr_t off = (uintptr_t)fn & 7;
    if (mprotect((void *)start, end - start,
                 PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
        return false;
    if (off + ENTRY_JUMP <= 8) {
        uint64_t *word = (uint64_t *)((uintptr_t)fn - off);
        uint64_t v = *word;
        memcpy((unsigned char *)&v + off, jump, ENTRY_JUMP);
        __atomic_store_n(word, v, __ATOMIC_SEQ_CST);
    }
    else
        memcpy(fn, jump, ENTRY_JUMP);
    mprotect((void *)start, end - start, PROT_READ | PROT_EXEC);
    return true;
}

/* the stub's head: through le_patch_call to 'by', or a plain jump to it */
static void *
patch(void *fn, void *by, bool direct)
{
    static const unsigned char endbr64[4] = {0xf3, 0x0f, 0x1e, 0xfa};
    unsigned char *stub, *moved, jump[ENTRY_JUMP];
    if ((stub = stub_alloc(fn)) == NULL)
        return NULL;
    moved = stub + STUB_JUMP;
    memcpy(moved, endbr64, 4);
    if (move_entry(fn, moved + 4) == 0)
        return NULL; /* the stub's room is left unused */
    if (direct) {
        static const unsigned char head[6] = {0xff, 0x25, 0, 0, 0, 0};
        memcpy(stub, head, 6);
        memcpy(stub + 6, &by, 8);
    }
    else {
        static const unsigned char head[12] = {0xff, 0x35, 6, 0, 0, 0,
                                               0xff, 0x25, 8, 0, 0, 0};
        const void *call = le_patch_call;
        memcpy(stub, head, 12);
        memcpy(stub + 12, &by, 8);
        memcpy(stub + 20, &call, 8);
    }
    jump[0] = 0xe9;
    if (!put_rel32(jump + 1, (unsigned char *)fn + ENTRY_JUMP,
                   (intptr_t)stub) ||
        !write_entry(fn, jump))
        return NULL;
    return moved;
}

DEFINEFN void *
le_patch_entry(void *fn, void *by)
{
    return patch(fn, by, false);
}

DEFINEFN void *
le_patch_entry_jump(void *fn, void *by)
{
    return patch(fn, by, true);
}
