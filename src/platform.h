/*
 * OS-specific utilities.
 */

/* maps 'size' bytes of executable memory at '*result': returns 'size', or
   0 if it can't */
extern long
psyco_allocate_executable_buffer(long size, char **result);
