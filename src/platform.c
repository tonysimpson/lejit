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

long psyco_allocate_executable_buffer(long size, char **result)
{
  DWORD old;
  char *p = (char*) VirtualAlloc(NULL, size, MEM_COMMIT|MEM_RESERVE, 
                                 PAGE_EXECUTE_READWRITE);
  if (p == NULL)
    return 0;
  VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old);
  /* ignore errors, just try */
  *result = p;
  return size;
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

long psyco_allocate_executable_buffer(long size, char **result)
{
#if defined(MAP_ANONYMOUS) && defined(MAP_PRIVATE)
  /* x64: jumps between code buffers are rel32, so every region must lie
     within 2 GB of every other. Ask for each new region
     just after the previous one, and refuse one that lands too far from
     the first. */
  static char *first = NULL, *next_hint = NULL;
  char *p = (char*) mmap(next_hint, size,
                         PROT_EXEC|PROT_READ|PROT_WRITE,
                         MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED || p == NULL)
    return 0;
  if (first == NULL) {
    first = p;
#ifdef MADV_NOHUGEPAGE
    /* under transparent huge pages a program's first bytes of code would
       cost 2 MB of RSS; it gets huge pages only for code beyond that */
    madvise(p, 2 * 1024 * 1024 < size ? 2 * 1024 * 1024 : size,
            MADV_NOHUGEPAGE);
#endif
  }
  else if (p + size - first > 0x7fff0000L || first + size - p > 0x7fff0000L) {
    munmap(p, size);
    fprintf(stderr, "ni: no executable memory within 2 GB of the first code "
                    "buffer; rel32 jumps can't reach it\n");
    return 0;
  }
  next_hint = p + size;
  *result = p;
  return size;

#else

  return 0;

#endif
}

/************************************************************/
#endif                                       /* !MS_WINDOWS */
/************************************************************/
