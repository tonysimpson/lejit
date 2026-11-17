#include "codemanager.h"
#include <time.h>
#include "compat2to3.h"
#include "platform.h"
#include <ipyencoding.h>

/*** Allocators for Large Executable Blocks of Memory ***/

#define BUFFER_SIGNATURE 0xE673B506 /* arbitrary */

typedef struct codemanager_buf_s {
  long signature;
  char *position;
  bool inuse;
  struct codemanager_buf_s *next;
} codemanager_buf_t;

static codemanager_buf_t *big_buffers = NULL;
static codemanager_buf_t *completed_big_buffers = NULL;

PSY_INLINE void check_signature(codemanager_buf_t *b) {
  if (b->signature != BUFFER_SIGNATURE)
    Py_FatalError("psyco: code buffer overwrite detected");
}

static void allocate_more_buffers(codemanager_buf_t **bb) {
  static char plat_ok = 0;
  char *p;
  long allocated;
  int num_bigblocks = 0;

  if (plat_ok != 'n') {
    /* try the platform-specific allocator */
    allocated = psyco_allocate_executable_buffer(BIG_BUFFER_SIZE, &p);
    num_bigblocks = allocated / BIG_BUFFER_SIZE;
    if (num_bigblocks <= 0) {
      /* failed */
      if (plat_ok == 0)
        plat_ok = 'n'; /* doesn't work, don't try again */
      else
        OUT_OF_MEMORY();
    } else
      plat_ok = 'y'; /* works */
  }
  if (num_bigblocks <= 0) {
    p = (char *)PyMem_MALLOC(BIG_BUFFER_SIZE);
    if (p == NULL)
      OUT_OF_MEMORY();
    num_bigblocks = 1;
  }
  while (--num_bigblocks >= 0) {
    /* the codemanager_buf_t structure is put at the end of the buffer,
       with its signature to detect overflows (just in case) */
#define BUFFER_START_OFFSET (BIG_BUFFER_SIZE - sizeof(codemanager_buf_t))
    codemanager_buf_t *b;
    b = (codemanager_buf_t *)(p + BUFFER_START_OFFSET);
    b->signature = BUFFER_SIGNATURE;
    b->position = p;
    b->next = NULL;
    /* insert 'b' at the end of the chained list */
    *bb = b;
    bb = &b->next;
    p += BIG_BUFFER_SIZE;
  }
}

static code_t *get_next_buffer(code_t **limit) {
  codemanager_buf_t *b;
  codemanager_buf_t **bb;
  for (b = big_buffers; b != NULL; b = b->next) {
    check_signature(b);
    if (!b->inuse)
      break; /* returns the first (oldest) unlocked buffer */
  }

  if (b == NULL) {
    /* no more free buffers, allocate one or a few new ones */
    for (bb = &big_buffers; *bb != NULL; bb = &(*bb)->next)
      ;
    allocate_more_buffers(bb);
    b = *bb;
  }
  b->inuse = true;
  *limit = ((code_t *)b) - GUARANTEED_MINIMUM;
  return (code_t *)b->position;
}

DEFINEFN int psyco_locked_buffers(void) {
  codemanager_buf_t *b;
  int count = 0;
  for (b = big_buffers; b != NULL; b = b->next)
    if (b->inuse)
      count++;
  return count;
}

static void close_buffer_use(code_t *code) {
  codemanager_buf_t *b;
  for (b = big_buffers; b != NULL; b = b->next) {
    check_signature(b);
    if (b->position <= (char *)code && (char *)code <= (char *)b) {
      extra_assert(b->inuse);
      ALIGN_NO_FILL();
      psyco_stat_code_bytes += (char *)code - b->position;
      /* unlock the buffer */
      b->position = (char *)code;
      b->inuse = false;

      if (code > ((code_t *)b) - (BUFFER_MARGIN + 2 * GUARANTEED_MINIMUM)) {
        /* buffer nearly full, remove it from the chained list */
        codemanager_buf_t **bb;
        for (bb = &big_buffers; *bb != b; bb = &(*bb)->next)
          ;
        *bb = b->next;
        /* add it to the list of completed buffers */
        b->next = completed_big_buffers;
        completed_big_buffers = b;
      }
      return;
    }
  }
  Py_FatalError("psyco: code buffer allocator corruption");
}

DEFINEVAR unsigned long psyco_next_serial = 0;
DEFINEVAR long psyco_stat_code_buffers = 0;
DEFINEVAR long psyco_stat_code_bytes = 0;
DEFINEVAR long psyco_stat_compile_ns[NI_ENTER_KINDS];
DEFINEVAR long psyco_stat_compile_count[NI_ENTER_KINDS];
DEFINEVAR int psyco_stat_compile_depth = 0;

DEFINEFN long psyco_stat_now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000000L + t.tv_nsec;
}

static CodeBufferObject *new_code_buffer(PsycoObject *po, global_entries_t *ge,
                                         code_t *proxy_to, code_t **plimit) {
  CodeBufferObject *b;
  code_t *limit;
  psyco_trash_object(NULL);
  if (plimit == NULL)
    plimit = &limit;

  b = PyObject_New(CodeBufferObject, &CodeBuffer_Type);
  if (b == NULL)
    OUT_OF_MEMORY();
  b->serial = psyco_next_serial++;
  if (proxy_to != NULL) {
    *plimit = NULL;
    b->codestart = proxy_to; /* points inside another code buffer */
    SET_CODEMODE(b, "proxy");
  } else {
    /* start a new code buffer */
    b->codestart = get_next_buffer(plimit);
    psyco_stat_code_buffers++;
    SET_CODEMODE(b, "(compiling)");
  }

  fpo_mark_new(&b->snapshot);
  if (po == NULL)
    fpo_mark_unused(&b->snapshot);
  else {
    if (is_respawning(po))
      Py_FatalError("psyco: internal bug: respawning in new_code_buffer()");
    fpo_build(&b->snapshot, po);
    if (ge != NULL)
      register_codebuf(ge, b);
    po->respawn_cnt = 0;
    po->respawn_proxy = b;
  }
  return b;
}

DEFINEFN
CodeBufferObject *psyco_new_code_buffer(PsycoObject *po, global_entries_t *ge,
                                        code_t **plimit) {
  return new_code_buffer(po, ge, NULL, plimit);
}

DEFINEFN
CodeBufferObject *psyco_proxy_code_buffer(PsycoObject *po,
                                          global_entries_t *ge) {
  return new_code_buffer(po, ge, po->code, NULL);
}

DEFINEFN
void psyco_shrink_code_buffer(CodeBufferObject *obj, code_t *codeend) {
  close_buffer_use(codeend);
  SET_CODEMODE(obj, "normal");
}

DEFINEFN
void psyco_emergency_enlarge_buffer(code_t **pcode, code_t **pcodelimit) {
  code_t *code = *pcode;
  code_t *nextcode;
  if (code - *pcodelimit > GUARANTEED_MINIMUM - MAXIMUM_SIZE_OF_FAR_JUMP)
    Py_FatalError("psyco: code buffer overflowing");

  nextcode = get_next_buffer(pcodelimit);

  ni_trace_begin_simple_code(code);
  JUMP_TO(nextcode);
  ni_trace_end_simple_code(code);
  close_buffer_use(code);

  *pcode = insn_code_label(nextcode);
}

/*****************************************************************/

static PyObject *trashed = NULL;

DEFINEFN
void psyco_trash_object(PyObject *obj) {
  Py_XDECREF(trashed);
  trashed = obj;
}

/*****************************************************************/

static PyObject *codebuf_repr(CodeBufferObject *self) {
  return NiCompatStr_FromFormat("<code buffer ptr %p at %p>", self->codestart,
                                self);
}

DEFINEVAR long psyco_stat_code_buffers_freed = 0;

static void codebuf_dealloc(CodeBufferObject *self) {
  psyco_stat_code_buffers_freed++;
  fpo_release(&self->snapshot);
  PyObject_Del(self);
}

DEFINEVAR
PyTypeObject CodeBuffer_Type = {
    PyVarObject_HEAD_INIT(NULL, 0) "CodeBuffer", /*tp_name*/
    sizeof(CodeBufferObject),                    /*tp_basicsize*/
    0,                                           /*tp_itemsize*/
    /* methods */
    (destructor)codebuf_dealloc, /*tp_dealloc*/
    0,                           /*tp_print*/
    0,                           /*tp_getattr*/
    0,                           /*tp_setattr*/
    0,                           /*tp_compare*/
    (reprfunc)codebuf_repr,      /*tp_repr*/
    0,                           /*tp_as_number*/
    0,                           /*tp_as_sequence*/
    0,                           /*tp_as_mapping*/
    0,                           /*tp_hash*/
    0,                           /*tp_call*/
};

DEFINEFN
void codemanager_init() { PyType_Ready(&CodeBuffer_Type); }