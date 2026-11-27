#include "codemanager.h"
#include <time.h>
#include "platform.h"
#include "mechanisms/exc.h"
#include <ipyencoding.h>

/*** Executable memory ***/

/* Machine code is written into extents of executable memory, mapped
   REGION_SIZE at a time (platform.c). An extent is
   - free: in free_ext, sorted by address and coalesced; unaddressable
     under AddressSanitizer (LE_POISON), since only LeJIT's C code is checked,
     not the code it emits;
   - open: being written (psyco_code_open), in open_ext with the owner
     that was current when it was opened. Its writer has a limit
     GUARANTEED_MINIMUM before its end (psyco.h);
   - owned: closed, the part written held by that owner (o->extents), and
     free again when the owner is released (psyco_owner_release), the
     pages wholly free then given back to the system; with no owner, kept
     for good.
   An open extent is the whole free extent it was taken from, up to
   EXTENT_MAX, and closing it (psyco_code_close) frees what wasn't written,
   so while nothing is freed code is laid out one block after another, as
   by a bump allocator. psyco_code_continue closes one with a jump to a new
   one; psyco_code_take hands out an owned piece at once, for stub pools.
   An abandoned compile's open extents go to their owners whole, unused
   (psyco_guard_abandon). */
typedef struct {
    code_t *start, *end;
} extent_t;

static extent_t *free_ext = NULL;
static int nfree = 0, freecap = 0;

typedef struct {
    code_t *start, *end;
    le_owner_t *owner;
} open_extent_t;
static open_extent_t *open_ext = NULL;
static int nopen = 0, opencap = 0;

/* executable memory is mapped this much at a time */
#define REGION_SIZE (32 * 1024 * 1024)
/* an open extent with less room than this is full: what is left of it
   stays with the code before it */
#define EXTENT_MIN (BUFFER_MARGIN + 2 * GUARANTEED_MINIMUM)
/* a compile prefers an extent this large, so that it doesn't pause soon */
#define EXTENT_PREFERRED 16384
/* and takes no more than this, so that the few extents written at once
   (codemanager.h:OPEN_EXTENTS_MAX) share the first pages of one free
   extent instead of each taking the whole free tail and making the next
   one map a new region: under transparent huge pages every region touched
   costs at least 2 MB of RSS */
#define EXTENT_MAX (256 * 1024)

static void
free_insert(code_t *start, code_t *end)
{
    int lo = 0, hi = nfree;
    LE_POISON(start, end - start);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (free_ext[mid].start < start)
            lo = mid + 1;
        else
            hi = mid;
    }
    extra_assert(lo == nfree || end <= free_ext[lo].start);
    extra_assert(lo == 0 || free_ext[lo - 1].end <= start);
    if (lo > 0 && free_ext[lo - 1].end == start) {
        free_ext[lo - 1].end = end;
        if (lo < nfree && free_ext[lo].start == end) {
            free_ext[lo - 1].end = free_ext[lo].end;
            memmove(free_ext + lo, free_ext + lo + 1,
                    (nfree - lo - 1) * sizeof(extent_t));
            nfree--;
        }
        return;
    }
    if (lo < nfree && free_ext[lo].start == end) {
        free_ext[lo].start = start;
        return;
    }
    if (nfree == freecap) {
        int cap = freecap ? 2 * freecap : 64;
        extent_t *n = PyMem_Realloc(free_ext, cap * sizeof(extent_t));
        if (n == NULL)
            OUT_OF_MEMORY();
        free_ext = n;
        freecap = cap;
    }
    memmove(free_ext + lo + 1, free_ext + lo, (nfree - lo) * sizeof(extent_t));
    free_ext[lo].start = start;
    free_ext[lo].end = end;
    nfree++;
}

/* gives back the pages of the free extent holding [start, end), freed just
   now, that lie wholly in it and touch [start, end): those of its other
   pages were given back when they were freed, or never touched */
static void
free_release_pages(code_t *start, code_t *end)
{
    uintptr_t page = (uintptr_t)psyco_page_size();
    int lo = 0, hi = nfree;
    uintptr_t from, to;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (free_ext[mid].end <= start)
            lo = mid + 1;
        else
            hi = mid;
    }
    extra_assert(lo < nfree && free_ext[lo].start <= start &&
                 end <= free_ext[lo].end);
    from = ((uintptr_t)free_ext[lo].start + page - 1) & ~(page - 1);
    to = (uintptr_t)free_ext[lo].end & ~(page - 1);
    if (from < ((uintptr_t)start & ~(page - 1)))
        from = (uintptr_t)start & ~(page - 1);
    if (to > (((uintptr_t)end + page - 1) & ~(page - 1)))
        to = ((uintptr_t)end + page - 1) & ~(page - 1);
    if (from >= to)
        return;
    psyco_release_pages((void *)from, (long)(to - from));
#if ALL_CHECKS
    /* zeroed pages again, so that a stale jump still traps */
    LE_UNPOISON((void *)from, to - from);
    memset((void *)from, 0xCC, to - from);
    LE_POISON((void *)from, to - from);
#endif
}

/* Regions lie wherever the kernel puts them, so a rel32 jump may not reach
   its target. Such a jump goes to a far jump to it (x64/iprocessor.c:
   psyco_write_far_jump) in an island within its reach: ISLAND_SIZE bytes
   at the end of each region, and more taken from the end of a free extent
   when those are full, all kept for good, since a far jump is never
   changed or freed. One far jump serves every jump to its target in reach
   of it. */
#define ISLAND_SIZE (64 * 1024)
static extent_t *regions = NULL, *islands = NULL; /* island: start is free */
static int nregions = 0, nislands = 0;
static le_addrmap_t far_jumps; /* target -> its latest far jump */

/* a new region, near 'near' if there is room there (NULL: anywhere) */
static void
map_region(code_t *near)
{
    char *p = (char *)near;
    extent_t *r;
    if (psyco_allocate_executable_buffer(REGION_SIZE, &p) < REGION_SIZE)
        OUT_OF_MEMORY();
    r = PyMem_Realloc(regions, (nregions + 1) * sizeof(extent_t));
    if (r == NULL)
        OUT_OF_MEMORY();
    regions = r;
    r = PyMem_Realloc(islands, (nislands + 1) * sizeof(extent_t));
    if (r == NULL)
        OUT_OF_MEMORY();
    islands = r;
    regions[nregions].start = (code_t *)p;
    regions[nregions++].end = (code_t *)p + REGION_SIZE;
    islands[nislands].start = (code_t *)p + REGION_SIZE - ISLAND_SIZE;
    islands[nislands++].end = (code_t *)p + REGION_SIZE;
    LE_LSAN_ROOT(p, REGION_SIZE);
    free_insert((code_t *)p, (code_t *)p + REGION_SIZE - ISLAND_SIZE);
}

DEFINEFN bool
psyco_in_code_regions(const void *p)
{
    int r;
    for (r = 0; r < nregions; r++)
        if ((const void *)regions[r].start <= p &&
            p < (const void *)regions[r].end)
            return true;
    return false;
}

static int
amap_reserve(le_addrmap_t *m);
static void *
amap_get(le_addrmap_t *m, void *key);
static void
amap_put(le_addrmap_t *m, void *key, void *val);
static void
amap_remove(le_addrmap_t *m, void *key);

/* both ends of [start, end) are in reach of a jump ending at 'from' */
#define REACHES(from, start, end) \
    (FITS_IN_32BITS((start) - (from)) && FITS_IN_32BITS((end) - (from)))

/* an island with room for a far jump in reach of 'from', or -1 */
static int
island_find(code_t *from)
{
    int i, k;
    for (k = 0; k < nislands; k++)
        if (islands[k].end - islands[k].start >= SIZE_OF_FAR_JUMP_ANYWHERE &&
            REACHES(from, islands[k].start, islands[k].end))
            return k;
    for (i = nfree; i--;)
        if (free_ext[i].end - free_ext[i].start >= ISLAND_SIZE &&
            REACHES(from, free_ext[i].end - ISLAND_SIZE, free_ext[i].end)) {
            extent_t *n =
                PyMem_Realloc(islands, (nislands + 1) * sizeof(extent_t));
            if (n == NULL)
                return -1;
            islands = n;
            free_ext[i].end -= ISLAND_SIZE;
            islands[nislands].start = free_ext[i].end;
            islands[nislands].end = free_ext[i].end + ISLAND_SIZE;
            LE_UNPOISON(islands[nislands].start, ISLAND_SIZE);
            return nislands++;
        }
    return -1;
}

DEFINEFN code_t *
psyco_code_far_jump(code_t *end, code_t *target)
{
    code_t *j;
    int i, r;
    for (r = 0; r < nregions; r++)
        if (regions[r].start <= end && end <= regions[r].end)
            break;
    if (r == nregions)
        /* not in executable memory (a throwaway buffer): never run */
        return target;
    j = amap_get(&far_jumps, target);
    if (j != NULL && FITS_IN_32BITS(j - end))
        return j;
    if ((i = island_find(end)) < 0) {
        /* none in reach has room: a new region just after this one */
        map_region(regions[r].end);
        if ((i = island_find(end)) < 0)
            OUT_OF_MEMORY();
    }
    if (amap_reserve(&far_jumps) < 0)
        OUT_OF_MEMORY();
    j = islands[i].start;
    islands[i].start = psyco_write_far_jump(j, target);
    amap_remove(&far_jumps, target);
    amap_put(&far_jumps, target, j);
    return j;
}

/* the first free extent of at least 'n' bytes, or -1 */
static int
free_find(size_t n)
{
    int i;
    for (i = 0; i < nfree; i++)
        if ((size_t)(free_ext[i].end - free_ext[i].start) >= n)
            return i;
    return -1;
}

/*** The emergency reserve ***/

/* A compile entered from compiled code (a pause, respawn or promotion) must
   finish, as the code can't go back to the interpreter or raise without
   it. Memory is held back for it: released just before such a compile's
   last try (psyco_guard_retry), so that the try can allocate when memory
   has run out for good, and taken again by a later compile once memory
   allows. RESERVE_SIZE covers a new pymalloc arena (1 MB), a new piece of
   malloc's heap (1 MB) and what one compile allocates (under 0.5 MB in
   all but one of the pyperformance benchmarks on 3.10; unpack_sequence's
   largest needed 2.5 MB). Its code needed at most 170 kB there: the
   reserve holds SPARE_CODE_SIZE at the end of a free extent too, for when
   no new region can be mapped. */
#define RESERVE_SIZE (4 * 1024 * 1024)
#define SPARE_CODE_SIZE (1024 * 1024)
static void *reserve = NULL;
static code_t *spare_code = NULL;

static void
reserve_refill(void)
{
    int i;
    if (reserve == NULL)
        reserve = psyco_map_reserve(RESERVE_SIZE);
    if (spare_code == NULL && (i = free_find(SPARE_CODE_SIZE + 1)) >= 0) {
        free_ext[i].end -= SPARE_CODE_SIZE;
        spare_code = free_ext[i].end;
    }
}

static void
reserve_release(void)
{
    if (reserve != NULL) {
        psyco_unmap_reserve(reserve, RESERVE_SIZE);
        reserve = NULL;
    }
    if (spare_code != NULL) {
        free_insert(spare_code, spare_code + SPARE_CODE_SIZE);
        spare_code = NULL;
    }
}

static code_t *
take_extent(size_t want, size_t need, code_t **end)
{
    int i = free_find(want);
    code_t *start;
    if (i < 0 && need < want)
        i = free_find(need);
    if (i < 0) {
        map_region(NULL);
        i = free_find(need);
        psyco_assert(i >= 0);
    }
    start = free_ext[i].start;
    *end = free_ext[i].end;
    memmove(free_ext + i, free_ext + i + 1,
            (nfree - i - 1) * sizeof(extent_t));
    nfree--;
    LE_UNPOISON(start, *end - start);
    return start;
}

#if ALL_CHECKS
static void
made_add(le_owner_t *o, code_t *start, code_t *end);
#else
#define made_add(o, start, end) ((void)0)
#endif

static void
owner_add_extent(le_owner_t *o, code_t *start, code_t *end)
{
    LE_STAT(psyco_stat_code_bytes_live += end - start);
    if (o == NULL)
        return; /* kept for good */
    if (o->nextents > 0 && o->extents[o->nextents - 1].end == start) {
        o->extents[o->nextents - 1].end = end;
        made_add(o, start, end);
        return;
    }
    if (o->nextents == o->extentscap) {
        int cap = o->extentscap ? 2 * o->extentscap : o->parent ? 1 : 4;
        void *n = PyMem_Realloc(o->extents, cap * sizeof(extent_t));
        if (n == NULL)
            return; /* out of memory: kept for good, as an abandoned
                       compile must not run out again */
        o->extents = n;
        o->extentscap = cap;
    }
    o->extents[o->nextents].start = start;
    o->extents[o->nextents].end = end;
    o->nextents++;
    made_add(o, start, end);
}

#if ALL_CHECKS
DEFINEVAR long psyco_stat_code_buffers = 0;
#endif
#if ALL_CHECKS || LE_CODESTATS
DEFINEVAR long psyco_stat_code_bytes = 0;
#endif

static code_t *
open_extent(code_t **limit)
{
    code_t *start, *end;
    start = take_extent(EXTENT_PREFERRED, EXTENT_MIN, &end);
    if (end - start > EXTENT_MAX) {
        free_insert(start + EXTENT_MAX, end);
        end = start + EXTENT_MAX;
    }
    if (nopen == opencap) {
        int cap = opencap ? 2 * opencap : 16;
        open_extent_t *n =
            PyMem_Realloc(open_ext, cap * sizeof(open_extent_t));
        if (n == NULL)
            OUT_OF_MEMORY();
        open_ext = n;
        opencap = cap;
    }
    open_ext[nopen].start = start;
    open_ext[nopen].end = end;
    open_ext[nopen].owner = psyco_owner;
    nopen++;
    *limit = end - GUARANTEED_MINIMUM;
    return start;
}

DEFINEFN code_t *
psyco_code_open(code_t **limit)
{
    LE_STAT(psyco_stat_code_buffers++);
    return open_extent(limit);
}

DEFINEFN int
psyco_code_open_count(void)
{
    return nopen;
}

DEFINEFN void
psyco_code_close(code_t *code)
{
    int i;
    for (i = nopen; i--;) {
        open_extent_t x = open_ext[i];
        if (x.start <= code && code <= x.end) {
            memmove(open_ext + i, open_ext + i + 1,
                    (nopen - i - 1) * sizeof(open_extent_t));
            nopen--;
            /* checked builds fill the padding, which must stay in the extent
             */
            if (x.end - code >= 16)
                ALIGN_NO_FILL();
#if ALL_CHECKS || LE_CODESTATS
            psyco_stat_code_bytes += code - x.start;
#endif
            if (x.end - code < EXTENT_MIN)
                code = x.end; /* full */
            else
                free_insert(code, x.end);
            owner_add_extent(x.owner, x.start, code);
            return;
        }
    }
    Py_FatalError("psyco: code buffer allocator corruption");
}

DEFINEFN void
psyco_code_continue(code_t **pcode, code_t **plimit)
{
    code_t *code = *pcode;
    code_t *next;
    if (code - *plimit > GUARANTEED_MINIMUM - MAXIMUM_SIZE_OF_FAR_JUMP)
        Py_FatalError("psyco: code buffer overflowing");
    next = open_extent(plimit);
    le_trace_begin_simple_code(code);
    JUMP_TO(next);
    le_trace_end_simple_code(code);
    psyco_code_close(code);
    *pcode = insn_code_label(next);
}

DEFINEFN code_t *
psyco_code_take(size_t n)
{
    code_t *end, *start = take_extent(n, n, &end);
    if ((size_t)(end - start) - n < EXTENT_MIN)
        n = end - start;
    else
        free_insert(start + n, end);
    owner_add_extent(psyco_owner, start, start + n);
    return start;
}

#if ALL_CHECKS
/*** Moving the code a compile made ***/

/* The pieces of owned code the outermost compile running made, with where
   each went ('to', the same if it stayed), and the relocations noted
   meanwhile (psyco.h). A piece moves by a multiple of 16 bytes, so that
   its alignment stays, within its region, so that what it reached stays
   in reach (moved_room). One with no room there, or whose RIP-relative
   data would be out of reach, stays. The old copy is filled with INT 3, so
   that a jump or call to it traps, and freed when the next compile ends. */
/* bytes moved, bytes that stayed, relocations the owners keep */
DEFINEVAR long psyco_stat_code_moved[3] = {0, 0, 0};
typedef struct {
    le_owner_t *owner; /* NULL: its owner died */
    code_t *start, *end, *to;
    int gap; /* bytes before 'to' that go with it */
} made_t;
static made_t *made = NULL;
static int nmade = 0, madecap = 0;

typedef struct {
    code_t *at;
    int kind, end;
    long value; /* what was written, for JUMP and DATA */
    long seq;
} note_t;
static note_t *notes = NULL;
static long nnotes = 0, notescap = 0;

struct le_reloc_s {
    code_t *at;
    signed char kind, end;
};

static void
made_add(le_owner_t *o, code_t *start, code_t *end)
{
    if (le_compile_depth == 0 || start == end)
        return;
    if (nmade == madecap) {
        int cap = madecap ? 2 * madecap : 16;
        made_t *n = PyMem_Realloc(made, cap * sizeof(made_t));
        if (n == NULL)
            return; /* stays where it is */
        made = n;
        madecap = cap;
    }
    made[nmade].owner = o;
    made[nmade].start = start;
    made[nmade].end = end;
    made[nmade].to = start;
    made[nmade].gap = 0;
    nmade++;
}

static bool
in_owner(const le_owner_t *o, code_t *p)
{
    void **b;
    int k;
    for (k = 0; k < o->nextents; k++)
        if (o->extents[k].start <= p && p < o->extents[k].end)
            return true;
    for (b = o->blocks; b != NULL; b = *b)
        if ((code_t *)(b + 2) <= p && p < (code_t *)(b + 2) + (size_t)b[1])
            return true;
    return false;
}

/* 'o' died, and what it held ('dead''s code and records) is about to be
   freed: its pieces stay, and the notes in what it held are dropped, since
   that memory may be anything's by the time the compile ends */
static void
made_forget(le_owner_t *o, const le_owner_t *dead)
{
    long i;
    for (i = 0; i < nmade; i++)
        if (made[i].owner == o)
            made[i].owner = NULL;
    if (le_compile_depth == 0)
        return;
    for (i = 0; i < nnotes; i++)
        if (notes[i].at != NULL && in_owner(dead, notes[i].at))
            notes[i].at = NULL;
}

DEFINEFN void
le_reloc_note(void *at, int kind, int end)
{
    note_t *n;
    if (le_compile_depth == 0)
        return;
    if (nnotes == notescap) {
        long cap = notescap ? 2 * notescap : 1024;
        note_t *p = PyMem_Realloc(notes, cap * sizeof(note_t));
        if (p == NULL)
            OUT_OF_MEMORY();
        notes = p;
        notescap = cap;
    }
    n = &notes[nnotes];
    n->at = (code_t *)at;
    n->kind = kind;
    n->end = end;
    n->seq = nnotes++;
    if (kind == LE_RELOC_WORD)
        n->value = 0;
    else {
        dword_t v;
        memcpy(&v, at, sizeof(v));
        n->value = v;
    }
}

DEFINEFN long
le_reloc_jump(code_t *end, long value)
{
    if (le_compile_depth != 0) {
        le_reloc_note(end - 4, LE_RELOC_JUMP, 4);
        notes[nnotes - 1].value = (dword_t)value;
    }
    return value;
}

/* the piece holding 'p', once the pieces are sorted, or NULL */
static made_t *
made_find(code_t *p)
{
    int lo = 0, hi = nmade;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (made[mid].end <= p)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo < nmade && made[lo].start <= p && made[lo].owner != NULL)
        return &made[lo];
    return NULL;
}

static int
made_cmp(const void *a, const void *b)
{
    const made_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

static code_t *
moved(code_t *p)
{
    made_t *m = made_find(p);
    return m == NULL ? p : m->to + (p - m->start);
}

DEFINEFN code_t *
psyco_code_moved(code_t *p)
{
    return le_compile_depth == 0 ? moved(p) : p;
}

/* Where moved code goes: the end of the last free extent of its region
   with room, taken MOVED_ROOM at a time, away from where code is written
   (take_extent takes the first). */
#define MOVED_ROOM (1024 * 1024)
static code_t *moved_next = NULL, *moved_end = NULL;

static int
region_of(code_t *p)
{
    int r;
    for (r = 0; r < nregions; r++)
        if (regions[r].start <= p && p < regions[r].end)
            return r;
    return -1;
}

/* the last free extent of region 'r' of at least 'n' bytes, or -1 */
static int
free_last(int r, size_t n)
{
    int f;
    for (f = nfree; f--;)
        if (free_ext[f].start >= regions[r].start &&
            free_ext[f].end <= regions[r].end &&
            (size_t)(free_ext[f].end - free_ext[f].start) >= n)
            break;
    return f;
}

/* room for 'size' bytes at the same offset from 16 as 'from', or 'from';
   the bytes skipped to keep that offset go in '*gap' */
static code_t *
moved_room(code_t *from, size_t size, int *gap)
{
    int r = region_of(from), f;
    code_t *d, *old_next = NULL, *old_end = NULL;
    if (r < 0)
        return from;
    if (moved_next == NULL || region_of(moved_next) != r ||
        (size_t)(moved_end - moved_next) < size + 16) {
        size_t want = size + 16 > MOVED_ROOM ? size + 16 : MOVED_ROOM;
        f = free_last(r, want);
        if (f < 0)
            f = free_last(r, size + 16);
        if (f < 0)
            return from;
        old_next = moved_next;
        old_end = moved_end;
        moved_end = free_ext[f].end;
        if ((size_t)(free_ext[f].end - free_ext[f].start) > want) {
            free_ext[f].end -= want;
            moved_next = free_ext[f].end;
        }
        else {
            moved_next = free_ext[f].start;
            memmove(free_ext + f, free_ext + f + 1,
                    (nfree - f - 1) * sizeof(extent_t));
            nfree--;
        }
        LE_UNPOISON(moved_next, moved_end - moved_next);
        if (old_next != NULL && old_end > old_next)
            free_insert(old_next, old_end);
    }
    /* the bytes skipped go with the piece, so that they are freed with it:
       freed alone, between pieces, they would fill the free list with holes
       too small to use */
    d = moved_next + ((from - moved_next) & 15);
    *gap = (int)(d - moved_next);
    moved_next = d + size;
    return d;
}

/* old copies: those of the compile before are the first nold_freed */
static extent_t *old = NULL;
static int nold = 0, oldcap = 0, nold_freed = 0;

static void
old_add(code_t *start, code_t *end)
{
    if (nold == oldcap) {
        int cap = oldcap ? 2 * oldcap : 64;
        extent_t *n = PyMem_Realloc(old, cap * sizeof(extent_t));
        if (n == NULL) {
            free_insert(start, end);
            return;
        }
        old = n;
        oldcap = cap;
    }
    old[nold].start = start;
    old[nold].end = end;
    nold++;
}

static bool
in_regions(code_t *p)
{
    int r;
    for (r = 0; r < nregions; r++)
        if (regions[r].start <= p && p < regions[r].end)
            return true;
    return false;
}

#define NOTE_SIZE(x) ((x)->kind == LE_RELOC_WORD ? 8 : 4)

/* the new copies, sorted */
static extent_t *copies = NULL;
static int ncopies = 0, copiescap = 0;

static int
extent_cmp(const void *a, const void *b)
{
    const extent_t *x = a, *y = b;
    return x->start < y->start ? -1 : x->start > y->start;
}

/* whether [p, p + size) meets a new copy */
static bool
in_copies(code_t *p, int size)
{
    int lo = 0, hi = ncopies;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (copies[mid].end <= p)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < ncopies && copies[lo].start < p + size;
}

static int
note_cmp(const void *a, const void *b)
{
    const note_t *x = a, *y = b;
    if (x->at != y->at)
        return x->at < y->at ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* where the rel32 of note 'n' goes, the old place in 'at', and the far
   jumps it goes through followed */
static code_t *
note_target(note_t *n)
{
    dword_t v;
    code_t *t;
    memcpy(&v, n->at, sizeof(v));
    t = n->at + n->end + v;
    if (n->kind == LE_RELOC_JUMP)
        while (made_find(t) == NULL && in_regions(t) && t[0] == 0xFF &&
               t[1] == 0x25 && *(dword_t *)(t + 2) == 0)
            t = *(code_t **)(t + 6);
    return t;
}

static void
piece_stay(made_t *m)
{
    if (m->to != m->start) {
        free_insert(m->to - m->gap, m->to + (m->end - m->start));
        m->to = m->start;
        m->gap = 0;
    }
}

/* the extent of 'o' holding [start, end) is now at 'to', with the 'gap'
   bytes before it */
static void
owner_extent_moved(le_owner_t *o, code_t *start, code_t *end, code_t *to,
                   int gap)
{
    int k;
    for (k = 0; k < o->nextents; k++) {
        code_t *xstart = o->extents[k].start, *xend = o->extents[k].end;
        if (xstart <= start && end <= xend) {
            int i = k;
            if (xstart < start) {
                o->extents[k].end = start;
                i = o->nextents++;
            }
            o->extents[i].start = to - gap;
            o->extents[i].end = to + (end - start);
            if (end < xend) {
                o->extents[o->nextents].start = end;
                o->extents[o->nextents++].end = xend;
            }
            return;
        }
    }
    Py_FatalError("psyco: moved code not in its owner's extents");
}

static void
owner_add_reloc(le_owner_t *o, code_t *at, int kind, int end)
{
    if (o->nrelocs == o->relocscap) {
        int cap = o->relocscap ? 2 * o->relocscap : 16;
        struct le_reloc_s *n =
            PyMem_Realloc(o->relocs, cap * sizeof(struct le_reloc_s));
        if (n == NULL)
            return;
        o->relocs = n;
        o->relocscap = cap;
    }
    o->relocs[o->nrelocs].at = at;
    o->relocs[o->nrelocs].kind = (signed char)kind;
    o->relocs[o->nrelocs].end = (signed char)end;
    o->nrelocs++;
    psyco_stat_code_moved[2]++;
}

DEFINEFN void
psyco_code_move_begin(void)
{
    nmade = 0;
    nnotes = 0;
}

DEFINEFN void
psyco_code_move_made(void)
{
    long i, j, n;
    int k;
    bool again;
    /* the last note at each place; a jump or distance no longer as noted
       was overwritten, and a place outside executable memory is a scratch
       copy */
    /* the pieces of owners still alive, which do not overlap */
    for (k = j = 0; k < nmade; k++)
        if (made[k].owner != NULL)
            made[j++] = made[k];
    nmade = (int)j;
    qsort(made, nmade, sizeof(made_t), made_cmp);
    for (k = 1; k < nmade; k++)
        if (made[k - 1].end > made[k].start)
            Py_FatalError("psyco: pieces of code made overlap");
    qsort(notes, nnotes, sizeof(note_t), note_cmp);
    for (i = j = 0; i < nnotes; i++) {
        note_t *x = &notes[i];
        made_t *m;
        if (x->at == NULL)
            continue;
        if (x->kind != LE_RELOC_WORD) {
            dword_t v;
            if (!in_regions(x->at))
                continue;
            memcpy(&v, x->at, sizeof(v));
            if (v != (dword_t)x->value)
                continue;
        }
        m = made_find(x->at);
        if (m != NULL && x->at + NOTE_SIZE(x) > m->end)
            continue;
        /* of notes whose places overlap, the last written */
        if (j > 0 && notes[j - 1].at + NOTE_SIZE(&notes[j - 1]) > x->at) {
            if (notes[j - 1].seq > x->seq)
                continue;
            j--;
        }
        notes[j++] = *x;
    }
    n = j;
    if (copiescap < nmade) {
        extent_t *c = PyMem_Realloc(copies, nmade * sizeof(extent_t));
        if (c == NULL)
            nmade = 0; /* all stay */
        else {
            copies = c;
            copiescap = nmade;
        }
    }
    /* destinations */
    for (k = 0; k < nmade; k++) {
        made_t *m = &made[k];
        size_t size = m->end - m->start;
        if (m->owner != NULL)
            m->to = moved_room(m->start, size, &m->gap);
    }
    /* each distance must still reach */
    do {
        again = false;
        for (i = 0; i < n; i++) {
            note_t *x = &notes[i];
            code_t *t, *at;
            if (x->kind != LE_RELOC_DATA)
                continue;
            t = note_target(x);
            at = moved(x->at);
            if (!FITS_IN_32BITS(moved(t) - (at + x->end))) {
                made_t *a = made_find(x->at), *b = made_find(t);
                if (a != NULL && a->to != a->start)
                    piece_stay(a), again = true;
                if (b != NULL && b->to != b->start)
                    piece_stay(b), again = true;
            }
        }
    } while (again);
    /* room in the owners' extents: a piece may split one in three */
    for (k = 0; k < nmade; k++) {
        made_t *m = &made[k];
        le_owner_t *o = m->owner;
        int need = o == NULL ? 0 : o->nextents, l;
        if (m->to == m->start)
            continue;
        for (l = 0; l < nmade; l++)
            if (made[l].owner == o && made[l].to != made[l].start)
                need += 2;
        if (need > o->extentscap) {
            int cap = need + (need >> 1);
            void *p = PyMem_Realloc(o->extents, cap * sizeof(extent_t));
            if (p == NULL) {
                piece_stay(m);
                continue;
            }
            o->extents = p;
            o->extentscap = cap;
        }
    }
    ncopies = 0;
    for (k = 0; k < nmade; k++)
        if (made[k].to != made[k].start) {
            memcpy(made[k].to, made[k].start, made[k].end - made[k].start);
            copies[ncopies].start = made[k].to;
            copies[ncopies++].end = made[k].to + (made[k].end - made[k].start);
        }
    qsort(copies, ncopies, sizeof(extent_t), extent_cmp);
    /* patched in the new copies, read from the old ones; a note in code
       freed since, where a copy now is, is dropped */
    for (i = 0; i < n; i++) {
        note_t *x = &notes[i];
        code_t *at = moved(x->at);
        made_t *m = made_find(x->at);
        if (m == NULL && in_copies(x->at, NOTE_SIZE(x)))
            continue;
        if (x->kind == LE_RELOC_WORD) {
            code_t *v, *w;
            memcpy(&v, x->at, sizeof(v));
            w = x->end ? moved(v - 1) + 1 : moved(v);
            if (at != x->at || w != v)
                memcpy(at, &w, sizeof(w));
        }
        else {
            code_t *t = note_target(x), *to = moved(t), *j;
            long d = to - (at + x->end);
            dword_t v;
            memcpy(&v, x->at, sizeof(v));
            j = x->at + x->end + v;
            if (x->kind == LE_RELOC_JUMP && to != t && j != t &&
                made_find(j) == NULL && *(code_t **)(j + 6) == t &&
                FITS_IN_32BITS(j - (at + x->end))) {
                /* a far jump to code this compile made goes there now; any
                   other jump to it was made by this compile too */
                *(code_t **)(j + 6) = to;
                if (amap_get(&far_jumps, t) == j &&
                    amap_reserve(&far_jumps) >= 0) {
                    amap_remove(&far_jumps, t);
                    amap_put(&far_jumps, to, j);
                }
                t = to;
                to = j;
                d = j - (at + x->end);
            }
            if (!FITS_IN_32BITS(d)) {
                if (x->kind == LE_RELOC_DATA)
                    Py_FatalError("psyco: moved code out of reach");
                d = psyco_code_far_jump(at + x->end, to) - (at + x->end);
            }
            if (at != x->at || to != t) {
                v = (dword_t)d;
                memcpy(at, &v, sizeof(v));
            }
        }
        if (m != NULL)
            owner_add_reloc(m->owner, at, x->kind, x->end);
    }
    for (k = 0; k < nmade; k++) {
        made_t *m = &made[k];
        if (m->to == m->start) {
            if (m->owner != NULL)
                psyco_stat_code_moved[1] += m->end - m->start;
            continue;
        }
        LE_STAT(psyco_stat_code_moved[0] += m->end - m->start);
        owner_extent_moved(m->owner, m->start, m->end, m->to, m->gap);
        LE_STAT(psyco_stat_code_bytes_live += m->gap);
        memset(m->start, 0xCC, m->end - m->start); /* INT 3 */
        old_add(m->start, m->end);
    }
    /* the old copies of the compile before are freed */
    for (k = 0; k < nold_freed; k++) free_insert(old[k].start, old[k].end);
    memmove(old, old + nold_freed, (nold - nold_freed) * sizeof(extent_t));
    nold -= nold_freed;
    nold_freed = nold;
    nnotes = 0;
}
#endif

/*** Owners ***/

DEFINEVAR le_owner_t *psyco_owner = NULL;
#if ALL_CHECKS
DEFINEVAR long psyco_stat_code_bytes_live = 0;
DEFINEVAR long psyco_stat_kept_for_good = 0;
#endif

DEFINEFN le_owner_t *
psyco_code_owner(PyCodeObject *co)
{
    le_code_extra_t *x = le_code_extra(co);
    return x == NULL ? NULL : &x->owner;
}

/* open addressing on the address; a hole is NULL */
static size_t
pin_slot(le_owner_t *o, PyObject *p)
{
    size_t i = ((size_t)p >> 4) & o->pinmask;
    while (o->pins[i] != NULL && o->pins[i] != p) i = (i + 1) & o->pinmask;
    return i;
}

static void
pins_grow(le_owner_t *o)
{
    size_t i, n = o->pins == NULL ? 0 : o->pinmask + 1;
    PyObject **old = o->pins;
    size_t cap = n == 0 ? 8 : 2 * n;
    o->pins = PyMem_Calloc(cap, sizeof(PyObject *));
    if (o->pins == NULL)
        OUT_OF_MEMORY();
    o->pinmask = cap - 1;
    for (i = 0; i < n; i++)
        if (old[i] != NULL)
            o->pins[pin_slot(o, old[i])] = old[i];
    PyMem_Free(old);
}

DEFINEFN void
psyco_owner_keep(PyObject *p)
{
    le_owner_t *o = psyco_owner;
    size_t i;
    if (p == NULL)
        return;
    if (o == NULL) {
        LE_STAT(psyco_stat_kept_for_good++);
        return;
    }
    if (o->pins == NULL || 2 * (o->npins + 1) > o->pinmask + 1)
        pins_grow(o);
    i = pin_slot(o, p);
    if (o->pins[i] == p) {
        Py_DECREF(p); /* it holds another */
        return;
    }
    o->pins[i] = p;
    if (o->npins++ == 0 && o->parent == NULL)
        psyco_owner_track(
            (le_code_extra_t *)((char *)o - offsetof(le_code_extra_t, owner)));
}

DEFINEFN void
psyco_owner_pin(PyObject *p)
{
    Py_XINCREF(p);
    psyco_owner_keep(p);
}

/* each block is a list link, its size in checked builds, then the record
 */
#define BLOCK_HEAD (ALL_CHECKS ? 2 : 1)
DEFINEFN void *
psyco_owner_alloc(size_t size)
{
    le_owner_t *o = psyco_owner;
    void **p = PyMem_Malloc(BLOCK_HEAD * sizeof(void *) + size);
    if (p == NULL || LE_FAIL(LE_FAIL_OWNER))
        OUT_OF_MEMORY();
    memset(p + BLOCK_HEAD, 0, size);
#if ALL_CHECKS
    p[1] = (void *)size;
#endif
    if (o != NULL) {
        *p = o->blocks;
        o->blocks = p;
    }
    return p + BLOCK_HEAD;
}

static void
codebuf_unlist(CodeBufferObject *b);
#if ALL_CHECKS
static void
made_forget(le_owner_t *o, const le_owner_t *dead);
#endif
EXTERNFN void
psyco_owner_release_records(le_owner_t *o); /* dispatcher.c */
EXTERNFN bool
psyco_ge_in_report(PyObject *mergepoints,
                   global_entries_t *ge); /* mergepoints.c */
static void
amap_remove(le_addrmap_t *m, void *key);

static le_owner_t *
owner_root(le_owner_t *o)
{
    while (o->parent != NULL) o = o->parent;
    return o;
}

/* the groups' blocks: block -> group (psyco_owner_depend) */
static le_addrmap_t group_blocks;

/* moves the references 'from' holds into 'to' */
static void
pins_take(le_owner_t *to, le_owner_t *from)
{
    size_t j, i;
    for (j = 0; from->pins != NULL && j <= from->pinmask; j++) {
        PyObject *p = from->pins[j];
        if (p == NULL)
            continue;
        if (to->pins == NULL || 2 * (to->npins + 1) > to->pinmask + 1)
            pins_grow(to);
        i = pin_slot(to, p);
        if (to->pins[i] == p) {
            Py_DECREF(p); /* 'to' holds another, so never the last */
        }
        else {
            to->pins[i] = p;
            to->npins++;
        }
    }
    PyMem_Free(from->pins);
    from->pins = NULL;
    from->npins = from->pinmask = 0;
}

static void
pins_drop(le_owner_t *o)
{
    PyObject **pins = o->pins;
    size_t j, mask = o->pinmask;
    o->pins = NULL;
    o->npins = o->pinmask = 0;
    if (pins != NULL) {
        for (j = 0; j <= mask; j++) Py_XDECREF(pins[j]);
        PyMem_Free(pins);
    }
}

/* releases everything 'o' holds but its references, which it keeps: a
   dealloc may run any code, so they are dropped last. A block listed in
   'mergepoints' stays listed, since the report dies with it. */
static void
owner_free(le_owner_t *o, PyObject *mergepoints, PyObject *entry_point)
{
    le_owner_t dead;
    int i;
    LE_OWNER_ENTER(o);
    /* a block compiled in an inlined frame is listed at the callee's join
       point; the callee is held until here (psyfunc.c) */
    for (i = 0; i < o->nbufs; i++) {
        CodeBufferObject *b = o->bufs[i];
        if (o->parent != NULL)
            amap_remove(&group_blocks, b);
        if (b->ge != NULL && !psyco_ge_in_report(mergepoints, b->ge))
            codebuf_unlist(b);
    }
    /* what the records, buffers and snapshots release comes back here */
    psyco_owner_release_records(o);
    Py_XDECREF(entry_point);
    Py_XDECREF(mergepoints);
    for (i = 0; i < o->nbufs; i++) Py_DECREF(o->bufs[i]);
    LE_OWNER_LEAVE();
    dead = *o;
    memset(o, 0, sizeof(*o));
    o->pins = dead.pins;
    o->npins = dead.npins;
    o->pinmask = dead.pinmask;
    for (i = 0; i < dead.nextents; i++) {
        code_t *s = dead.extents[i].start, *e = dead.extents[i].end;
#if ALL_CHECKS
        memset(s, 0xCC, e - s); /* INT 3: a stale jump traps */
#endif
        LE_STAT(psyco_stat_code_bytes_live -= e - s);
        free_insert(s, e);
        free_release_pages(s, e);
    }
#if ALL_CHECKS
    made_forget(o, &dead);
    PyMem_Free(dead.relocs);
#endif
    PyMem_Free(dead.extents);
    PyMem_Free(dead.bufs);
    psyco_release_notes(dead.notes, dead.nnotes);
    PyMem_Free(dead.notes);
    while (dead.blocks != NULL) {
        void *next = *(void **)dead.blocks;
        PyMem_Free(dead.blocks);
        dead.blocks = next;
    }
}

/* frees 'g', its references moved into 'pins_to'; 'forget': its site
   lives on */
static void
group_free(le_group_t *g, PyObject *mergepoints, le_owner_t *pins_to,
           bool forget)
{
    if (forget && g->site != NULL)
        psyco_promotion_forget(g->site, (long)g->key);
    Py_XINCREF(mergepoints); /* owner_free releases it */
    owner_free(&g->owner, mergepoints, NULL);
    pins_take(pins_to, &g->owner);
    PyMem_Free(g->deps);
    le_owners_counted--;
    LE_STAT(psyco_stat_groups_freed++);
    PyMem_Free(g);
}

DEFINEFN void
psyco_owner_release(le_owner_t *o, PyObject *mergepoints,
                    PyObject *entry_point)
{
    /* its groups first, the newest first, so that a group is freed
       before the group its site is in; their sites go too */
    while (o->groups != NULL) {
        le_group_t *g = o->groups;
        o->groups = g->next;
        group_free(g, mergepoints, o, false);
    }
    owner_free(o, mergepoints, entry_point);
    pins_drop(o); /* last, since a dealloc may run any code */
}

/*** Records per address ***/

struct le_addrmap_item_s {
    void *key, *val;
};

/* linear probing; a removal shifts the rest of the run back, so there are
   no tombstones */
static size_t
amap_home(le_addrmap_t *m, void *key)
{
    return (((size_t)key >> 3) * (size_t)0x9E3779B97F4A7C15ULL >> 20) &
           m->mask;
}

static void *
amap_get(le_addrmap_t *m, void *key)
{
    size_t i;
    if (m->items == NULL)
        return NULL;
    for (i = amap_home(m, key); m->items[i].key != NULL; i = (i + 1) & m->mask)
        if (m->items[i].key == key)
            return m->items[i].val;
    return NULL;
}

/* room for one more; -1 if out of memory */
static int
amap_reserve(le_addrmap_t *m)
{
    struct le_addrmap_item_s *old = m->items;
    size_t i, oldsize = old == NULL ? 0 : m->mask + 1;
    size_t size = oldsize ? 2 * oldsize : 64;
    if (2 * (m->n + 1) <= oldsize)
        return 0;
    m->items = PyMem_Calloc(size, sizeof(*old));
    if (m->items == NULL) {
        m->items = old;
        return -1;
    }
    m->mask = size - 1;
    for (i = 0; i < oldsize; i++)
        if (old[i].key != NULL) {
            size_t j = amap_home(m, old[i].key);
            while (m->items[j].key != NULL) j = (j + 1) & m->mask;
            m->items[j] = old[i];
        }
    PyMem_Free(old);
    return 0;
}

/* after amap_reserve; 'key' must be new */
static void
amap_put(le_addrmap_t *m, void *key, void *val)
{
    size_t i = amap_home(m, key);
    while (m->items[i].key != NULL) i = (i + 1) & m->mask;
    m->items[i].key = key;
    m->items[i].val = val;
    m->n++;
}

static void
amap_remove(le_addrmap_t *m, void *key)
{
    size_t i, j;
    if (m->items == NULL)
        return;
    for (i = amap_home(m, key); m->items[i].key != key; i = (i + 1) & m->mask)
        if (m->items[i].key == NULL)
            return;
    m->items[i].key = NULL;
    m->n--;
    /* move back each later item of the run whose home isn't after the hole */
    for (j = (i + 1) & m->mask; m->items[j].key != NULL;
         j = (j + 1) & m->mask) {
        size_t h = amap_home(m, m->items[j].key);
        if (i < j ? (h <= i || h > j) : (h <= i && h > j)) {
            m->items[i] = m->items[j];
            m->items[j].key = NULL;
            i = j;
        }
    }
}

typedef struct {
    PyObject_HEAD le_addrmap_t *map;
    void *key;
    PyObject *value;
} record_t;

/* a GC type for its tp_traverse, which the owner scan follows; never
   tracked, since only owners hold records */
static void
record_dealloc(record_t *r)
{
    amap_remove(r->map, r->key);
    Py_XDECREF(r->value);
    PyObject_GC_Del(r);
}

static int
record_traverse(record_t *r, visitproc visit, void *arg)
{
    Py_VISIT(r->value);
    return 0;
}

static PyTypeObject Record_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "lejit.record",
    .tp_basicsize = sizeof(record_t),
    .tp_dealloc = (destructor)record_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)record_traverse,
};

DEFINEFN PyObject **
psyco_record(le_addrmap_t *map, void *key)
{
    record_t *r = amap_get(map, key);
    if (r != NULL) {
        psyco_owner_pin((PyObject *)r);
        return &r->value;
    }
    if (amap_reserve(map) < 0)
        OUT_OF_MEMORY();
    r = PyObject_GC_New(record_t, &Record_Type);
    if (r == NULL)
        OUT_OF_MEMORY();
    r->map = map;
    r->key = key;
    r->value = NULL;
    amap_put(map, key, r);
    psyco_owner_keep((PyObject *)r);
    return &r->value;
}

/*** Cycles through code objects ***/

/* Code objects aren't tracked by CPython's GC, so it can't see that an
   owner's references belong to its code object: a cycle through one, such
   as a function whose code holds a cached global that leads back to it
   (an exec'd namespace, a namedtuple class), looks alive for good. This
   scan finds such owners by trial deletion over what owners reach. Each
   object reached (one with a tp_traverse, or a code object with a record,
   its references being its owner's set) has the references the others
   hold subtracted from its count; one with a reference left is held from
   elsewhere, so it and all it reaches are alive. A code object not found
   alive that way is garbage: no frame runs it (a frame holds its code), no
   live caller's code jumps into it (a caller holds what it calls or
   inlines), and every reference its code relies on is in its owner's set
   (constants, sk_new; records, psyco_record). Its record is taken off it
   and freed with its owner, so that a finaliser calling it compiles it
   again; CPython's GC then frees the rest of the cycle. What the scan
   doesn't follow (module objects and their dicts, where it stops to stay
   small, or a reference held outside an owner's set) only makes more look
   alive.
   Records are counted when their owner first holds a reference
   (psyco_owner_keep): one that holds none can't keep anything alive. Once
   half as many have been counted since the last scan as there are (and at
   least 512, more than a short program makes), the scan is asked for as
   a pending call (Py_AddPendingCall), so it runs between bytecodes, in the
   main interpreter and never during a compile; its cost per record stays
   the same and dead records are at most about half the live. It also runs
   at a full collection (gc.collect()) if a record was counted since the
   last: a gc.callbacks entry, listed only once the program has imported
   gc (as any program calling gc.collect() has), and only while there are
   such records, acts on generation 2 only. Importing gc for it would run
   importlib's Python code, which LeJIT compiles, in every program.
   A group (codemanager.h) is a node too, held by nothing; one of a dead
   code object dies with it. Otherwise it is alive if an alive group
   (of any code object) depends on it or is under it, if code that is no
   group's jumps into it, if a frame runs in its code on any thread, or may
   (frames.c:psyco_frames_running, mark_running), or if every key from it up to
   its code object is alive. That is decided after the rest, until no more
   groups come alive; the dead ones of live code objects are freed, each
   before the one its site is in, and their references dropped last.
   Groups are counted with the records. */

#if ALL_CHECKS
DEFINEVAR long psyco_stat_owner_scans = 0;
DEFINEVAR long psyco_stat_owners_collected = 0;
DEFINEVAR long psyco_stat_groups_made = 0;
DEFINEVAR long psyco_stat_groups_freed = 0;
#endif

DEFINEVAR long le_owners_counted = 0;
static long extras_since_scan = 0; /* records counted since the last scan */
#define SCAN_MIN 512               /* records counted before the first scan */
static long scan_after = SCAN_MIN; /* more after a scan too big */
static bool scan_asked = false;    /* the pending call is queued */
static PyObject *gc_callbacks = NULL, *scan_callback = NULL;

#define SCAN_MAX_NODES ((size_t)1 << 20)
enum { NODE_GC, NODE_CODE, NODE_PRUNED, NODE_GROUP };
typedef struct {
    PyObject *o; /* a group's le_group_t */
    Py_ssize_t refs;
    size_t root; /* a group's: its code object's node */
    char kind, alive;
} node_t;

static struct {
    node_t *nodes;
    size_t n, cap;
    le_addrmap_t index; /* object -> node number + 1 */
    size_t *stack;
    size_t nstack, stackcap;
    le_addrmap_t running; /* code objects a frame runs somewhere: -> 1 */
    le_addrmap_t placed;  /* groups a frame runs in: -> 1 */
    bool failed, unknown; /* unknown: which frames run is not known */
} scan;

/* every code object with a record is a node, so that one only garbage
   reaches is found to be garbage too, whatever its owner holds */
static bool
has_record(PyObject *o)
{
    return le_code_extra_get((PyCodeObject *)o) != NULL;
}

static le_owner_t *
node_owner(size_t i)
{
    node_t *nd = &scan.nodes[i];
    if (nd->kind == NODE_GROUP)
        return &((le_group_t *)nd->o)->owner;
    return &le_code_extra_get((PyCodeObject *)nd->o)->owner;
}

/* the node of 'o', made if it is one; -1 if it isn't or the scan failed */
static long
scan_node(PyObject *o, char kind)
{
    size_t i = (size_t)amap_get(&scan.index, o);
    if (i != 0)
        return i - 1;
    if (kind == NODE_GC || kind == NODE_CODE) {
        if (PyCode_Check(o) && has_record(o))
            kind = NODE_CODE;
        else if (PyObject_IS_GC(o) && Py_TYPE(o)->tp_traverse != NULL)
            kind = NODE_GC;
        else
            return -1;
    }
    if (scan.n == scan.cap) {
        size_t cap = scan.cap ? 2 * scan.cap : 1024;
        node_t *n = cap > SCAN_MAX_NODES
                        ? NULL
                        : PyMem_Realloc(scan.nodes, cap * sizeof(node_t));
        if (n == NULL) {
            scan.failed = true;
            return -1;
        }
        scan.nodes = n;
        scan.cap = cap;
    }
    if (amap_reserve(&scan.index) < 0) {
        scan.failed = true;
        return -1;
    }
    amap_put(&scan.index, o, (void *)(scan.n + 1));
    scan.nodes[scan.n].o = o;
    /* nothing but its code object's list refers to a group */
    scan.nodes[scan.n].refs = kind == NODE_GROUP ? 0 : Py_REFCNT(o);
    scan.nodes[scan.n].root = 0;
    scan.nodes[scan.n].kind = kind;
    scan.nodes[scan.n].alive = false;
    return scan.n++;
}

/* calls 'visit' on every reference node 'i' holds that the scan follows */
static void
scan_edges(size_t i, visitproc visit)
{
    node_t *nd = &scan.nodes[i];
    if (nd->kind == NODE_CODE || nd->kind == NODE_GROUP) {
        le_owner_t *o = node_owner(i);
        size_t j;
        for (j = 0; o->pins != NULL && j <= o->pinmask; j++)
            if (o->pins[j] != NULL)
                visit(o->pins[j], NULL);
    }
    else if (nd->kind == NODE_GC)
        Py_TYPE(nd->o)->tp_traverse(nd->o, visit, NULL);
}

static int
visit_count(PyObject *o, void *unused)
{
    long i = scan_node(o, NODE_GC);
    if (i >= 0)
        scan.nodes[i].refs--;
    return 0;
}

static void
push_alive(size_t i)
{
    if (scan.nodes[i].alive)
        return;
    scan.nodes[i].alive = true;
    if (scan.nstack == scan.stackcap) {
        size_t cap = scan.stackcap ? 2 * scan.stackcap : 1024;
        size_t *s = PyMem_Realloc(scan.stack, cap * sizeof(size_t));
        if (s == NULL) {
            scan.failed = true;
            return;
        }
        scan.stack = s;
        scan.stackcap = cap;
    }
    scan.stack[scan.nstack++] = i;
}

static int
visit_alive(PyObject *o, void *unused)
{
    size_t i = (size_t)amap_get(&scan.index, o);
    if (i != 0)
        push_alive(i - 1);
    return 0;
}

/* everything alive reaches is alive. A group alive keeps the groups its
   code jumps into, and the one its site is in. */
static void
propagate(void)
{
    while (scan.nstack > 0 && !scan.failed) {
        size_t i = scan.stack[--scan.nstack];
        scan_edges(i, visit_alive);
        if (scan.nodes[i].kind == NODE_GROUP) {
            le_group_t *g = (le_group_t *)scan.nodes[i].o;
            int j;
            if (g->owner.parent->parent != NULL)
                visit_alive((PyObject *)g->owner.parent, NULL);
            for (j = 0; j < g->ndeps; j++)
                visit_alive((PyObject *)g->deps[j], NULL);
        }
    }
}

static void
map_mark(le_addrmap_t *m, void *key)
{
    if (amap_get(m, key) != NULL)
        return;
    if (amap_reserve(m) < 0)
        scan.unknown = true;
    else
        amap_put(m, key, (void *)1);
}

static bool
owner_holds_code(le_owner_t *o, code_t *p)
{
    int k;
    for (k = 0; k < o->nextents; k++)
        if (o->extents[k].start <= p && p < o->extents[k].end)
            return true;
    return false;
}

/* A compiled frame runs code of its code object, or of a frame inlined in
   it: where it goes on, 'at', is in that code object's own code or in one
   of its groups, which only then is kept for it. Where 'at' is not known,
   or not found, every group of those code objects is. */
static void
mark_running(PyCodeObject **cos, int ncos, code_t *at, void *unused)
{
    int k;
    if (at != NULL) {
        /* the call ends at 'at': its last byte is before it */
        code_t *p = at - 1;
        for (k = 0; k < ncos; k++) {
            le_code_extra_t *x = le_code_extra_get(cos[k]);
            le_group_t *g;
            if (x == NULL)
                continue;
            if (owner_holds_code(&x->owner, p))
                return;
            for (g = x->owner.groups; g != NULL; g = g->next)
                if (owner_holds_code(&g->owner, p)) {
                    map_mark(&scan.placed, g);
                    return;
                }
        }
    }
    for (k = 0; k < ncos; k++) map_mark(&scan.running, cos[k]);
}

/* can the group's code be reached? Its site is reached while every key
   from it up is alive; a frame may be in it (mark_running). */
static bool
group_reached(size_t i)
{
    le_group_t *g = (le_group_t *)scan.nodes[i].o;
    le_owner_t *p;
    if (g->held || scan.unknown || amap_get(&scan.placed, g) != NULL ||
        amap_get(&scan.running, scan.nodes[scan.nodes[i].root].o) != NULL)
        return true;
    for (p = &g->owner; p->parent != NULL; p = p->parent) {
        size_t k = (size_t)amap_get(&scan.index, ((le_group_t *)p)->key);
        if (k != 0 && !scan.nodes[k - 1].alive)
            return false;
        /* a key the scan doesn't follow counts as alive */
    }
    return true;
}

/* the garbage code objects, as new references, in *out, and the dead
   groups of the others in *groups; their count (and *ngroups), or -1 if
   the scan didn't finish */
static long
scan_find(PyCodeObject ***out, le_group_t ***groups, long *ngroups)
{
    le_code_extra_t *x;
    PyObject *modules = le_modules_dict();
    Py_ssize_t pos = 0;
    PyObject *k, *m;
    size_t i;
    long n = 0, ng = 0;
    bool changed;
    while (modules != NULL && PyDict_Check(modules) &&
           PyDict_Next(modules, &pos, &k, &m))
        if (PyModule_Check(m)) {
            scan_node(m, NODE_PRUNED);
            if (PyModule_GetDict(m) != NULL)
                scan_node(PyModule_GetDict(m), NODE_PRUNED);
        }
    /* every code object whose code holds something */
    for (x = le_code_extras; x != NULL; x = x->next)
        if (x->owner.npins > 0 || x->owner.groups != NULL)
            scan_node((PyObject *)le_code_extra_code(x), NODE_CODE);
    for (i = 0; i < scan.n && !scan.failed; i++) {
        scan_edges(i, visit_count);
        if (scan.nodes[i].kind == NODE_CODE) {
            le_group_t *g;
            for (g = node_owner(i)->groups; g != NULL; g = g->next) {
                long j = scan_node((PyObject *)g, NODE_GROUP);
                if (j >= 0)
                    scan.nodes[j].root = i;
            }
        }
    }
    for (i = 0; i < scan.n && !scan.failed; i++)
        if (scan.nodes[i].refs > 0 || scan.nodes[i].kind == NODE_PRUNED)
            push_alive(i);
    propagate();
    /* then the groups reached, until no more are */
    if (scan.n > 0 && psyco_frames_running(mark_running, NULL, true) < 0)
        scan.unknown = true;
    do {
        changed = false;
        for (i = 0; i < scan.n && !scan.failed; i++)
            if (scan.nodes[i].kind == NODE_GROUP && !scan.nodes[i].alive &&
                scan.nodes[scan.nodes[i].root].alive && group_reached(i)) {
                push_alive(i);
                propagate();
                changed = true;
            }
    } while (changed && !scan.failed);
    if (scan.failed)
        return -1;
    for (i = 0; i < scan.n; i++)
        if (!scan.nodes[i].alive) {
            if (scan.nodes[i].kind == NODE_CODE)
                n++;
            else if (scan.nodes[i].kind == NODE_GROUP &&
                     scan.nodes[scan.nodes[i].root].alive)
                ng++;
        }
    if ((n > 0 && (*out = PyMem_Malloc(n * sizeof(PyCodeObject *))) == NULL) ||
        (ng > 0 &&
         (*groups = PyMem_Malloc(ng * sizeof(le_group_t *))) == NULL))
        return -1;
    n = ng = 0;
    /* a code object's groups are nodes in its list's order, the newest
       first: a group before the group its site is in */
    for (i = 0; i < scan.n; i++)
        if (!scan.nodes[i].alive) {
            if (scan.nodes[i].kind == NODE_CODE) {
                Py_INCREF(scan.nodes[i].o);
                (*out)[n++] = (PyCodeObject *)scan.nodes[i].o;
            }
            else if (scan.nodes[i].kind == NODE_GROUP &&
                     scan.nodes[scan.nodes[i].root].alive)
                (*groups)[ng++] = (le_group_t *)scan.nodes[i].o;
        }
    *ngroups = ng;
    return n;
}

/* takes the dead groups off their code objects' lists */
static void
groups_unlist(le_group_t **dead, long n)
{
    long i;
    for (i = 0; i < n; i++) dead[i]->dead = 1;
    for (i = 0; i < n; i++) {
        le_group_t **p = &owner_root(&dead[i]->owner)->groups;
        while (*p != NULL)
            if ((*p)->dead)
                *p = (*p)->next;
            else
                p = &(*p)->next;
    }
}

static void
owners_scan(void)
{
    PyCodeObject **dead_co = NULL;
    le_code_extra_t **dead = NULL;
    le_group_t **dead_groups = NULL;
    PyObject **reports = NULL;
    le_owner_t dropped;
    long i, ng = 0, n = scan_find(&dead_co, &dead_groups, &ng);
    PyMem_Free(scan.nodes);
    PyMem_Free(scan.stack);
    PyMem_Free(scan.index.items);
    PyMem_Free(scan.running.items);
    PyMem_Free(scan.placed.items);
    memset(&scan, 0, sizeof(scan));
    LE_STAT(psyco_stat_owner_scans++);
    if (n < 0) {
        PyMem_Free(dead_co);
        PyMem_Free(dead_groups);
        n = ng = 0;
        scan_after = 2 * (scan_after > le_owners_counted ? scan_after
                                                         : le_owners_counted);
    }
    else
        scan_after = le_owners_counted / 2 > SCAN_MIN ? le_owners_counted / 2
                                                      : SCAN_MIN;
    /* the dead groups go first, each before the group its site is in;
       their references are dropped last */
    memset(&dropped, 0, sizeof(dropped));
    if (ng > 0) {
        groups_unlist(dead_groups, ng);
        for (i = 0; i < ng; i++)
            group_free(dead_groups[i], NULL, &dropped, true);
    }
    PyMem_Free(dead_groups);
    if (n > 0)
        dead = PyMem_Malloc(n * sizeof(le_code_extra_t *));
    if (n > 0 && dead != NULL &&
        (reports = PyMem_Malloc(n * sizeof(PyObject *))) == NULL) {
        PyMem_Free(dead);
        dead = NULL;
    }
    if (dead == NULL) {
        for (i = 0; i < n; i++)
            Py_DECREF(dead_co[i]); /* found again by the next scan */
        n = 0;
    }
    /* all records are taken off their code objects before any reference is
       dropped, so that code a finaliser runs is compiled afresh */
    for (i = 0; i < n; i++) dead[i] = le_code_extra_detach(dead_co[i]);
    /* a block of one may be listed in another's report (an inlined
       callee's join point), so every report lives until all are released */
    for (i = 0; i < n; i++) {
        reports[i] = dead[i]->merge_points;
        Py_XINCREF(reports[i]);
    }
    for (i = 0; i < n; i++) le_code_extra_free(dead[i]);
    for (i = 0; i < n; i++) Py_XDECREF(reports[i]);
    PyMem_Free(reports);
    LE_STAT(psyco_stat_owners_collected += n);
    PyMem_Free(dead);
    for (i = 0; i < n; i++) Py_DECREF(dead_co[i]);
    PyMem_Free(dead_co);
    pins_drop(&dropped);
}

/*** The sweep: compiled code not used lately is released ***/

/* After every full collection, the compiled code of each code object that
   no frame has entered through the hook (eval_hook.c:le_code_to_run,
   le_gen_to_run) or was found running in for SWEEP_COLD sweeps is
   released with everything its owner and groups hold, as if the code
   object had died, unless code that stays may jump into it: code that
   stays keeps the code of every code object it pins (compiled code holds
   the code objects whose code it calls or inlines) and of every group it
   depends on, so cold code objects that pin each other go together. The
   next call compiles afresh. Code released SWEEP_RELEASES times stays.
   A full collection is told by an object of LeJIT's own that the
   collector tracks: once it is old, only a full collection traverses it,
   and its traverse asks for the sweep as a pending call, which runs
   between bytecodes in the main interpreter, as the owner scan's does
   (owners_pending). Unlike a gc.callbacks entry, it costs nothing at the
   other collections and needs no import of gc. */
#define SWEEP_COLD 4
#define SWEEP_RELEASES 3

#if ALL_CHECKS
DEFINEVAR long le_stat_sweeps[3]; /* sweeps run, code released, its bytes */
#endif

/* has code the sweep may release once it is cold */
static bool
sweep_may_release(le_code_extra_t *x)
{
    return x->owner.nbufs > 0 && x->entry_point != Py_None &&
           x->releases < SWEEP_RELEASES && x != le_gen_interpreted_x;
}

static void
sweep_running(PyCodeObject **cos, int ncos, code_t *at, void *unused)
{
    int k;
    for (k = 0; k < ncos; k++) {
        le_code_extra_t *x = le_code_extra_get(cos[k]);
        if (x != NULL)
            x->cold = 0;
    }
}

/* 'x' stays; so does 'y', onto 'stack', if it was to go */
static void
sweep_keep_one(le_code_extra_t *y, le_code_extra_t **stack, size_t *n)
{
    if (y != NULL && y->swept) {
        y->swept = 0;
        stack[(*n)++] = y;
    }
}

static void
sweep_keep_pins(le_owner_t *o, le_code_extra_t **stack, size_t *n)
{
    size_t j;
    for (j = 0; o->pins != NULL && j <= o->pinmask; j++)
        if (o->pins[j] != NULL && PyCode_Check(o->pins[j]))
            sweep_keep_one(le_code_extra_get((PyCodeObject *)o->pins[j]),
                           stack, n);
}

static void
sweep_keep(le_code_extra_t *x, le_code_extra_t **stack, size_t *n)
{
    le_group_t *g;
    int i;
    sweep_keep_pins(&x->owner, stack, n);
    for (g = x->owner.groups; g != NULL; g = g->next) {
        sweep_keep_pins(&g->owner, stack, n);
        for (i = 0; i < g->ndeps; i++) {
            le_owner_t *root = &g->deps[i]->owner;
            while (root->parent != NULL) root = root->parent;
            sweep_keep_one(
                (le_code_extra_t *)((char *)root -
                                    offsetof(le_code_extra_t, owner)),
                stack, n);
        }
    }
}

static bool sweep_asked = false; /* the pending call is queued */

static void
sweep(void)
{
    le_code_extra_t *x, **cand;
    PyObject **reports;
    le_owner_t dropped;
    size_t n = 0, ncand = 0, nstack = 0, i, k;
    LE_STAT(le_stat_sweeps[0]++);
    for (x = le_code_extras; x != NULL; x = x->next) {
        if (x->cold < SWEEP_COLD)
            x->cold++;
        n++;
    }
    if (psyco_frames_running(sweep_running, NULL, false) < 0)
        return;
    for (x = le_code_extras; x != NULL; x = x->next)
        if (x->cold == SWEEP_COLD && sweep_may_release(x))
            ncand++;
    if (ncand == 0)
        return;
    ncand = 0;
    /* the candidates, then a stack of those found to stay */
    cand = PyMem_Malloc(2 * n * sizeof(*cand));
    if (cand == NULL)
        return;
    for (x = le_code_extras; x != NULL; x = x->next) {
        if (x->cold < SWEEP_COLD || !sweep_may_release(x))
            continue;
        x->swept = 1;
        cand[ncand++] = x;
    }
    if (ncand == 0) {
        PyMem_Free(cand);
        return;
    }
    for (x = le_code_extras; x != NULL; x = x->next)
        if (!x->swept)
            sweep_keep(x, cand + n, &nstack);
    while (nstack > 0) sweep_keep(cand[n + --nstack], cand + n, &nstack);
    for (i = k = 0; i < ncand; i++)
        if (cand[i]->swept)
            cand[k++] = cand[i];
    ncand = k;
    /* each one's report and entry point */
    reports = ncand > 0 ? PyMem_Malloc(2 * ncand * sizeof(PyObject *)) : NULL;
    if (reports == NULL) {
        for (i = 0; i < ncand; i++) cand[i]->swept = 0;
        PyMem_Free(cand);
        return;
    }
    /* as owners_scan frees records: all are emptied before any is
       released, so that code a dealloc runs compiles afresh, and every
       report lives until all are released; their references are dropped
       last */
    for (i = 0; i < ncand; i++) {
        le_code_extra_reset(cand[i], &reports[i], &reports[ncand + i]);
        Py_XINCREF(reports[i]);
    }
    memset(&dropped, 0, sizeof(dropped));
    for (i = 0; i < ncand; i++) {
        le_owner_t *o = &cand[i]->owner;
        LE_STAT(le_stat_sweeps[1]++);
#if ALL_CHECKS
        {
            le_group_t *g;
            int e;
            for (e = 0; e < o->nextents; e++)
                le_stat_sweeps[2] += o->extents[e].end - o->extents[e].start;
            for (g = o->groups; g != NULL; g = g->next)
                for (e = 0; e < g->owner.nextents; e++)
                    le_stat_sweeps[2] +=
                        g->owner.extents[e].end - g->owner.extents[e].start;
        }
#endif
        while (o->groups != NULL) {
            le_group_t *g = o->groups;
            o->groups = g->next;
            group_free(g, reports[i], o, false);
        }
        owner_free(o, reports[i], reports[ncand + i]);
        pins_take(&dropped, o);
        psyco_owner_untrack(cand[i]);
        cand[i]->swept = 0;
        cand[i]->releases++;
    }
    for (i = 0; i < ncand; i++) Py_XDECREF(reports[i]);
    PyMem_Free(reports);
    PyMem_Free(cand);
    pins_drop(&dropped);
}

static int
sweep_pending(void *arg)
{
    PyObject *t, *v, *tb;
    sweep_asked = false;
    if (PyThreadState_GET()->interp != PyInterpreterState_Main() ||
        psyco_owner != NULL || le_compile_depth != 0 || nopen != 0 ||
        psyco_compile_guard != NULL || le_is_finalizing())
        return 0;
    PyErr_Fetch(&t, &v, &tb);
    sweep();
    PyErr_Restore(t, v, tb);
    return 0;
}

static int
sweep_clock_traverse(PyObject *self, visitproc visit, void *arg)
{
    PyObject *t, *v, *tb;
    if (sweep_asked || le_is_finalizing())
        return 0;
    /* some versions set an error when the call can't be added */
    PyErr_Fetch(&t, &v, &tb);
    if (Py_AddPendingCall(sweep_pending, NULL) == 0)
        sweep_asked = true;
    else
        PyErr_Clear();
    PyErr_Restore(t, v, tb);
    return 0;
}

static PyTypeObject SweepClock_Type = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "lejit._core.sweep_clock",
    .tp_basicsize = sizeof(PyObject),
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = sweep_clock_traverse,
};

static int
owners_pending(void *arg)
{
    PyObject *t, *v, *tb;
    scan_asked = false;
    /* pending calls run in whatever interpreter the main thread is in; one
       skipped is asked for again by the next record counted */
    if (PyThreadState_GET()->interp != PyInterpreterState_Main() ||
        psyco_owner != NULL || le_compile_depth != 0 || nopen != 0 ||
        psyco_compile_guard != NULL || le_is_finalizing())
        return 0;
    extras_since_scan = 0;
    PyErr_Fetch(&t, &v, &tb);
    owners_scan();
    PyErr_Restore(t, v, tb);
    return 0;
}

/* whether func is one of LeJIT's own pending calls */
DEFINEFN bool
le_pending_is_own(int (*func)(void *))
{
    return func == sweep_pending || func == owners_pending;
}

static bool
scan_listed_last(void)
{
    Py_ssize_t n = PyList_GET_SIZE(gc_callbacks);
    return n > 0 && PyList_GET_ITEM(gc_callbacks, n - 1) == scan_callback;
}

static PyObject *
owners_gc_callback(PyObject *self, PyObject *args)
{
    const char *phase;
    PyObject *info, *gen;
    if (!PyArg_ParseTuple(args, "sO", &phase, &info))
        return NULL;
    /* gc.callbacks are shared by every interpreter */
    if (PyThreadState_GET()->interp != PyInterpreterState_Main())
        Py_RETURN_NONE;
    if (strcmp(phase, "start") == 0) {
        gen = PyDict_Check(info) ? PyDict_GetItemString(info, "generation")
                                 : NULL;
        if (extras_since_scan > 0 && gen != NULL && PyLong_Check(gen) &&
            PyLong_AsLong(gen) == 2 && psyco_owner == NULL &&
            le_compile_depth == 0 && nopen == 0 &&
            psyco_compile_guard == NULL && !le_is_finalizing()) {
            extras_since_scan = 0;
            owners_scan();
        }
    }
    else if (extras_since_scan == 0 && scan_listed_last()) {
        /* the last one, so that removing it skips no other callback */
        Py_ssize_t n = PyList_GET_SIZE(gc_callbacks);
        if (PyList_SetSlice(gc_callbacks, n - 1, n, NULL) < 0)
            PyErr_Clear();
    }
    Py_RETURN_NONE;
}

static PyMethodDef owners_gc_callback_def = {
    "le_owner_scan", owners_gc_callback, METH_VARARGS, NULL};

/* lists the callback if gc has been imported */
static void
scan_callback_list(void)
{
    PyObject *t, *v, *tb;
    PyErr_Fetch(&t, &v, &tb);
    if (scan_callback == NULL) {
        PyObject *modules = le_modules_dict();
        PyObject *gc =
            modules != NULL ? PyDict_GetItemString(modules, "gc") : NULL;
        if (gc == NULL || !PyModule_Check(gc))
            goto done;
        gc_callbacks = PyObject_GetAttrString(gc, "callbacks");
        if (gc_callbacks == NULL || !PyList_Check(gc_callbacks) ||
            (scan_callback = PyCFunction_New(&owners_gc_callback_def, NULL)) ==
                NULL) {
            Py_CLEAR(gc_callbacks);
            goto done;
        }
    }
    if (!scan_listed_last() &&
        PySequence_Contains(gc_callbacks, scan_callback) == 0)
        PyList_Append(gc_callbacks, scan_callback);
done:
    PyErr_Clear(); /* out of memory: listed with a later record */
    PyErr_Restore(t, v, tb);
}

/* one more record or group counted */
static void
owners_count_one(void)
{
    le_owners_counted++;
    extras_since_scan++;
    if (scan_callback == NULL || !scan_listed_last())
        scan_callback_list();
    if (extras_since_scan >= scan_after && !scan_asked &&
        Py_AddPendingCall(owners_pending, NULL) == 0)
        scan_asked = true; /* a full queue: asked again by the next record */
}

DEFINEFN void
psyco_owner_track(le_code_extra_t *x)
{
    if (x->counted)
        return;
    x->counted = 1;
    owners_count_one();
}

/* the dealloc of a class made by Python code (a class statement or
   type()): one a C extension made has its own */
static destructor python_class_dealloc;

static bool
has_locals(PyObject *qualname)
{
    static PyObject *marker;
    if (marker == NULL &&
        (marker = PyUnicode_InternFromString("<locals>")) == NULL) {
        PyErr_Clear();
        return false;
    }
    return PyUnicode_Find(qualname, marker, 0, PyUnicode_GET_LENGTH(qualname),
                          1) >= 0;
}

/* Does 'o' look as if it may die while code specialised on it lives? A
   function or class made inside a function ('<locals>' in its qualified
   name), or a class made by Python code that its module doesn't name (by
   type(), or replaced); not one a module names, which lives as long as
   it. Getting it wrong costs memory, never correctness. Only dict
   lookups, so no Python code runs. */
static bool
looks_short_lived(PyObject *o)
{
    PyTypeObject *tp = (PyTypeObject *)o;
    PyObject *qualname, *module, *d, *found = NULL;
    Py_ssize_t start = 0, i, n;
    if (PyFunction_Check(o)) {
        qualname = ((PyFunctionObject *)o)->func_qualname;
        return qualname != NULL && PyUnicode_Check(qualname) &&
               has_locals(qualname);
    }
    if (!PyType_Check(o) || !(tp->tp_flags & Py_TPFLAGS_HEAPTYPE) ||
        tp->tp_dealloc != python_class_dealloc)
        return false;
    qualname = ((PyHeapTypeObject *)tp)->ht_qualname;
    module = PyDict_GetItemString(le_type_dict(tp), "__module__");
    if (qualname == NULL || !PyUnicode_Check(qualname) || module == NULL ||
        !PyUnicode_Check(module) || has_locals(qualname))
        return true;
    d = le_modules_dict();
    if (d != NULL && PyDict_CheckExact(d))
        found = PyDict_GetItem(d, module);
    if (found == NULL || !PyModule_Check(found))
        return true;
    /* the dotted name, from the module's dict through classes' */
    n = PyUnicode_GET_LENGTH(qualname);
    d = PyModule_GetDict(found);
    for (i = 0; i <= n; i++) {
        PyObject *part;
        if (i < n && PyUnicode_READ_CHAR(qualname, i) != '.')
            continue;
        if (d == NULL || !PyDict_CheckExact(d))
            return true;
        part = PyUnicode_Substring(qualname, start, i);
        if (part == NULL) {
            PyErr_Clear();
            return true;
        }
        found = PyDict_GetItem(d, part);
        Py_DECREF(part);
        if (found == NULL)
            return true;
        d = PyType_Check(found) ? le_type_dict((PyTypeObject *)found) : NULL;
        start = i + 1;
    }
    return found != o;
}

DEFINEFN le_owner_t *
psyco_group_new(PyObject *key, void *site)
{
    le_owner_t *root;
    le_group_t *g;
    if (psyco_owner == NULL || !looks_short_lived(key))
        return NULL;
    g = PyMem_Calloc(1, sizeof(le_group_t));
    if (g == NULL)
        OUT_OF_MEMORY();
    root = owner_root(psyco_owner);
    g->owner.parent = psyco_owner;
    g->key = key;
    g->site = site;
    g->next = root->groups;
    root->groups = g;
    LE_STAT(psyco_stat_groups_made++);
    owners_count_one();
    return &g->owner;
}

static bool
holds(le_owner_t *o, source_known_t *sk);

/* the current owner, or a group it is under, holds 'sk' */
static bool
chain_holds(source_known_t *sk)
{
    le_owner_t *o;
    for (o = psyco_owner; o != NULL; o = o->parent)
        if (holds(o, sk))
            return true;
    return false;
}

/* a new group, which holds its key and becomes the current owner, for
   'sk' if it is an object that looks short-lived and isn't held yet */
static void
enter_group_for(source_known_t *sk)
{
    le_owner_t *g;
    if (chain_holds(sk) ||
        (g = psyco_group_new((PyObject *)sk->value, NULL)) == NULL)
        return;
    psyco_owner = g;
    psyco_owner_pin((PyObject *)sk->value);
}

DEFINEFN void
psyco_owner_enter_groups(vinfo_array_t *a)
{
    int i;
    for (i = 0; i < a->count; i++) {
        vinfo_t *v = a->items[i];
        if (v == NULL)
            continue;
        if (is_compiletime(v->source))
            enter_group_for(CompileTime_Get(v->source));
        if (v->type_hint != NULL)
            enter_group_for(v->type_hint);
        if (v->array != NullArray)
            psyco_owner_enter_groups(v->array);
    }
}

static bool
holds(le_owner_t *o, source_known_t *sk)
{
    PyObject *p = (PyObject *)sk->value;
    return (sk->refcount1_flags & (SkFlagPyObj | SkFlagBorrowed)) !=
               SkFlagPyObj ||
           (PyType_Check(p) &&
            !(((PyTypeObject *)p)->tp_flags & Py_TPFLAGS_HEAPTYPE)) ||
           (o->pins != NULL && o->pins[pin_slot(o, p)] == p);
}

/* 'o' holds every constant in 'a'; with 'drop', the type hints it doesn't
   hold are dropped (they are only hints) */
static bool
holds_all(le_owner_t *o, vinfo_array_t *a, bool drop)
{
    int i;
    for (i = a->count; i--;) {
        vinfo_t *v = a->items[i];
        if (v == NULL)
            continue;
        if (!drop && is_compiletime(v->source) &&
            !holds(o, CompileTime_Get(v->source)))
            return false;
        if (drop && v->type_hint != NULL && !holds(o, v->type_hint)) {
            sk_decref(v->type_hint);
            v->type_hint = NULL;
        }
        if (v->array != NullArray && !holds_all(o, v->array, drop))
            return false;
    }
    return true;
}

DEFINEFN le_owner_t *
psyco_owner_root_holding(vinfo_array_t *a)
{
    le_owner_t *root;
    if (psyco_owner == NULL || psyco_owner->parent == NULL)
        return NULL;
    root = owner_root(psyco_owner);
    if (!holds_all(root, a, false))
        return NULL;
    holds_all(root, a, true);
    return root;
}

DEFINEFN bool
psyco_owner_would_hold(CodeBufferObject *b)
{
    return (psyco_owner == NULL || psyco_owner->parent == NULL) &&
           group_blocks.n != 0 && amap_get(&group_blocks, b) != NULL;
}

DEFINEFN void
psyco_owner_depend(CodeBufferObject *b)
{
    le_owner_t *o = psyco_owner, *p;
    le_group_t *g, *from;
    int i;
    if (group_blocks.n == 0 || (g = amap_get(&group_blocks, b)) == NULL)
        return;
    for (p = o; p != NULL; p = p->parent)
        if (p == &g->owner)
            return; /* its own, or one of a group it is under */
    if (o == NULL || o->parent == NULL) {
        g->held = 1;
        return;
    }
    from = (le_group_t *)o;
    for (i = 0; i < from->ndeps; i++)
        if (from->deps[i] == g)
            return;
    if (from->ndeps == from->depscap) {
        int cap = from->depscap ? 2 * from->depscap : 2;
        void *n = PyMem_Realloc(from->deps, cap * sizeof(le_group_t *));
        if (n == NULL)
            OUT_OF_MEMORY();
        from->deps = n;
        from->depscap = cap;
    }
    from->deps[from->ndeps++] = g;
}

DEFINEFN void
psyco_owner_untrack(le_code_extra_t *x)
{
    if (x->counted)
        le_owners_counted--;
    x->counted = 0;
}

static void
owners_scan_init(void)
{
    PyObject *c;
    if (PyType_Ready(&Record_Type) < 0)
        PyErr_Clear();
    /* held for good */
    if (PyType_Ready(&SweepClock_Type) == 0) {
        PyObject *clock =
            (PyObject *)PyObject_GC_New(PyObject, &SweepClock_Type);
        if (clock != NULL)
            PyObject_GC_Track(clock);
    }
    PyErr_Clear();
    c = PyObject_CallFunction((PyObject *)&PyType_Type, "s()N", "C",
                              PyDict_New());
    if (c != NULL)
        python_class_dealloc = ((PyTypeObject *)c)->tp_dealloc;
    else
        PyErr_Clear(); /* then no class gets a group */
    Py_XDECREF(c);
}

DEFINEVAR unsigned long psyco_next_serial = 0;

#if LE_CODESTATS
DEFINEVAR PyObject *psyco_codestats_buffers =
    NULL; /* kind -> [count, bytes] */
DEFINEFN void
psyco_codestats_buffer(const char *kind, code_t *start, code_t *end)
{
    long n = end - start;
    PyObject *e;
    if (psyco_codestats_buffers == NULL &&
        (psyco_codestats_buffers = PyDict_New()) == NULL)
        OUT_OF_MEMORY();
    e = PyDict_GetItemString(psyco_codestats_buffers, kind);
    if (e == NULL) {
        e = Py_BuildValue("[ll]", 0L, 0L);
        if (e == NULL ||
            PyDict_SetItemString(psyco_codestats_buffers, kind, e) < 0)
            OUT_OF_MEMORY();
        Py_DECREF(e);
    }
    PyList_SetItem(e, 0,
                   PyLong_FromLong(PyLong_AsLong(PyList_GET_ITEM(e, 0)) + 1));
    PyList_SetItem(e, 1,
                   PyLong_FromLong(PyLong_AsLong(PyList_GET_ITEM(e, 1)) + n));
}
#endif
DEFINEVAR int le_compile_depth = 0;

/*** The compiler's allocations, kept out of tracemalloc ***/

/* Plain CPython makes none of the compiler's allocations, so while
   tracemalloc traces they are made past it, in every domain (pymalloc
   takes large blocks from the raw one): by the allocators it was
   installed over, those seen the last time a compile started while
   nothing traced. A block freed or reallocated is forgotten by
   tracemalloc too, as its own hooks do, so no trace outlives its block.
   Each hook's context is the allocator under it, as tracemalloc's is:
   another thread allocating raw memory without the GIL while the hooks
   are swapped may read one's function with the other's context. */
typedef struct {
    PyMemAllocatorDomain domain;
    PyMemAllocatorEx under, traced;
} untraced_domain_t;
static untraced_domain_t untraced_domains[3] = {
    {PYMEM_DOMAIN_RAW}, {PYMEM_DOMAIN_MEM}, {PYMEM_DOMAIN_OBJ}};
static bool have_under = false, untraced = false;

/* tracemalloc's domain for PyMem and PyObject blocks */
#define LE_TRACEMALLOC_DOMAIN 0

static void *
untraced_malloc(void *ctx, size_t size)
{
    PyMemAllocatorEx *a = ctx;
    return a->malloc(a->ctx, size);
}

static void *
untraced_calloc(void *ctx, size_t nelem, size_t elsize)
{
    PyMemAllocatorEx *a = ctx;
    return a->calloc(a->ctx, nelem, elsize);
}

static void *
untraced_realloc(void *ctx, void *ptr, size_t size)
{
    PyMemAllocatorEx *a = ctx;
    if (ptr != NULL)
        PyTraceMalloc_Untrack(LE_TRACEMALLOC_DOMAIN, (uintptr_t)ptr);
    return a->realloc(a->ctx, ptr, size);
}

static void
untraced_free(void *ctx, void *ptr)
{
    PyMemAllocatorEx *a = ctx;
    if (ptr == NULL)
        return;
    a->free(a->ctx, ptr);
    PyTraceMalloc_Untrack(LE_TRACEMALLOC_DOMAIN, (uintptr_t)ptr);
}

static bool
same_allocator(PyMemAllocatorEx *a, PyMemAllocatorEx *b)
{
    return a->ctx == b->ctx && a->malloc == b->malloc &&
           a->calloc == b->calloc && a->realloc == b->realloc &&
           a->free == b->free;
}

/* _tracemalloc.is_tracing, once _tracemalloc is imported */
static PyObject *is_tracing_fn = NULL;

/* 1 if tracemalloc traces, 0 if not, -1 if that can't be told. It can
   trace with its module not imported only if started as Python started
   (codemanager_init imports it then). */
static int
tracemalloc_tracing(void)
{
    PyObject *t, *v, *tb, *r;
    int result = -1;
    PyErr_Fetch(&t, &v, &tb);
    if (is_tracing_fn == NULL) {
        PyObject *name = PyUnicode_FromString("_tracemalloc");
        PyObject *m = name == NULL ? NULL : PyImport_GetModule(name);
        Py_XDECREF(name);
        if (m != NULL) {
            is_tracing_fn = PyObject_GetAttrString(m, "is_tracing");
            Py_DECREF(m);
        }
        else if (!PyErr_Occurred())
            result = 0;
    }
    if (is_tracing_fn != NULL) {
        r = PyObject_CallObject(is_tracing_fn, NULL);
        if (r != NULL) {
            result = PyObject_IsTrue(r);
            Py_DECREF(r);
        }
    }
    PyErr_Clear();
    PyErr_Restore(t, v, tb);
    return result;
}

DEFINEFN void
le_untraced_begin(void)
{
    PyMemAllocatorEx now[3];
    int i, tracing;
    if (untraced)
        return;
    /* tracemalloc hooks every domain: the object one tells */
    PyMem_GetAllocator(PYMEM_DOMAIN_OBJ, &now[2]);
    if (have_under && same_allocator(&now[2], &untraced_domains[2].under))
        return; /* nothing is installed over them */
    for (i = 0; i < 2; i++)
        PyMem_GetAllocator(untraced_domains[i].domain, &now[i]);
    tracing = tracemalloc_tracing();
    if (tracing == 0) {
        for (i = 0; i < 3; i++) untraced_domains[i].under = now[i];
        have_under = true;
    }
    if (tracing != 1 || !have_under)
        return;
    for (i = 0; i < 3; i++) {
        PyMemAllocatorEx hook = {&untraced_domains[i].under, untraced_malloc,
                                 untraced_calloc, untraced_realloc,
                                 untraced_free};
        untraced_domains[i].traced = now[i];
        PyMem_SetAllocator(untraced_domains[i].domain, &hook);
    }
    untraced = true;
}

DEFINEFN void
le_untraced_end(void)
{
    int i;
    if (!untraced)
        return;
    untraced = false;
    /* tracemalloc's own, unless something replaced the hook meanwhile (a
       tracemalloc.stop() run by the compile) */
    for (i = 0; i < 3; i++) {
        PyMemAllocatorEx now;
        PyMem_GetAllocator(untraced_domains[i].domain, &now);
        if (now.malloc == untraced_malloc)
            PyMem_SetAllocator(untraced_domains[i].domain,
                               &untraced_domains[i].traced);
    }
}

#if ALL_CHECKS
DEFINEVAR long psyco_stat_compile_ns[LE_ENTER_KINDS];
DEFINEVAR long psyco_stat_compile_count[LE_ENTER_KINDS];
#endif
#if ALL_CHECKS || LE_CODESTATS
DEFINEVAR int psyco_stat_compile_kind = 0;

DEFINEFN long
le_now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000L + t.tv_nsec;
}
#endif

/* the blocks registered under the current compile guards, to unregister
   if the compile is abandoned */
static CodeBufferObject **journal = NULL;
static int njournal = 0, journalcap = 0;

static void
journal_add(CodeBufferObject *b)
{
    if (psyco_compile_guard == NULL)
        return;
    if (njournal == journalcap) {
        int cap = journalcap ? 2 * journalcap : 64;
        void *n = PyMem_Realloc(journal, cap * sizeof(CodeBufferObject *));
        if (n == NULL)
            OUT_OF_MEMORY();
        journal = n;
        journalcap = cap;
    }
    journal[njournal++] = b;
}

DEFINEVAR le_compile_guard_t *psyco_compile_guard = NULL;
#if ALL_CHECKS
DEFINEVAR long psyco_fail_after[LE_FAIL_SITES];
EXTERNVAR const char *psyco_respawn_file; /* dispatcher.h */
EXTERNVAR int psyco_respawn_line;
#endif

DEFINEFN void
psyco_guard_push(le_compile_guard_t *g)
{
    PyThreadState *ts = PyThreadState_GET();
    g->prev = psyco_compile_guard;
    g->owner = psyco_owner;
    g->depth = le_compile_depth;
    g->nopen = nopen;
    g->njournal = njournal;
    g->tries = 0;
    if (psyco_compile_guard == NULL && (reserve == NULL || spare_code == NULL))
        reserve_refill(); /* not while a compile may need what was released */
    le_exc_get_current(ts, &g->exc_type, &g->exc_value, &g->exc_tb);
#if ALL_CHECKS
    g->respawn_file = psyco_respawn_file;
    g->respawn_line = psyco_respawn_line;
#endif
    psyco_compile_guard = g;
}

DEFINEFN void
psyco_guard_pop(le_compile_guard_t *g)
{
    extra_assert(psyco_compile_guard == g);
    psyco_compile_guard = g->prev;
    if (g->prev == NULL)
        njournal = 0;
}

DEFINEFN void
psyco_guard_retry(le_compile_guard_t *g)
{
    if (g->tries == LE_GUARD_RETRIES)
        reserve_release();
    else if (g->tries > LE_GUARD_RETRIES) {
        psyco_guard_pop(g);
        psyco_out_of_memory(__FILE__, __LINE__);
    }
}

EXTERNFN void
psyco_compile_abandoned(void); /* dispatcher.c */

DEFINEFN void
psyco_guard_abandon(void)
{
    le_compile_guard_t *g = psyco_compile_guard;
    PyThreadState *ts = PyThreadState_GET();
    PyObject *type, *value, *tb;
    /* nothing below may run out of memory under this guard again */
    psyco_compile_guard = g->prev;
    while (njournal > g->njournal) {
        CodeBufferObject *b = journal[--njournal];
        if (b->ge != NULL)
            codebuf_unlist(b);
    }
    while (nopen > g->nopen) {
        open_extent_t x = open_ext[--nopen];
        owner_add_extent(x.owner, x.start, x.end);
    }
    psyco_owner = g->owner;
    le_compile_depth = g->depth;
#if ALL_CHECKS
    /* the jump skipped LE_RESPAWN_SITE's restore: the next point recorded
       would carry the abandoned one's site, which its replay can't match */
    psyco_respawn_file = g->respawn_file;
    psyco_respawn_line = g->respawn_line;
#endif
    psyco_compile_abandoned();
    le_exc_get_current(ts, &type, &value, &tb);
    if (type != g->exc_type || value != g->exc_value || tb != g->exc_tb) {
        PyErr_Clear();
        if (g->exc_type != NULL)
            PyErr_NoMemory(); /* the compile lost the run-time exception */
    }
    g->tries++;
    psyco_compile_guard = g;
    siglongjmp(g->env, 1);
}

/*** Blocks ***/

/* A block (CodeBufferObject) is held by its owner, which takes the new
   reference: it is returned borrowed. A registered one is held by its
   list too, which it leaves when its compile is abandoned or its owner is
   released while the list lives on (an inlined callee's join point). */
static void
owner_add_buffer(le_owner_t *o, CodeBufferObject *b)
{
    if (o == NULL)
        return; /* kept for good */
    if (o->nbufs == o->bufscap) {
        int cap = o->bufscap ? 2 * o->bufscap : o->parent ? 2 : 8;
        void *n = PyMem_Realloc(o->bufs, cap * sizeof(CodeBufferObject *));
        if (n == NULL)
            OUT_OF_MEMORY();
        o->bufs = n;
        o->bufscap = cap;
    }
    o->bufs[o->nbufs++] = b;
    if (o->parent != NULL) {
        if (amap_reserve(&group_blocks) < 0)
            OUT_OF_MEMORY();
        amap_put(&group_blocks, b, o);
    }
}

static void
codebuf_unlist(CodeBufferObject *b)
{
    PyObject *l = b->ge->fatlist;
    Py_ssize_t j;
    for (j = PyList_GET_SIZE(l); j--;)
        if (PyList_GET_ITEM(l, j) == (PyObject *)b) {
            PyList_SetSlice(l, j, j + 1, NULL);
            psyco_ge_changed(b->ge);
            break;
        }
    b->ge = NULL;
}

/* a block at 'codestart' of the state 'po' (none if NULL), registered in
   'ge' if both are given; 'po' then compiles as that block */
static CodeBufferObject *
codebuf_new(PsycoObject *po, global_entries_t *ge, code_t *codestart,
            bool proxy)
{
    CodeBufferObject *b;
    PyTypeObject *tp;

    if (po == NULL || ge == NULL)
        tp = &CodeBuffer_Type;
    else if (psyco_summary_short(&po->vlocals))
        tp = &CodeBufferShort_Type;
    else
        tp = &CodeBufferSummary_Type;
    b = PyObject_New(CodeBufferObject, tp);
    if (b == NULL || LE_FAIL(LE_FAIL_CODEBUF))
        OUT_OF_MEMORY();
    b->serial = psyco_next_serial++;
    b->ge = NULL;
    b->codestart = codestart;
    LE_RELOC_SLOT(&b->codestart);
    /* in the journal before it is registered, so that running out of
       memory can't leave it registered */
    if (po != NULL && ge != NULL)
        journal_add(b);

    fpo_mark_new(&b->snapshot);
    if (po == NULL)
        fpo_mark_unused(&b->snapshot);
    else {
        if (is_respawning(po))
            Py_FatalError("psyco: internal bug: respawning in codebuf_new()");
        fpo_build(&b->snapshot, po);
        if (ge != NULL) {
            if (tp == &CodeBufferShort_Type)
                psyco_summary_build_short(&b->summary.head, &po->vlocals);
            else
                psyco_summary_build(&b->summary.full, &po->vlocals);
            if (register_codebuf(ge, b) < 0)
                OUT_OF_MEMORY();
            b->ge = ge;
        }
        po->respawn_cnt = 0;
        po->respawn_proxy = b;
    }
    owner_add_buffer(psyco_owner, b);
    return b;
}

DEFINEFN
CodeBufferObject *
psyco_new_code_buffer(PsycoObject *po, global_entries_t *ge, code_t **limit)
{
    return codebuf_new(po, ge, psyco_code_open(limit), false);
}

DEFINEFN
CodeBufferObject *
psyco_proxy_code_buffer(PsycoObject *po, global_entries_t *ge)
{
    return codebuf_new(po, ge, po->code, true);
}

static PyObject *
codebuf_repr(CodeBufferObject *self)
{
    return PyUnicode_FromFormat("<code buffer ptr %p at %p>", self->codestart,
                                self);
}

#if ALL_CHECKS
DEFINEVAR long psyco_stat_code_buffers_freed = 0;
#endif

static void
codebuf_dealloc(CodeBufferObject *self)
{
    LE_STAT(psyco_stat_code_buffers_freed++);
    fpo_release(&self->snapshot);
    PyObject_Del(self);
}

#define CODEBUF_TYPE(size)                                     \
    {                                                          \
        PyVarObject_HEAD_INIT(NULL, 0).tp_name = "CodeBuffer", \
        .tp_basicsize = (size),                                \
        .tp_dealloc = (destructor)codebuf_dealloc,             \
        .tp_repr = (reprfunc)codebuf_repr,                     \
    }
/* the three sizes of CodeBufferObject (codemanager.h) */
DEFINEVAR PyTypeObject CodeBuffer_Type =
    CODEBUF_TYPE(offsetof(CodeBufferObject, summary));
DEFINEVAR PyTypeObject CodeBufferShort_Type = CODEBUF_TYPE(
    offsetof(CodeBufferObject, summary) + sizeof(fz_summary_head_t));
DEFINEVAR PyTypeObject CodeBufferSummary_Type =
    CODEBUF_TYPE(sizeof(CodeBufferObject));

DEFINEFN
void
codemanager_init(void)
{
    PyType_Ready(&CodeBuffer_Type);
    PyType_Ready(&CodeBufferShort_Type);
    PyType_Ready(&CodeBufferSummary_Type);
    owners_scan_init();
    /* started as Python started, tracemalloc may trace with its module
       not imported */
    {
        const char *env = getenv("PYTHONTRACEMALLOC");
        PyObject *xoptions = PySys_GetXOptions();
        if ((env != NULL && env[0] != '\0') ||
            (xoptions != NULL &&
             PyDict_GetItemString(xoptions, "tracemalloc") != NULL)) {
            PyObject *m = PyImport_ImportModule("_tracemalloc");
            if (m != NULL) {
                is_tracing_fn = PyObject_GetAttrString(m, "is_tracing");
                Py_DECREF(m);
            }
        }
        PyErr_Clear();
    }
}