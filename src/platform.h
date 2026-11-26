/*
 * OS-specific utilities.
 */

/* maps 'size' bytes of executable memory, at '*result' if that is not
   NULL and is free, and sets '*result' to where: returns 'size', or 0 if
   it can't */
extern long
psyco_allocate_executable_buffer(long size, char **result);

/* maps 'size' bytes of memory, readable and writable but never touched,
   so that they cost address space and commit charge but no RSS: NULL if
   it can't. Unmapped by psyco_unmap_reserve. */
extern void *
psyco_map_reserve(long size);
extern void
psyco_unmap_reserve(void *p, long size);

/* gives the pages of [p, p + size) back to the system, which maps zeroed
   ones again when they are next written; 'p' and 'size' are multiples of
   psyco_page_size() */
extern void
psyco_release_pages(void *p, long size);
extern long
psyco_page_size(void);
