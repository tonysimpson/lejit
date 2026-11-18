/* Whether a GC object is tracked is in its PyGC_Head, the 32 bytes before
   it: gc.gc_refs shifted right by _PyGC_REFS_SHIFT is _PyGC_REFS_UNTRACKED
   for an untracked one (_PyObject_GC_IS_TRACKED). The inline attribute
   store (x64/ipyencoding.c) tests the dict with it (MAINTAIN_TRACKING,
   STORES.md; D77). Included by ipyencoding.c after iencoding.h. */
_Static_assert(sizeof(PyGC_Head) == 32 &&
                   offsetof(PyGC_Head, gc.gc_refs) == 16 &&
                   __builtin_types_compatible_p(
                       __typeof__(((PyGC_Head *)0)->gc.gc_refs), Py_ssize_t) &&
                   _PyGC_REFS_SHIFT == 1 && _PyGC_REFS_UNTRACKED == -2,
               "PyGC_Head with gc.gc_refs, as the inline GC test reads it");
#define GC_REFS_OFFSET                                                         \
  ((long)offsetof(PyGC_Head, gc.gc_refs) - (long)sizeof(PyGC_Head))

/* emits a test of the GC object in register 'rg' after which CC_E means
   untracked; uses REG_TRANSIENT_1 */
#define NI_GC_UNTRACKED_TEST(rg)                                               \
  do {                                                                         \
    MOV_R_O(REG_TRANSIENT_1, rg, GC_REFS_OFFSET);                              \
    SHIFT_SIGNED_RIGHT_BY(REG_TRANSIENT_1, _PyGC_REFS_SHIFT);                  \
    CMP_I8_R(_PyGC_REFS_UNTRACKED, REG_TRANSIENT_1);                           \
  } while (0)
