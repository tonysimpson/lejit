#ifndef LE_MECHANISMS_TSTATE_H
#define LE_MECHANISMS_TSTATE_H
#if LE_HAS_THREAD_LOCAL_TSTATE
/* The running thread's state is a thread-local of libpython's, which
   PyThreadState_Get() reads: compiled code calls it each time it needs
   the state. Included after vcompiler.h. */
static inline vinfo_t *
le_tstate_load(PsycoObject *po)
{
    return psyco_generic_call(po, PyThreadState_Get,
                              CfReturnTypePtr | CfNoBlock | CfNoPython, "");
}

static inline bool
le_tstate_check(void)
{
    return true;
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
