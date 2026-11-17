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
};

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
#define NI_COMPILE_BEGIN(kind)                                                 \
  long _ni_t0 = psyco_stat_compile_depth++ ? 0 : psyco_stat_now_ns()
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

#define SHRINK_CODE_BUFFER(obj, nend, mode)    do {    \
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
