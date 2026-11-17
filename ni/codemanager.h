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
};

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
   A merge point may drop a run-time value's known type that no block
   since the last merge point has stamped (po->mp_serial,
   dispatcher.c:compatible_unused_fact, D50).
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
/* bytes of code emitted, proxies included */
EXTERNVAR long psyco_stat_code_bytes;

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
