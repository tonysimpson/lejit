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

long
psyco_allocate_executable_buffer(long size, char **result)
{
    DWORD old;
    char *p = (char *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
    if (p == NULL)
        return 0;
    VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &old);
    /* ignore errors, just try */
    *result = p;
    return size;
}

/************************************************************/
#else /* Assume UNIX */
/************************************************************/

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#if defined(MAP_ANON) && !defined(MAP_ANONYMOUS)
#define MAP_ANONYMOUS MAP_ANON
#endif

/* x64: jumps between code buffers are rel32, so all the regions must lie
   within one span of under 2 GB */
#define CODE_SPAN ((uintptr_t)0x7fff0000)

static uintptr_t span_lo, span_hi; /* the regions so far; 0 before the first */

/* a region of 'size' at 'at' (anywhere if 0) that keeps the span, or NULL */
static char *
map_code_at(uintptr_t at, uintptr_t size)
{
    int flags = MAP_PRIVATE | MAP_ANONYMOUS;
    char *p;
#ifdef MAP_FIXED_NOREPLACE
    if (at != 0)
        flags |= MAP_FIXED_NOREPLACE; /* fails where 'at' is taken */
#endif
    p = (char *)mmap((void *)at, size, PROT_EXEC | PROT_READ | PROT_WRITE,
                     flags, -1, 0);
    if (p == MAP_FAILED || p == NULL)
        return NULL;
    if (span_hi != 0) {
        uintptr_t lo = (uintptr_t)p < span_lo ? (uintptr_t)p : span_lo;
        uintptr_t hi =
            (uintptr_t)p + size > span_hi ? (uintptr_t)p + size : span_hi;
        if (hi - lo <= CODE_SPAN)
            return p;
        munmap(p, size); /* a kernel without MAP_FIXED_NOREPLACE put it
                            elsewhere */
        return NULL;
    }
    return p;
}

long
psyco_allocate_executable_buffer(long size, char **result)
{
#if defined(MAP_ANONYMOUS) && defined(MAP_PRIVATE)
    uintptr_t n = (uintptr_t)size, k;
    char *p = NULL;
    if (span_hi == 0) {
        p = map_code_at(0, n);
        if (p == NULL)
            return 0;
#ifdef MADV_NOHUGEPAGE
        /* under transparent huge pages a program's first bytes of code would
           cost 2 MB of RSS; it gets huge pages only for code beyond that */
        madvise(p, 2 * 1024 * 1024 < size ? 2 * 1024 * 1024 : size,
                MADV_NOHUGEPAGE);
#endif
        span_lo = (uintptr_t)p;
        span_hi = (uintptr_t)p + n;
    }
    else {
        /* just after the regions, then just before them, then each place
           further out while the span allows */
        for (k = 0;
             p == NULL && (k + 1) * n + (span_hi - span_lo) <= CODE_SPAN;
             k++) {
            p = map_code_at(span_hi + k * n, n);
            if (p == NULL && span_lo > (k + 1) * n)
                p = map_code_at(span_lo - (k + 1) * n, n);
        }
        if (p == NULL) {
            fprintf(stderr,
                    "lejit: no executable memory within 2 GB of the "
                    "first code buffer; rel32 jumps can't reach it\n");
            return 0;
        }
        if ((uintptr_t)p < span_lo)
            span_lo = (uintptr_t)p;
        if ((uintptr_t)p + n > span_hi)
            span_hi = (uintptr_t)p + n;
    }
    *result = p;
    return size;

#else

    return 0;

#endif
}

void *
psyco_map_reserve(long size)
{
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void
psyco_unmap_reserve(void *p, long size)
{
    munmap(p, size);
}

/************************************************************/
#endif /* !MS_WINDOWS */
/************************************************************/
