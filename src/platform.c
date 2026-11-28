/* Memory for compiled code, from the OS. */

#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

long
psyco_allocate_executable_buffer(long size, char **result)
{
    static int first = 1;
    char *p = (char *)mmap(*result, size, PROT_EXEC | PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED || p == NULL)
        return 0;
#ifdef MADV_NOHUGEPAGE
    if (first)
        /* under transparent huge pages a program's first bytes of code would
           cost 2 MB of RSS; it gets huge pages only for code beyond that */
        madvise(p, 2 * 1024 * 1024 < size ? 2 * 1024 * 1024 : size,
                MADV_NOHUGEPAGE);
#endif
    first = 0;
    *result = p;
    return size;
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

void
psyco_release_pages(void *p, long size)
{
#ifdef MADV_DONTNEED
    madvise(p, size, MADV_DONTNEED); /* a failure only keeps them */
#endif
}

long
psyco_page_size(void)
{
    static long size = 0;
    if (size == 0)
        size = sysconf(_SC_PAGESIZE);
    return size;
}
