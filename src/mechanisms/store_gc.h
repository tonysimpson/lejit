#ifndef LE_MECHANISMS_STORE_GC_H
#define LE_MECHANISMS_STORE_GC_H
#if LE_HAS_GC_HEAD_INTERNAL
/* A GC object is tracked while its PyGC_Head's _gc_next isn't 0
   (_PyObject_GC_IS_TRACKED). PyGC_Head is internal: it is
   two uintptr_t words, _gc_next first, so _gc_next is the word 16 bytes
   before the object. The inline attribute store
   (x64/ipyencoding.c) tests the dict with it (MAINTAIN_TRACKING);
   pdictobject.c tests it in C. A free-threaded build tests
   a bit of ob_gc_bits instead, which this would have to refuse. Included
   after iencoding.h, or where only LE_GC_IS_TRACKED is used. */
#define GC_NEXT_OFFSET (-2 * (long)sizeof(uintptr_t))

/* emits a test of the GC object in register 'rg' after which CC_E means
   untracked; uses REG_TRANSIENT_1 */
#define LE_GC_UNTRACKED_TEST(rg)                      \
    do {                                              \
        MOV_R_O(REG_TRANSIENT_1, rg, GC_NEXT_OFFSET); \
        CMP_I8_R(0, REG_TRANSIENT_1);                 \
    } while (0)

/* the same test in C */
#define LE_GC_IS_TRACKED(o) (*(uintptr_t *)((char *)(o) + GC_NEXT_OFFSET) != 0)
#elif LE_HAS_GC_TRACKED_BY_NEXT
/* A GC object is tracked while its PyGC_Head's _gc_next, the first word
   of the 16 bytes before it, isn't 0 (_PyObject_GC_IS_TRACKED). The
   inline attribute store (x64/ipyencoding.c) tests the dict with it
   (MAINTAIN_TRACKING). A free-threaded build tests a bit of
   ob_gc_bits instead, which this would have to refuse. Included by
   ipyencoding.c after iencoding.h. */
_Static_assert(
    sizeof(PyGC_Head) == 16 && offsetof(PyGC_Head, _gc_next) == 0 &&
        sizeof(((PyGC_Head *)0)->_gc_next) == 8,
    "PyGC_Head with _gc_next first, as the inline GC test reads it");
#define GC_NEXT_OFFSET \
    ((long)offsetof(PyGC_Head, _gc_next) - (long)sizeof(PyGC_Head))

/* emits a test of the GC object in register 'rg' after which CC_E means
   untracked; uses REG_TRANSIENT_1 */
#define LE_GC_UNTRACKED_TEST(rg)                      \
    do {                                              \
        MOV_R_O(REG_TRANSIENT_1, rg, GC_NEXT_OFFSET); \
        CMP_I8_R(0, REG_TRANSIENT_1);                 \
    } while (0)

/* the same test in C (pdictobject.c) */
#define LE_GC_IS_TRACKED(o) _PyObject_GC_IS_TRACKED(o)
#else
/* Whether a GC object is tracked is in its PyGC_Head, the 32 bytes before
   it: gc.gc_refs shifted right by _PyGC_REFS_SHIFT is _PyGC_REFS_UNTRACKED
   for an untracked one (_PyObject_GC_IS_TRACKED). The inline attribute
   store (x64/ipyencoding.c) tests the dict with it (MAINTAIN_TRACKING).
   Included by ipyencoding.c after iencoding.h. */
_Static_assert(sizeof(PyGC_Head) == 32 &&
                   offsetof(PyGC_Head, gc.gc_refs) == 16 &&
                   __builtin_types_compatible_p(
                       __typeof__(((PyGC_Head *)0)->gc.gc_refs), Py_ssize_t) &&
                   _PyGC_REFS_SHIFT == 1 && _PyGC_REFS_UNTRACKED == -2,
               "PyGC_Head with gc.gc_refs, as the inline GC test reads it");
#define GC_REFS_OFFSET \
    ((long)offsetof(PyGC_Head, gc.gc_refs) - (long)sizeof(PyGC_Head))

/* emits a test of the GC object in register 'rg' after which CC_E means
   untracked; uses REG_TRANSIENT_1 */
#define LE_GC_UNTRACKED_TEST(rg)                                  \
    do {                                                          \
        MOV_R_O(REG_TRANSIENT_1, rg, GC_REFS_OFFSET);             \
        SHIFT_SIGNED_RIGHT_BY(REG_TRANSIENT_1, _PyGC_REFS_SHIFT); \
        CMP_I8_R(_PyGC_REFS_UNTRACKED, REG_TRANSIENT_1);          \
    } while (0)

/* the same test in C (pdictobject.c) */
#define LE_GC_IS_TRACKED(o) _PyObject_GC_IS_TRACKED(o)
#endif
#endif /* LE_MECHANISMS_STORE_GC_H */
