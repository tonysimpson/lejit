/***************************************************************/
/***         Support to manage the emitted code chunks         ***/
/***************************************************************/

#ifndef _CODEMANAGER_H
#define _CODEMANAGER_H

#include "psyco.h"
#include "dispatcher.h"

/* Executable memory (codemanager.c). psyco_code_open gives an extent to
   write code into, up to '*limit' and GUARANTEED_MINIMUM past it
   (psyco.h). psyco_code_close ends the extent 'end' is in at 'end',
   giving back the rest; psyco_code_continue ends the one '*pcode' is in
   with a jump to a new one, which '*pcode' and '*plimit' then describe.
   psyco_code_take gives 'n' bytes at once, for stub pools
   (dispatcher.c:stub_pool_reserve). What is written is held by the owner
   current when its extent was opened (taken). */
EXTERNFN code_t *
psyco_code_open(code_t **limit);
EXTERNFN void
psyco_code_close(code_t *end);
EXTERNFN void
psyco_code_continue(code_t **pcode, code_t **plimit);
EXTERNFN code_t *
psyco_code_take(size_t n);
/* the extents open now: a new block is compiled at once only while fewer
   than OPEN_EXTENTS_MAX are, and starts with a coding pause otherwise
   (vcompiler.c:psyco_compile_code) */
EXTERNFN int
psyco_code_open_count(void);
#define OPEN_EXTENTS_MAX 5

#if LE_CODESTATS
/* bytes and count of the code closed, by kind */
EXTERNFN void
psyco_codestats_buffer(const char *kind, code_t *start, code_t *end);
#else
#define psyco_codestats_buffer(kind, start, end) ((void)(start))
#endif
/* psyco_code_close, the code from 'start' counted as 'kind' */
#define CLOSE_CODE(kind, start, end)                          \
    do {                                                      \
        psyco_codestats_buffer(kind, (code_t *)(start), end); \
        psyco_code_close(end);                                \
    } while (0)

/* A block: an entry point into compiled code, with the snapshot of the
   compiler state it was compiled from, and that snapshot's summary if the
   block is registered at a merge or join point. It is made at the start
   of a new extent (psyco_new_code_buffer) or where the code being written
   has got to (psyco_proxy_code_buffer, a proxy), and lives as long as its
   owner (codemanager.c, "Blocks"). */
struct CodeBufferObject_s {
    PyObject_HEAD void *codestart;
    FrozenPsycoObject snapshot;
    unsigned long serial;    /* order of creation: psyco_block_serial() */
    global_entries_t *ge;    /* the list it is registered in, or NULL */
    unsigned long exact_key; /* at a join point (dispatcher.h) */
    /* of the snapshot, if 'ge' (dispatcher.h); last, and as large as its
       type says: none in a CodeBuffer_Type, 'head' in a
       CodeBufferShort_Type, 'full' in a CodeBufferSummary_Type */
    union {
        fz_summary_head_t head;
        fz_summary_t full;
    } summary;
};

/* The owner whose code is being compiled: the record of the code object
   whose merge points the blocks are registered at (an inlined callee's
   code belongs to its caller). What a compile allocates is held by it and
   released with that code object (psyco_owner_release); NULL while
   nothing is compiled, and what is made then is kept for good. Entering a
   compile sets it with LE_OWNER_ENTER, and LE_OWNER_LEAVE restores it. */
EXTERNVAR le_owner_t *psyco_owner;
#define LE_OWNER_ENTER(o)                     \
    le_owner_t *_le_prev_owner = psyco_owner; \
    psyco_owner = (o)
#define LE_OWNER_LEAVE() (psyco_owner = _le_prev_owner)

/* the owner of 'co''s compiled code; NULL with an exception set */
EXTERNFN le_owner_t *
psyco_code_owner(PyCodeObject *co);
/* give the current owner a reference, which it drops with its code
   (stolen: a constant's when its source_known_t is made, vcompiler.h:sk_new)
 */
EXTERNFN void
psyco_owner_keep(PyObject *o);
/* the same with a new reference: what the code relies on staying alive */
EXTERNFN void
psyco_owner_pin(PyObject *o);
/* memory for a record the code uses, freed with the owner (zeroed) */
EXTERNFN void *
psyco_owner_alloc(size_t size);
/* release everything 'o' holds; its code object is dead. The references
   to the code's report and entry point are released with it. */
EXTERNFN void
psyco_owner_release(le_owner_t *o, PyObject *mergepoints,
                    PyObject *entry_point);

/* A group is a child owner: the code a promotion site compiles for one
   value that can die, and everything that compile and the code it leads
   to hold. Its code is reached only through its site, through blocks it
   registered (a compile that jumps into one depends on it,
   psyco_owner_depend), and from a frame running its code object, so the
   owner scan frees it once its key, or a key of a group above it, is
   found dead, no frame of its code object runs, and nothing alive
   depends on it: the site forgets the value but still counts it
   (dispatcher.c:psyco_promotion_forget). It lives at most as long as its
   code object. */
typedef struct le_group_s {
    le_owner_t owner;         /* first: a group is its owner */
    struct le_group_s *next;  /* in its code object's list */
    PyObject *key;            /* the promoted value, held in owner.pins */
    void *site;               /* the promotion site, in its parent's code */
    struct le_group_s **deps; /* other groups its code jumps into */
    int ndeps, depscap;
    char held; /* its code object's own code jumps into it */
    char dead; /* being freed by the owner scan */
} le_group_t;
/* a new group under the current owner, for 'key' (held by the caller) at
   'site'; NULL if no owner is current or 'key' looks as if it lives as
   long as its module */
EXTERNFN le_owner_t *
psyco_group_new(PyObject *key, void *site);
/* the current owner's code jumps into 'b' */
EXTERNFN void
psyco_owner_depend(CodeBufferObject *b);
/* the code object's owner above the current owner, if that is a group
   and the code object's owner holds every constant 'a' has; else NULL.
   With it, the type hints in 'a' that owner doesn't hold are dropped. */
EXTERNFN le_owner_t *
psyco_owner_root_holding(vinfo_array_t *a);
/* frames.c: calls 'mark' on the code object of every compiled frame
   running on any thread, and on some more; -1 if it can't tell */
EXTERNFN int
psyco_frames_running(void (*mark)(PyCodeObject *co, void *arg), void *arg);
/* dispatcher.c: the site forgets the code it compiled for 'key', if any */
EXTERNFN void
psyco_promotion_forget(void *site, long key);

/* A record is counted once its owner holds a reference (psyco_owner_keep):
   the count since the last scan decides when the scan that frees owners
   whose code objects are reachable only from garbage runs (codemanager.c,
   cycles through code objects); one that holds none can't keep anything
   alive. */
EXTERNFN void
psyco_owner_track(le_code_extra_t *x);
EXTERNFN void
psyco_owner_untrack(le_code_extra_t *x);
/* scans run, owners they freed, records counted: lejit._core._stats() */
#if ALL_CHECKS
EXTERNVAR long psyco_stat_owner_scans;
EXTERNVAR long psyco_stat_owners_collected;
EXTERNVAR long psyco_stat_groups_made;
EXTERNVAR long psyco_stat_groups_freed;
#endif
EXTERNVAR long le_owners_counted;

/* A map from addresses, for the caches that record what a compile saw at
   an address and must see the same in a replay. psyco_record gives the
   slot of the record for 'key', NULL in a new one, for the caller to fill
   with a reference the record then holds. The current owner holds the
   record, which dies, and leaves the map, when the last owner relying on
   it dies: the value is alive while some code relies on it, and the key
   object is too if the code holds it, so no address it records is ever
   reused under it. */
typedef struct {
    struct le_addrmap_item_s *items;
    size_t mask, n;
} le_addrmap_t;
EXTERNFN PyObject **
psyco_record(le_addrmap_t *map, void *key);

/* Compile guards. Each way into the compiler (eval_hook.c, the stub
   functions) pushes one; running out of memory under it (OUT_OF_MEMORY)
   drops what the compile registered, so that no half-written block can be
   matched, and returns to its sigsetjmp. The machine code and records
   already made stay with their owners, unused; Python references the
   compile held are lost. Run-time error state is kept, or becomes a
   MemoryError if the compile replaced it. The caller then runs the
   interpreter instead (psyco_guard_pop), or compiles again from a state
   the compile didn't consume (psyco_guard_retry, which releases the
   emergency reserve before the last of LE_GUARD_RETRIES + 1 tries and
   aborts after it: compiled code can't go back to the interpreter). */
#include <setjmp.h>
#define LE_GUARD_RETRIES 3
typedef struct le_compile_guard_s {
    sigjmp_buf env;
    struct le_compile_guard_s *prev;
    le_owner_t *owner;
    int depth, nopen, njournal, tries;
    PyObject *exc_type, *exc_value, *exc_tb;
#if ALL_CHECKS
    const char *respawn_file; /* dispatcher.h:LE_RESPAWN_SITE's, restored */
    int respawn_line;
#endif
} le_compile_guard_t;
EXTERNVAR le_compile_guard_t *psyco_compile_guard;
EXTERNFN void
psyco_guard_push(le_compile_guard_t *g);
EXTERNFN void
psyco_guard_pop(le_compile_guard_t *g);
EXTERNFN void
psyco_guard_retry(le_compile_guard_t *g);
EXTERNFN void
psyco_guard_abandon(void); /* for psyco_out_of_memory */

/* code buffers created so far; the next one's serial */
EXTERNVAR unsigned long psyco_next_serial;

/* The serial of the block that the current compile started from. A replay
   (dispatcher.c:do_respawn) starts from the snapshot at the head of its
   chain of respawned buffers, so it has the same serial as the original
   compile, and a decision that depends only on this serial and state that
   changes one way can be repeated exactly in the replay. */
PSY_INLINE unsigned long
psyco_block_serial(PsycoObject *po)
{
    CodeBufferObject *b = po->respawn_proxy;
    while (b->snapshot.fz_respawned_from != NULL)
        b = b->snapshot.fz_respawned_from;
    return b->serial;
}

/* A compile-time value that code is being compiled to rely on: fixed (it
   may never be un-promoted), and stamped with the current block's serial.
   A merge point may drop a run-time value's known field that no block
   since the last merge point has stamped (po->mp_serial,
   dispatcher.c:compatible_unused_fact).
   The stamp is only a guess about what code after the merge point needs;
   being wrong costs speed, never correctness. */
PSY_INLINE void
sk_mark_used(PsycoObject *po, source_known_t *sk)
{
    sk->refcount1_flags |= SkFlagFixed;
    if (po->respawn_proxy != NULL)
        sk->used = po->respawn_proxy->serial;
}

EXTERNVAR PyTypeObject CodeBuffer_Type, CodeBufferShort_Type,
    CodeBufferSummary_Type;
#define CodeBuffer_Check(v)                                                   \
    (Py_TYPE(v) == &CodeBuffer_Type || Py_TYPE(v) == &CodeBufferShort_Type || \
     Py_TYPE(v) == &CodeBufferSummary_Type)
#if ALL_CHECKS
/* extents opened by psyco_code_open, and blocks freed: lejit._core._stats()'s
   code_buffers and code_buffers_freed */
EXTERNVAR long psyco_stat_code_buffers;
EXTERNVAR long psyco_stat_code_buffers_freed;
/* references held for good, given while no owner was current */
EXTERNVAR long psyco_stat_kept_for_good;
/* bytes of code not freed yet */
EXTERNVAR long psyco_stat_code_bytes_live;
#endif
#if ALL_CHECKS || LE_CODESTATS
/* bytes of code emitted, proxies included */
EXTERNVAR long psyco_stat_code_bytes;
#endif

/* LE_COMPILE_BEGIN/END bracket each way in from run time, by how the
   compiler was entered; le_compile_depth counts the compiles open, during
   which no owner scan runs, and the outermost keeps the compiler's
   allocations out of tracemalloc (le_untraced_begin). Checked builds time
   the outermost (lejit._core._stats()), so a compile nested in another
   isn't counted twice; codestats builds keep its kind. A normal build reads
   no clock. */
enum {
    LE_ENTER_ANALYSE,
    LE_ENTER_CALL,
    LE_ENTER_PAUSE,
    LE_ENTER_RESPAWN,
    LE_ENTER_PROMOTION,
    LE_ENTER_KINDS
};
EXTERNVAR int le_compile_depth;
/* from begin to end, while tracemalloc traces, the compiler's allocations
   are made past it; nested calls do nothing */
EXTERNFN void
le_untraced_begin(void);
EXTERNFN void
le_untraced_end(void);
#if ALL_CHECKS || LE_CODESTATS
EXTERNFN long
le_now_ns(void);
EXTERNVAR int psyco_stat_compile_kind; /* the outermost entry's kind */
#endif
#if ALL_CHECKS
EXTERNVAR long psyco_stat_compile_ns[LE_ENTER_KINDS];
EXTERNVAR long psyco_stat_compile_count[LE_ENTER_KINDS];
#define LE_COMPILE_BEGIN(kind)                \
    long _le_t0 = le_compile_depth++          \
                      ? 0                     \
                      : (le_untraced_begin(), \
                         psyco_stat_compile_kind = (kind), le_now_ns())
#define LE_COMPILE_END(kind)                                     \
    do {                                                         \
        if (--le_compile_depth == 0) {                           \
            psyco_stat_compile_ns[kind] += le_now_ns() - _le_t0; \
            psyco_stat_compile_count[kind]++;                    \
            le_untraced_end();                                   \
        }                                                        \
    } while (0)
#elif LE_CODESTATS
#define LE_COMPILE_BEGIN(kind) \
    (void)(le_compile_depth++  \
               ? 0             \
               : (le_untraced_begin(), psyco_stat_compile_kind = (kind)))
#define LE_COMPILE_END(kind) \
    ((void)(--le_compile_depth == 0 ? (le_untraced_end(), 0) : 0))
#else
#define LE_COMPILE_BEGIN(kind) \
    ((void)(le_compile_depth++ ? 0 : (le_untraced_begin(), 0)))
#define LE_COMPILE_END(kind) \
    ((void)(--le_compile_depth == 0 ? (le_untraced_end(), 0) : 0))
#endif

/* A block at the start of a new extent (psyco_code_open, whose limit it
   returns in '*limit'), or at 'po->code' (a proxy). If 'po' is given, the
   block holds a snapshot of it and 'po' goes on compiling as that block
   (its respawn_proxy); if 'ge' is given too, the block is registered
   there, as in psyco_compile(). */
EXTERNFN
CodeBufferObject *
psyco_new_code_buffer(PsycoObject *po, global_entries_t *ge, code_t **limit);
EXTERNFN
CodeBufferObject *
psyco_proxy_code_buffer(PsycoObject *po, global_entries_t *ge);

EXTERNFN
void
codemanager_init(void);

#endif /* _CODEMANAGER_H */
