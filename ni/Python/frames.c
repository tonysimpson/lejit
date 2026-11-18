/*
 * Makes compiled frames visible to Python. stack_frame_info_t records (code,
 * globals) for each compiled call, and the machine stack is walked through the
 * finfo slot that each Psyco frame keeps (x64/iprocessor.c). sys._getframe
 * is replaced by ni_getframe, which counts compiled frames and emulates
 * them. An emulated frame's f_lasti is the call it is in: each record says
 * where the frame out of it called (back_lasti), and the innermost frame
 * notes its place in its slot before a residual call that may run Python
 * (codegen.c:psyco_note_call_site); before its first, f_lineno is the
 * first line. The note and the record also name a map of where the
 * frame's locals are during that call (struct locals_map_s), read when
 * f_locals is asked for (slot_locals): a stack slot, a constant, a
 * virtual value's parts, or a callee-saved register, which the C frames
 * in between keep or saved; those are found by unwinding them with
 * libunwind (slot_registers). Code naming locals, vars, dir, eval or exec
 * is not compiled; reached by another name, they run with the frame
 * emulated and current (cimpl_with_frame). A traceback's frame gets the
 * locals when the exception passes (psyco_frame_write_locals). Known gaps:
 * dead locals (dropped, D15), virtual lists, inlined frames' locals.
 * frame.f_back, frame.f_lineno and frame.f_locals are replaced in
 * PyFrame_Type's dict: a real frame entered
 * through C from compiled code keeps the f_back ceval gave it, which the
 * evaluators put tstate->frame back to, and the getter gives the compiled
 * frames it skips (frame_get_back); a real frame NiCode_Run runs gets its
 * current line from Ni's chain (frame_get_lineno). A traceback frame gets
 * the entry's over it as f_back, or the caller for the one that catches
 * (psyco_frame_link_callee, psyco_frame_caller). C code reading f_back or
 * f_lasti itself bypasses the getters: _warnings.warn runs with every
 * frame linked and placed (ni_warn); other such C code (PyEval_GetFrame,
 * a freshly imported _warnings) still skips compiled frames, and reads
 * the locals a real frame NiCode_Run runs started with.
 * When tracing starts, a compiled frame goes on in ceval
 * (psyco_resume_run); pvisitframes then hides its compiled record, since
 * the real frame now stands for it.
 */
#include "frames.h"
#include "../Objects/pobject.h"
#include "../codemanager.h"
#include "../vcompiler.h"
#include "pyver.h"
#include "pycheader.h"
/* ni_frame_t and its accessors */
#include "mechanism_frames_gen.h"
/* ni_recursion_suspend and ni_recursion_resume */
#include "../codegen.h"
#include "pycompiler.h"
#include "../dispatcher.h"
#include "mechanism_recursion_gen.h"
/* after Python.h, for _GNU_SOURCE: ucontext_t's REG_* */
#define UNW_LOCAL_ONLY
#include <dlfcn.h>
#include <libunwind.h>

/***************************************************************/

/* the instruction of an inlined frame's parent that called it, or -1 */
static int inline_parent_lasti(PsycoObject *po) {
  vinfo_t *v = po->vlocals.items[INDEX_LOC_INLINING];
  if (v == NULL || v->array->count <= LOC_INLINE_NEXT_INSTR ||
      v->array->items[LOC_INLINE_NEXT_INSTR] == NULL)
    return -1;
  return (int)CompileTime_Get(v->array->items[LOC_INLINE_NEXT_INSTR]->source)
             ->value - 2;
}

/* held by the code that passes it, so it lives as long as that code's
   owner (codemanager.h). Each entry says where the frame out of it is,
   for f_lineno: the innermost compiled frame's own place isn't known. */
DEFINEFN
stack_frame_info_t *psyco_finfo(PsycoObject *caller, PsycoObject *callee) {
  Source sglobals;
  stack_frame_info_t *p;
  int inlining = caller != NULL && caller->pr.is_inlining;

  p = psyco_owner_alloc((inlining + 1) * sizeof(stack_frame_info_t));
  p->link_stack_depth = -inlining;
  p->co = callee->pr.co;
  p->back_lasti = caller != NULL ? caller->pr.next_instr - 2
                                 : inline_parent_lasti(callee);
  sglobals = callee->vlocals.items[INDEX_LOC_GLOBALS]->source;
  if (is_compiletime(sglobals))
    p->globals = (PyObject *)CompileTime_Get(sglobals)->value;
  else
    p->globals = NULL; /* uncommon */
  if (inlining) {
    (p + 1)->co = caller->pr.co;
    (p + 1)->back_lasti = inline_parent_lasti(caller);
    sglobals = caller->vlocals.items[INDEX_LOC_GLOBALS]->source;
    if (is_compiletime(sglobals))
      (p + 1)->globals = (PyObject *)CompileTime_Get(sglobals)->value;
    else
      (p + 1)->globals = NULL; /* uncommon */
  }
  return p;
}

static void frame_runtime_capsule_dealloc(PyObject *capsule) {
  FrameRuntime *fr = (FrameRuntime *)PyCapsule_GetPointer(capsule, NULL);
  int i;
  for (i = 0; i < fr->n_entry_locals; i++)
    Py_XDECREF(fr->entry_locals[i]);
  PyMem_Free(fr->entry_locals);
  /* release the references taken in frame_runtime_capsule_new */
  Py_DECREF(fr->py_frame);
  Py_DECREF(fr->psy_code);
  Py_DECREF(fr->psy_globals);
  PyMem_Free(fr);
}

static PyObject *
frame_runtime_capsule_new(PyFrameObject *py_frame,
                          stack_frame_info_t ***psy_frames_start) {
  PyObject *capsule = NULL;
  FrameRuntime *fr = (FrameRuntime *)PyMem_Malloc(sizeof(FrameRuntime));
  if (fr == NULL)
    return PyErr_NoMemory();
  fr->psy_frames_start = psy_frames_start;
  fr->py_frame = py_frame;
  fr->psy_code = py_frame->f_code;
  fr->psy_globals = py_frame->f_globals;
  fr->entry_locals = NULL;
  fr->n_entry_locals = 0;
  capsule = PyCapsule_New((void *)fr, NULL, frame_runtime_capsule_dealloc);
  if (capsule == NULL) {
    PyMem_Free(fr);
    return NULL;
  }
  Py_INCREF(py_frame);
  Py_INCREF(py_frame->f_code);
  Py_INCREF(py_frame->f_globals);
  return capsule;
}

DEFINEFN int
psyco_frame_start_register(PyFrameObject *f,
                           stack_frame_info_t ***psy_frames_start) {
  PyObject *tdict;
  tdict = psyco_thread_dict();
  if (tdict == NULL) {
    return 0;
  }
  PyObject *fr_cap = frame_runtime_capsule_new(f, psy_frames_start);
  if (fr_cap == NULL) {
    return 0;
  }
  if (PyDict_SetItem(tdict, (PyObject *)f, fr_cap) == -1) {
    Py_DECREF(fr_cap);
    return 0;
  }
  Py_DECREF(fr_cap);
  return 1;
}

DEFINEFN FrameRuntime *psyco_frame_start_get(PyFrameObject *f) {
  PyObject *tdict;
  tdict = psyco_thread_dict();
  if (tdict == NULL) {
    return NULL;
  }
  {
    /* most frames are not running compiled code: no entry, and no error */
    PyObject *cap = PyDict_GetItem(tdict, (PyObject *)f);
    if (cap == NULL)
      return NULL;
    return (FrameRuntime *)PyCapsule_GetPointer(cap, NULL);
  }
}

DEFINEFN int psyco_frame_start_unregister(PyFrameObject *f) {
  PyObject *tdict;
  tdict = psyco_thread_dict();
  if (tdict == NULL) {
    return 0;
  }
  if (PyDict_DelItem(tdict, (PyObject *)f) == -1) {
    return 0;
  }
  return 1;
}

/* The real frame's f_localsplus holds what compiled code was started
   with, which it borrows (arguments, cells) until it returns: the first
   write keeps those alive in its FrameRuntime, released with it. */
DEFINEFN
void psyco_frame_write_locals(PyFrameObject *f, int start, PyObject **values,
                              int count) {
  PyCodeObject *co = f->f_code;
  int n = co->co_nlocals + PyTuple_GET_SIZE(co->co_cellvars) +
          PyTuple_GET_SIZE(co->co_freevars);
  int i;
  /* an emulated frame isn't executing, and the first write is from 0 */
  FrameRuntime *fr =
      start == 0 && f->f_executing ? psyco_frame_start_get(f) : NULL;
  if (fr != NULL && fr->entry_locals == NULL) {
    PyObject **keep = PyMem_Malloc(n * sizeof(PyObject *) + 1);
    if (keep == NULL)
      return; /* the locals stay as they were */
    for (i = 0; i < n; i++) {
      keep[i] = f->f_localsplus[i];
      Py_XINCREF(keep[i]);
    }
    fr->entry_locals = keep;
    fr->n_entry_locals = n;
  }
  for (i = 0; i < count && start + i < n; i++) {
    PyObject *old = f->f_localsplus[start + i];
    if (values[i] == NULL)
      continue; /* unbound, or dropped as dead: an argument stays */
    Py_INCREF(values[i]);
    f->f_localsplus[start + i] = values[i];
    Py_XDECREF(old);
  }
}

/* The callee-saved registers of the compiled frame whose slot is
   'slot', as it called C, which keeps them or saves them in its frame:
   found by unwinding the C frames from here down to it. Ni's code has no
   unwind information, so each run of compiled code nearer here (from the
   real frame NiCode_Run runs, innermost first) is stepped over from
   where its glue saved the registers of the C code that called it
   (iprocessor.c:write_glue_run_code_fn). False if that frame isn't the
   innermost of a run, or the walk doesn't come out where expected. */
#define MAX_RUNS 64
static bool slot_registers(stack_frame_info_t **slot, long regs[REG_TOTAL],
                           code_t **ret_ip) {
  stack_frame_info_t **roots[MAX_RUNS], **inners[MAX_RUNS], **f1;
  ni_frame_t *f = ni_frame_current(PyThreadState_GET());
  unw_context_t uc;
  unw_cursor_t c;
  int i, n = 0, steps;
  for (; f != NULL && n < MAX_RUNS; f = ni_frame_back(f)) {
    FrameRuntime *fr = psyco_frame_start_get(ni_frame_object(f));
    if (fr == NULL || !ni_frame_object(f)->f_executing)
      continue;
    roots[n] = *fr->psy_frames_start;
    for (f1 = roots[n]; (((long)(*f1)) & 1) == 0;
         f1 = psyco_next_stack_frame(f1))
      if ((*f1)->link_stack_depth == 0)
        break;
    inners[n] = f1;
    if (inners[n++] == slot)
      break;
  }
  if (n == 0 || inners[n - 1] != slot)
    return false;
  if (unw_getcontext(&uc) < 0 || unw_init_local(&c, &uc) < 0)
    return false;
  for (i = 0; i < n; i++) {
    unw_word_t sp, ip, v;
    long *saved = (long *)roots[i], *ret;
    /* the first frame outside every loaded object is compiled code */
    for (steps = 0;; steps++) {
      Dl_info info;
      if (unw_get_reg(&c, UNW_REG_IP, &ip) < 0)
        return false;
      if (dladdr((void *)ip, &info) == 0)
        break;
      if (steps > 4096 || unw_step(&c) <= 0)
        return false;
    }
    if (unw_get_reg(&c, UNW_X86_64_RSP, &sp) < 0 ||
        sp >= (unw_word_t)inners[i] || (unw_word_t)inners[i] - sp > 65536)
      return false;
    if (i == n - 1) {
      static const int nums[] = {UNW_X86_64_RBX, UNW_X86_64_RBP,
                                 UNW_X86_64_R12, UNW_X86_64_R13,
                                 UNW_X86_64_R14, UNW_X86_64_R15};
      static const int regnums[] = {REG_X64_RBX, REG_X64_RBP, REG_X64_R12,
                                    REG_X64_R13, REG_X64_R14, REG_X64_R15};
      int k;
      *ret_ip = (code_t *)ip;
      for (k = 0; k < REG_TOTAL; k++)
        regs[k] = 0;
      for (k = 0; k < 6; k++) {
        if (unw_get_reg(&c, nums[k], &v) < 0)
          return false;
        regs[regnums[k]] = (long)v;
      }
      return true;
    }
    /* above the slot: R15, R14, R13, R12, RBX, RBP, [pad,] return */
    ret = saved + 7;
    if (*ret == NI_GLUE_PAD)
      ret++;
    uc.uc_mcontext.gregs[REG_R15] = saved[1];
    uc.uc_mcontext.gregs[REG_R14] = saved[2];
    uc.uc_mcontext.gregs[REG_R13] = saved[3];
    uc.uc_mcontext.gregs[REG_R12] = saved[4];
    uc.uc_mcontext.gregs[REG_RBX] = saved[5];
    uc.uc_mcontext.gregs[REG_RBP] = saved[6];
    uc.uc_mcontext.gregs[REG_RIP] = *ret;
    uc.uc_mcontext.gregs[REG_RSP] = (long)(ret + 1);
    if (unw_init_local2(&c, &uc, 0) < 0)
      return false;
  }
  return false;
}

static vinfo_t *lm_vinfo(long **p, const long *regs) {
  long *it = *p;
  vinfo_t *vi;
  int i;
  *p += 3;
  switch (it[0]) {
  case LM_STACK:
    return vinfo_new(RunTime_NewStack((int)it[1], false, false));
  case LM_REG:
    if (regs == NULL || it[1] < 0 || it[1] >= REG_TOTAL)
      return NULL;
    return vinfo_new(CompileTime_New(regs[it[1]]));
  case LM_CONST:
    return vinfo_new(CompileTime_New(it[1]));
  case LM_VIRTUAL:
    vi = vinfo_new(VirtualTime_New((source_virtual_t *)it[1]));
    vi->array = array_new((int)it[2]);
    for (i = 0; i < it[2]; i++)
      vi->array->items[i] = lm_vinfo(p, regs);
    return vi;
  default:
    return NULL;
  }
}

static int n_locals_plus(PyCodeObject *co) {
  return co->co_nlocals + (int)PyTuple_GET_SIZE(co->co_cellvars) +
         (int)PyTuple_GET_SIZE(co->co_freevars);
}

/* The locals of the compiled frame of code 'co' whose stack slot is
   'slot', as new references in values[n_locals_plus(co)], NULL where
   unknown (unbound, dead, or a virtual value not computable here). False
   if the slot doesn't say: the frame is between calls that may run
   Python, or is inlined. Read while the frame is in that call. */
static bool slot_locals(stack_frame_info_t **slot, PyCodeObject *co,
                        PyObject **values) {
  long v = (long)*slot;
  struct locals_map_s *map;
  PyObject *t, *val, *tb;
  long *p, regs[REG_TOTAL], *pregs = NULL;
  char *data;
  int i, n = n_locals_plus(co);
  if (v == -1)
    return false;
  if ((v & 1) != 0) {
    /* a note is for the call the frame is in only if that call returns
       where the note's did */
    code_t *ip;
    map = psyco_note_map(v);
    if (map->nvalues != n || !slot_registers(slot, regs, &ip) ||
        ip != map->retaddr)
      return false;
    pregs = regs;
  } else {
    map = ((stack_frame_info_t *)v)->back_locals;
    if (map == NULL || map->nvalues != n)
      return false;
  }
  data = (char *)slot - map->below;
  p = map->items;
  PyErr_Fetch(&t, &val, &tb);
  for (i = 0; i < n; i++) {
    vinfo_t *vi = lm_vinfo(&p, pregs);
    values[i] = NULL;
    if (vi == NULL)
      continue;
    values[i] = direct_xobj_vinfo(vi, data);
    vinfo_decref(vi, NULL);
    if (values[i] == NULL)
      PyErr_Clear();
    else if (i >= co->co_nlocals && !PyCell_Check(values[i]))
      Py_CLEAR(values[i]);
  }
  PyErr_Restore(t, val, tb);
  return true;
}

/* An emulated frame's locals are read from its compiled frame when first
   asked for (frame_get_locals), not made with it: most emulated frames
   are walked over for their code and line. Its empty value stack holds
   LOCALS_NOTE and its tag meanwhile. A frame no longer running by then
   (no tuple of its code and tag in Ni's chain) has none. */
static char locals_note;
#define LOCALS_NOTE ((PyObject *)&locals_note)

static PyObject *pvisitframes(PyObject *(*callback)(PyObject *, void *),
                              void *data);

static PyObject *visit_tag(PyObject *o, void *f) {
  PyFrameObject *frame = (PyFrameObject *)f;
  if (PyTuple_Check(o) && !PyFrame_Check(PyTuple_GET_ITEM(o, 2)) &&
      PyTuple_GET_ITEM(o, 0) == (PyObject *)frame->f_code &&
      PyLong_AsLong(PyTuple_GET_ITEM(o, 2)) == (long)frame->f_valuestack[1]) {
    Py_INCREF(Py_True);
    return Py_True;
  }
  return NULL;
}

static void fill_noted_locals(PyFrameObject *f) {
  PyObject *found;
  long tag;
  if (f->f_executing || f->f_stacktop != f->f_valuestack ||
      f->f_code->co_stacksize < 2 || f->f_valuestack[0] != LOCALS_NOTE)
    return;
  tag = (long)f->f_valuestack[1];
  found = pvisitframes(visit_tag, f);
  f->f_valuestack[0] = f->f_valuestack[1] = NULL;
  if (found != NULL) {
    Py_DECREF(found);
    slot_locals(psyco_next_stack_frame((stack_frame_info_t **)tag),
                f->f_code, f->f_localsplus);
  }
}

PSY_INLINE PyFrameObject *psyco_build_pyframe(PyObject *co, PyObject *globals,
                                              PyThreadState *tstate) {
  ni_frame_t *back;
  PyFrameObject *result;

  /* frame objects are not created in stack order
     with Psyco, so it's probably better not to
     create plain wrong chained lists */
  back = ni_frame_current(tstate);
  ni_frame_set_current(tstate, NULL);
  result = PyFrame_New(tstate, (PyCodeObject *)co, globals, NULL);
  if (result == NULL)
    OUT_OF_MEMORY();
  result->f_lasti = -1;
  ni_frame_set_current(tstate, back);
  return result;
}

DEFINEFN
PyFrameObject *psyco_emulate_frame(PyObject *o) {
  if (PyFrame_Check(o)) {
    /* a real Python frame */
    Py_INCREF(o);
    return (PyFrameObject *)o;
  } else if (PyFrame_Check(PyTuple_GET_ITEM(o, 2))) {
    /* (co, globals, f, lasti): the real frame whose code NiCode_Run is
       running */
    PyObject *f = PyTuple_GET_ITEM(o, 2);
    Py_INCREF(f);
    return (PyFrameObject *)f;
  } else {
    /* a Psyco frame: emulate it */
    PyObject *co = PyTuple_GetItem(o, 0);
    PyObject *globals = PyTuple_GetItem(o, 1);
    PyFrameObject *result;
    extra_assert(co != NULL);
    extra_assert(globals != NULL);
    long tag = PyLong_AsLong(PyTuple_GET_ITEM(o, 2));
    result = psyco_build_pyframe(co, globals, PyThreadState_GET());
    result->f_lasti = (int)PyLong_AsLong(PyTuple_GET_ITEM(o, 3));
    /* the tag is the slot holding the frame's record, or less one per
       inlined frame before it */
    if ((tag & (sizeof(long) - 1)) == 0) {
      if (((PyCodeObject *)co)->co_stacksize >= 2) {
        result->f_valuestack[0] = LOCALS_NOTE;
        result->f_valuestack[1] = (PyObject *)tag;
      } else {
        slot_locals(psyco_next_stack_frame((stack_frame_info_t **)tag),
                    (PyCodeObject *)co, result->f_localsplus);
      }
    }
    return result;
  }
}

struct sfitmp_s {
  stack_frame_info_t **fi;
  struct sfitmp_s *next;
};

/* The compiled frames now running interpreted (psyco_resume_run): each is
   the first entry of the record in a stack slot, which pvisitframes skips,
   since the real frame stands for it. Slots are stack addresses, so the
   list can mix threads. */
struct hidden_s {
  stack_frame_info_t **slot;
  struct hidden_s *next;
};
static struct hidden_s *hidden_frames = NULL;

static bool is_hidden(stack_frame_info_t **slot) {
  struct hidden_s *h;
  for (h = hidden_frames; h != NULL; h = h->next)
    if (h->slot == slot)
      return true;
  return false;
}

static PyObject *pvisitframes(PyObject *(*callback)(PyObject *, void *),
                              void *data) {
  /* Whenever we run Psyco-produced machine code, we mark the current
     Python frame as the starting point of a chained list of Psyco
     frames. The machine code will update this chained list so that
     psyco_next_stack_frame() can be used to visit the list from
     the outermost to the innermost frames. Note that the list does
     not contain the first Psyco frame, the one directly run by a
     call to psyco_processor_run(). This still gives the expected
     result, because psyco_processor_run() is only called by
     eval_hook.c:NiCode_Run() on a real Python frame, which replaces
     the missing Psyco frame.
     XXX this would no longer work if we filled the emulated frames
         with more information, like local variables */

  PyObject *result = NULL;
  FrameRuntime *fstart;
  ni_frame_t *f = ni_frame_current(PyThreadState_Get());
  bool linked = false; /* the previous frame was emulated, so the compiled
                          frames up to this one are already in the chain
                          (linked_frame) */
  ni_recursion_suspend();
  while (f != NULL) {
    /* is this Python frame the starting point of a chained
       list of Psyco frames ? */
    fstart = linked ? NULL : psyco_frame_start_get(ni_frame_object(f));
    linked = !ni_frame_object(f)->f_executing;
    if (fstart != NULL) {
      /* Yes. Get the list start. */
      struct sfitmp_s *revlist;
      struct sfitmp_s *p;
      PyObject *o;
      PyObject *g;
      long tag;
      stack_frame_info_t **f1;
      stack_frame_info_t **finfo;
      stack_frame_info_t *fdata;
      stack_frame_info_t *flimit;
      int lasti = -1; /* the place of the frame visited next */
      finfo = *(fstart->psy_frames_start);

      /* Enumerate the frames and store them in a
         last-in first-out linked list. The end is marked by
         a pointer with an odd integer value (actually with
         i386 the least significant byte of the integer value
         is -1, and with ivm the end pointer's value is
         exactly 1; but real pointers cannot be odd at all
         because they are aligned anyway). */
      revlist = NULL;
      for (f1 = finfo; (((long)(*f1)) & 1) == 0;
           f1 = psyco_next_stack_frame(f1)) {
        p = (struct sfitmp_s *)PyMem_MALLOC(sizeof(struct sfitmp_s));
        if (p == NULL)
          OUT_OF_MEMORY();
        p->fi = f1;
        p->next = revlist;
        revlist = p;
        if ((*f1)->link_stack_depth == 0)
          break; /* stack top is an inline frame */
      }
      /* the innermost frame's place, if it noted one */
      if ((((long)*f1) & 1) != 0 && (long)*f1 != -1)
        lasti = psyco_note_map((long)*f1)->lasti;

      /* now actually visit them in the correct order */
      while (revlist) {
        p = revlist;
        /* a Psyco frame is represented as
           (co, globals, address_of(*fi)) */
        if (result == NULL) {
          tag = (long)(p->fi);
          fdata = *p->fi;
          flimit = finfo_last(fdata);
          if (hidden_frames != NULL && is_hidden(p->fi)) {
            lasti = fdata->back_lasti;
            if (fdata == flimit)
              goto next_record;
            fdata++, tag--;
          }
          while (1) {
            g = fdata->globals;
            if (g == NULL)
              g = ni_frame_globals(f);
            o = Py_BuildValue("OOli", fdata->co, g, tag, lasti);
            lasti = fdata->back_lasti;
            if (o == NULL)
              OUT_OF_MEMORY();
            result = callback(o, data);
            Py_DECREF(o);
            if (result != NULL)
              break;
            if (fdata == flimit)
              break;
            fdata++, tag--;
          }
        }
      next_record:
        revlist = p->next;
        PyMem_FREE(p);
      }
      if (result != NULL)
        break;

      /* there is still the real Python frame
         which is shadowed by a Psyco frame, i.e. a
         proxy function. Represented as
         (co, globals, f, lasti) */
      o = Py_BuildValue("OOOi", fstart->psy_code, fstart->psy_globals,
                        ni_frame_object(f), lasti);
      if (o == NULL)
        OUT_OF_MEMORY();
      result = callback(o, data);
      Py_DECREF(o);
    } else {
      /* a real unshadowed Python frame */
      result = callback((PyObject *)ni_frame_object(f), data);
    }
    if (result != NULL)
      break;
    f = ni_frame_back(f);
  }
  ni_recursion_resume();
  return result;
}

static PyObject *visit_get_globals(PyObject *o, void *ignored) {
  if (PyFrame_Check(o))
    return ((PyFrameObject *)o)->f_globals;
  else
    return PyTuple_GetItem(o, 1);
}

DEFINEFN
PyObject *psyco_get_globals(void) {
  PyObject *result = pvisitframes(visit_get_globals, NULL);
  if (result == NULL)
    psyco_fatal_msg("sorry, don't know what to do with no globals");
  return result;
}

/* sys._getframe, replaced (as upstream's support.py did) by a version that
   also counts compiled frames, which have no real frame object: they are
   emulated, new objects each time (KNOWN_BUGS C11). */

/* the frame 'depth' calls below the top, or NULL (no exception set),
   with the emulated frames from it out (new, so with no f_back yet)
   linked by f_back to the next frame out. A real frame keeps its own
   f_back, as the evaluators put tstate->frame back to it; frame.f_back
   gives the compiled frames it skips (frame_get_back). With 'undo', a
   list, for C code that reads f_back itself: real frames are relinked
   too, each as (frame, old f_back) in it, and a frame NiCode_Run runs
   gets its place, as (frame, old f_lasti), for restore_links. */
struct link_s {
  int depth;
  PyFrameObject *top, *prev;
  PyObject *undo;
};

static PyObject *visit_link_frame(PyObject *o, void *data) {
  struct link_s *l = (struct link_s *)data;
  PyFrameObject *f, *old;
  PyObject *t;
  if (l->depth > 0) {
    l->depth--;
    return NULL;
  }
  f = psyco_emulate_frame(o);
  if (l->undo != NULL && PyTuple_Check(o) &&
      PyFrame_Check(PyTuple_GET_ITEM(o, 2)) &&
      PyLong_AsLong(PyTuple_GET_ITEM(o, 3)) >= 0 &&
      (t = Py_BuildValue("Oi", f, f->f_lasti)) != NULL) {
    /* a frame NiCode_Run runs, at its current place for the call */
    if (PyList_Append(l->undo, t) == 0)
      f->f_lasti = (int)PyLong_AsLong(PyTuple_GET_ITEM(o, 3));
    Py_DECREF(t);
    PyErr_Clear();
  }
  if (l->top == NULL)
    l->top = f; /* the reference returned */
  else if (l->prev->f_back == NULL)
    l->prev->f_back = f; /* takes the reference */
  else if (l->prev->f_back == f)
    Py_DECREF(f);
  else if (l->undo == NULL ||
           (t = PyTuple_Pack(2, l->prev, old = l->prev->f_back)) == NULL ||
           PyList_Append(l->undo, t) < 0) {
    Py_DECREF(f);
    PyErr_Clear();
    return Py_None; /* stop: the rest is in the real frame's own chain */
  } else {
    Py_DECREF(t);
    Py_DECREF(old); /* the tuple holds it */
    l->prev->f_back = f; /* takes the reference */
  }
  l->prev = f;
  return NULL;
}

static PyFrameObject *linked_frame(int depth, PyObject *undo) {
  struct link_s l = {depth, NULL, NULL, undo};
  pvisitframes(visit_link_frame, &l);
  return l.top;
}

static void restore_links(PyObject *undo) {
  Py_ssize_t i = PyList_GET_SIZE(undo);
  while (i--) {
    PyObject *t = PyList_GET_ITEM(undo, i);
    PyFrameObject *frame = (PyFrameObject *)PyTuple_GET_ITEM(t, 0);
    PyFrameObject *old, *cur;
    if (PyLong_Check(PyTuple_GET_ITEM(t, 1))) {
      frame->f_lasti = (int)PyLong_AsLong(PyTuple_GET_ITEM(t, 1));
      continue;
    }
    old = (PyFrameObject *)PyTuple_GET_ITEM(t, 1);
    cur = frame->f_back;
    Py_INCREF(old);
    frame->f_back = old;
    Py_XDECREF(cur);
  }
}

/* whether NiCode_Run is running 'f' (its f_lasti is -1, or the place of
   the last traceback entry made for it) */
static bool runs_compiled(PyFrameObject *f) {
  return f->f_executing && psyco_frame_start_get(f) != NULL;
}

/* frame.f_back, replaced in PyFrame_Type's dict. A running real frame
   entered through C from compiled code has as f_back the real frame
   NiCode_Run runs, which skips any compiled frames in
   between: they are found where the frame is in Ni's chain and given,
   emulated and linked to the real one. The stored f_back stays, since
   the evaluators put tstate->frame back to it. */
struct back_s {
  PyFrameObject *frame, *back, *top, *prev;
  bool found;
  long tag; /* with 'frame' NULL: the compiled frame to find, by its tag */
  PyCodeObject *co;
};

static PyObject *visit_back_frame(PyObject *o, void *data) {
  struct back_s *b = (struct back_s *)data;
  PyFrameObject *f;
  if (!b->found) {
    if (b->frame == NULL)
      b->found = PyTuple_Check(o) && PyLong_Check(PyTuple_GET_ITEM(o, 2)) &&
                 PyLong_AsLong(PyTuple_GET_ITEM(o, 2)) == b->tag &&
                 PyTuple_GET_ITEM(o, 0) == (PyObject *)b->co;
    else {
      PyObject *real = PyFrame_Check(o) ? o
                       : PyFrame_Check(PyTuple_GET_ITEM(o, 2))
                           ? PyTuple_GET_ITEM(o, 2) : NULL;
      b->found = real == (PyObject *)b->frame;
    }
    return NULL;
  }
  f = psyco_emulate_frame(o);
  if (f == NULL)
    return Py_None; /* stop */
  if (!PyTuple_Check(o) || PyFrame_Check(PyTuple_GET_ITEM(o, 2))) {
    /* a real frame: the one out, as a rule */
    if (b->prev != NULL)
      b->prev->f_back = f; /* takes the reference */
    else
      Py_DECREF(f);
    return Py_None;
  }
  if (b->prev == NULL)
    b->top = f; /* the reference returned */
  else
    b->prev->f_back = f;
  b->prev = f;
  return NULL;
}

/* The traceback frame of a compiled frame that catches an exception gets
   its caller as f_back when that is read, not when it catches, which
   exception-heavy code does often: until then its empty value stack holds
   CALLER_NOTE, the frame NiCode_Run runs under it and its tag
   (pvisitframes), and the caller is found if that frame still runs. */
static char caller_note;
#define CALLER_NOTE ((PyObject *)&caller_note)

static bool has_caller_note(PyFrameObject *f) {
  return f->f_back == NULL && !f->f_executing &&
         f->f_stacktop == f->f_valuestack && f->f_code->co_stacksize >= 3 &&
         f->f_valuestack[0] == CALLER_NOTE;
}

static PyFrameObject *noted_caller(PyFrameObject *f) {
  PyFrameObject *root = (PyFrameObject *)f->f_valuestack[1];
  struct back_s b = {NULL, NULL, NULL, NULL, false,
                     (long)f->f_valuestack[2], f->f_code};
  ni_frame_t *t = ni_frame_current(PyThreadState_GET());
  f->f_valuestack[0] = f->f_valuestack[1] = f->f_valuestack[2] = NULL;
  for (; t != NULL; t = ni_frame_back(t))
    if (ni_frame_object(t) == root)
      break;
  if (t == NULL || !runs_compiled(root))
    return NULL; /* gone */
  b.back = root;
  pvisitframes(visit_back_frame, &b);
  PyErr_Clear();
  if (b.top == NULL && b.found) {
    Py_INCREF(root);
    return root;
  }
  return b.top;
}

static PyObject *frame_get_back(PyFrameObject *frame, void *closure) {
  PyFrameObject *back = frame->f_back;
  if (back == NULL && has_caller_note(frame))
    back = frame->f_back = noted_caller(frame); /* takes the reference */
  if (back == NULL)
    Py_RETURN_NONE;
  if (frame->f_executing && runs_compiled(back)) {
    struct back_s b = {frame, back, NULL, NULL, false, 0, NULL};
    pvisitframes(visit_back_frame, &b);
    PyErr_Clear();
    if (b.top != NULL)
      return (PyObject *)b.top;
  }
  Py_INCREF(back);
  return (PyObject *)back;
}

static PyGetSetDef back_getset = {"f_back", (getter)frame_get_back, NULL,
                                  NULL, NULL};

/* frame.f_lineno, replaced too: a frame NiCode_Run runs doesn't keep its
   f_lasti, so its place comes from Ni's chain, where its tuple carries
   it. The setter (a jump, under tracing) is ceval's. */
static PyObject *lineno_descr = NULL;

static PyObject *visit_root_lasti(PyObject *o, void *frame) {
  if (PyTuple_Check(o) && PyTuple_GET_ITEM(o, 2) == (PyObject *)frame) {
    Py_INCREF(PyTuple_GET_ITEM(o, 3));
    return PyTuple_GET_ITEM(o, 3);
  }
  return NULL;
}

static PyObject *frame_get_lineno(PyFrameObject *frame, void *closure) {
  if (frame->f_trace == NULL && runs_compiled(frame)) {
    PyObject *o = pvisitframes(visit_root_lasti, frame);
    int lasti = o == NULL ? -1 : (int)PyLong_AsLong(o);
    Py_XDECREF(o);
    PyErr_Clear();
    if (lasti >= 0)
      return PyLong_FromLong(PyCode_Addr2Line(frame->f_code, lasti));
  }
  return PyLong_FromLong(PyFrame_GetLineNumber(frame));
}

static int frame_set_lineno(PyFrameObject *frame, PyObject *v, void *c) {
  return Py_TYPE(lineno_descr)->tp_descr_set(lineno_descr, (PyObject *)frame,
                                              v);
}

static PyGetSetDef lineno_getset = {"f_lineno", (getter)frame_get_lineno,
                                    (setter)frame_set_lineno, NULL, NULL};

/* frame.f_locals, replaced in PyFrame_Type's dict: a real frame that
   NiCode_Run runs has in f_localsplus what it started with; its locals
   now are in its slot while it is in a call (slot_locals). */
static PyObject *locals_descr = NULL;

static PyObject *frame_get_locals(PyFrameObject *frame, void *closure) {
  PyObject *result;
  fill_noted_locals(frame);
  if (runs_compiled(frame)) {
    FrameRuntime *fr = psyco_frame_start_get(frame);
    int i, n = n_locals_plus(frame->f_code);
    PyObject **values = PyMem_Malloc(2 * n * sizeof(PyObject *) + 1);
    if (values == NULL)
      return PyErr_NoMemory();
    if (fr->psy_code == frame->f_code &&
        slot_locals(*fr->psy_frames_start, frame->f_code, values)) {
      /* PyFrame_FastToLocals reads them from f_localsplus */
      for (i = 0; i < n; i++) {
        values[n + i] = frame->f_localsplus[i];
        frame->f_localsplus[i] = values[i];
      }
      result = Py_TYPE(locals_descr)
                   ->tp_descr_get(locals_descr, (PyObject *)frame,
                                  (PyObject *)&PyFrame_Type);
      for (i = 0; i < n; i++) {
        frame->f_localsplus[i] = values[n + i];
        Py_XDECREF(values[i]);
      }
      PyMem_Free(values);
      return result;
    }
    PyMem_Free(values);
  }
  return Py_TYPE(locals_descr)
      ->tp_descr_get(locals_descr, (PyObject *)frame,
                     (PyObject *)&PyFrame_Type);
}

static PyGetSetDef locals_getset = {"f_locals", (getter)frame_get_locals,
                                    NULL, NULL, NULL};

/* A traceback entry for a compiled frame, 'f', is being added over
   'callee''s, the frame the exception came out of: that frame, if it has
   returned, gets 'f' as f_back. It is an emulated frame, whose f_back is
   none or another emulated frame of the same code made when its entry
   was, or a real frame entered through C whose f_back is the frame
   NiCode_Run runs, while the code that one runs is in a compiled callee
   or inlined frame, which 'f' is. */
DEFINEFN
void psyco_frame_link_callee(PyFrameObject *callee, PyFrameObject *f) {
  PyFrameObject *back = callee->f_back;
  FrameRuntime *fr;
  if (callee == f || back == f || callee->f_executing)
    return;
  if (back != NULL && !(back->f_code == f->f_code && !back->f_executing)) {
    if (!runs_compiled(back))
      return;
    /* the root slot is even while that code is in a compiled callee or
       inlined frame (psyco_running_real_frame) */
    fr = psyco_frame_start_get(back);
    if ((((long)**fr->psy_frames_start) & 1) != 0)
      return;
  }
  Py_INCREF(f);
  callee->f_back = f;
  Py_XDECREF(back);
}

/* 'f', the traceback frame of the innermost compiled frame, which catches
   an exception, gets that frame's caller as f_back, when read */
DEFINEFN
void psyco_frame_note_caller(PyFrameObject *f) {
  PyFrameObject *root = ni_frame_object(ni_frame_current(PyThreadState_GET()));
  FrameRuntime *fr = root == NULL ? NULL : psyco_frame_start_get(root);
  stack_frame_info_t **f1, **last = NULL;
  if (fr == NULL || f->f_back != NULL || f->f_stacktop != f->f_valuestack ||
      f->f_code->co_stacksize < 3)
    return;
  for (f1 = *fr->psy_frames_start; (((long)*f1) & 1) == 0;
       f1 = psyco_next_stack_frame(f1)) {
    last = f1;
    if ((*f1)->link_stack_depth == 0)
      break;
  }
  if (last == NULL)
    return;
  f->f_valuestack[0] = CALLER_NOTE;
  f->f_valuestack[1] = (PyObject *)root;
  f->f_valuestack[2] =
      (PyObject *)((long)last - (finfo_last(*last) - *last));
}
DEFINEFN
PyFrameObject *psyco_running_real_frame(PyCodeObject *co) {
  ni_frame_t *f = ni_frame_current(PyThreadState_GET());
  FrameRuntime *fstart;
  if (f == NULL || ni_frame_code(f) != co)
    return NULL;
  fstart = psyco_frame_start_get(ni_frame_object(f));
  /* the top compiled frame's finfo slot is odd (-1) except while it is in a
     compiled sub-call (ABOUT_TO_CALL_SUBFUNCTION) */
  if (fstart == NULL || (((long)**fstart->psy_frames_start) & 1) == 0)
    return NULL;
  return ni_frame_object(f);
}

static PyObject *ni_getframe(PyObject *self, PyObject *args) {
  int depth = 0;
  PyFrameObject *top;
  if (!PyArg_ParseTuple(args, "|i:_getframe", &depth))
    return NULL;
  if (depth < 0)
    depth = 0;
  top = linked_frame(depth, NULL);
  if (top == NULL) {
    PyErr_SetString(PyExc_ValueError, "call stack is not deep enough");
    return NULL;
  }
  return (PyObject *)top;
}

/* _warnings.warn walks tstate->frame in C: it runs with the compiled
   frames over the current real frame emulated and current. */
static PyObject *original_warn = NULL;

static PyObject *ni_warn(PyObject *self, PyObject *args, PyObject *kwds) {
  PyThreadState *ts = PyThreadState_GET();
  PyFrameObject *saved = ts->frame, *top;
  PyObject *result, *undo = PyList_New(0);
  if (undo == NULL)
    return NULL;
  /* every frame is linked for the call, and put back after it */
  top = linked_frame(0, undo);
  if (top != NULL && top != saved)
    ts->frame = top;
  result = PyObject_Call(original_warn, args, kwds);
  ts->frame = saved;
  restore_links(undo);
  Py_DECREF(undo);
  Py_XDECREF(top);
  return result;
}

/* locals, vars, dir, eval and exec read their caller's frame through
   PyEval_GetFrame, which is tstate->frame. Called from compiled code
   (through a meta of each: code naming them is left to the interpreter,
   mergepoints.c, but they are reached by other names), they run as warn
   does, with the calling frame emulated, its locals in, and current. A
   real frame NiCode_Run runs stands in as a copy with its locals now. */
#define N_FRAME_BUILTINS 5
static const char *const frame_builtin_names[N_FRAME_BUILTINS] = {
    "locals", "vars", "dir", "eval", "exec"};
static PyMethodDef *frame_builtins[N_FRAME_BUILTINS];

static PyObject *visit_root_lasti(PyObject *o, void *frame);
static PyObject *frame_get_back(PyFrameObject *frame, void *closure);

static PyFrameObject *root_copy(PyFrameObject *f) {
  FrameRuntime *fr = psyco_frame_start_get(f);
  PyFrameObject *c;
  PyObject *o;
  if (fr->psy_code != f->f_code)
    return f;
  c = psyco_build_pyframe((PyObject *)f->f_code, f->f_globals,
                          PyThreadState_GET());
  if (!slot_locals(*fr->psy_frames_start, f->f_code, c->f_localsplus)) {
    Py_DECREF(c);
    return f;
  }
  o = pvisitframes(visit_root_lasti, f);
  c->f_lasti = o == NULL ? -1 : (int)PyLong_AsLong(o);
  Py_XDECREF(o);
  c->f_back = (PyFrameObject *)frame_get_back(f, NULL);
  if (c->f_back == NULL) {
    PyErr_Clear();
  } else if ((PyObject *)c->f_back == Py_None) {
    Py_DECREF(Py_None);
    c->f_back = NULL;
  }
  Py_DECREF(f);
  return c;
}

static PyObject *cimpl_with_frame(long which, PyObject *self,
                                  PyObject *args) {
  PyMethodDef *ml = frame_builtins[which];
  int flags = ml->ml_flags & ~(METH_CLASS | METH_STATIC | METH_COEXIST);
  Py_ssize_t n = args == NULL ? 0 : PyTuple_GET_SIZE(args);
  PyThreadState *ts = PyThreadState_GET();
  PyFrameObject *saved = ts->frame, *top = NULL;
  PyObject *result, *undo = NULL;
  /* locals always reads the frame; vars and dir with no argument, eval
     and exec with no globals */
  if (which == 0 || n < (which >= 3 ? 2 : 1)) {
    undo = PyList_New(0);
    if (undo == NULL)
      return NULL;
    top = linked_frame(0, undo);
    if (top != NULL && runs_compiled(top))
      top = root_copy(top);
    else if (top != NULL)
      fill_noted_locals(top);
    if (top != NULL && top != saved)
      ts->frame = top;
  }
  switch (flags) {
  case METH_NOARGS:
    result = ml->ml_meth(self, NULL);
    break;
  case METH_VARARGS:
    result = ml->ml_meth(self, args);
    break;
  case METH_FASTCALL:
    result = ((_PyCFunctionFast)ml->ml_meth)(self, &PyTuple_GET_ITEM(args, 0),
                                             n);
    break;
  case METH_FASTCALL | METH_KEYWORDS:
    result = ((_PyCFunctionFastWithKeywords)ml->ml_meth)(
        self, &PyTuple_GET_ITEM(args, 0), n, NULL);
    break;
  default:
    PyErr_SetString(PyExc_SystemError, "Ni: unexpected builtin form");
    result = NULL;
  }
  ts->frame = saved;
  if (undo != NULL) {
    restore_links(undo);
    Py_DECREF(undo);
  }
  Py_XDECREF(top);
  return result;
}

/* exec can change a mutable local (a virtual list, say) through the
   locals mapping, so each virtual local is made a real object first */
static bool locals_made_real(PsycoObject *po) {
  int i, n = n_locals_plus(po->pr.co);
  for (i = 0; i < n; i++) {
    vinfo_t *v = LOC_LOCALS_PLUS[i];
    if (v != NULL && is_virtualtime(v->source) && !compute_vinfo(v, po))
      return false;
  }
  return true;
}

#define FRAME_META(i)                                                          \
  static vinfo_t *meta_with_frame##i(PsycoObject *po, vinfo_t *vself,         \
                                     vinfo_t *vargs) {                         \
    if (!po->pr.is_inlining && !locals_made_real(po))                          \
      return NULL;                                                             \
    if (vargs == NULL)                                                         \
      return psyco_generic_call(po, cimpl_with_frame, CfCommonNewRefPyObject, \
                                "lvl", (long)(i), vself, 0L);                  \
    return psyco_generic_call(po, cimpl_with_frame, CfCommonNewRefPyObject,   \
                              "lvv", (long)(i), vself, vargs);                 \
  }
FRAME_META(0)
FRAME_META(1)
FRAME_META(2)
FRAME_META(3)
FRAME_META(4)
#undef FRAME_META
static void *const frame_metas[N_FRAME_BUILTINS] = {
    meta_with_frame0, meta_with_frame1, meta_with_frame2, meta_with_frame3,
    meta_with_frame4};

static void frame_builtins_init(void) {
  PyObject *b = PyEval_GetBuiltins();
  int i;
  for (i = 0; i < N_FRAME_BUILTINS; i++) {
    PyObject *fn = b == NULL ? NULL
                             : PyDict_GetItemString(b, frame_builtin_names[i]);
    if (fn == NULL || !PyCFunction_Check(fn))
      continue;
    frame_builtins[i] = ((PyCFunctionObject *)fn)->m_ml;
    Psyco_DefineMeta(frame_builtins[i]->ml_meth, frame_metas[i]);
  }
}

static PyMethodDef ni_warn_def = {
    "warn", (PyCFunction)ni_warn, METH_VARARGS | METH_KEYWORDS,
    "warn(message, category=None, stacklevel=1)\n\n"
    "Issue a warning, or maybe ignore it or raise an exception, counting\n"
    "frames of code compiled by Ni for stacklevel."};

/* replaces warn in _warnings and, if imported, warnings */
static void warn_init(void) {
  PyObject *mod = PyImport_ImportModule("_warnings"), *fn, *w;
  if (mod == NULL) {
    PyErr_Clear(); /* no C warnings: warnings.py walks sys._getframe */
    return;
  }
  original_warn = PyObject_GetAttrString(mod, "warn");
  fn = PyCFunction_New(&ni_warn_def, NULL);
  if (original_warn == NULL || fn == NULL ||
      PyObject_SetAttrString(mod, "warn", fn) < 0)
    PyErr_Print();
  w = PyDict_GetItemString(PyImport_GetModuleDict(), "warnings");
  if (w != NULL && fn != NULL) {
    PyObject *cur = PyObject_GetAttrString(w, "warn");
    if (cur == original_warn && PyObject_SetAttrString(w, "warn", fn) < 0)
      PyErr_Print();
    Py_XDECREF(cur);
    PyErr_Clear();
  }
  Py_XDECREF(fn);
  Py_DECREF(mod);
}

static PyMethodDef ni_getframe_def = {
    "_getframe", ni_getframe, METH_VARARGS,
    "_getframe([depth]) -> frameobject\n\n"
    "Return a frame object from the call stack, counting frames of code\n"
    "compiled by Ni (emulated)."};

INITIALIZATIONFN
void psyco_frames_init(void) {
  /* Upstream also hooked _PyThreadState_GetFrame (in 3.7,
     _PyRuntime.gilstate.getframe, behind PyEval_GetFrame), not done yet */
  PyObject *sysmod = PyImport_ImportModule("sys");
  PyObject *fn = PyCFunction_New(&ni_getframe_def, NULL);
  if (sysmod == NULL || fn == NULL ||
      PyObject_SetAttrString(sysmod, "_getframe", fn) < 0)
    PyErr_Print();
  Py_XDECREF(fn);
  Py_XDECREF(sysmod);
  warn_init();
  frame_builtins_init();
  fn = PyDescr_NewGetSet(&PyFrame_Type, &back_getset);
  if (fn == NULL ||
      PyDict_SetItemString(PyFrame_Type.tp_dict, "f_back", fn) < 0)
    PyErr_Print();
  Py_XDECREF(fn);
  lineno_descr = PyDict_GetItemString(PyFrame_Type.tp_dict, "f_lineno");
  Py_XINCREF(lineno_descr);
  fn = lineno_descr == NULL ? NULL
                            : PyDescr_NewGetSet(&PyFrame_Type, &lineno_getset);
  if (fn == NULL ||
      PyDict_SetItemString(PyFrame_Type.tp_dict, "f_lineno", fn) < 0)
    PyErr_Print();
  Py_XDECREF(fn);
  locals_descr = PyDict_GetItemString(PyFrame_Type.tp_dict, "f_locals");
  Py_XINCREF(locals_descr);
  fn = locals_descr == NULL ? NULL
                            : PyDescr_NewGetSet(&PyFrame_Type, &locals_getset);
  if (fn == NULL ||
      PyDict_SetItemString(PyFrame_Type.tp_dict, "f_locals", fn) < 0)
    PyErr_Print();
  Py_XDECREF(fn);
  PyType_Modified(&PyFrame_Type);
}

/***************************************************************/
/* A compiled frame going on interpreted (pycompiler.c:resume_if_tracing):
   its locals, value stack and blocks are copied into a frame object, the
   real one NiCode_Run runs or a new one, and ceval's own loop resumes it
   after f_lasti, as it resumes a generator. */

struct resume_s {
  PyFrameObject *f;
  stack_frame_info_t **slot; /* the compiled frame's record, or NULL */
  int nvalues, iblock;
  PyTryBlock blocks[CO_MAXBLOCKS];
  PyObject *values[1];
};

/* the slot of the record whose first entry is the innermost compiled
   frame, NULL when that frame is the real one 'real' is set to */
static stack_frame_info_t **innermost_slot(PyFrameObject **real) {
  ni_frame_t *f = ni_frame_current(PyThreadState_GET());
  FrameRuntime *fstart =
      f == NULL ? NULL : psyco_frame_start_get(ni_frame_object(f));
  stack_frame_info_t **f1, **last = NULL;
  *real = NULL;
  if (fstart == NULL)
    return NULL;
  *real = fstart->py_frame;
  for (f1 = *(fstart->psy_frames_start); (((long)(*f1)) & 1) == 0;
       f1 = psyco_next_stack_frame(f1)) {
    last = f1;
    if ((*f1)->link_stack_depth == 0)
      break;
  }
  return last;
}

DEFINEFN
struct resume_s *psyco_resume_begin(PyCodeObject *co, PyObject *globals,
                                    PyObject *builtins, long nvalues) {
  PyFrameObject *real;
  struct resume_s *st =
      PyMem_Malloc(sizeof(struct resume_s) + nvalues * sizeof(PyObject *));
  if (st == NULL) {
    PyErr_NoMemory();
    return NULL;
  }
  st->slot = innermost_slot(&real);
  if (st->slot == NULL && real != NULL && real->f_code == co) {
    st->f = real;
    Py_INCREF(real);
  } else {
    st->f = PyFrame_New(PyThreadState_GET(), co, globals, NULL);
    if (st->f == NULL) {
      PyMem_Free(st);
      return NULL;
    }
    Py_INCREF(builtins);
    Py_XSETREF(st->f->f_builtins, builtins);
  }
  st->nvalues = 0;
  st->iblock = 0;
  return st;
}

DEFINEFN
void psyco_resume_value(struct resume_s *st, long i, PyObject *v) {
  Py_XINCREF(v);
  st->values[i] = v;
  st->nvalues = i + 1;
}

DEFINEFN
void psyco_resume_block(struct resume_s *st, long type, long handler,
                        long level) {
  PyTryBlock *b = &st->blocks[st->iblock++];
  b->b_type = type;
  b->b_handler = handler;
  b->b_level = level;
}

/* Stand-ins for the trace and profile functions while the resumed frame
   starts: ceval would give it a 'call' event, which CPython never gives a
   frame already running when tracing starts. Each puts its own function
   back on its first event, before any of the frame's code runs. */
typedef struct {
  Py_tracefunc func;
  PyObject *obj;
} hook_t;

static int skip_call_trace(PyObject *obj, PyFrameObject *frame, int what,
                           PyObject *arg) {
  hook_t *h = (hook_t *)obj;
  PyThreadState *ts = PyThreadState_GET();
  ts->c_tracefunc = h->func;
  ts->c_traceobj = h->obj;
  if (what == PyTrace_CALL)
    return 0;
  return h->func(h->obj, frame, what, arg);
}

static int skip_call_profile(PyObject *obj, PyFrameObject *frame, int what,
                             PyObject *arg) {
  hook_t *h = (hook_t *)obj;
  PyThreadState *ts = PyThreadState_GET();
  ts->c_profilefunc = h->func;
  ts->c_profileobj = h->obj;
  if (what == PyTrace_CALL)
    return 0;
  return h->func(h->obj, frame, what, arg);
}

DEFINEFN
PyObject *psyco_resume_run(struct resume_s *st, long lasti, long nstack) {
  PyFrameObject *f = st->f;
  PyThreadState *ts = PyThreadState_GET();
  int nlocals = st->nvalues - nstack;
  int i;
  PyObject *result;
  hook_t trace = {ts->c_tracefunc, ts->c_traceobj};
  hook_t profile = {ts->c_profilefunc, ts->c_profileobj};
  struct hidden_s hidden = {st->slot, hidden_frames};
  extra_assert(nlocals == f->f_code->co_nlocals +
                              PyTuple_GET_SIZE(f->f_code->co_cellvars) +
                              PyTuple_GET_SIZE(f->f_code->co_freevars));
  extra_assert(nstack <= f->f_code->co_stacksize);

  /* the old locals go only once all the new ones are in: one may hold
     the last reference to another's value */
  for (i = 0; i < nlocals; i++) {
    PyObject *old = f->f_localsplus[i];
    f->f_localsplus[i] = st->values[i];
    st->values[i] = old;
  }
  for (i = 0; i < nstack; i++)
    f->f_valuestack[i] = st->values[nlocals + i];
  f->f_stacktop = f->f_valuestack + nstack;
  f->f_lasti = lasti;
  f->f_iblock = st->iblock;
  memcpy(f->f_blockstack, st->blocks, st->iblock * sizeof(PyTryBlock));
  for (i = 0; i < nlocals; i++)
    Py_XDECREF(st->values[i]);

  if (st->slot != NULL)
    hidden_frames = &hidden;
  if (!ts->tracing) {
    if (trace.func != NULL) {
      ts->c_tracefunc = skip_call_trace;
      ts->c_traceobj = (PyObject *)&trace;
    }
    if (profile.func != NULL) {
      ts->c_profilefunc = skip_call_profile;
      ts->c_profileobj = (PyObject *)&profile;
    }
  }
  result = _PyEval_EvalFrameDefault(f, 0);
  /* not put back if the frame never started (RecursionError) */
  if (ts->c_tracefunc == skip_call_trace) {
    ts->c_tracefunc = trace.func;
    ts->c_traceobj = trace.obj;
  }
  if (ts->c_profilefunc == skip_call_profile) {
    ts->c_profilefunc = profile.func;
    ts->c_profileobj = profile.obj;
  }
  if (st->slot != NULL) {
    struct hidden_s **h = &hidden_frames;
    while (*h != &hidden)
      h = &(*h)->next;
    *h = hidden.next;
  }
  Py_DECREF(f);
  PyMem_Free(st);
  return result;
}
