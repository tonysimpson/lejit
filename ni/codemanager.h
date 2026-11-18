 /***************************************************************/
/***         Support to manage the emitted code chunks         ***/
 /***************************************************************/

#ifndef _CODEMANAGER_H
#define _CODEMANAGER_H


#include "psyco.h"
#include "dispatcher.h"


#define WARN_TOO_MANY_BUFFERS    6    /* to detect missing buffer unlocks */


/* a CodeBufferObject is a pointer to emitted code.
   The 'state' PsycoObject records the state of the compiler at
   the start of the emission of code. Consider this field as private.
   Future versions of the code manager will probably encode the recorded
   states in a more sophisticated form than just a dump copy.
   (There are usually a lot of small CodeBufferObjects, so if each
   one has a full copy of the state big projects will explode the memory.)
*/
struct CodeBufferObject_s {
	PyObject_HEAD
	void* codestart;
	FrozenPsycoObject snapshot;
	unsigned long serial;  /* order of creation: psyco_block_serial() */
	global_entries_t* ge;  /* the list it is registered in, or NULL */
};

/* The owner whose code is being compiled: the record of the code object
   whose merge points the blocks are registered at (an inlined callee's
   code belongs to its caller). What a compile allocates is held by it and
   released with that code object (psyco_owner_release); NULL while
   nothing is compiled, and what is made then is kept for good. Entering a
   compile sets it with NI_OWNER_ENTER, and NI_OWNER_LEAVE restores it. */
EXTERNVAR ni_owner_t *psyco_owner;
#define NI_OWNER_ENTER(o)  ni_owner_t *_ni_prev_owner = psyco_owner;          \
                           psyco_owner = (o)
#define NI_OWNER_LEAVE()   (psyco_owner = _ni_prev_owner)

/* the owner of 'co''s compiled code; NULL with an exception set */
EXTERNFN ni_owner_t *psyco_code_owner(PyCodeObject *co);
/* give the current owner a reference, which it drops with its code
   (stolen: a constant's when its source_known_t is made, vcompiler.h:sk_new) */
EXTERNFN void psyco_owner_keep(PyObject *o);
/* the same with a new reference: what the code relies on staying alive */
EXTERNFN void psyco_owner_pin(PyObject *o);
/* memory for a record the code uses, freed with the owner (zeroed) */
EXTERNFN void *psyco_owner_alloc(size_t size);
/* release everything 'o' holds; its code object is dead. The references
   to the code's report and entry point are released with it. */
EXTERNFN void psyco_owner_release(ni_owner_t *o, PyObject *mergepoints,
                                  PyObject *entry_point);

/* machine code for stubs and the like: 'n' bytes, held by the current
   owner (dispatcher.c:stub_pool_reserve) */
EXTERNFN code_t *psyco_code_take(size_t n);

/* Every code object's record is listed (ni.c), for the scan that frees
   owners whose code objects are reachable only from garbage
   (codemanager.c, cycles through code objects). */
EXTERNFN void psyco_owner_track(ni_code_extra_t *x);
EXTERNFN void psyco_owner_untrack(ni_code_extra_t *x);
/* scans run and owners they freed: ni._stats() */
EXTERNVAR long psyco_stat_owner_scans;
EXTERNVAR long psyco_stat_owners_collected;

/* A map from addresses, for the caches that record what a compile saw at
   an address and must see the same in a replay. psyco_record gives the
   slot of the record for 'key', NULL in a new one, for the caller to fill
   with a reference the record then holds. The current owner holds the
   record, which dies, and leaves the map, when the last owner relying on
   it dies: the value is alive while some code relies on it, and the key
   object is too if the code holds it, so no address it records is ever
   reused under it. */
typedef struct {
  struct ni_addrmap_item_s *items;
  size_t mask, n;
} ni_addrmap_t;
EXTERNFN PyObject **psyco_record(ni_addrmap_t *map, void *key);

/* Compile guards. Each way into the compiler (eval_hook.c, the stub
   functions) pushes one; running out of memory under it (OUT_OF_MEMORY)
   drops what the compile registered, so that no half-written block can be
   matched, and returns to its sigsetjmp. The machine code and records
   already made stay with their owners, unused; Python references the
   compile held are lost. Run-time error state is kept, or becomes a
   MemoryError if the compile replaced it. The caller then runs the
   interpreter instead (psyco_guard_pop), or compiles again from a state
   the compile didn't consume (psyco_guard_retry, which aborts after
   NI_GUARD_RETRIES tries: compiled code can't go back to the
   interpreter). */
#include <setjmp.h>
#define NI_GUARD_RETRIES 3
typedef struct ni_compile_guard_s {
  sigjmp_buf env;
  struct ni_compile_guard_s *prev;
  ni_owner_t *owner;
  int depth, nlocked, njournal, tries;
  PyObject *exc_type, *exc_value, *exc_tb;
#if ALL_CHECKS
  const char *respawn_file; /* dispatcher.h:NI_RESPAWN_SITE's, restored */
  int respawn_line;
#endif
} ni_compile_guard_t;
EXTERNVAR ni_compile_guard_t *psyco_compile_guard;
EXTERNFN void psyco_guard_push(ni_compile_guard_t *g);
EXTERNFN void psyco_guard_pop(ni_compile_guard_t *g);
EXTERNFN void psyco_guard_retry(ni_compile_guard_t *g);
EXTERNFN void psyco_guard_abandon(void); /* for psyco_out_of_memory */

/* code buffers created so far; the next one's serial */
EXTERNVAR unsigned long psyco_next_serial;

/* The serial of the block that the current compile started from. A replay
   (dispatcher.c:do_respawn) starts from the snapshot at the head of its
   chain of respawned buffers, so it has the same serial as the original
   compile, and a decision that depends only on this serial and state that
   changes one way can be repeated exactly in the replay. */
PSY_INLINE unsigned long psyco_block_serial(PsycoObject *po) {
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
PSY_INLINE void sk_mark_used(PsycoObject *po, source_known_t *sk) {
  sk->refcount1_flags |= SkFlagFixed;
  if (po->respawn_proxy != NULL)
    sk->used = po->respawn_proxy->serial;
}

#define SET_CODEMODE(b, mode)   do { } while (0)   /* nothing */

EXTERNVAR PyTypeObject CodeBuffer_Type;
#define CodeBuffer_Check(v) (Py_TYPE(v) == &CodeBuffer_Type)
/* code buffers started so far (not proxies): ni._stats() */
EXTERNVAR long psyco_stat_code_buffers;
EXTERNVAR long psyco_stat_code_buffers_freed;
/* bytes of code emitted, proxies included; those not freed yet */
EXTERNVAR long psyco_stat_code_bytes;
EXTERNVAR long psyco_stat_code_bytes_live;
/* references held for good, given while no owner was current */
EXTERNVAR long psyco_stat_kept_for_good;

/* Time spent compiling, by how the compiler was entered (ni._stats()).
   NI_COMPILE_BEGIN/END bracket each way in from run time; only the
   outermost counts, so a compile nested in another isn't counted twice. */
enum { NI_ENTER_ANALYSE, NI_ENTER_CALL, NI_ENTER_PAUSE, NI_ENTER_RESPAWN, NI_ENTER_PROMOTION,
       NI_ENTER_KINDS };
EXTERNVAR long psyco_stat_compile_ns[NI_ENTER_KINDS];
EXTERNVAR long psyco_stat_compile_count[NI_ENTER_KINDS];
EXTERNVAR int psyco_stat_compile_depth;
EXTERNFN long psyco_stat_now_ns(void);
EXTERNVAR int psyco_stat_compile_kind; /* the outermost entry's kind */
#define NI_COMPILE_BEGIN(kind)                                                 \
  long _ni_t0 = psyco_stat_compile_depth++                                     \
                    ? 0                                                        \
                    : (psyco_stat_compile_kind = (kind), psyco_stat_now_ns())
#define NI_COMPILE_END(kind)                                                   \
  do {                                                                         \
    if (--psyco_stat_compile_depth == 0) {                                     \
      psyco_stat_compile_ns[kind] += psyco_stat_now_ns() - _ni_t0;             \
      psyco_stat_compile_count[kind]++;                                        \
    }                                                                          \
  } while (0)


/* starts a new code buffer. The limit is returned in the optional last argument.
   'po' is the state of the compiler at this point, of which a
   frozen copy will be made. It can be NULL. If not, set 'ge' as in
   psyco_compile(). */
EXTERNFN
CodeBufferObject* psyco_new_code_buffer(PsycoObject* po, global_entries_t* ge, code_t** plimit);

/* creates a CodeBufferObject pointing to an already existing code target */
EXTERNFN
CodeBufferObject* psyco_proxy_code_buffer(PsycoObject* po, global_entries_t* ge);


/* shrink a buffer returned by new_code_buffer() */
EXTERNFN
void psyco_shrink_code_buffer(CodeBufferObject* obj, code_t* codeend);

/* emergency enlarge a buffer (by coding a jump to a new buffer) */
EXTERNFN
void psyco_emergency_enlarge_buffer(code_t** pcode, code_t** pcodelimit);

EXTERNFN
int psyco_locked_buffers(void);

#if NI_CODESTATS
/* bytes and count of the code buffers closed, by kind (tools/codesize.py) */
EXTERNFN void psyco_codestats_buffer(const char *kind, CodeBufferObject *obj,
                                     code_t *nend);
#else
#define psyco_codestats_buffer(kind, obj, nend) do { } while (0)
#endif

#define SHRINK_CODE_BUFFER(obj, nend, mode)    do {    \
      psyco_codestats_buffer(mode, obj, nend);         \
      psyco_shrink_code_buffer(obj, nend);             \
      SET_CODEMODE(obj, mode);                         \
} while (0)


/* a replacement for Py_XDECREF(obj) which does not release the object
   immediately, but only at the next call to psyco_trash_object() */
EXTERNFN
void psyco_trash_object(PyObject* obj);

EXTERNFN
void codemanager_init();

#endif /* _CODEMANAGER_H */
