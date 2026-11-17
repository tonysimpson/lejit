/*
 * Compile-time Python-to-Python calls (psyco_call_pyfunc). When the callee and
 * its argument count are known at compile time, the arguments are bound now.
 * The callee is then either inlined into the caller (the EInline
 * pseudo-exception swaps po->vlocals) or compiled separately and called with
 * the Psyco calling convention (psyco_call_psyco). Closure code is called this
 * way too: the closure's cells are passed as extra inputs into the
 * free-variable slots, where LOAD_DEREF reads them as the borrowed cells of
 * a frame. They are the virtual closure tuple's items while the function is
 * still the one MAKE_FUNCTION made (comprehensions, local lambdas),
 * constants read now for a constant function (a method, a decorated
 * global), or loaded at run time from func_closure (psyco_call_closure).
 * Anything else (keyword-only arguments, code with cells of its own,
 * unknown arity, recursion depth used up) becomes a residual call of the
 * function object, as does a call made while a trace or profile function is
 * set (tested at run time: tracing_on).
 * PsycoFunctionObject (the "proxy") is a
 * leftover of psyco.bind() and is dead code under the eval-frame hook.
 */
#include "psyfunc.h"
#include "Objects/pfuncobject.h"
#include "Objects/ptupleobject.h"
#include "Python/frames.h"
#include "Python/pycompiler.h"
#include "codemanager.h"
#include "mergepoints.h"
#include "pycodegen.h"
#include "vcompiler.h"


/***************************************************************/
/***   Frame and arguments building                            ***/
/***************************************************************/

static void fix_run_time_args(PsycoObject *po, vinfo_array_t *target,
                              vinfo_array_t *source, RunTimeSource *sources) {
  int i = target->count;
  extra_assert(target->count <= source->count);
  while (i--) {
    vinfo_t *a = source->items[i];
    if (a != NULL && a->tmp != NULL) {
      vinfo_t *b = a->tmp;
      if (is_runtime(a->source)) {
        if (target->items[i] == NULL)
          continue; /* item was removed by psyco_simplify_array() */
        if (sources != NULL) {
          int argc = (po->stack_depth - INITIAL_STACK_DEPTH) / sizeof(long);
          sources[argc] = a->source;
        }
        po->stack_depth += sizeof(long);
        /* arguments get borrowed references */
        b->source = RunTime_NewStack(po->stack_depth, false, false);
      }
      extra_assert(b == target->items[i]);
      a->tmp = NULL;
      if (a->array != NullArray)
        fix_run_time_args(po, b->array, a->array, sources);
    }
  }
}

struct fncall_arg_s {
  PyCodeObject *co;
  PyObject *merge_points;
  vinfo_array_t *inputvinfos; /* globals, arguments, then the free cells */
  int nfrees;
  int po_size;
};

/* free variables only when the caller has the cells (psyco_call_pyfunc);
   code with cells of its own needs them made at entry: not done */
static bool fncall_init(struct fncall_arg_s *fncall, PyCodeObject *co,
                        int nfrees) {
  if (co->co_flags & CO_VARKEYWORDS) {
    return false;
  }
  if (PyTuple_GET_SIZE(co->co_cellvars) != 0 ||
      PyTuple_GET_SIZE(co->co_freevars) != nfrees) {
    return false;
  }
  fncall->co = co;
  fncall->nfrees = nfrees;
  fncall->merge_points = psyco_get_merge_points(co, 0);
  return fncall->merge_points != Py_None;
}

static bool fncall_collect_arguments(struct fncall_arg_s *fncall,
                                     vinfo_t *vglobals, vinfo_t *vbuiltins,
                                     vinfo_t **argarray,
                                     int argcount, vinfo_t **defarray,
                                     int defcount, vinfo_t **frees) {
  PyCodeObject *co = fncall->co;
  vinfo_array_t *inputvinfos;
  int i, minargcnt, inputargs, extras;

  minargcnt = co->co_argcount - defcount;
  inputargs = argcount;
  if (inputargs != co->co_argcount) {
    if (inputargs > co->co_argcount && (co->co_flags & CO_VARARGS))
      /* ok, extra args will be collected below */;
    else {
      if (inputargs < minargcnt || inputargs > co->co_argcount)
        return false; /* the caller makes the real call, which raises */
      inputargs = co->co_argcount; /* add default arguments */
    }
  }

  /* Collect all input vinfo_t's (globals, arguments, needed default values)
     into a new array that mimics the PsycoObject's vlocals. */
  inputvinfos = array_new(INDEX_LOC_LOCALS_PLUS + inputargs + fncall->nfrees);
  inputvinfos->items[INDEX_LOC_GLOBALS] = vglobals;
  inputvinfos->items[INDEX_LOC_BUILTINS] = vbuiltins;
  for (i = 0; i < argcount; i++)
    inputvinfos->items[INDEX_LOC_LOCALS_PLUS + i] = argarray[i];
  for (; i < inputargs; i++)
    inputvinfos->items[INDEX_LOC_LOCALS_PLUS + i] = defarray[i - minargcnt];
  for (i = 0; i < fncall->nfrees; i++)
    inputvinfos->items[INDEX_LOC_LOCALS_PLUS + inputargs + i] = frees[i];

  extras = co->co_stacksize + co->co_nlocals + fncall->nfrees; /* no cells */
  fncall->inputvinfos = inputvinfos;
  fncall->po_size = INDEX_LOC_LOCALS_PLUS + extras;
  return true;
}

PSY_INLINE void fncall_finish_store(struct fncall_arg_s *fncall,
                                    PsycoObject *po) {
  array_release(fncall->inputvinfos);

  /* store the code object */
  /* borrowed (C14): the code object's co_extra owns its compiled code, so
     an owned reference here was a cycle that kept both forever. Whenever
     this code can run, something else holds the code: the frame the eval
     hook was given, or the caller's promotion cache, which keeps every
     function it switched on */
  po->pr.co = fncall->co;
  pyc_data_build(po, fncall->merge_points);
}

static void fncall_store_arguments(struct fncall_arg_s *fncall,
                                   vinfo_t **vlocals) {
  PyCodeObject *co = fncall->co;
  vinfo_array_t *arraycopy = fncall->inputvinfos;
  vinfo_t **pp;
  int i;
  int inputargs = arraycopy->count - INDEX_LOC_LOCALS_PLUS - fncall->nfrees;
  int j;

  /* initialize po->vlocals */
  vlocals[INDEX_LOC_GLOBALS] = arraycopy->items[INDEX_LOC_GLOBALS];
  vlocals[INDEX_LOC_BUILTINS] = arraycopy->items[INDEX_LOC_BUILTINS];

  /* move the arguments into their target place,
     excluding the ones that map to the '*' parameter */
  pp = arraycopy->items + INDEX_LOC_LOCALS_PLUS;
  for (i = 0; i < co->co_argcount; i++)
    vlocals[INDEX_LOC_LOCALS_PLUS + i] = *pp++;
  if (co->co_flags & CO_VARARGS) {
    /* store the extra args in a virtual-time tuple */
    vlocals[INDEX_LOC_LOCALS_PLUS + i] = PsycoTuple_New(inputargs - i, pp);
    for (; inputargs > i; inputargs--)
      vinfo_decref(*pp++, NULL);
    i++;
  } else
    extra_assert(i == inputargs);

  /* the rest of locals is uninitialized */
  for (; i < co->co_nlocals; i++)
    vlocals[INDEX_LOC_LOCALS_PLUS + i] = psyco_vi_Zero();
  /* the free cells follow the locals, as in a frame */
  pp = arraycopy->items + arraycopy->count - fncall->nfrees;
  for (j = 0; j < fncall->nfrees; j++)
    vlocals[INDEX_LOC_LOCALS_PLUS + co->co_nlocals + j] = *pp++;
  /* the rest of the array is the currently empty stack,
     set to NULL by array_new(). */
}

/* Build a PsycoObject "frame" corresponding to the call of a Python
   function.  If 'sources!=NULL', it is set to an array of the sources of
   the values that must be pushed to make the call. */
static PsycoObject *psyco_build_frame(struct fncall_arg_s *fncall,
                                      int recursion, RunTimeSource **sources) {
  /* build a "frame" in a PsycoObject according to the given code object. */
  vinfo_array_t *arraycopy;
  PsycoObject *po;
  int rtcount;
  RunTimeSource *source1;

  po = PsycoObject_New(fncall->po_size);
  po->stack_depth = INITIAL_STACK_DEPTH;
  po->vlocals.count = fncall->po_size;
  INIT_PROCESSOR_PSYCOOBJECT(po);
  po->pr.auto_recursion = AUTO_RECURSION(recursion);

  /* duplicate the inputvinfos. If two arguments share some common part, they
     will also share it in the copy. */
  clear_tmp_marks(fncall->inputvinfos);
  arraycopy = array_new(fncall->inputvinfos->count);
  duplicate_array(arraycopy, fncall->inputvinfos);

  /* simplify arraycopy in the sense of psyco_simplify_array() */
  rtcount = psyco_simplify_array(arraycopy, NULL);

  /* all run-time arguments or argument parts must be corrected: in the
     input vinfo_t's they have arbitrary sources, but in the new frame's
     sources they will have to be fetched from the machine stack, where
     the caller will have pushed them. */
  if (sources != NULL) {
    source1 = PyMem_NEW(RunTimeSource, rtcount);
    if (source1 == NULL && rtcount > 0)
      OUT_OF_MEMORY();
    *sources = source1;
  } else
    source1 = NULL;
  fix_run_time_args(po, arraycopy, fncall->inputvinfos, source1);
  array_release(fncall->inputvinfos);
  fncall->inputvinfos = arraycopy;

  /* initialize po->vlocals */
  fncall_store_arguments(fncall, po->vlocals.items);
  fncall_finish_store(fncall, po);

  /* set up the CALL return address */
  po->stack_depth += sizeof(long);
  LOC_CONTINUATION = vinfo_new(RunTime_NewStack(po->stack_depth, false, false));
  return po;
}


static vinfo_t *call_with_inline_frame(PsycoObject *po,
                                       struct fncall_arg_s *fncall,
                                       int recursion) {
  /* note: 'recursion' is ignored */
  vinfo_t *vresult;
  if (LOC_INLINING != NULL) /* we have the result ready from the
                               initial case below */
  {
    vresult = LOC_INLINING;
    LOC_INLINING = NULL;
  } else {
    /* initial case: collect the arguments from the call into an
       EInline pseudo-exception */
    int i;
    vinfo_t **pp = fncall->inputvinfos->items;
    vinfo_t *v = vinfo_new(SOURCE_NOT_IMPORTANT);
    v->array = array_new(TOTAL_LOC_INLINE + fncall->po_size);
    v->array->items[LOC_INLINE_CO] =
        vinfo_new(CompileTime_New((long)fncall->co)); /* borrowed, as above */
    for (i = fncall->inputvinfos->count; i--;)
      if (pp[i] != NULL)
        vinfo_incref(pp[i]);
    fncall_store_arguments(fncall, v->array->items + TOTAL_LOC_INLINE);
    PycException_Raise(po, vinfo_new(VirtualTime_New(&EInline)), v);
    vresult = NULL;
  }
  array_release(fncall->inputvinfos);
  return vresult;
}

DEFINEFN
vinfo_t *psyco_save_inline_po(PsycoObject *po) {
  ni_code_extra_t *x;
  int i = po->vlocals.count;
  vinfo_t **pp = po->vlocals.items;
  vinfo_t *v = vinfo_new(VirtualTime_New(&EInline));
  v->array = array_new(TOTAL_LOC_INLINE + i);
  /* fixed: the return reads them at compile time, so a match must never
     make them run-time (a join point in the inlined frame, D44) */
  v->array->items[LOC_INLINE_CO] =
      vinfo_new(CompileTime_NewSk(sk_new((long)po->pr.co, SkFlagFixed)));
  v->array->items[LOC_INLINE_NEXT_INSTR] =
      vinfo_new(CompileTime_NewSk(sk_new(po->pr.next_instr, SkFlagFixed)));
  /* The caller's code object is borrowed, so once it dies another can take
     its address. Blocks compiled inside the inlined frame stay in the
     callee's join points (vcompiler.c:psyco_compile_join), and their code
     goes on in the dead caller's: the serial keeps a new caller from
     matching them (KNOWN_BUGS C30). */
  x = ni_code_extra(po->pr.co);
  psyco_assert(x != NULL); /* made with the merge points */
  v->array->items[LOC_INLINE_SERIAL] =
      vinfo_new(CompileTime_NewSk(sk_new(x->serial, SkFlagFixed)));
  while (i--) {
    v->array->items[TOTAL_LOC_INLINE + i] = pp[i];
    pp[i] = NULL;
  }
  return v;
}

DEFINEFN
PsycoObject *psyco_restore_inline_po(PsycoObject *po, vinfo_array_t **a) {
  int i, oldcount = po->vlocals.count;
  bool was_inlining = po->pr.is_inlining;
  vinfo_t *v;
  vinfo_t **old;
  vinfo_array_t *array = *a;
  *a = NullArray;

  /* the current locals are dropped only once the restored frame is in
     place: a decref may call a dealloc, and C calls are aligned from the
     current frame's return address (STACK_DEPTH_SINCE_CALL), which the
     inlined frame being left doesn't have (KNOWN_BUGS B6) */
  old = PyMem_Malloc((oldcount + 1) * sizeof(vinfo_t *));
  if (old == NULL)
    OUT_OF_MEMORY();
  memcpy(old, po->vlocals.items, oldcount * sizeof(vinfo_t *));

  i = array->count - TOTAL_LOC_INLINE;
  po = PsycoObject_Resize(po, i);
  po->vlocals.count = i; /* attention, vlocals.count cannot be larger than i
                            even if there is enough memory already allocated */
  while (i--)
    po->vlocals.items[i] = array->items[TOTAL_LOC_INLINE + i];

  v = array->items[LOC_INLINE_CO];
  po->pr.co = (PyCodeObject *)CompileTime_Get(v->source)->value;
  vinfo_decref(v, NULL);
  v = array->items[LOC_INLINE_NEXT_INSTR];
  po->pr.next_instr = v ? CompileTime_Get(v->source)->value : 0;
  vinfo_xdecref(v, NULL);
  vinfo_xdecref(array->items[LOC_INLINE_SERIAL], NULL);
  array_release(array);

  pyc_data_build(po, psyco_get_merge_points(po->pr.co, -1));

  /* leaving an inlined frame restores its (never inlining) parent; on
     entry the old locals were already moved out (psyco_save_inline_po) */
  po->pr.is_inlining = false;
  for (i = oldcount; i--;)
    vinfo_xdecref(old[i], po);
  po->pr.is_inlining = was_inlining;
  PyMem_Free(old);
  return po;
}

/***************************************************************/
/***   Recursion depth of compiled calls                       ***/
/***************************************************************/

/* Py_EnterRecursiveCall and Py_LeaveRecursiveCall (Include/ceval.h) as
   emitted code (KNOWN_BUGS C12). The current thread state's recursion_depth
   is counted in place and compared with _Py_CheckRecursionLimit, read at
   run time (sys.setrecursionlimit changes it). Only an overflow calls
   _Py_CheckRecursiveCall, which knows recursion_critical and overflowed and
   raises RecursionError. A leave calls C only while overflowed is set, to
   clear it below the low-water mark. Both rare paths are runtime_condition_f
   branches, compiled when first taken. */
typedef struct {
  PyThreadState *ts;
} tstate_slot_t;
typedef struct {
  int value;
} int_slot_t;
#define TSTATE_current                                                         \
  FMUT(DEF_FIELD(tstate_slot_t, PyThreadState *, ts, NO_PREV_FIELD))
#define TSTATE_depth                                                           \
  FMUT(DEF_FIELD(PyThreadState, int, recursion_depth, NO_PREV_FIELD))
#define TSTATE_overflowed                                                      \
  FMUT(DEF_FIELD(PyThreadState, char, overflowed, NO_PREV_FIELD))
#define INT_value FMUT(DEF_FIELD(int_slot_t, int, value, NO_PREV_FIELD))

EXTERNFN void *ni_tstate_current_address(void); /* Python/pyruntime.c */
static tstate_slot_t *tstate_current;

/* Py_LeaveRecursiveCall after the decrement */
static void cimpl_leave_overflowed(void) {
  PyThreadState *ts = PyThreadState_GET();
  if (ts->recursion_depth <
      _Py_RecursionLimitLowerWaterMark(_Py_CheckRecursionLimit))
    ts->overflowed = 0;
}

/* the running thread's state, read at run time */
static vinfo_t *load_tstate(PsycoObject *po) {
  vinfo_t *vslot = vinfo_new(CompileTime_New((long)tstate_current));
  vinfo_t *vts = psyco_get_field(po, vslot, TSTATE_current);
  vinfo_decref(vslot, po);
  return vts;
}

/* adds 'delta' to the depth of thread state 'vts': the new depth */
static vinfo_t *add_to_depth(PsycoObject *po, long delta, vinfo_t *vts) {
  vinfo_t *vdepth, *vnew;
  vdepth = psyco_get_field(po, vts, TSTATE_depth);
  if (vdepth == NULL)
    return NULL;
  vnew = integer_add_i(po, vdepth, delta, true);
  vinfo_decref(vdepth, po);
  if (vnew == NULL)
    return NULL;
  if (psyco_put_field(po, vts, TSTATE_depth, vnew))
    return vnew;
  vinfo_decref(vnew, po);
  return NULL;
}

/* false with an exception set in 'po' */
static bool enter_recursive_call(PsycoObject *po, vinfo_t *vts) {
  vinfo_t *vlimit, *vslot;
  condition_code_t cc = CC_ERROR;
  vinfo_t *vdepth = add_to_depth(po, 1, vts);
  if (vdepth == NULL)
    return false;
  vslot = vinfo_new(CompileTime_New((long)&_Py_CheckRecursionLimit));
  vlimit = psyco_get_field(po, vslot, INT_value);
  vinfo_decref(vslot, po);
  if (vlimit != NULL) {
    cc = integer_cmp(po, vdepth, vlimit, Py_GT);
    vinfo_decref(vlimit, po);
  }
  vinfo_decref(vdepth, po);
  if (cc == CC_ERROR)
    return false;
  if (runtime_condition_f(po, cc))
    return psyco_generic_call(po, _Py_CheckRecursiveCall, CfCommonIntZeroOk,
                              "l", (long)"") != NULL;
  return true;
}

DEFINEFN
bool psyco_leave_recursive_call(PsycoObject *po) {
  vinfo_t *vover, *vdepth;
  condition_code_t cc;
  vinfo_t *vts = load_tstate(po);
  if (vts == NULL)
    return false;
  vdepth = add_to_depth(po, -1, vts);
  if (vdepth == NULL) {
    vinfo_decref(vts, po);
    return false;
  }
  vinfo_decref(vdepth, po);
  vover = psyco_get_field(po, vts, TSTATE_overflowed);
  vinfo_decref(vts, po);
  cc = integer_NON_NULL(po, vover); /* consumes vover */
  if (cc == CC_ERROR)
    return false;
  if (runtime_condition_f(po, cc))
    return psyco_generic_call(po, cimpl_leave_overflowed, CfNoReturnValue,
                              "") != NULL;
  return true;
}

/* ceval's test of eval_breaker before an instruction (KNOWN_BUGS C24, D45):
   an inline compare of the word with 0, and only when it is set a call to
   the C helper that does what ceval does then (pending calls and signal
   handlers, a GIL drop for other threads, an async exception). The helper
   can run any Python code, as any residual call can. Its branch is a
   runtime_condition_f respawn point, compiled when first taken. */
EXTERNFN void *ni_eval_breaker_address(void); /* Python/pyruntime.c */
EXTERNFN int ni_eval_breaker_handler(void);
static void *eval_breaker;

DEFINEFN
bool psyco_check_eval_breaker(PsycoObject *po) {
  condition_code_t cc = psyco_int32_nonzero_at(po, eval_breaker);
  if (runtime_condition_f(po, cc))
    return psyco_generic_call(po, ni_eval_breaker_handler, CfCommonIntZeroOk,
                              "") != NULL;
  return true;
}

/* The C functions after which a CALL_* needs no eval_breaker test (D53,
   pycompiler.c:breaker_after_call). Each runs briefly, can't block or
   release the GIL, and runs only CPython's own code for builtin types, so
   what arrives during it waits only until the next test, as it would
   during any other instruction. Allocation can collect garbage and so run
   finalisers, as at any allocation in compiled code; the str methods can
   call an argument's __index__. A Python function's tp_call runs Python
   code, which tests eval_breaker itself (ceval, or compiled code entered
   through NiCode_Run), as an inlined or directly compiled callee does.
   Left out, though common at call sites: whatever can call back into
   arbitrary objects (dict lookups, PyObject_IsTrue, getattr, isinstance,
   a class's tp_call, list.extend, str.join), since that may be C code
   that blocks. Filled at start-up. */
#define CANNOT_BLOCK_MAX 40
static void *cannot_block[CANNOT_BLOCK_MAX];
static int cannot_block_count = 0;

DEFINEFN
bool psyco_cannot_block(void *c_function) {
  int i;
  for (i = 0; i < cannot_block_count; i++)
    if (cannot_block[i] == c_function)
      return true;
  return false;
}

static void add_cannot_block(void *c_function) {
  psyco_assert(c_function != NULL && cannot_block_count < CANNOT_BLOCK_MAX);
  cannot_block[cannot_block_count++] = c_function;
}

/* by ml_meth: a builtin type's method, or a builtin function (below) */
static void add_method(PyTypeObject *tp, const char *name) {
  PyObject *d = PyDict_GetItemString(tp->tp_dict, name);
  psyco_assert(d != NULL && Py_TYPE(d) == &PyMethodDescr_Type);
  add_cannot_block(((PyMethodDescrObject *)d)->d_method->ml_meth);
}

static void add_builtin(PyObject *builtins, const char *name) {
  PyObject *f = PyObject_GetAttrString(builtins, name);
  psyco_assert(f != NULL && PyCFunction_Check(f));
  add_cannot_block(((PyCFunctionObject *)f)->m_ml->ml_meth);
  Py_DECREF(f);
}

static void cannot_block_init(void) {
  static const char *const str_methods[] = {
      "find", "rfind", "index", "rindex", "startswith", "endswith",
      "partition", "rpartition", "lower", "upper", "strip", "lstrip",
      "rstrip", "replace", "split", "isdigit", "isalpha", "isspace", NULL};
  const char *const *m;
  PyObject *builtins = PyImport_ImportModule("builtins");
  psyco_assert(builtins != NULL);
  add_cannot_block(PyTuple_New);
  add_cannot_block(PyList_New);
  add_cannot_block(PyDict_New);
  add_cannot_block(_PyDict_NewPresized);
  add_cannot_block(PyLong_FromLong);
  add_cannot_block(PyFloat_FromDouble);
  add_cannot_block(PyFunction_NewWithQualName);
  add_cannot_block(PyMethod_New);
  add_cannot_block(PyThreadState_Get);
  add_cannot_block(PyLong_Type.tp_richcompare);
  add_cannot_block(PyFloat_Type.tp_richcompare);
  add_cannot_block(PyFunction_Type.tp_call);
  add_method(&PyList_Type, "append");
  add_method(&PyList_Type, "reverse");
  add_method(&PyDict_Type, "keys");
  add_method(&PyDict_Type, "items");
  add_method(&PyDict_Type, "values");
  for (m = str_methods; *m != NULL; m++)
    add_method(&PyUnicode_Type, *m);
  add_builtin(builtins, "ord");
  add_builtin(builtins, "chr");
  add_builtin(builtins, "callable");
  Py_DECREF(builtins);
}

/* Whether a trace or profile function is set in this thread (KNOWN_BUGS
   C23, D46): a compiled call or inlined frame skips the eval hook, which
   runs frames interpreted while tstate->use_tracing is set, so each one
   tests that int first. When set, the caller makes the real call instead
   (rare path: a runtime_condition_f respawn point, compiled when first
   taken). Only the callee's entry is tested: a frame already running
   compiled when tracing starts has f_trace None, so CPython gives it no
   trace events either; its profile events are missed (KNOWN_BUGS C29).
   use_tracing is per thread and set by every way in, C ones too
   (PyEval_SetTrace and PyEval_SetProfile, as cProfile uses). */
EXTERNVAR int ni_trace_fallback; /* eval_hook.c */
static bool tracing_on(PsycoObject *po, vinfo_t *vts) {
  /* off only for tools/interpreted.py, before anything is compiled */
  if (!ni_trace_fallback)
    return false;
  return runtime_condition_f(
      po, psyco_int32_nonzero_in(po, vts, offsetof(PyThreadState, use_tracing)));
}

/* the same test on its own; -1 with an exception set in 'po' */
static int psyco_tracing_on(PsycoObject *po) {
  bool on;
  vinfo_t *vts;
  if (!ni_trace_fallback)
    return 0;
  vts = load_tstate(po);
  if (vts == NULL)
    return -1;
  on = tracing_on(po, vts);
  vinfo_decref(vts, po);
  return on;
}

#define COMPUTE_DEFCOUNT()                                                     \
  do {                                                                         \
    /* is vdefaults!=NULL at run-time ? */                                     \
    condition_code_t cc = object_non_null(po, vdefaults);                      \
    if (cc == CC_ERROR) /* error or more likely promotion */                   \
      return NULL;                                                             \
    if (runtime_condition_t(po, cc))                                           \
      defcount = PsycoTuple_Load(vdefaults);                                   \
    else                                                                       \
      defcount = 0; /* vdefaults==NULL at run-time */                          \
  } while (0)

/* The callee frame's builtins, as frameobject.c:_PyFrame_New_NoTrack picks
   them: the caller's when the globals are the same, else the globals'
   __builtins__ (KNOWN_BUGS F49). A new reference, or NULL if the globals
   aren't known now. The second case is read at compile time, so a module's
   __builtins__ replaced after a call site compiled is not seen there. */
static vinfo_t *callee_builtins(PsycoObject *po, vinfo_t *vglobals) {
  PyObject *b;
  if (vglobals == LOC_GLOBALS ||
      (is_compiletime(vglobals->source) &&
       is_compiletime(LOC_GLOBALS->source) &&
       CompileTime_Get(vglobals->source)->value ==
           CompileTime_Get(LOC_GLOBALS->source)->value)) {
    vinfo_incref(LOC_BUILTINS);
    return LOC_BUILTINS;
  }
  if (!is_compiletime(vglobals->source))
    return NULL;
  b = psy_get_builtins((PyObject *)CompileTime_Get(vglobals->source)->value);
  Py_INCREF(b); /* owned by the constant, as in PsycoObject_FromCode */
  return vinfo_new(CompileTime_NewSk(sk_new((long)b, SkFlagPyObj)));
}

#define MAX_CALL_FREES 64

DEFINEFN
vinfo_t *psyco_call_pyfunc(PsycoObject *po, vinfo_t *vfunc, PyCodeObject *co,
                           vinfo_t *vglobals, vinfo_t *vdefaults,
                           vinfo_t *arg_tuple, int recursion) {
  CodeBufferObject *codebuf;
  PsycoObject *mypo;
  Source *sources;
  vinfo_t *result;
  stack_frame_info_t *finfo;
  int tuple_size, argcount, defcount = -2;
  int nfrees;
  bool inlining;
  vinfo_t *vbuiltins;
  vinfo_t **frees = NULL;
  vinfo_t *freebuf[MAX_CALL_FREES];
  struct fncall_arg_s fncall;

  if (is_proxycode(co)) {
    PsycoFunctionObject *pf;
    pf = (PsycoFunctionObject *)PyTuple_GET_ITEM(co->co_consts, 1);
    co = pf->psy_code;
    if (pf->psy_defaults == NULL)
      vdefaults = PsycoTuple_New(0, NULL);
    else {
      Py_INCREF(pf->psy_defaults); /* XXX looses a ref */
      vdefaults = vinfo_new(
          CompileTime_NewSk(sk_new((long)pf->psy_defaults, SkFlagPyObj)));
    }
    /* the recursion limit only applies to non-proxified functions */
    recursion++;
    result =
        psyco_call_pyfunc(po, vfunc, co, vglobals, vdefaults, arg_tuple,
                          recursion);
    vinfo_decref(vdefaults, po);
    return result;
  }
  if (--recursion < 0)
    goto fail_to_default;
  /* a generator expression is run only by a consumer that knows it
     (psyco_call_genexpr); until then it is a pending generator */
  if (co->co_flags & CO_GENERATOR) {
    if (vfunc != NULL && is_virtualtime(vfunc->source) &&
        psyco_is_genexpr(co) && PsycoTuple_Load(arg_tuple) == 1)
      return PsycoGenexpr_New(vfunc, arg_tuple);
    goto fail_to_default;
  }
  /* closure code: the cells of a function still virtual (MAKE_FUNCTION
     here: a comprehension, say) are its closure tuple's items; those of
     any other are read from func_closure below */
  nfrees = PyCode_GetNumFree(co);
  if (nfrees != 0) {
    vinfo_t *closure;
    if (vfunc == NULL || nfrees > MAX_CALL_FREES)
      goto fail_to_default;
    if (is_virtualtime(vfunc->source)) {
      closure = vinfo_getitem(vfunc, iFUNC_CLOSURE);
      if (closure == NULL || PsycoTuple_Load(closure) != nfrees)
        goto fail_to_default;
      frees = &PsycoTuple_GET_ITEM(closure, 0);
    }
  }

  /* XXX tony: kw only args are a Py3 feature that we need to add support for */
  if (co->co_kwonlyargcount != 0) {
    goto fail_to_default;
  }

  tuple_size = PsycoTuple_Load(arg_tuple);
  if (tuple_size == -1)
    goto fail_to_default;
  /* XXX calling with an unknown-at-compile-time number of arguments
     is not implemented, revert to the default behaviour */

  COMPUTE_DEFCOUNT();
  if (defcount == -1)
    goto fail_to_default;
  /* calling with an unknown-at-compile-time number of default arguments
     is not implemented (but this is probably not useful to implement);
     revert to the default behaviour */

  /* Force mutable arguments out of virtual-time,
     including the processor condition codes */
  /* This is expected to have already been done on default arguments;
     see PsycoFunction_New() */
  if (!psyco_forking(po, arg_tuple->array))
    return NULL;

  /* prepare a frame */
  if (!fncall_init(&fncall, co, nfrees))
    goto fail_to_default; /* unsupported bytecode features */
  vbuiltins = callee_builtins(po, vglobals);
  if (vbuiltins == NULL)
    goto fail_to_default;
  if (nfrees != 0 && frees == NULL) {
    /* func_closure can't be set from Python, and its length is the code's
       number of free variables (funcobject.c:func_set_code checks it), as
       ceval assumes. A constant function's cells are constants read now:
       borrowed, since code compiled for a cell only ever uses its address,
       which a new cell at the same address may as well share. */
    int i;
    vinfo_t *closure = psyco_get_const(po, vfunc, FUNC_closure);
    for (i = 0; closure != NULL && i < nfrees; i++)
      if ((freebuf[i] = psyco_get_nth_const(po, closure, TUPLE_ob_item, i)) ==
          NULL)
        closure = NULL;
    if (closure == NULL) {
      vinfo_decref(vbuiltins, po);
      return NULL;
    }
    frees = freebuf;
  }
  if (!fncall_collect_arguments(&fncall, vglobals, vbuiltins,
                                &PsycoTuple_GET_ITEM(arg_tuple, 0), tuple_size,
                                &PsycoTuple_GET_ITEM(vdefaults, 0), defcount,
                                frees)) {
    /* the wrong number of arguments: the real call raises CPython's
       TypeError, whose wording depends on the defaults and keyword-only
       arguments (ceval.c:too_many_positional, missing_arguments; F92) */
    vinfo_decref(vbuiltins, po);
    goto fail_to_default;
  }

  /* not with cells loaded at run time: inlining such a closure crashed
     (KNOWN_BUGS C49) */
  inlining = !po->pr.is_inlining &&
             (psyco_mp_flags(fncall.merge_points) & MP_FLAGS_INLINABLE) &&
             !(frees == freebuf && is_runtime(vfunc->source));
  if (inlining && frees == freebuf && is_runtime(vfunc->source)) {
    /* Cells loaded from a run-time function are borrowed from it, which is
       enough for a call: the caller holds the function until it returns a
       real object. An inlined body's result can still be virtual and hold
       a cell (a closure it made), and the caller drops the function before
       computing it: the cell could be freed first (KNOWN_BUGS C49). So each
       cell gets a reference of its own, dropped with its last vinfo_t. */
    int i;
    for (i = 0; i < nfrees; i++)
      need_reference(po, freebuf[i]);
  }
  /* a traced call goes through the eval hook (tracing_on). Not again when
     the instruction restarts after the inlined body with its result. */
  if (!inlining || LOC_INLINING == NULL) {
    bool traced, entered = true;
    vinfo_t *vts = load_tstate(po);
    if (vts == NULL) {
      array_release(fncall.inputvinfos);
      vinfo_decref(vbuiltins, po);
      return NULL;
    }
    traced = tracing_on(po, vts);
    /* count the call's depth, as ceval does on entering a frame, so that
       deep recursion raises RecursionError instead of overflowing the C
       stack (KNOWN_BUGS C12); psyco_call_psyco leaves it after the call.
       Emitted before the frame is built, so no argument source moves. */
    if (!traced && !inlining)
      entered = enter_recursive_call(po, vts);
    vinfo_decref(vts, po);
    if (traced || !entered) {
      array_release(fncall.inputvinfos);
      vinfo_decref(vbuiltins, po);
      if (!entered)
        return NULL;
      goto fail_to_default;
    }
  }

  /* try to inline the call */
  if (inlining) {
    /* inlining call */
    result = call_with_inline_frame(po, &fncall, recursion);
  } else {
    /* non-inlining call */
    mypo = psyco_build_frame(&fncall, recursion, &sources);
    if (mypo == NULL) {
      psyco_leave_recursive_call(po);
      goto pyerr;
    }
    argcount = get_arguments_count(&mypo->vlocals);
    finfo = psyco_finfo(po, mypo);

    /* compile the function (this frees mypo) */
    codebuf = psyco_compile_code(mypo, PsycoObject_Ready(mypo));

    /* get the run-time argument sources and push them on the stack
       and write the actual CALL */
    result = psyco_call_psyco(po, codebuf, sources, argcount, finfo);
    PyMem_FREE(sources);
  }
  vinfo_decref(vbuiltins, po);
  return result;

fail_to_default:
  /* call the function object itself: rebuilding the call from its code,
     globals and defaults (upstream's cimpl_call_pyfunc) lost its name and
     qualname (generators took co_name), closure and keyword-only defaults */
  return psyco_generic_call(po, PyFunction_Type.tp_call,
                            CfCommonNewRefPyObject, "vvl", vfunc, arg_tuple,
                            (long)0);

pyerr:
  psyco_virtualize_exception(po);
  return NULL;
}

/* A closure known only at run time, with its code 'co' promoted (closures
   are new function objects on every run of the code that makes them, but
   their code is not). Neither the function nor anything it holds is made a
   constant, which would keep each one alive and specialise per object:
   its cells are loaded from func_closure at each call. The globals must
   be this frame's (a run-time test), as for a closure made here, so that
   they and the builtins stay constants that no promotion holds. Defaults
   are per function object, so a call that would use them is the real one;
   keyword-only arguments never come here (pfunction_call). */
DEFINEFN
vinfo_t *psyco_call_closure(PsycoObject *po, vinfo_t *vfunc, PyCodeObject *co,
                            vinfo_t *arg_tuple) {
  vinfo_t *vglobals, *vnodefaults, *result;
  condition_code_t cc;
  int n = PsycoTuple_Load(arg_tuple);
  if (n < co->co_argcount)
    goto fallback; /* unknown count, or defaults wanted */
  vglobals = psyco_get_const(po, vfunc, FUNC_globals);
  if (vglobals == NULL)
    return NULL;
  cc = integer_cmp(po, vglobals, LOC_GLOBALS, Py_NE);
  if (cc == CC_ERROR)
    return NULL;
  if (runtime_condition_f(po, cc))
    goto fallback;
  vnodefaults = psyco_vi_Zero();
  result = psyco_call_pyfunc(po, vfunc, co, LOC_GLOBALS, vnodefaults,
                             arg_tuple, po->pr.auto_recursion);
  vinfo_decref(vnodefaults, po);
  return result;

fallback:
  return psyco_generic_call(po, PyFunction_Type.tp_call,
                            CfCommonNewRefPyObject, "vvl", vfunc, arg_tuple,
                            (long)0);
}

/* Run a pending generator expression (PsycoGenexpr_New) for its consumer:
   its code compiled with 'mode' and 'acc' in the two GENEXPR slots
   (pycheader.h), called like a function of its iterator, returning the
   consumer's result as a new reference. */
DEFINEFN
vinfo_t *psyco_call_genexpr(PsycoObject *po, vinfo_t *vgen, int mode,
                            vinfo_t *acc) {
  vinfo_t *vfunc = vgen->array->items[iGENEXPR_FUNC];
  vinfo_t *args = vgen->array->items[iGENEXPR_ARGS];
  vinfo_t *vcode, *vglobals, *vbuiltins, *closure, *vmode, *result, *vts;
  vinfo_t *extra[64];
  PyCodeObject *co;
  CodeBufferObject *codebuf;
  PsycoObject *mypo;
  Source *sources;
  stack_frame_info_t *finfo;
  int i, nfrees, argcount;
  struct fncall_arg_s fncall;

  vcode = vinfo_getitem(vfunc, iFUNC_CODE);
  vglobals = vinfo_getitem(vfunc, iFUNC_GLOBALS);
  extra_assert(vcode != NULL && is_compiletime(vcode->source));
  co = (PyCodeObject *)CompileTime_Get(vcode->source)->value;
  nfrees = PyCode_GetNumFree(co);
  if (vglobals == NULL || nfrees + GENEXPR_SLOTS > 64)
    return NULL; /* XXX */
  for (i = 0; i < nfrees; i++) {
    closure = vinfo_getitem(vfunc, iFUNC_CLOSURE);
    if (closure == NULL || PsycoTuple_Load(closure) != nfrees)
      psyco_fatal_msg("genexpr closure"); /* checked by the consumer */
    extra[i] = PsycoTuple_GET_ITEM(closure, i);
  }
  vmode = vinfo_new(CompileTime_New(mode));
  extra[nfrees] = vmode;
  extra[nfrees + 1] = acc;

  if (!psyco_forking(po, args->array) || !fncall_init(&fncall, co, nfrees))
    psyco_fatal_msg("genexpr call"); /* checked by the consumer */
  fncall.nfrees = nfrees + GENEXPR_SLOTS;
  /* made by MAKE_FUNCTION here, so with this frame's globals and builtins */
  vbuiltins = callee_builtins(po, vglobals);
  if (vbuiltins == NULL ||
      !fncall_collect_arguments(&fncall, vglobals, vbuiltins,
                                &PsycoTuple_GET_ITEM(args, 0), 1, NULL, 0,
                                extra))
    psyco_fatal_msg("genexpr arguments");

  /* as psyco_call_pyfunc's non-inlining call */
  result = NULL;
  vts = load_tstate(po);
  if (vts != NULL && enter_recursive_call(po, vts)) {
    vinfo_decref(vts, po);
    vts = NULL;
    mypo = psyco_build_frame(&fncall, po->pr.auto_recursion, &sources);
    if (mypo == NULL) {
      psyco_leave_recursive_call(po);
      psyco_virtualize_exception(po);
    } else {
      argcount = get_arguments_count(&mypo->vlocals);
      finfo = psyco_finfo(po, mypo);
      codebuf = psyco_compile_code(mypo, PsycoObject_Ready(mypo));
      result = psyco_call_psyco(po, codebuf, sources, argcount, finfo);
      PyMem_FREE(sources);
    }
  }
  vinfo_xdecref(vts, po);
  vinfo_decref(vmode, po);
  vinfo_decref(vbuiltins, po);
  return result;
}

/* whether psyco_call_genexpr can run 'v' */
DEFINEFN
bool psyco_genexpr_fusable(vinfo_t *v) {
  vinfo_t *vfunc, *vcode, *closure, *args;
  PyCodeObject *co;
  if (!PsycoGenexpr_Check(v))
    return false;
  /* only a temporary: the stack and the argument tuple hold it. A
     generator something else can still use must stay one object, which
     after a partial run (any, all, an exception) keeps its position, inner
     loops included */
  if (v->refcount > 2)
    return false;
  vfunc = v->array->items[iGENEXPR_FUNC];
  args = v->array->items[iGENEXPR_ARGS];
  if (args == NULL || PsycoTuple_Load(args) != 1)
    return false;
  vcode = vinfo_getitem(vfunc, iFUNC_CODE);
  if (vcode == NULL || !is_compiletime(vcode->source))
    return false;
  co = (PyCodeObject *)CompileTime_Get(vcode->source)->value;
  if (!psyco_is_genexpr(co) || psyco_get_merge_points(co, 0) == Py_None ||
      PyCode_GetNumFree(co) + GENEXPR_SLOTS > 64)
    return false;
  if (PyCode_GetNumFree(co) > 0) {
    closure = vinfo_getitem(vfunc, iFUNC_CLOSURE);
    if (closure == NULL || PsycoTuple_Load(closure) != PyCode_GetNumFree(co))
      return false;
  }
  return true;
}

/* the fused body is the generator's frame: a tracer must see it */
DEFINEFN
int psyco_genexpr_fuse(PsycoObject *po, vinfo_t *v) {
  int on;
  if (!psyco_genexpr_fusable(v))
    return 0;
  on = psyco_tracing_on(po);
  return on < 0 ? -1 : !on;
}

/***************************************************************/
/***   PsycoFunctionObjects                                    ***/
/***************************************************************/

DEFINEFN
PsycoFunctionObject *psyco_PsycoFunction_NewEx(PyCodeObject *code,
                                               PyObject *globals,
                                               PyObject *defaults, int rec) {
  PsycoFunctionObject *result =
      PyObject_GC_New(PsycoFunctionObject, &PsycoFunction_Type);
  if (result != NULL) {
    result->psy_code = code;
    Py_INCREF(code);
    result->psy_globals = globals;
    Py_INCREF(globals);
    result->psy_defaults = NULL;
    result->psy_recursion = rec;
    result->psy_fastcall = PyList_New(0);
    PyObject_GC_Track(result);

    if (result->psy_fastcall == NULL) {
      Py_DECREF(result);
      return NULL;
    }

    if (defaults != NULL) {
      if (!PyTuple_Check(defaults)) {
        Py_DECREF(result);
        PyErr_SetString(PyExc_PsycoError, "Psyco proxies need a tuple "
                                          "for default arguments");
        return NULL;
      }
      if (PyTuple_GET_SIZE(defaults) > 0) {
        result->psy_defaults = defaults;
        Py_INCREF(defaults);
      }
    }
  }
  return result;
}

static void psycofunction_dealloc(PsycoFunctionObject *self) {
  PyObject_GC_UnTrack(self);
  Py_XDECREF(self->psy_fastcall);
  Py_XDECREF(self->psy_defaults);
  Py_DECREF(self->psy_globals);
  Py_DECREF(self->psy_code);
  PyObject_GC_Del(self);
}

static PyObject *psycofunction_call(PsycoFunctionObject *self, PyObject *arg,
                                    PyObject *kw) {
  PyObject *codebuf;
  PyObject *result;
  PyFrameObject *f;
  stack_frame_info_t **finfo;
  long *initial_stack;
  int key;

  if (kw != NULL && PyDict_Check(kw) && PyDict_Size(kw) > 0) {
    /* keyword arguments not supported yet */
    goto unsupported;
  }

  key = PyTuple_GET_SIZE(arg);
  if (key < PyList_GET_SIZE(self->psy_fastcall))
    codebuf = PyList_GET_ITEM(self->psy_fastcall, key);
  else
    codebuf = NULL;

  if (codebuf == NULL) {
    /* not already seen with this number of arguments */
    vinfo_t *vglobals, *vbuiltins;
    vinfo_array_t *vdefaults;
    vinfo_array_t *arginfo;
    PsycoObject *po = NULL;
    source_known_t *sk;
    struct fncall_arg_s fncall;
    bool support;
    int i = key;

    /* make an array of run-time values */
    arginfo = array_new(i);
    while (i--) {
      /* arbitrary values for the source */
      arginfo->items[i] = vinfo_new(SOURCE_DUMMY);
    }

    /* build the globals and defaults as compile-time values */
    Py_INCREF(self->psy_globals);
    sk = sk_new((long)self->psy_globals, SkFlagPyObj);
    vglobals = vinfo_new(CompileTime_NewSk(sk));

    if (self->psy_defaults == NULL)
      vdefaults = NullArray;
    else {
      i = PyTuple_GET_SIZE(self->psy_defaults);
      vdefaults = array_new(i);
      while (i--) {
        PyObject *v = PyTuple_GET_ITEM(self->psy_defaults, i);
        Py_INCREF(v);
        sk = sk_new((long)v, SkFlagPyObj);
        vdefaults->items[i] = vinfo_new(CompileTime_NewSk(sk));
      }
    }

    /* make a "frame" */
    support = fncall_init(&fncall, self->psy_code, 0);
    vbuiltins = vinfo_new(CompileTime_NewSk(
        sk_new((long)psy_get_builtins(self->psy_globals), SkFlagPyObj)));
    Py_INCREF((PyObject *)CompileTime_Get(vbuiltins->source)->value);
    if (support && fncall_collect_arguments(&fncall, vglobals, vbuiltins,
                                            arginfo->items, arginfo->count,
                                            vdefaults->items, vdefaults->count,
                                            NULL)) {
      po = psyco_build_frame(&fncall, self->psy_recursion, NULL);
    }
    array_delete(vdefaults, NULL);
    vinfo_decref(vbuiltins, NULL);
    vinfo_decref(vglobals, NULL);
    array_delete(arginfo, NULL);

    if (po == NULL) {
      if (!support) {
        /* unsupported bytecode features */
        codebuf = Py_None;
        Py_INCREF(codebuf);
      } else /* Python exception (wrong # of arguments) */
        return NULL;
    } else {
      /* compile the function */
      codebuf = (PyObject *)psyco_compile_code(po, PsycoObject_Ready(po));
    }
    /* cache 'codebuf' (note that this is not necessary, as
       multiple calls to psyco_compile_code() will just return
       the same codebuf, but it makes things faster because we
       don't have to build a whole PsycoObject the next time. */
    i = key + 1 - PyList_GET_SIZE(self->psy_fastcall);
    if (i > 0) {
      /* list too short, first enlarge it with NULLs */
      PyObject *tmp = PyList_New(i);
      if (tmp != NULL) {
        PyList_SetSlice(self->psy_fastcall, PyList_GET_SIZE(self->psy_fastcall),
                        PyList_GET_SIZE(self->psy_fastcall), tmp);
        Py_DECREF(tmp);
      }
      /* errors are detected by the failing PyList_SetItem()
         call below */
    }
    /* Eats a reference to codebuf */
    if (PyList_SetItem(self->psy_fastcall, key, codebuf))
      PyErr_Clear(); /* not fatal, ignore error */
  }

  if (codebuf == Py_None)
    goto unsupported;

  /* over the current Python frame, a lightweight chained list of
     Psyco frames will be built. Mark the current Python frame as
     the starting point of this chained list. */
  f = PyEval_GetFrame();
  if (f == NULL) {
    goto unsupported;
  }
  if (!psyco_frame_start_register(f, &finfo)) {
    return NULL;
  }
  /* Warning, no 'return' between this point and the PyDict_DelItem()
     below */

  /* get the actual arguments */
  extra_assert(RUN_ARGC(codebuf) == PyTuple_GET_SIZE(arg));
  initial_stack = (long *)(&PyTuple_GET_ITEM(arg, 0));

  /* run! */
  Py_INCREF(codebuf);
  result =
      psyco_processor_run((CodeBufferObject *)codebuf, initial_stack, &finfo);
  Py_DECREF(codebuf);
  psyco_trash_object(NULL); /* free any trashed object now */

  if (!psyco_frame_start_unregister(f)) {
    Py_XDECREF(result);
    result = NULL;
  }

  if (result == NULL)
    extra_assert(PyErr_Occurred());
  else
    extra_assert(!PyErr_Occurred());
  return result;

unsupported : { /* Code copied from function_call() in funcobject.c */
  PyObject **d, **k;
  Py_ssize_t nk, nd;

  PyObject *argdefs = self->psy_defaults;
  if (argdefs != NULL) {
    d = &PyTuple_GET_ITEM((PyTupleObject *)argdefs, 0);
    nd = PyTuple_Size(argdefs);
  } else {
    d = NULL;
    nd = 0;
  }

  if (kw != NULL && PyDict_Check(kw)) {
    Py_ssize_t pos, i;
    nk = PyDict_Size(kw);
    k = PyMem_NEW(PyObject *, 2 * nk);
    if (k == NULL) {
      PyErr_NoMemory();
      return NULL;
    }
    pos = i = 0;
    while (PyDict_Next(kw, &pos, &k[i], &k[i + 1]))
      i += 2;
    nk = i / 2;
  } else {
    k = NULL;
    nk = 0;
  }

  result = PyEval_EvalCodeEx((PyObject *)self->psy_code, self->psy_globals,
                             (PyObject *)NULL, &PyTuple_GET_ITEM(arg, 0),
                             PyTuple_Size(arg), k, nk, d, nd, NULL, NULL);

  if (k != NULL)
    PyMem_DEL(k);

  return result;
}
}

static int psycofunction_traverse(PsycoFunctionObject *f, visitproc visit,
                                  void *arg) {
  int err;
  if (f->psy_fastcall) {
    err = visit(f->psy_fastcall, arg);
    if (err)
      return err;
  }
  if (f->psy_defaults) {
    err = visit(f->psy_defaults, arg);
    if (err)
      return err;
  }
  err = visit(f->psy_globals, arg);
  if (err)
    return err;
  return 0;
}

static int psycofunction_clear(PsycoFunctionObject *f) {
  PyObject *o;
  o = f->psy_fastcall;
  if (o) {
    f->psy_fastcall = NULL;
    Py_DECREF(o);
  }
  o = f->psy_defaults;
  if (o) {
    f->psy_defaults = NULL;
    Py_DECREF(o);
  }
  o = f->psy_globals;
  f->psy_globals = Py_None;
  Py_INCREF(Py_None);
  Py_DECREF(o);
  return 0;
}

DEFINEVAR
PyTypeObject PsycoFunction_Type = {
    PyVarObject_HEAD_INIT(NULL, 0) /*ob_size*/
    "Psyco_function",              /*tp_name*/
    sizeof(PsycoFunctionObject),   /*tp_basicsize*/
    0,                             /*tp_itemsize*/
    /* methods */
    (destructor)psycofunction_dealloc,       /*tp_dealloc*/
    0,                                       /*tp_print*/
    0,                                       /*tp_getattr*/
    0,                                       /*tp_setattr*/
    0,                                       /*tp_compare*/
    0,                                       /*tp_repr*/
    0,                                       /*tp_as_number*/
    0,                                       /*tp_as_sequence*/
    0,                                       /*tp_as_mapping*/
    0,                                       /*tp_hash*/
    (ternaryfunc)psycofunction_call,         /*tp_call*/
    0,                                       /* tp_str */
    0,                                       /* tp_getattro */
    0,                                       /* tp_setattro */
    0,                                       /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC, /* tp_flags */
    0,                                       /* tp_doc */
    (traverseproc)psycofunction_traverse,    /* tp_traverse */
    (inquiry)psycofunction_clear,            /* tp_clear */
};

DEFINEFN
void psycofunction_init() {
  PyType_Ready(&PsycoFunction_Type);
  tstate_current = ni_tstate_current_address();
  psyco_assert(tstate_current->ts == PyThreadState_GET());
  eval_breaker = ni_eval_breaker_address();
  psyco_assert(eval_breaker != NULL);
  cannot_block_init();
}
