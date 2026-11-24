#ifndef LE_MECHANISMS_TSTATE_H
#define LE_MECHANISMS_TSTATE_H
#if LE_HAS_THREAD_LOCAL_TSTATE
/* The running thread's state is a thread-local of libpython's, which
   PyThreadState_Get() reads. Where it is at one offset from every thread's
   thread pointer (Python/pyruntime.c:le_tstate_thread_offset), compiled
   code loads it with one move; else it calls PyThreadState_Get each time
   it needs the state. Included after vcompiler.h. */
extern bool
le_tstate_thread_offset(long *offset);
/* a run-time load of the word at 'offset' from the thread pointer
   (x64/ipyencoding.c) */
EXTERNFN vinfo_t *
psyco_thread_word_load(PsycoObject *po, long offset);

static inline vinfo_t *
le_tstate_load(PsycoObject *po)
{
    long offset;
    if (le_tstate_thread_offset(&offset))
        return psyco_thread_word_load(po, offset);
    return psyco_generic_call(po, PyThreadState_Get,
                              CfReturnTypePtr | CfNoBlock | CfNoPython, "");
}

/* at start-up: finds the offset, which then holds what PyThreadState_Get()
   gives */
static inline bool
le_tstate_check(void)
{
    long offset;
    char *tp;
    if (!le_tstate_thread_offset(&offset))
        return true;
    __asm__("mov %%fs:0, %0" : "=r"(tp));
    return *(PyThreadState **)(tp + offset) == PyThreadState_Get();
}
#else
/* The running thread's state is the word at an address fixed for the
   process (Python/pyruntime.c:le_tstate_current_address): compiled code
   loads it with one move, a mutable field since another thread may run
   between two loads. Included
   after vcompiler.h. */
typedef struct {
    PyThreadState *ts;
} tstate_slot_t;
#define TSTATE_current \
    FMUT(DEF_FIELD(tstate_slot_t, PyThreadState *, ts, NO_PREV_FIELD))

extern void *
le_tstate_current_address(void);

/* the running thread's state, read at run time */
static inline vinfo_t *
le_tstate_load(PsycoObject *po)
{
    vinfo_t *vslot =
        vinfo_new(CompileTime_New((long)le_tstate_current_address()));
    vinfo_t *vts = psyco_get_field(po, vslot, TSTATE_current);
    vinfo_decref(vslot, po);
    return vts;
}

/* at start-up: the word holds what PyThreadState_GET() gives */
static inline bool
le_tstate_check(void)
{
    return ((tstate_slot_t *)le_tstate_current_address())->ts ==
           PyThreadState_GET();
}
#endif
#endif /* LE_MECHANISMS_TSTATE_H */
