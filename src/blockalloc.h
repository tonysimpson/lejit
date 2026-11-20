/***************************************************************/
/***           Generic fixed-sized-blocks allocator            ***/
/***************************************************************/

#ifndef _BLOCKALLOC_H
#define _BLOCKALLOC_H

#include "psyco.h"

/* Define two functions to allocate and free items of type 'type'.
   The functions are named psyco_llalloc_[name] and psyco_llfree_[name]. */
#define BLOCKALLOC_INTERFACE(name, type)                                       \
  BLOCKALLOC_INTERN_DEF(name, type, EXTERNVAR, EXTERNFN)

/* Implements the required internal variables and functions.
   The allocator will reserve and subdivide blocks of the given size. */
#define BLOCKALLOC_IMPLEMENTATION(name, type, blocksize)                       \
  BLOCKALLOC_INTERN_IMPL(name, type, blocksize, DEFINEVAR, DEFINEFN)

/* Same as the combination of the above two when the result need only
   be local to a .c source (i.e. static). */
#define BLOCKALLOC_STATIC(name, type, blocksize)                               \
  BLOCKALLOC_INTERN_DEF(name, type, static, static /*forward*/)         \
  BLOCKALLOC_INTERN_IMPL(name, type, blocksize, static, static)

/* Under AddressSanitizer (setup.py NI_SANITIZE) each item is its own
   malloc'd block, so that a use after free or an overflow into the next
   item is reported, with the stacks that allocated and freed it */
#if defined(__SANITIZE_ADDRESS__)
#define BLOCKALLOC_INTERN_DEF(name, type, EVAR, EFN)                           \
  PSY_INLINE type *psyco_llalloc_##name(void) {                                \
    type *vi = (type *)PyMem_MALLOC(sizeof(type));                             \
    if (vi == NULL)                                                            \
      OUT_OF_MEMORY();                                                         \
    return vi;                                                                 \
  }                                                                            \
  PSY_INLINE void psyco_llfree_##name(type *vi) { PyMem_FREE(vi); }
#else
#define BLOCKALLOC_INTERN_DEF(name, type, EVAR, EFN)                           \
  EVAR void **psyco_linked_list_##name;                                        \
  EVAR long psyco_ll_items_##name; /* items made, in use or free */            \
  EFN void *psyco_ll_newblock_##name(void);                                    \
  PSY_INLINE type *psyco_llalloc_##name(void) {                                \
    type *vi;                                                                  \
    if (psyco_linked_list_##name == NULL)                                      \
      vi = (type *)psyco_ll_newblock_##name();                                 \
    else {                                                                     \
      vi = (type *)psyco_linked_list_##name;                                   \
      psyco_linked_list_##name = *(void **)vi;                                 \
    }                                                                          \
    return vi;                                                                 \
  }                                                                            \
  PSY_INLINE void psyco_llfree_##name(type *vi) {                              \
    *(void **)vi = psyco_linked_list_##name;                                   \
    psyco_linked_list_##name = (void **)vi;                                    \
  }
#endif

#if defined(__SANITIZE_ADDRESS__)
#define BLOCKALLOC_INTERN_IMPL(name, type, blocksize, DVAR, DFN)
#else
#define BLOCKALLOC_INTERN_IMPL(name, type, blocksize, DVAR, DFN)               \
  DVAR void **psyco_linked_list_##name = NULL;                                 \
  DVAR long psyco_ll_items_##name = 0;                                         \
  DFN void *psyco_ll_newblock_##name() {                                       \
    int block_count = (blocksize) / sizeof(type);                              \
    size_t sze = block_count * sizeof(type);                                   \
    type *p;                                                                   \
    type *prev = (type *)psyco_linked_list_##name;                             \
    type *block = (type *)PyMem_MALLOC(sze);                                   \
    extra_assert(block_count > 0 && sizeof(type) >= sizeof(void *));           \
    if (block == NULL)                                                         \
      OUT_OF_MEMORY();                                                         \
    psyco_ll_items_##name += block_count;                                      \
    for (p = block + block_count; --p != block;) {                             \
      *(type **)p = prev;                                                      \
      prev = p;                                                                \
    }                                                                          \
    psyco_linked_list_##name = *(void ***)prev;                                \
    return prev;                                                               \
  }
#endif
/***************************************************************/
#endif /* _BLOCKALLOC_H */
