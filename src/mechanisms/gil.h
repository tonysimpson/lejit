#ifndef LE_MECHANISMS_GIL_H
#define LE_MECHANISMS_GIL_H
/* The GIL released around a call of C code from compiled code and taken
   back after it, as Py_BEGIN_ALLOW_THREADS and Py_END_ALLOW_THREADS do,
   with the running thread's state compiled code loads itself
   (Python/pyruntime.c:le_gil_release, le_gil_take). Included after
   mechanisms/tstate.h. */
extern void
le_gil_calls(void **release, void **take);

/* the GIL released: the thread state to take it back with, a new
   reference, or NULL */
static inline vinfo_t *
le_gil_release_call(PsycoObject *po)
{
    void *release, *take;
    vinfo_t *vts = le_tstate_load(po);
    if (vts == NULL)
        return NULL;
    le_gil_calls(&release, &take);
    if (psyco_generic_call(po, release,
                           CfNoReturnValue | CfNoPython | CfNoBlock, "v",
                           vts) == NULL) {
        vinfo_decref(vts, po);
        return NULL;
    }
    return vts;
}

/* the GIL taken back by the thread state 'vts' */
static inline bool
le_gil_take_call(PsycoObject *po, vinfo_t *vts)
{
    void *release, *take;
    le_gil_calls(&release, &take);
    return psyco_generic_call(po, take, CfNoReturnValue | CfNoPython, "v",
                              vts) != NULL;
}
#endif /* LE_MECHANISMS_GIL_H */
