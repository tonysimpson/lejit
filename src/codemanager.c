#include "codemanager.h"
#include <time.h>
#include "platform.h"
#include <ipyencoding.h>

/*** Executable memory ***/

/* Machine code is written into extents of executable memory, mapped
   REGION_SIZE at a time (platform.c). An extent is
   - free: in free_ext, sorted by address and coalesced; unaddressable
     under AddressSanitizer (NI_POISON), since only Ni's C code is checked,
     not the code it emits;
   - open: being written (psyco_code_open), in open_ext with the owner
     that was current when it was opened. Its writer has a limit
     GUARANTEED_MINIMUM before its end (psyco.h);
   - owned: closed, the part written held by that owner (o->extents), and
     free again when the owner is released (psyco_owner_release); with no
     owner, kept for good.
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
  ni_owner_t *owner;
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

static void free_insert(code_t *start, code_t *end) {
  int lo = 0, hi = nfree;
  NI_POISON(start, end - start);
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

static void map_region(void) {
  char *p;
  if (psyco_allocate_executable_buffer(REGION_SIZE, &p) < REGION_SIZE)
    OUT_OF_MEMORY();
  NI_LSAN_ROOT(p, REGION_SIZE);
  free_insert((code_t *)p, (code_t *)p + REGION_SIZE);
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
    map_region();
    i = free_find(need);
    psyco_assert(i >= 0);
  }
  start = free_ext[i].start;
  *end = free_ext[i].end;
  memmove(free_ext + i, free_ext + i + 1, (nfree - i - 1) * sizeof(extent_t));
  nfree--;
  NI_UNPOISON(start, *end - start);
  return start;
}

static void owner_add_extent(ni_owner_t *o, code_t *start, code_t *end) {
  NI_STAT(psyco_stat_code_bytes_live += end - start);
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

#if ALL_CHECKS
DEFINEVAR long psyco_stat_code_buffers = 0;
#endif
#if ALL_CHECKS || NI_CODESTATS
DEFINEVAR long psyco_stat_code_bytes = 0;
#endif

static code_t *open_extent(code_t **limit) {
  code_t *start, *end;
  start = take_extent(EXTENT_PREFERRED, EXTENT_MIN, &end);
  if (end - start > EXTENT_MAX) {
    free_insert(start + EXTENT_MAX, end);
    end = start + EXTENT_MAX;
  }
  if (nopen == opencap) {
    int cap = opencap ? 2 * opencap : 16;
    open_extent_t *n = PyMem_Realloc(open_ext, cap * sizeof(open_extent_t));
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

DEFINEFN code_t *psyco_code_open(code_t **limit) {
  NI_STAT(psyco_stat_code_buffers++);
  return open_extent(limit);
}

DEFINEFN int psyco_code_open_count(void) { return nopen; }

DEFINEFN void psyco_code_close(code_t *code) {
  int i;
  for (i = nopen; i--;) {
    open_extent_t x = open_ext[i];
    if (x.start <= code && code <= x.end) {
      memmove(open_ext + i, open_ext + i + 1,
              (nopen - i - 1) * sizeof(open_extent_t));
      nopen--;
      /* checked builds fill the padding, which must stay in the extent */
      if (x.end - code >= 16)
        ALIGN_NO_FILL();
#if ALL_CHECKS || NI_CODESTATS
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

DEFINEFN void psyco_code_continue(code_t **pcode, code_t **plimit) {
  code_t *code = *pcode;
  code_t *next;
  if (code - *plimit > GUARANTEED_MINIMUM - MAXIMUM_SIZE_OF_FAR_JUMP)
    Py_FatalError("psyco: code buffer overflowing");
  next = open_extent(plimit);
  ni_trace_begin_simple_code(code);
  JUMP_TO(next);
  ni_trace_end_simple_code(code);
  psyco_code_close(code);
  *pcode = insn_code_label(next);
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
#if ALL_CHECKS
DEFINEVAR long psyco_stat_code_bytes_live = 0;
DEFINEVAR long psyco_stat_kept_for_good = 0;
#endif

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
    NI_STAT(psyco_stat_kept_for_good++);
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
  if (o->npins++ == 0)
    psyco_owner_track((ni_code_extra_t *)((char *)o -
                                          offsetof(ni_code_extra_t, owner)));
}

DEFINEFN void psyco_owner_pin(PyObject *p) {
  Py_XINCREF(p);
  psyco_owner_keep(p);
}

/* each block is a list link, then the record */
DEFINEFN void *psyco_owner_alloc(size_t size) {
  ni_owner_t *o = psyco_owner;
  void **p = PyMem_Malloc(sizeof(void *) + size);
  if (p == NULL || NI_FAIL(NI_FAIL_OWNER))
    OUT_OF_MEMORY();
  memset(p + 1, 0, size);
  if (o != NULL) {
    *p = o->blocks;
    o->blocks = p;
  }
  return p + 1;
}

static void codebuf_unlist(CodeBufferObject *b);
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
    if (b->ge != NULL && !psyco_ge_in_report(mergepoints, b->ge))
      codebuf_unlist(b);
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
    NI_STAT(psyco_stat_code_bytes_live -= e - s);
    free_insert(s, e);
  }
  PyMem_Free(dead.extents);
  PyMem_Free(dead.bufs);
  psyco_release_notes(dead.notes, dead.nnotes);
  PyMem_Free(dead.notes);
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

/*** Records per address ***/

struct ni_addrmap_item_s {
  void *key, *val;
};

/* linear probing; a removal shifts the rest of the run back, so there are
   no tombstones */
static size_t amap_home(ni_addrmap_t *m, void *key) {
  return (((size_t)key >> 3) * (size_t)0x9E3779B97F4A7C15ULL >> 20) & m->mask;
}

static void *amap_get(ni_addrmap_t *m, void *key) {
  size_t i;
  if (m->items == NULL)
    return NULL;
  for (i = amap_home(m, key); m->items[i].key != NULL; i = (i + 1) & m->mask)
    if (m->items[i].key == key)
      return m->items[i].val;
  return NULL;
}

/* room for one more; -1 if out of memory */
static int amap_reserve(ni_addrmap_t *m) {
  struct ni_addrmap_item_s *old = m->items;
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
      while (m->items[j].key != NULL)
        j = (j + 1) & m->mask;
      m->items[j] = old[i];
    }
  PyMem_Free(old);
  return 0;
}

/* after amap_reserve; 'key' must be new */
static void amap_put(ni_addrmap_t *m, void *key, void *val) {
  size_t i = amap_home(m, key);
  while (m->items[i].key != NULL)
    i = (i + 1) & m->mask;
  m->items[i].key = key;
  m->items[i].val = val;
  m->n++;
}

static void amap_remove(ni_addrmap_t *m, void *key) {
  size_t i, j;
  if (m->items == NULL)
    return;
  for (i = amap_home(m, key); m->items[i].key != key; i = (i + 1) & m->mask)
    if (m->items[i].key == NULL)
      return;
  m->items[i].key = NULL;
  m->n--;
  /* move back each later item of the run whose home isn't after the hole */
  for (j = (i + 1) & m->mask; m->items[j].key != NULL; j = (j + 1) & m->mask) {
    size_t h = amap_home(m, m->items[j].key);
    if (i < j ? (h <= i || h > j) : (h <= i && h > j)) {
      m->items[i] = m->items[j];
      m->items[j].key = NULL;
      i = j;
    }
  }
}

typedef struct {
  PyObject_HEAD
  ni_addrmap_t *map;
  void *key;
  PyObject *value;
} record_t;

/* a GC type for its tp_traverse, which the owner scan follows; never
   tracked, since only owners hold records */
static void record_dealloc(record_t *r) {
  amap_remove(r->map, r->key);
  Py_XDECREF(r->value);
  PyObject_GC_Del(r);
}

static int record_traverse(record_t *r, visitproc visit, void *arg) {
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

DEFINEFN PyObject **psyco_record(ni_addrmap_t *map, void *key) {
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
   importlib's Python code, which Ni compiles, in every program. */

#if ALL_CHECKS
DEFINEVAR long psyco_stat_owner_scans = 0;
DEFINEVAR long psyco_stat_owners_collected = 0;
#endif

DEFINEVAR long ni_owners_counted = 0;
static long extras_since_scan = 0; /* records counted since the last scan */
#define SCAN_MIN 512 /* records counted before the first scan */
static long scan_after = SCAN_MIN; /* more after a scan too big */
static bool scan_asked = false; /* the pending call is queued */
static PyObject *get_objects = NULL; /* gc.get_objects, once imported */
static PyObject *gc_callbacks = NULL, *scan_callback = NULL;

#define SCAN_MAX_NODES ((size_t)1 << 20)
enum { NODE_GC, NODE_CODE, NODE_PRUNED };
typedef struct {
  PyObject *o;
  Py_ssize_t refs;
  char kind, alive;
} node_t;

static struct {
  node_t *nodes;
  size_t n, cap;
  ni_addrmap_t index; /* object -> node number + 1 */
  size_t *stack;
  size_t nstack, stackcap;
  bool failed;
} scan;

/* every code object with a record is a node, so that one only garbage
   reaches is found to be garbage too, whatever its owner holds */
static bool has_record(PyObject *o) {
  return ni_code_extra_get((PyCodeObject *)o) != NULL;
}

/* the node of 'o', made if it is one; -1 if it isn't or the scan failed */
static long scan_node(PyObject *o, char kind) {
  size_t i = (size_t)amap_get(&scan.index, o);
  if (i != 0)
    return i - 1;
  if (kind != NODE_PRUNED) {
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
  scan.nodes[scan.n].refs = Py_REFCNT(o);
  scan.nodes[scan.n].kind = kind;
  scan.nodes[scan.n].alive = false;
  return scan.n++;
}

/* calls 'visit' on every reference node 'i' holds that the scan follows */
static void scan_edges(size_t i, visitproc visit) {
  node_t *nd = &scan.nodes[i];
  if (nd->kind == NODE_CODE) {
    ni_owner_t *o = &ni_code_extra_get((PyCodeObject *)nd->o)->owner;
    size_t j;
    for (j = 0; o->pins != NULL && j <= o->pinmask; j++)
      if (o->pins[j] != NULL)
        visit(o->pins[j], NULL);
  } else if (nd->kind == NODE_GC)
    Py_TYPE(nd->o)->tp_traverse(nd->o, visit, NULL);
}

static int visit_count(PyObject *o, void *unused) {
  long i = scan_node(o, NODE_GC);
  if (i >= 0)
    scan.nodes[i].refs--;
  return 0;
}

static void push_alive(size_t i) {
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

static int visit_alive(PyObject *o, void *unused) {
  size_t i = (size_t)amap_get(&scan.index, o);
  if (i != 0)
    push_alive(i - 1);
  return 0;
}

/* the garbage code objects, as new references, in *out; their count, or
   -1 if the scan didn't finish */
static long scan_find(PyCodeObject ***out) {
  ni_code_extra_t *x;
  PyObject *all;
  PyObject *modules = PyImport_GetModuleDict();
  Py_ssize_t pos = 0;
  PyObject *k, *m;
  size_t i;
  long n = 0;
  while (modules != NULL && PyDict_Check(modules) &&
         PyDict_Next(modules, &pos, &k, &m))
    if (PyModule_Check(m)) {
      scan_node(m, NODE_PRUNED);
      if (PyModule_GetDict(m) != NULL)
        scan_node(PyModule_GetDict(m), NODE_PRUNED);
    }
  /* every function is tracked by CPython's GC, so its code is found
     here; one only something else holds is never a seed, which only
     makes more look alive */
  if (get_objects == NULL) {
    /* not at start-up: importing runs Python code, which Ni compiles */
    PyObject *gc = PyImport_ImportModule("gc");
    if (gc != NULL) {
      get_objects = PyObject_GetAttrString(gc, "get_objects");
      Py_DECREF(gc);
    }
  }
  all = get_objects != NULL ? PyObject_CallObject(get_objects, NULL) : NULL;
  if (all == NULL || !PyList_Check(all)) {
    PyErr_Clear();
    Py_XDECREF(all);
    return -1;
  }
  for (i = 0; i < (size_t)PyList_GET_SIZE(all); i++) {
    PyObject *f = PyList_GET_ITEM(all, i);
    if (PyFunction_Check(f)) {
      PyObject *co = PyFunction_GET_CODE(f);
      x = ni_code_extra_get((PyCodeObject *)co);
      if (x != NULL && x->owner.npins > 0)
        scan_node(co, NODE_CODE);
    }
  }
  Py_DECREF(all);
  for (i = 0; i < scan.n && !scan.failed; i++)
    scan_edges(i, visit_count);
  for (i = 0; i < scan.n && !scan.failed; i++)
    if (scan.nodes[i].refs > 0 || scan.nodes[i].kind == NODE_PRUNED)
      push_alive(i);
  while (scan.nstack > 0 && !scan.failed)
    scan_edges(scan.stack[--scan.nstack], visit_alive);
  if (scan.failed)
    return -1;
  for (i = 0; i < scan.n; i++)
    if (scan.nodes[i].kind == NODE_CODE && !scan.nodes[i].alive)
      n++;
  if (n == 0)
    return 0;
  if ((*out = PyMem_Malloc(n * sizeof(PyCodeObject *))) == NULL)
    return -1;
  n = 0;
  for (i = 0; i < scan.n; i++)
    if (scan.nodes[i].kind == NODE_CODE && !scan.nodes[i].alive) {
      Py_INCREF(scan.nodes[i].o);
      (*out)[n++] = (PyCodeObject *)scan.nodes[i].o;
    }
  return n;
}

static void owners_scan(void) {
  PyCodeObject **dead_co = NULL;
  ni_code_extra_t **dead = NULL;
  long i, n = scan_find(&dead_co);
  PyMem_Free(scan.nodes);
  PyMem_Free(scan.stack);
  PyMem_Free(scan.index.items);
  memset(&scan, 0, sizeof(scan));
  NI_STAT(psyco_stat_owner_scans++);
  if (n < 0)
    scan_after = 2 * (scan_after > ni_owners_counted
                          ? scan_after
                          : ni_owners_counted);
  else
    scan_after = ni_owners_counted / 2 > SCAN_MIN
                     ? ni_owners_counted / 2
                     : SCAN_MIN;
  if (n > 0)
    dead = PyMem_Malloc(n * sizeof(ni_code_extra_t *));
  if (dead == NULL) {
    for (i = 0; i < n; i++)
      Py_DECREF(dead_co[i]); /* found again by the next scan */
    n = 0;
  }
  /* all records are taken off their code objects before any reference is
     dropped, so that code a finaliser runs is compiled afresh */
  for (i = 0; i < n; i++)
    dead[i] = ni_code_extra_detach(dead_co[i]);
  for (i = 0; i < n; i++)
    ni_code_extra_free(dead[i]);
  NI_STAT(psyco_stat_owners_collected += n);
  PyMem_Free(dead);
  for (i = 0; i < n; i++)
    Py_DECREF(dead_co[i]);
  PyMem_Free(dead_co);
}

static int owners_pending(void *arg) {
  PyObject *t, *v, *tb;
  scan_asked = false;
  /* pending calls run in whatever interpreter the main thread is in; one
     skipped is asked for again by the next record counted */
  if (PyThreadState_GET()->interp != PyInterpreterState_Main() ||
      psyco_owner != NULL || ni_compile_depth != 0 || nopen != 0 ||
      psyco_compile_guard != NULL || _Py_IsFinalizing())
    return 0;
  extras_since_scan = 0;
  PyErr_Fetch(&t, &v, &tb);
  owners_scan();
  PyErr_Restore(t, v, tb);
  return 0;
}

static bool scan_listed_last(void) {
  Py_ssize_t n = PyList_GET_SIZE(gc_callbacks);
  return n > 0 && PyList_GET_ITEM(gc_callbacks, n - 1) == scan_callback;
}

static PyObject *owners_gc_callback(PyObject *self, PyObject *args) {
  const char *phase;
  PyObject *info, *gen;
  if (!PyArg_ParseTuple(args, "sO", &phase, &info))
    return NULL;
  /* gc.callbacks are shared by every interpreter */
  if (PyThreadState_GET()->interp != PyInterpreterState_Main())
    Py_RETURN_NONE;
  if (strcmp(phase, "start") == 0) {
    gen = PyDict_Check(info) ? PyDict_GetItemString(info, "generation") : NULL;
    if (extras_since_scan > 0 && gen != NULL && PyLong_Check(gen) &&
        PyLong_AsLong(gen) == 2 && psyco_owner == NULL &&
        ni_compile_depth == 0 && nopen == 0 &&
        psyco_compile_guard == NULL && !_Py_IsFinalizing()) {
      extras_since_scan = 0;
      owners_scan();
    }
  } else if (extras_since_scan == 0 && scan_listed_last()) {
    /* the last one, so that removing it skips no other callback */
    Py_ssize_t n = PyList_GET_SIZE(gc_callbacks);
    if (PyList_SetSlice(gc_callbacks, n - 1, n, NULL) < 0)
      PyErr_Clear();
  }
  Py_RETURN_NONE;
}

static PyMethodDef owners_gc_callback_def = {"ni_owner_scan",
                                             owners_gc_callback, METH_VARARGS,
                                             NULL};

/* lists the callback if gc has been imported */
static void scan_callback_list(void) {
  PyObject *t, *v, *tb;
  PyErr_Fetch(&t, &v, &tb);
  if (scan_callback == NULL) {
    PyObject *gc = PyDict_GetItemString(PyImport_GetModuleDict(), "gc");
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

DEFINEFN void psyco_owner_track(ni_code_extra_t *x) {
  if (x->counted)
    return;
  x->counted = 1;
  ni_owners_counted++;
  extras_since_scan++;
  if (scan_callback == NULL || !scan_listed_last())
    scan_callback_list();
  if (extras_since_scan >= scan_after && !scan_asked &&
      Py_AddPendingCall(owners_pending, NULL) == 0)
    scan_asked = true; /* a full queue: asked again by the next record */
}

DEFINEFN void psyco_owner_untrack(ni_code_extra_t *x) {
  if (x->counted)
    ni_owners_counted--;
  x->counted = 0;
}

static void owners_scan_init(void) {
  if (PyType_Ready(&Record_Type) < 0)
    PyErr_Clear();
}

DEFINEVAR unsigned long psyco_next_serial = 0;

#if NI_CODESTATS
DEFINEVAR PyObject *psyco_codestats_buffers = NULL; /* kind -> [count, bytes] */
DEFINEFN void psyco_codestats_buffer(const char *kind, code_t *start,
                                     code_t *end) {
  long n = end - start;
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
DEFINEVAR int ni_compile_depth = 0;
#if ALL_CHECKS
DEFINEVAR long psyco_stat_compile_ns[NI_ENTER_KINDS];
DEFINEVAR long psyco_stat_compile_count[NI_ENTER_KINDS];
#endif
#if ALL_CHECKS || NI_CODESTATS
DEFINEVAR int psyco_stat_compile_kind = 0;

DEFINEFN long ni_now_ns(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec * 1000000000L + t.tv_nsec;
}
#endif

/* the blocks registered under the current compile guards, to unregister
   if the compile is abandoned */
static CodeBufferObject **journal = NULL;
static int njournal = 0, journalcap = 0;

static void journal_add(CodeBufferObject *b) {
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

DEFINEVAR ni_compile_guard_t *psyco_compile_guard = NULL;
#if ALL_CHECKS
DEFINEVAR long psyco_fail_after[NI_FAIL_SITES];
EXTERNVAR const char *psyco_respawn_file; /* dispatcher.h */
EXTERNVAR int psyco_respawn_line;
#endif

DEFINEFN void psyco_guard_push(ni_compile_guard_t *g) {
  PyThreadState *ts = PyThreadState_GET();
  g->prev = psyco_compile_guard;
  g->owner = psyco_owner;
  g->depth = ni_compile_depth;
  g->nopen = nopen;
  g->njournal = njournal;
  g->tries = 0;
  g->exc_type = ts->curexc_type;
  g->exc_value = ts->curexc_value;
  g->exc_tb = ts->curexc_traceback;
#if ALL_CHECKS
  g->respawn_file = psyco_respawn_file;
  g->respawn_line = psyco_respawn_line;
#endif
  psyco_compile_guard = g;
}

DEFINEFN void psyco_guard_pop(ni_compile_guard_t *g) {
  extra_assert(psyco_compile_guard == g);
  psyco_compile_guard = g->prev;
  if (g->prev == NULL)
    njournal = 0;
}

DEFINEFN void psyco_guard_retry(ni_compile_guard_t *g) {
  if (g->tries > NI_GUARD_RETRIES) {
    psyco_guard_pop(g);
    psyco_out_of_memory(__FILE__, __LINE__);
  }
}

EXTERNFN void psyco_compile_abandoned(void); /* dispatcher.c */

DEFINEFN void psyco_guard_abandon(void) {
  ni_compile_guard_t *g = psyco_compile_guard;
  PyThreadState *ts = PyThreadState_GET();
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
  ni_compile_depth = g->depth;
#if ALL_CHECKS
  /* the jump skipped NI_RESPAWN_SITE's restore: the next point recorded
     would carry the abandoned one's site, which its replay can't match */
  psyco_respawn_file = g->respawn_file;
  psyco_respawn_line = g->respawn_line;
#endif
  psyco_compile_abandoned();
  if (ts->curexc_type != g->exc_type || ts->curexc_value != g->exc_value ||
      ts->curexc_traceback != g->exc_tb) {
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

static void codebuf_unlist(CodeBufferObject *b) {
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
static CodeBufferObject *codebuf_new(PsycoObject *po, global_entries_t *ge,
                                   code_t *codestart, bool proxy) {
  CodeBufferObject *b;
  PyTypeObject *tp;

  if (po == NULL || ge == NULL)
    tp = &CodeBuffer_Type;
  else if (psyco_summary_short(&po->vlocals))
    tp = &CodeBufferShort_Type;
  else
    tp = &CodeBufferSummary_Type;
  b = PyObject_New(CodeBufferObject, tp);
  if (b == NULL || NI_FAIL(NI_FAIL_CODEBUF))
    OUT_OF_MEMORY();
  b->serial = psyco_next_serial++;
  b->ge = NULL;
  b->codestart = codestart;
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
CodeBufferObject *psyco_new_code_buffer(PsycoObject *po, global_entries_t *ge,
                                        code_t **limit) {
  return codebuf_new(po, ge, psyco_code_open(limit), false);
}

DEFINEFN
CodeBufferObject *psyco_proxy_code_buffer(PsycoObject *po,
                                          global_entries_t *ge) {
  return codebuf_new(po, ge, po->code, true);
}

static PyObject *codebuf_repr(CodeBufferObject *self) {
  return PyUnicode_FromFormat("<code buffer ptr %p at %p>", self->codestart,
                                self);
}

#if ALL_CHECKS
DEFINEVAR long psyco_stat_code_buffers_freed = 0;
#endif

static void codebuf_dealloc(CodeBufferObject *self) {
  NI_STAT(psyco_stat_code_buffers_freed++);
  fpo_release(&self->snapshot);
  PyObject_Del(self);
}

#define CODEBUF_TYPE(size)                                                     \
  {                                                                            \
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "CodeBuffer",                     \
    .tp_basicsize = (size), .tp_dealloc = (destructor)codebuf_dealloc,         \
    .tp_repr = (reprfunc)codebuf_repr,                                         \
  }
/* the three sizes of CodeBufferObject (codemanager.h) */
DEFINEVAR PyTypeObject CodeBuffer_Type =
    CODEBUF_TYPE(offsetof(CodeBufferObject, summary));
DEFINEVAR PyTypeObject CodeBufferShort_Type = CODEBUF_TYPE(
    offsetof(CodeBufferObject, summary) + sizeof(fz_summary_head_t));
DEFINEVAR PyTypeObject CodeBufferSummary_Type =
    CODEBUF_TYPE(sizeof(CodeBufferObject));

DEFINEFN
void codemanager_init() {
  PyType_Ready(&CodeBuffer_Type);
  PyType_Ready(&CodeBufferShort_Type);
  PyType_Ready(&CodeBufferSummary_Type);
  owners_scan_init();
}