#include "codemanager.h"
#include <time.h>
#include "compat2to3.h"
#include "platform.h"
#include <ipyencoding.h>

/*** Executable memory ***/

/* Free memory is a sorted list of disjoint extents. A compile takes a whole
   extent and gives back what it didn't use when it closes the buffer
   (close_buffer_use); while nothing is freed that is the tail of the
   newest big buffer, so code is laid out one block after another as by a
   bump allocator. The part used goes to the current owner, which returns
   it here when its code object dies. */
typedef struct {
  code_t *start, *end;
} extent_t;

static extent_t *free_ext = NULL;
static int nfree = 0, freecap = 0;

/* extents taken and not closed yet, with their owner */
typedef struct {
  code_t *start, *end;
  ni_owner_t *owner;
} locked_t;
static locked_t *locked = NULL;
static int nlocked = 0, lockedcap = 0;

/* a buffer with less room than this is full: what is left of it stays
   with the code before it */
#define EXTENT_MIN (BUFFER_MARGIN + 2 * GUARANTEED_MINIMUM)
/* a compile prefers an extent this large, so that it doesn't pause soon */
#define EXTENT_PREFERRED 16384

static void free_insert(code_t *start, code_t *end) {
  int lo = 0, hi = nfree;
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

static void allocate_more_buffers(void) {
  static char plat_ok = 0;
  char *p;
  long allocated = 0;

  if (plat_ok != 'n') {
    /* try the platform-specific allocator */
    allocated = psyco_allocate_executable_buffer(BIG_BUFFER_SIZE, &p);
    if (allocated < BIG_BUFFER_SIZE) {
      /* failed */
      if (plat_ok == 0)
        plat_ok = 'n'; /* doesn't work, don't try again */
      else
        OUT_OF_MEMORY();
    } else
      plat_ok = 'y'; /* works */
  }
  if (plat_ok == 'n') {
    p = (char *)PyMem_MALLOC(BIG_BUFFER_SIZE);
    if (p == NULL)
      OUT_OF_MEMORY();
    allocated = BIG_BUFFER_SIZE;
  }
  free_insert((code_t *)p, (code_t *)p + allocated);
}

/* the first free extent of at least 'n' bytes, or -1 */
static int free_find(size_t n) {
  int i;
  for (i = 0; i < nfree; i++)
    if ((size_t)(free_ext[i].end - free_ext[i].start) >= n)
      return i;
  return -1;
}

static code_t *take_extent(size_t want, size_t need, code_t **end) {
  int i = free_find(want);
  code_t *start;
  if (i < 0 && need < want)
    i = free_find(need);
  if (i < 0) {
    allocate_more_buffers();
    i = free_find(need);
    psyco_assert(i >= 0);
  }
  start = free_ext[i].start;
  *end = free_ext[i].end;
  memmove(free_ext + i, free_ext + i + 1, (nfree - i - 1) * sizeof(extent_t));
  nfree--;
  return start;
}

static void owner_add_extent(ni_owner_t *o, code_t *start, code_t *end) {
  psyco_stat_code_bytes_live += end - start;
  if (o == NULL)
    return; /* kept for good */
  if (o->nextents > 0 && o->extents[o->nextents - 1].end == start) {
    o->extents[o->nextents - 1].end = end;
    return;
  }
  if (o->nextents == o->extentscap) {
    int cap = o->extentscap ? 2 * o->extentscap : 4;
    void *n = PyMem_Realloc(o->extents, cap * sizeof(extent_t));
    if (n == NULL)
      OUT_OF_MEMORY();
    o->extents = n;
    o->extentscap = cap;
  }
  o->extents[o->nextents].start = start;
  o->extents[o->nextents].end = end;
  o->nextents++;
}

static code_t *get_next_buffer(code_t **limit) {
  code_t *start, *end;
  start = take_extent(EXTENT_PREFERRED, EXTENT_MIN, &end);
  if (nlocked == lockedcap) {
    int cap = lockedcap ? 2 * lockedcap : 16;
    locked_t *n = PyMem_Realloc(locked, cap * sizeof(locked_t));
    if (n == NULL)
      OUT_OF_MEMORY();
    locked = n;
    lockedcap = cap;
  }
  locked[nlocked].start = start;
  locked[nlocked].end = end;
  locked[nlocked].owner = psyco_owner;
  nlocked++;
  *limit = end - GUARANTEED_MINIMUM;
  return start;
}

DEFINEFN int psyco_locked_buffers(void) { return nlocked; }

static void close_buffer_use(code_t *code) {
  int i;
  for (i = nlocked; i--;) {
    locked_t l = locked[i];
    if (l.start <= code && code <= l.end) {
      ALIGN_NO_FILL();
      if (code > l.end)
        code = l.end;
      psyco_stat_code_bytes += code - l.start;
      memmove(locked + i, locked + i + 1, (nlocked - i - 1) * sizeof(locked_t));
      nlocked--;
      if (l.end - code < EXTENT_MIN)
        code = l.end; /* full */
      else
        free_insert(code, l.end);
      owner_add_extent(l.owner, l.start, code);
      return;
    }
  }
  Py_FatalError("psyco: code buffer allocator corruption");
}

DEFINEFN code_t *psyco_code_take(size_t n) {
  code_t *end, *start = take_extent(n, n, &end);
  if ((size_t)(end - start) - n < EXTENT_MIN)
    n = end - start;
  else
    free_insert(start + n, end);
  owner_add_extent(psyco_owner, start, start + n);
  return start;
}

/*** Owners ***/

DEFINEVAR ni_owner_t *psyco_owner = NULL;
DEFINEVAR long psyco_stat_code_bytes_live = 0;
DEFINEVAR long psyco_stat_kept_for_good = 0;

DEFINEFN ni_owner_t *psyco_code_owner(PyCodeObject *co) {
  ni_code_extra_t *x = ni_code_extra(co);
  return x == NULL ? NULL : &x->owner;
}

/* open addressing on the address; a hole is NULL */
static size_t pin_slot(ni_owner_t *o, PyObject *p) {
  size_t i = ((size_t)p >> 4) & o->pinmask;
  while (o->pins[i] != NULL && o->pins[i] != p)
    i = (i + 1) & o->pinmask;
  return i;
}

static void pins_grow(ni_owner_t *o) {
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

DEFINEFN void psyco_owner_keep(PyObject *p) {
  ni_owner_t *o = psyco_owner;
  size_t i;
  if (p == NULL)
    return;
  if (o == NULL) {
    psyco_stat_kept_for_good++;
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
  o->npins++;
}

DEFINEFN void psyco_owner_pin(PyObject *p) {
  Py_XINCREF(p);
  psyco_owner_keep(p);
}

/* each block is a list link, then the record */
DEFINEFN void *psyco_owner_alloc(size_t size) {
  ni_owner_t *o = psyco_owner;
  void **p = PyMem_Malloc(sizeof(void *) + size);
  if (p == NULL)
    OUT_OF_MEMORY();
  memset(p + 1, 0, size);
  if (o != NULL) {
    *p = o->blocks;
    o->blocks = p;
  }
  return p + 1;
}

EXTERNFN void psyco_owner_release_records(ni_owner_t *o); /* dispatcher.c */
EXTERNFN bool psyco_ge_in_report(PyObject *mergepoints,
                                 global_entries_t *ge); /* mergepoints.c */

DEFINEFN void psyco_owner_release(ni_owner_t *o, PyObject *mergepoints,
                                  PyObject *entry_point) {
  ni_owner_t dead;
  int i;
  NI_OWNER_ENTER(o);
  /* a block compiled in an inlined frame is listed at the callee's join
     point; the callee is held until here (psyfunc.c) */
  for (i = 0; i < o->nbufs; i++) {
    CodeBufferObject *b = o->bufs[i];
    if (b->ge != NULL && !psyco_ge_in_report(mergepoints, b->ge)) {
      PyObject *l = b->ge->fatlist;
      Py_ssize_t j;
      for (j = PyList_GET_SIZE(l); j--;)
        if (PyList_GET_ITEM(l, j) == (PyObject *)b) {
          PyList_SetSlice(l, j, j + 1, NULL);
          break;
        }
    }
  }
  /* what the records, buffers and snapshots release comes back here */
  psyco_owner_release_records(o);
  Py_XDECREF(entry_point);
  Py_XDECREF(mergepoints);
  for (i = 0; i < o->nbufs; i++)
    Py_DECREF(o->bufs[i]);
  NI_OWNER_LEAVE();
  dead = *o;
  memset(o, 0, sizeof(*o));
  for (i = 0; i < dead.nextents; i++) {
    code_t *s = dead.extents[i].start, *e = dead.extents[i].end;
#if ALL_CHECKS
    memset(s, 0xCC, e - s); /* INT 3: a stale jump traps */
#endif
    psyco_stat_code_bytes_live -= e - s;
    free_insert(s, e);
  }
  PyMem_Free(dead.extents);
  PyMem_Free(dead.bufs);
  while (dead.blocks != NULL) {
    void *next = *(void **)dead.blocks;
    PyMem_Free(dead.blocks);
    dead.blocks = next;
  }
  /* last, since a dealloc may run any code */
  if (dead.pins != NULL) {
    size_t j;
    for (j = 0; j <= dead.pinmask; j++)
      Py_XDECREF(dead.pins[j]);
    PyMem_Free(dead.pins);
  }
}

DEFINEVAR unsigned long psyco_next_serial = 0;

#if NI_CODESTATS
DEFINEVAR PyObject *psyco_codestats_buffers = NULL; /* kind -> [count, bytes] */
DEFINEFN void psyco_codestats_buffer(const char *kind, CodeBufferObject *obj,
                                     code_t *nend) {
  long n = (char *)nend - (char *)obj->codestart;
  PyObject *e;
  if (psyco_codestats_buffers == NULL &&
      (psyco_codestats_buffers = PyDict_New()) == NULL)
    OUT_OF_MEMORY();
  e = PyDict_GetItemString(psyco_codestats_buffers, kind);
  if (e == NULL) {
    e = Py_BuildValue("[ll]", 0L, 0L);
    if (e == NULL || PyDict_SetItemString(psyco_codestats_buffers, kind, e) < 0)
      OUT_OF_MEMORY();
    Py_DECREF(e);
  }
  PyList_SetItem(e, 0, PyLong_FromLong(PyLong_AsLong(PyList_GET_ITEM(e, 0)) + 1));
  PyList_SetItem(e, 1, PyLong_FromLong(PyLong_AsLong(PyList_GET_ITEM(e, 1)) + n));
}
#endif
DEFINEVAR long psyco_stat_code_buffers = 0;
DEFINEVAR long psyco_stat_code_bytes = 0;
DEFINEVAR long psyco_stat_compile_ns[NI_ENTER_KINDS];
DEFINEVAR long psyco_stat_compile_count[NI_ENTER_KINDS];
DEFINEVAR int psyco_stat_compile_depth = 0;
DEFINEVAR int psyco_stat_compile_kind = 0;

DEFINEFN long psyco_stat_now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000000L + t.tv_nsec;
}

/* the owner takes the new reference: a buffer is returned borrowed */
static void owner_add_buffer(ni_owner_t *o, CodeBufferObject *b) {
  if (o == NULL)
    return; /* kept for good */
  if (o->nbufs == o->bufscap) {
    int cap = o->bufscap ? 2 * o->bufscap : 8;
    void *n = PyMem_Realloc(o->bufs, cap * sizeof(CodeBufferObject *));
    if (n == NULL)
      OUT_OF_MEMORY();
    o->bufs = n;
    o->bufscap = cap;
  }
  o->bufs[o->nbufs++] = b;
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
  b->ge = NULL;
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
    if (ge != NULL) {
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