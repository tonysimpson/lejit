/*
 * Stack segments. Where CPython makes a Python-to-Python call with no C
 * stack, a compiled call still takes a little, so deep recursion that
 * returns in CPython would overflow the thread's stack. Before such a call
 * compiled code compares the stack pointer with this thread's low mark
 * (x64/iencoding.c:psyco_stack_low); below it, it makes the call through
 * CPython instead, with le_call_on_segment, which runs it on a segment: a
 * stack of its own, with a guard page below. The callee is then a real
 * frame entered through the eval hook, as for any call made through C, so
 * nothing that walks frames needs to know about segments.
 *
 * The mark is half way down the thread's stack and each segment, leaving
 * room for the C code that compiled code calls. A thread keeps one free
 * segment for the next call; others are unmapped when their call returns,
 * and the kept one when the thread ends.
 *
 * Where CPython checks C calls by the stack pointer, the limits are the
 * segment's while on it. The room above its soft limit is what was left on
 * the stack it came from, plus what the compiled calls there that CPython
 * makes with no C stack took: only C, and calls CPython makes through C,
 * use it up, so C recursion between compiled calls (sum() of a generator
 * whose body calls on) still raises RecursionError.
 */
#include "segments.h"

#if LE_STACK_SEGMENTS
#include "Python/frames.h"
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#define SEGMENT_SIZE (8L << 20)

/* Read by compiled code at every direct call: in the static TLS block
   (initial-exec), at one offset from every thread's thread pointer.
   0 until le_stack_init: no stack pointer is below it. */
static _Thread_local uintptr_t stack_low
    __attribute__((tls_model("initial-exec")));
/* the stack the thread is on: its own, or a segment */
static _Thread_local char *stack_lo, *stack_hi;
/* this thread's free segment, or NULL */
static _Thread_local char *spare_segment;

DEFINEVAR long le_stack_low_at;

static pthread_key_t spare_key;
static pthread_once_t spare_key_once = PTHREAD_ONCE_INIT;

static void
spare_free(void *segment)
{
    munmap(segment, SEGMENT_SIZE);
}

static void
spare_key_make(void)
{
    if (pthread_key_create(&spare_key, spare_free) != 0)
        spare_key = (pthread_key_t)-1;
}

static char *
thread_pointer(void)
{
    char *tp;
    __asm__("mov %%fs:0, %0" : "=r"(tp));
    return tp;
}

/* the same for every thread, known once the module is loaded */
static void __attribute__((constructor))
stack_low_find(void)
{
    le_stack_low_at = (char *)&stack_low - thread_pointer();
}

DEFINEFN void
le_stack_init(void)
{
    pthread_attr_t attr;
    void *addr;
    size_t size, guard;
    if (stack_low != 0 || pthread_getattr_np(pthread_self(), &attr) != 0)
        return;
    if (pthread_attr_getstack(&attr, &addr, &size) == 0 &&
        pthread_attr_getguardsize(&attr, &guard) == 0 && size > guard) {
        stack_lo = (char *)addr + guard;
        stack_hi = (char *)addr + size;
        stack_low = (uintptr_t)stack_lo + (size - guard) / 2;
    }
    pthread_attr_destroy(&attr);
}

/* Calls fn(arg) with the stack pointer at 'top', returning to the caller's
   stack after. rbp keeps the caller's stack pointer, and the unwind
   information says so, so that debuggers and libunwind walk from the
   segment back to the caller. */
extern void
le_switch_call(void *arg, void (*fn)(void *), void *top);
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".type le_switch_call, @function\n"
    "le_switch_call:\n"
    ".cfi_startproc\n"
    "push %rbp\n"
    ".cfi_def_cfa_offset 16\n"
    ".cfi_offset %rbp, -16\n"
    "mov %rsp, %rbp\n"
    ".cfi_def_cfa_register %rbp\n"
    "mov %rdx, %rsp\n"
    "call *%rsi\n"
    "mov %rbp, %rsp\n"
    ".cfi_def_cfa_register %rsp\n"
    "pop %rbp\n"
    ".cfi_def_cfa_offset 8\n"
    "ret\n"
    ".cfi_endproc\n"
    ".size le_switch_call, .-le_switch_call\n");

typedef struct {
    long (*fn)(long, long, long, long);
    long a, b, c, d;
    long result;
} segment_call_t;

static void
segment_run(void *arg)
{
    segment_call_t *call = arg;
    call->result = call->fn(call->a, call->b, call->c, call->d);
}

static char *
segment_take(void)
{
    char *s = spare_segment;
    if (s != NULL) {
        spare_segment = NULL;
        return s;
    }
    s = mmap(NULL, SEGMENT_SIZE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (s == MAP_FAILED)
        return NULL;
    if (mprotect(s, sysconf(_SC_PAGESIZE), PROT_NONE) != 0) {
        munmap(s, SEGMENT_SIZE);
        return NULL;
    }
    return s;
}

static void
segment_give(char *s)
{
    if (spare_segment != NULL) {
        munmap(s, SEGMENT_SIZE);
        return;
    }
    spare_segment = s;
    pthread_once(&spare_key_once, spare_key_make);
    if (spare_key != (pthread_key_t)-1)
        pthread_setspecific(spare_key, s);
}

/* pyruntime.c */
#if LE_HAS_C_STACK_SOFT_LIMIT
extern const long le_c_stack_soft_limit_at;
EXTERNFN void
le_c_stack_limits_set(PyThreadState *ts, uintptr_t base, uintptr_t top,
                      long room, uintptr_t saved[3]);
EXTERNFN void
le_c_stack_limits_put_back(PyThreadState *ts, const uintptr_t saved[3]);
#endif

/* CPython's entry takes from the C count where it has one, or is charged
   on the soft limit where it measures the stack (eval_hook.c); the call it
   stands for takes nothing (mechanisms/recursion.h, LE_C_RECURSION_UNITS) */
#define ENTRY_C_UNITS 2
#if LE_HAS_C_STACK_SOFT_LIMIT
extern long le_c_entry_charge;
#endif

DEFINEFN long
le_call_on_segment(void *fn, long a, long b, long c, long d)
{
    segment_call_t call = {
        (long (*)(long, long, long, long))fn, a, b, c, d, 0};
    uintptr_t low = stack_low;
    char *lo = stack_lo, *hi = stack_hi, *s, *base;
#if LE_HAS_C_STACK_SOFT_LIMIT || LE_HAS_C_RECURSION_REMAINING
    PyThreadState *ts = PyThreadState_GET();
#endif
#if LE_HAS_C_STACK_SOFT_LIMIT
    uintptr_t saved[3];
    uintptr_t soft = *(uintptr_t *)((char *)ts + le_c_stack_soft_limit_at);
    long room = SEGMENT_SIZE; /* CPython hasn't set the limits yet */
    if (soft != 0)
        room = (long)((uintptr_t)__builtin_frame_address(0) - soft) +
               psyco_direct_calls_size(lo, hi) + le_c_entry_charge;
#endif
    s = segment_take();
    if (s == NULL) {
        /* no memory for one: on this stack, as before */
        segment_run(&call);
        return call.result;
    }
    base = s + sysconf(_SC_PAGESIZE);
    stack_lo = base;
    stack_hi = s + SEGMENT_SIZE;
    stack_low = (uintptr_t)base + (stack_hi - base) / 2;
#if LE_HAS_C_STACK_SOFT_LIMIT
    le_c_stack_limits_set(ts, (uintptr_t)base, (uintptr_t)stack_hi, room,
                          saved);
#endif
#if LE_HAS_C_RECURSION_REMAINING
    ts->c_recursion_remaining += ENTRY_C_UNITS;
#endif
    le_switch_call(&call, segment_run, stack_hi);
#if LE_HAS_C_RECURSION_REMAINING
    ts->c_recursion_remaining -= ENTRY_C_UNITS;
#endif
#if LE_HAS_C_STACK_SOFT_LIMIT
    le_c_stack_limits_put_back(ts, saved);
#endif
    stack_low = low;
    stack_lo = lo;
    stack_hi = hi;
    segment_give(s);
    return call.result;
}
#endif /* LE_STACK_SEGMENTS */
