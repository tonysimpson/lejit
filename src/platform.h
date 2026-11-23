/*
 * OS-specific utilities.
 */

/* maps 'size' bytes of executable memory at '*result': returns 'size', or
   0 if it can't */
extern long
psyco_allocate_executable_buffer(long size, char **result);

/* maps 'size' bytes of memory, readable and writable but never touched,
   so that they cost address space and commit charge but no RSS: NULL if
   it can't. Unmapped by psyco_unmap_reserve. */
extern void *
psyco_map_reserve(long size);
extern void
psyco_unmap_reserve(void *p, long size);
