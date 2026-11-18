/*
 * OS-specific utilities.
 *
 * The rationale is that #include <windows.h> cannot be put anywhere else
 * because it messes up things like default alignments; if included in the
 * body of Psyco, we get crashes at run-time.
 *
 * This file is compiled separated.
 */

/************************************************************/
#ifdef _WIN32
/************************************************************/

#include <windows.h>

long psyco_allocate_executable_buffer(long basicsize, char **result)
{
  DWORD old;
  char *p = (char*) VirtualAlloc(NULL, basicsize, MEM_COMMIT|MEM_RESERVE, 
                                 PAGE_EXECUTE_READWRITE);
  if (p == NULL)
    return 0;
  VirtualProtect(p, basicsize, PAGE_EXECUTE_READWRITE, &old);
  /* ignore errors, just try */
  *result = p;
  return basicsize;
}

/************************************************************/
#else                                        /* Assume UNIX */
/************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#if defined(MAP_ANON) && !defined(MAP_ANONYMOUS)
#  define MAP_ANONYMOUS  MAP_ANON
#endif

long psyco_allocate_executable_buffer(long basicsize, char **result)
{
#if defined(MAP_ANONYMOUS) && defined(MAP_PRIVATE)
/* note that some platforms *require* the allocation to be performed
   by mmap, because PyMem_MALLOC() doesn't set the PROT_EXEC flag.
   On these platforms we just hope that the first allocation is
   successful, so that when another allocation fails, Psyco correctly
   signals the OUT_OF_MEMORY. */
  /* x64: jumps between code buffers are rel32, so every buffer must lie
     within 2 GB of every other. Ask for each new region
     just after the previous one, and refuse one that lands too far from
     the first. */
  static char *first = NULL, *next_hint = NULL;
  long bigsize = basicsize * 32;    /* allocate 32 blocks at a time */
  char *p = (char*) mmap(next_hint, bigsize,
                         PROT_EXEC|PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED || p == NULL)
    return 0;
  if (first == NULL)
    first = p;
  else if (p + bigsize - first > 0x7fff0000L || first + bigsize - p > 0x7fff0000L) {
    munmap(p, bigsize);
    fprintf(stderr, "ni: no executable memory within 2 GB of the first code "
                    "buffer; rel32 jumps can't reach it\n");
    return 0;
  }
  next_hint = p + bigsize;
  *result = p;
  return bigsize;

#else

  return 0;

#endif
}

/************************************************************/
#endif                                       /* !MS_WINDOWS */
/************************************************************/
