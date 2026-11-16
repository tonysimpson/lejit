/*
 * The bytecode compiler: a compile-time copy of ceval.c's main loop. Each
 * opcode case works on a stack of vinfo_t and calls the compile-time object
 * protocol (Objects/p*.c). Two things differ from ceval:
 *  - Restart. If an opcode needs a run-time value as a constant it raises the
 *    promotion pseudo-exception: compilation stops, a run-time switch is
 *    emitted, and the *same opcode is re-run from the start* once the value is
 *    known. So an opcode must do nothing irreversible before the last point
 *    where it can fail or promote (use TOP(), and POP() only at the end).
 *  - After the switch come the merge-point checks, then the handling of
 *    pseudo-exceptions (promotion, EInline, EReturn/EBreak/EContinue, Python
 *    exceptions) with ceval-style unwinding of the block stack.
 * The block opcodes (SETUP_LOOP/EXCEPT/FINALLY) exist only up to 3.7. Some
 * cases keep Python 2 meanings (CALL_FUNCTION's oparg, read as na | nk << 8,
 * which is right only below 256 arguments;
 * doc/history/removed-mechanisms.md D2).
 * Exceptions: catching one follows 3.7 (an EXCEPT_HANDLER block over six
 * stack items, cimpl_push_exc_info). A finally block differs from ceval: it
 * gets a single 3-tuple holding any pseudo-exception (EReturn too), which
 * END_FINALLY tells apart from an except clause's exception type, and
 * exc_info isn't set inside it. The Py2 per-frame saved exception
 * (FINALIZE_FRAME_LOCALS, cimpl_finalize_frame_locals) is now never set.
 */
#include "pycompiler.h"
#include "../codemanager.h"
#include "../dispatcher.h"
#include "../mergepoints.h"
#include "../processor.h"
#include "../psyfunc.h"
#include "../pycodegen.h"

#include "../Objects/pabstract.h"
#include "../Objects/pdictobject.h"
#include "../Objects/pfuncobject.h"
#include "../Objects/plistobject.h"
#include "../Objects/plongobject.h"
#include "../Objects/pboolobject.h"
#include "../Objects/ptupleobject.h"

#include "../compat2to3.h"

#include "pycinternal.h"
#include <eval.h>
#include <ipyencoding.h>

#define KNOWN_VAR(type, varname, loc)                                          \
  type varname = (type)(CompileTime_Get(loc->source)->value)

/*****************************************************************/
/***   Meta functions                                          ***/

DEFINEVAR PyObject *Psyco_Meta_Dict = NULL;

DEFINEFN
void Psyco_DefineMeta(void *c_function, void *psyco_function) {
  PyObject *key;
  PyObject *value;
  if (Psyco_Meta_Dict == NULL) {
    Psyco_Meta_Dict = PyDict_New();
    if (Psyco_Meta_Dict == NULL)
      return;
  }
  if (c_function == NULL) {
    /* should not occur */
    return;
  }
  key = NiCompatInt_FromLong((long)c_function);
  if (key != NULL) {
    value = NiCompatInt_FromLong((long)psyco_function);
    if (value != NULL) {
      PyDict_SetItem(Psyco_Meta_Dict, key, value);
      Py_DECREF(value);
    }
    Py_DECREF(key);
  }
}

DEFINEFN
PyObject *Psyco_DefineMetaModule(char *modulename) {
  PyObject *module = PyImport_ImportModule(modulename);
  if (module == NULL) {
    PyErr_Clear();
  }
  return module;
}

DEFINEFN
PyObject *Psyco_GetModuleObject(PyObject *module, char *name,
                                PyTypeObject *expected_type) {
  PyObject *fobj;
  if (module == NULL)
    return NULL;

  fobj = PyObject_GetAttrString(module, name);
  if (fobj == NULL) {
    PyErr_Clear();
    return NULL;
  }
  if (expected_type != NULL && !PyObject_TypeCheck(fobj, expected_type)) {
    Py_DECREF(fobj);
    fobj = NULL;
  }
  return fobj;
}

DEFINEFN
PyCFunction Psyco_DefineModuleFn(PyObject *module, char *meth_name,
                                 int meth_flags, void *meta_fn) {
  PyCFunction f;
  PyObject *fobj = Psyco_GetModuleObject(module, meth_name, &PyCFunction_Type);
  if (fobj == NULL)
    return NULL;

  if (PyCFunction_GET_FLAGS(fobj) != meth_flags) {
    f = NULL;
  } else {
    f = PyCFunction_GET_FUNCTION(fobj);
    Psyco_DefineMeta(f, meta_fn);
  }
  Py_DECREF(fobj);
  return f;
}

DEFINEFN
PyCFunction Psyco_DefineModuleC(PyObject *module, char *meth_name,
                                int meth_flags, void *meta_fn,
                                void *meta_type_new) {
  PyObject *o = Psyco_GetModuleObject(module, meth_name, NULL);
  if (o == NULL)
    return NULL;
  if (PyType_Check(o) && ((PyTypeObject *)o)->tp_new != NULL) {
    /* maps a callable type */
    Psyco_DefineMeta(((PyTypeObject *)o)->tp_new, meta_type_new);
    return NULL;
  } else
    return Psyco_DefineModuleFn(module, meth_name, meth_flags, meta_fn);
}

DEFINEFN
vinfo_t *Psyco_Meta1x(PsycoObject *po, void *c_function, int flags,
                      const char *arguments, long a1) {
  void *psyco_fn = Psyco_Lookup(c_function);
  if (psyco_fn == NULL) {
    return psyco_generic_call(po, c_function, flags, arguments, a1);
  } else
    return ((vinfo_t * (*)(PsycoObject *, long))(psyco_fn))(po, a1);
}

DEFINEFN
vinfo_t *Psyco_Meta2x(PsycoObject *po, void *c_function, int flags,
                      const char *arguments, long a1, long a2) {
  void *psyco_fn = Psyco_Lookup(c_function);
  if (psyco_fn == NULL)
    return psyco_generic_call(po, c_function, flags, arguments, a1, a2);
  else
    return ((vinfo_t * (*)(PsycoObject *, long, long))(psyco_fn))(po, a1, a2);
}

DEFINEFN
vinfo_t *Psyco_Meta3x(PsycoObject *po, void *c_function, int flags,
                      const char *arguments, long a1, long a2, long a3) {
  void *psyco_fn = Psyco_Lookup(c_function);
  if (psyco_fn == NULL)
    return psyco_generic_call(po, c_function, flags, arguments, a1, a2, a3);
  else
    return ((vinfo_t * (*)(PsycoObject *, long, long, long))(psyco_fn))(po, a1,
                                                                        a2, a3);
}

DEFINEFN
int psyco_pyobj_atcompiletime_mega(PsycoObject *po, vinfo_t *vi,
                                   PyObject **out) {
  if (!compute_vinfo(vi, po))
    return -1;
  if (is_runtime(vi->source)) {
    if (vi->source & RunTime_Megamorphic)
      return 0;
    PycException_Promote(po, vi, &psyco_nonfixed_pyobj_promotion_mega);
    return -1;
  } else {
    source_known_t *sk = CompileTime_Get(vi->source);
    sk->refcount1_flags |= SkFlagFixed;
    *out = (PyObject *)sk->value;
    return 1;
  }
}

/***************************************************************/
/***   pyc_data_t                                            ***/

DEFINEFN
void pyc_data_build(PsycoObject *po, PyObject *merge_points) {
  /* rebuild the data in the pyc_data_t */
  int i;
  PyCodeObject *co = po->pr.co;
  int stack_base = po->vlocals.count - co->co_stacksize;
  for (i = stack_base; i < po->vlocals.count; i++)
    if (po->vlocals.items[i] == NULL)
      break;
  po->pr.stack_base = stack_base;
  po->pr.stack_level = i - stack_base;
  po->pr.merge_points = merge_points;
}

static void block_setup(PsycoObject *po, int type, int handler, int level) {
  PyTryBlock *b;
  extra_assert(!po->pr.is_inlining);
  if (po->pr.iblock >= CO_MAXBLOCKS)
    Py_FatalError("block stack overflow");
  b = &po->pr.blockstack[(int)(po->pr.iblock++)];
  b->b_type = type;
  b->b_level = level;
  b->b_handler = handler;
}

static PyTryBlock *block_pop(PsycoObject *po) {
  extra_assert(!po->pr.is_inlining);
  if (po->pr.iblock <= 0)
    Py_FatalError("block stack underflow");
  return &po->pr.blockstack[(int)(--po->pr.iblock)];
}

/*****************************************************************/
/***   Compile-time Pseudo exceptions                          ***/

DEFINEVAR source_virtual_t ERtPython; /* Exception raised by Python */
DEFINEVAR source_virtual_t EReturn;   /* 'return' statement */
DEFINEVAR source_virtual_t EBreak;    /* 'break' statement */
DEFINEVAR source_virtual_t EContinue; /* 'continue' statement */
DEFINEVAR source_virtual_t EInline;   /* request to inline a function */

DEFINEFN
void PycException_SetString(PsycoObject *po, PyObject *e, const char *text) {
  PyObject *s = NiCompatStr_FromString(text);
  if (s == NULL)
    OUT_OF_MEMORY();
  PycException_SetObject(po, e, s);
}

DEFINEFN
void PycException_SetObject(PsycoObject *po, PyObject *e, PyObject *v) {
  PycException_Raise(
      po, vinfo_new(CompileTime_New((long)e)),
      vinfo_new(CompileTime_NewSk(sk_new((long)v, SkFlagPyObj))));
}

DEFINEFN
void PycException_SetVInfo(PsycoObject *po, PyObject *e, vinfo_t *v) {
  PycException_Raise(po, vinfo_new(CompileTime_New((long)e)), v);
}

DEFINEFN
void PycException_Promote(PsycoObject *po, vinfo_t *vi,
                          c_promotion_t *promotion) {
  vinfo_incref(vi);
  PycException_Raise(po, vinfo_new(VirtualTime_New(&promotion->header)), vi);
}

DEFINEFN
void PycException_SetFormat(PsycoObject *po, PyObject *e, const char *fmt,
                            ...) {
  PyObject *s;
  va_list vargs;

#ifdef HAVE_STDARG_PROTOTYPES
  va_start(vargs, fmt);
#else
  va_start(vargs);
#endif
  s = NiCompatStr_FromFormatV(fmt, vargs);
  va_end(vargs);

  if (s == NULL)
    OUT_OF_MEMORY();
  PycException_SetObject(po, e, s);
}

DEFINEFN
vinfo_t *PycException_Matches(PsycoObject *po, PyObject *e) {
  vinfo_t *result;
  if (PycException_Is(po, &ERtPython)) {
    /* Exception raised by Python, emit a call to
       PyErr_ExceptionMatches() */
    result = psyco_generic_call(po, PyErr_ExceptionMatches, CfReturnTypeInt,
                                "l", (long)e);
  } else if (PycException_IsPython(po)) {
    /* Exception virtually set, that is, present in the PsycoObject
       but not actually set at run-time by Python's PyErr_SetXxx */
    result =
        psyco_generic_call(po, PyErr_GivenExceptionMatches,
                           CfPure | CfReturnTypeInt, "vl", po->pr.exc, (long)e);
  } else { /* pseudo exceptions don't match real Python ones */
    result = psyco_vi_Zero();
  }
  return result;
}

PSY_INLINE void clear_pseudo_exception(PsycoObject *po) {
  extra_assert(PycException_Occurred(po));
  if (po->pr.tb != NULL) {
    vinfo_decref(po->pr.tb, po);
    po->pr.tb = NULL;
  }
  if (po->pr.val != NULL) {
    vinfo_decref(po->pr.val, po);
    po->pr.val = NULL;
  }
  vinfo_decref(po->pr.exc, po);
  po->pr.exc = NULL;
}

DEFINEFN
void PycException_Clear(PsycoObject *po) {
  if (PycException_Is(po, &ERtPython)) {
    /* Clear the Python exception set at run-time */
    psyco_generic_call(po, PyErr_Clear, CfNoReturnValue, "");
  }
  clear_pseudo_exception(po);
}

DEFINEFN
void psyco_virtualize_exception(PsycoObject *po) {
  /* fetch a Python exception set at compile-time (that is, now)
     and turn into a pseudo-exception (typically to be re-raised
     at run-time). */
  PyObject *exc, *val, *tb;
  vinfo_t *vexc, *vval, *vtb;
  PyErr_Fetch(&exc, &val, &tb);
  extra_assert(exc != NULL);

  vexc = vinfo_new(CompileTime_NewSk(sk_new((long)exc, SkFlagPyObj)));
  vval = vinfo_new(CompileTime_NewSk(sk_new((long)val, SkFlagPyObj)));
  vtb = tb == NULL
            ? NULL
            : vinfo_new(CompileTime_NewSk(sk_new((long)tb, SkFlagPyObj)));
  PycException_Restore(po, vexc, vval, vtb);
}

static void cimpl_pyerr_fetch(PyObject *target[]) {
  extra_assert(PyErr_Occurred());
  PyErr_Fetch(target + 0, target + 1, target + 2);
  if (target[0] == NULL) {
    target[0] = Py_None;
    Py_INCREF(Py_None);
  }
  if (target[1] == NULL) {
    target[1] = Py_None;
    Py_INCREF(Py_None);
  }
  if (target[2] == NULL) {
    target[2] = Py_None;
    Py_INCREF(Py_None);
  }
}

DEFINEFN
void cimpl_finalize_frame_locals(PyObject *f_exc_type, PyObject *f_exc_value,
                                 PyObject *f_exc_traceback) {
  /* Called by code emitted by pycencoding.h when a function exits,
     but only if f_exc_type!=NULL. Works like reset_exc_info() of
     Python. */
  PyThreadState *tstate = PyThreadState_GET();
  PyObject *tmp_type, *tmp_value, *tmp_tb;

  /* This frame caught an exception */
  tmp_type = tstate->curexc_type;
  tmp_value = tstate->curexc_value;
  tmp_tb = tstate->curexc_traceback;
  tstate->curexc_type = f_exc_type; /* references are transferred */
  tstate->curexc_value = f_exc_value;
  tstate->curexc_traceback = f_exc_traceback;
  Py_XDECREF(tmp_type);
  Py_XDECREF(tmp_value);
  Py_XDECREF(tmp_tb);
  /* For b/w compatibility */
  PySys_SetObject("exc_type", f_exc_type);
  PySys_SetObject("exc_value", f_exc_value);
  PySys_SetObject("exc_traceback", f_exc_traceback);
}

/* Catching an exception, as 3.7 ceval.c's exception_unwind does for
   SETUP_EXCEPT: the handled exception that was current (tstate->exc_info) is
   saved, then the pending error is fetched, normalised and made current.
   target[] gets the six objects ceval pushes, bottom first: old tb, value,
   type, then new tb, value, type (new references). ceval may push NULL for
   the old tb/value; Ni's stack can't hold NULL, and None means the same
   to exc_info readers. */
static void cimpl_push_exc_info(PyObject *target[]) {
  PyThreadState *tstate = PyThreadState_GET();
  _PyErr_StackItem *exc_info = tstate->exc_info;
  PyObject *exc, *val, *tb;
  extra_assert(PyErr_Occurred());
  target[0] = exc_info->exc_traceback ? exc_info->exc_traceback : Py_None;
  target[1] = exc_info->exc_value ? exc_info->exc_value : Py_None;
  target[2] = exc_info->exc_type ? exc_info->exc_type : Py_None;
  /* the old references move to target[]; borrowed Nones need one */
  if (exc_info->exc_traceback == NULL) Py_INCREF(Py_None);
  if (exc_info->exc_value == NULL) Py_INCREF(Py_None);
  if (exc_info->exc_type == NULL) Py_INCREF(Py_None);
  PyErr_Fetch(&exc, &val, &tb);
  PyErr_NormalizeException(&exc, &val, &tb);
  PyException_SetTraceback(val, tb != NULL ? tb : Py_None);
  Py_INCREF(exc);
  exc_info->exc_type = exc;
  Py_INCREF(val);
  exc_info->exc_value = val;
  exc_info->exc_traceback = tb;
  if (tb == NULL)
    tb = Py_None;
  Py_INCREF(tb);
  target[3] = tb;
  target[4] = val;
  target[5] = exc;
}

/* The restoring half (ceval's UNWIND_EXCEPT_HANDLER), for POP_EXCEPT and for
   unwinding through an EXCEPT_HANDLER block. Doesn't consume its arguments
   (psyco_generic_call can't express that); the caller decrefs them. */
static void cimpl_pop_exc_info(PyObject *type, PyObject *value,
                               PyObject *tb) {
  _PyErr_StackItem *exc_info = PyThreadState_GET()->exc_info;
  PyObject *otype = exc_info->exc_type, *ovalue = exc_info->exc_value,
           *otb = exc_info->exc_traceback;
  Py_INCREF(type);
  Py_INCREF(value);
  Py_INCREF(tb);
  exc_info->exc_type = type;
  exc_info->exc_value = value;
  exc_info->exc_traceback = tb;
  Py_XDECREF(otype);
  Py_XDECREF(ovalue);
  Py_XDECREF(otb);
}

DEFINEFN
void PycException_Fetch(PsycoObject *po) {
  if (PycException_Is(po, &ERtPython)) {
    /* 		vinfo_t* exc = vinfo_new(SOURCE_DUMMY_WITH_REF); */
    /* 		vinfo_t* val = vinfo_new(SOURCE_DUMMY_WITH_REF); */
    /* 		vinfo_t* tb  = vinfo_new(SOURCE_DUMMY_WITH_REF); */
    /* 		psyco_generic_call(po, PyErr_Fetch, CfNoReturnValue, */
    /* 				   "rrr", exc, val, tb); */
    /* 		vinfo_decref(tb, po); */
    vinfo_array_t *array = array_new(3);
    psyco_generic_call(po, cimpl_pyerr_fetch, CfNoReturnValue, "A", array);
    clear_pseudo_exception(po);
    po->pr.exc = array->items[0];
    po->pr.val = array->items[1]; /* po->pr.val!=NULL after this */
    po->pr.tb = array->items[2];  /* po->pr.tb!=NULL after this */
    array_release(array);
  }
}


static PyFrameObject *cimpl_new_frame(PyThreadState *tstate, PyCodeObject *code,
                                      PyObject *globals, int lasti,
                                      int lineno) {
  /* Make a minimalistic frame object, working around the specific
     expectations of PyFrame_New(), which is the only reasonable way
     to create frame objects (because of free lists etc) */
  PyFrameObject *result;
  PyFrameObject *back = tstate->frame;
  tstate->frame = NULL; /* frame objects are not created in stack order
                           with Psyco, so it's probably better not to
                           create plain wrong chained lists */
  result = PyFrame_New(tstate, code, globals, NULL);
  tstate->frame = back;
  if (result != NULL) {
    result->f_lasti = lasti;
    result->f_lineno = lineno;
  }
  return result;
}

static void cimpl_rt_traceback(PyCodeObject *code, PyObject *globals, int lasti,
                               int lineno) {
  PyThreadState *tstate = PyThreadState_GET();
  PyFrameObject *f = cimpl_new_frame(tstate, code, globals, lasti, lineno);
  /* an out-of-memory error just replaces the current exception */
  if (f != NULL) {
    PyTraceBack_Here(f);
    Py_DECREF(f);
  }
}

static PyObject *cimpl_vt_traceback(PyCodeObject *code, PyObject *globals,
                                    int lasti, int lineno) {
  /* This is a hack around the limited interface to tracebacks.
     PyTraceBack_Here() seems to be the only clean way to create new
     traceback objects. */
  PyObject *oldtb;
  PyObject *newtb;
  PyThreadState *tstate = PyThreadState_GET();
  PyFrameObject *f = cimpl_new_frame(tstate, code, globals, lasti, lineno);
  if (f == NULL) {
    /* out-of-memory error */
    Py_INCREF(Py_None);
    return Py_None;
  }
  oldtb = tstate->curexc_traceback; /* might be NULL */
  Py_XINCREF(oldtb);
  if (PyTraceBack_Here(f)) {
    /* out-of-memory error */
    Py_XDECREF(oldtb);
    Py_DECREF(f);
    Py_INCREF(Py_None);
    return Py_None;
  }
  /* the new traceback is now in curexc_traceback */
  newtb = tstate->curexc_traceback; /* extracts the reference */
  tstate->curexc_traceback = oldtb; /* consumes the reference */
  Py_DECREF(f);
  return newtb;
}

/* record a traceback entry for the current virtual Python exception */
PSY_INLINE void PsycoTraceBack_Here(PsycoObject *po, int lasti) {
  int lineno = PyCode_Addr2Line(po->pr.co, lasti);

  if (PycException_Is(po, &ERtPython)) {
    /* Python exception is actually set at run-time */
    psyco_generic_call(po, cimpl_rt_traceback, CfNoReturnValue, "lvll",
                       (long)po->pr.co, LOC_GLOBALS, lasti, lineno);
  } else {
    /* We only have a virtual-time exception (not set in Python),
       so we build po->pr.tb without actually setting it either */
    vinfo_t *tb = psyco_generic_call(
        po, cimpl_vt_traceback, CfCommonNewRefPyObjectNoError, "lvll",
        (long)po->pr.co, LOC_GLOBALS, lasti, lineno);
    vinfo_xdecref(po->pr.tb, po);
    po->pr.tb = tb;
  }
}

/***************************************************************/
/***                      Initialization                       ***/
/***************************************************************/

DEFINEVAR source_known_t psyco_skZero;     /* known value 0 */
DEFINEVAR source_known_t psyco_skOne;      /* known value 1 */
DEFINEVAR source_known_t psyco_skNone;     /* known value 'Py_None' */
DEFINEVAR source_known_t psyco_skPy_False; /* known value 'Py_False' */
DEFINEVAR source_known_t psyco_skPy_True;  /* known value 'Py_True' */
DEFINEVAR source_known_t psyco_skNotImplemented;

static PyObject *s_builtin_object; /* intern string '__builtins__' */

INITIALIZATIONFN
void psyco_pycompiler_init(void) {
  s_builtin_object = NiCompatStr_InternFromString("__builtins__");

  psyco_skZero.refcount1_flags = SkFlagFixed;
  psyco_skZero.value = (long)0;
  psyco_skOne.refcount1_flags = SkFlagFixed;
  psyco_skOne.value = (long)1;
  psyco_skNone.refcount1_flags = SkFlagFixed;
  psyco_skNone.value = (long)Py_None;
  psyco_skPy_False.refcount1_flags = SkFlagFixed;
  psyco_skPy_False.value = (long)Py_False;
  psyco_skPy_True.refcount1_flags = SkFlagFixed;
  psyco_skPy_True.value = (long)Py_True;
  psyco_skNotImplemented.refcount1_flags = SkFlagFixed;
  psyco_skNotImplemented.value = (long)Py_NotImplemented;

  ERtPython = psyco_vsource_not_important;
  EReturn = psyco_vsource_not_important;
  EContinue = psyco_vsource_not_important;
  EBreak = psyco_vsource_not_important;
  EInline = psyco_vsource_not_important;
}

/***************************************************************/
/***                          Compiler                         ***/
/***************************************************************/

#define CHKSTACK(n)                                                            \
  extra_assert(0 <= po->pr.stack_level + (n) &&                                \
               po->pr.stack_base + po->pr.stack_level + (n) <                  \
                   po->vlocals.count)

#define PUSH(v) (CHKSTACK(0), stack_a[po->pr.stack_level++] = v)
#define POP(targ) do {                                          \
  CHKSTACK(-1);                                                 \
  targ = stack_a[--po->pr.stack_level];                         \
  stack_a[po->pr.stack_level] = NULL;                           \
} while(0)
#define NTOP(n) (CHKSTACK(-(n)), stack_a[po->pr.stack_level - (n)])
#define TOP() NTOP(1)
#define POP_DECREF()                                                           \
  do {                                                                         \
    vinfo_t *v1;                                                               \
    POP(v1);                                                                   \
    vinfo_decref(v1, po);                                                      \
  } while (0)

/*#define GETCODEOBJ()    ((PyCodeObject*)(KNOWN_SOURCE(LOC_CODE)->value))*/
#define GETCONST(i) (PyTuple_GET_ITEM(co->co_consts, i))
#define GETNAMEV(i) (PyTuple_GET_ITEM(co->co_names, i))

#define GETLOCAL(i) (LOC_LOCALS_PLUS[i])
#define SETLOCAL(i, v)                                                         \
  do {                                                                         \
    vinfo_decref(GETLOCAL(i), po);                                             \
    GETLOCAL(i) = v;                                                           \
  } while (0)

#define STACK_POINTER() (stack_a + po->pr.stack_level)
#define INSTR_OFFSET() (next_instr)
#define STACK_LEVEL() (po->pr.stack_level)
#define JUMPBY(offset) (next_instr += (offset))
#define JUMPTO(target) (next_instr = (target))

/* ceval's UNWIND_EXCEPT_HANDLER: drop what the handler left above its six
   items, then restore the saved exc_info from the bottom three */
static void unwind_except_handler(PsycoObject *po, PyTryBlock *b) {
  vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
  vinfo_t *type, *value, *tb;
  extra_assert(STACK_LEVEL() >= b->b_level + 3);
  while (STACK_LEVEL() > b->b_level + 3)
    POP_DECREF();
  POP(type);
  POP(value);
  POP(tb);
  psyco_generic_call(po, cimpl_pop_exc_info, CfNoReturnValue, "vvv", type,
                     value, tb);
  vinfo_decref(tb, po);
  vinfo_decref(value, po);
  vinfo_decref(type, po);
}


#define SAVE_NEXT_INSTR(nextinstr1) (po->pr.next_instr = (nextinstr1))

/***************************************************************/
 /***   LOAD_GLOBAL tricks                                    ***/


static void mark_varying(PsycoObject* po, PyObject* key)
{
        if (po->pr.changing_globals == NULL) {
                po->pr.changing_globals = PyDict_New();
                if (po->pr.changing_globals == NULL)
                        OUT_OF_MEMORY();
        }
        if (PyDict_SetItem(po->pr.changing_globals, key, Py_True))
                OUT_OF_MEMORY();
}

/* #define MISSING_OPCODE(opcode)					 */
/* 	case opcode:						 */
/* 		PycException_SetString(po, PyExc_PsycoError, */
/* 				  "opcode '" #opcode "' not implemented"); */
/* 		break */

PyObject *psy_get_builtins(PyObject *globals) {
  static PyObject *minimal_builtins = NULL;
  PyObject *builtins;
  /* code copied from frameobject.c */
  builtins = PyDict_GetItem((PyObject *)globals, s_builtin_object);
  if (builtins) {
    if (PyDict_Check(builtins))
      goto done;
    if (PyModule_Check(builtins)) {
      builtins = PyModule_GetDict(builtins);
      if (builtins) {
        psyco_assert(PyDict_Check(builtins));
        goto done;
      }
    }
  }
  /* No builtins!  Make up a minimal one
     Give them 'None', at least. */
  if (minimal_builtins == NULL) {
    minimal_builtins = PyDict_New();
    if (minimal_builtins == NULL ||
        PyDict_SetItemString(minimal_builtins, "None", Py_None) < 0)
      OUT_OF_MEMORY();
  }
  builtins = minimal_builtins;
done:
  return builtins;
}

/* CALL_FUNCTION (oparg positional arguments) and CALL_FUNCTION_KW (oparg
   arguments, the last ones named by the constant tuple on top), as 3.7's
   call_function(). The stack, top down: [names tuple], arguments, callable.
   *stack_to_pop counts everything including the callable. Keyword
   arguments go in a fresh dict: duplicates are impossible here (the
   bytecode compiler rejects them) and the callee checks the rest. */
static vinfo_t *psyco_ext_do_calls(PsycoObject *po, int opcode, int oparg,
                                   vinfo_t **stack_top, int *stack_to_pop) {
  int na = oparg, nk = 0, i;
  PyObject *kwnames = NULL;
  vinfo_t **args;
  vinfo_t *vargs = NULL;
  vinfo_t *wdict = NULL;
  vinfo_t *result = NULL;

  if (opcode == CALL_FUNCTION_KW) {
    vinfo_t *names = stack_top[-1];
    if (!is_compiletime(names->source) ||
        !PyTuple_CheckExact(
            kwnames = (PyObject *)CompileTime_Get(names->source)->value) ||
        PyTuple_GET_SIZE(kwnames) > oparg) {
      PycException_SetString(po, PyExc_SystemError,
                             "bad CALL_FUNCTION_KW keyword names");
      return NULL;
    }
    nk = PyTuple_GET_SIZE(kwnames);
    na = oparg - nk;
    stack_top--;
  }
  args = stack_top - oparg;
  *stack_to_pop = oparg + 1 + (opcode == CALL_FUNCTION_KW);

  if (nk == 0)
    wdict = psyco_vi_Zero(); /* no keyword arguments */
  else {
    wdict = PsycoDict_NewPresized(po, nk);
    if (wdict == NULL)
      goto fail;
    for (i = 0; i < nk; i++)
      if (!PsycoDict_SetItemC(po, wdict, PyTuple_GET_ITEM(kwnames, i),
                              args[na + i]))
        goto fail;
  }
  vargs = PsycoTuple_New(na, args); /* virtual tuple */
  result = PsycoObject_Call(po, args[-1], vargs, wdict);
fail:
  vinfo_xdecref(vargs, po);
  vinfo_xdecref(wdict, po);
  return result;
}

/* CALL_FUNCTION_EX when the arguments are not already an exact tuple and
   dict: 3.7's conversions and messages, then the call. */
static PyObject *cimpl_call_function_ex(PyObject *func, PyObject *callargs,
                                        PyObject *kwargs) {
  PyObject *d = NULL, *t = NULL, *result;
  if (kwargs != NULL && !PyDict_CheckExact(kwargs)) {
    d = PyDict_New();
    if (d == NULL)
      return NULL;
    if (PyDict_Update(d, kwargs) != 0) {
      Py_DECREF(d);
      /* PyDict_Update raises AttributeError (from looking up 'keys')
         when its argument is not a mapping */
      if (PyErr_ExceptionMatches(PyExc_AttributeError))
        PyErr_Format(PyExc_TypeError,
                     "%.200s%.200s argument after ** "
                     "must be a mapping, not %.200s",
                     PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                     kwargs->ob_type->tp_name);
      return NULL;
    }
    kwargs = d;
  }
  if (!PyTuple_CheckExact(callargs)) {
    if (callargs->ob_type->tp_iter == NULL && !PySequence_Check(callargs)) {
      PyErr_Format(PyExc_TypeError,
                   "%.200s%.200s argument after * "
                   "must be an iterable, not %.200s",
                   PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                   callargs->ob_type->tp_name);
      Py_XDECREF(d);
      return NULL;
    }
    t = PySequence_Tuple(callargs);
    if (t == NULL) {
      Py_XDECREF(d);
      return NULL;
    }
    callargs = t;
  }
  result = PyObject_Call(func, callargs, kwargs);
  Py_XDECREF(d);
  Py_XDECREF(t);
  return result;
}

/* CALL_FUNCTION_EX: f(*args) or f(*args, **kwargs). The stack, top down:
   [kwargs], args, callable. An exact tuple and dict (the usual case) are
   passed straight to PsycoObject_Call, which can specialise the call;
   anything else is converted at run time, as ceval does. */
static vinfo_t *psyco_ext_do_call_ex(PsycoObject *po, int oparg,
                                     vinfo_t **stack_top,
                                     int *stack_to_pop) {
  vinfo_t *kwargs = (oparg & 1) ? stack_top[-1] : NULL;
  vinfo_t *callargs = stack_top[-1 - (oparg & 1)];
  vinfo_t *func = stack_top[-2 - (oparg & 1)];
  vinfo_t *zero = NULL, *result;
  PyTypeObject *t;
  *stack_to_pop = 2 + (oparg & 1);

  t = Psyco_NeedType(po, callargs);
  if (t == NULL)
    return NULL;
  if (kwargs == NULL)
    kwargs = zero = psyco_vi_Zero();
  if (t == &PyTuple_Type) {
    if (zero == NULL) {
      t = Psyco_NeedType(po, kwargs);
      if (t == NULL)
        return NULL;
    }
    if (zero != NULL || t == &PyDict_Type) {
      result = PsycoObject_Call(po, func, callargs, kwargs);
      goto done;
    }
  }
  result = psyco_generic_call(po, cimpl_call_function_ex,
                              CfCommonNewRefPyObject, "vvv", func, callargs,
                              kwargs);
done:
  vinfo_xdecref(zero, po);
  return result;
}

/***************************************************************/
/***   Run-time implementation of various opcodes            ***/

/* the code of the following functions is "copy-pasted" from ceval.c */

#define GLOBAL_NAME_ERROR_MSG "global name '%.200s' is not defined"
#define UNBOUNDLOCAL_ERROR_MSG                                                 \
  "local variable '%.200s' referenced before assignment"

static PyObject *cimpl_load_global(PyObject *globals, PyObject *w) {
  PyObject *x;
  RECLIMIT_SAFE_ENTER();
  x = PyDict_GetItem(globals, w);
  if (x == NULL) {
    x = PyDict_GetItem(psy_get_builtins(globals), w);
    if (x == NULL) {
      const char *obj_str = NiCompatStr_AsString(w);
      if (obj_str)
        PyErr_Format(PyExc_NameError, GLOBAL_NAME_ERROR_MSG, obj_str);
      RECLIMIT_SAFE_LEAVE();
      return NULL;
    }
  }
  RECLIMIT_SAFE_LEAVE();
  Py_INCREF(x);
  return x;
}

static int cimpl_unpack_iterable(PyObject *v, int argcnt, PyObject **sp) {
  int i = 0;
  PyObject *it; /* iter(v) */
  PyObject *w;

  extra_assert(v != NULL);

  it = PyObject_GetIter(v);
  if (it == NULL)
    goto Error;

  for (; i < argcnt; i++) {
    w = PyIter_Next(it);
    if (w == NULL) {
      /* Iterator done, via error or exhaustion. */
      if (!PyErr_Occurred()) {
        PyErr_Format(PyExc_ValueError, "need more than %d value%s to unpack", i,
                     i == 1 ? "" : "s");
      }
      goto Error;
    }
    *sp++ = w;
  }

  /* We better have exhausted the iterator now. */
  w = PyIter_Next(it);
  if (w == NULL) {
    if (PyErr_Occurred())
      goto Error;
    Py_DECREF(it);
    return 0;
  }
  PyErr_SetString(PyExc_ValueError, "too many values to unpack");
  Py_DECREF(w);
  /* fall through */
Error:
  for (; i > 0; i--) {
    --sp;
    Py_DECREF(*sp);
  }
  Py_XDECREF(it);
  return -1;
}

static int cimpl_unpack_list(PyObject *listobject, int argcnt, PyObject **sp) {
  int i;
  extra_assert(PyList_Check(listobject));

  if (PyList_GET_SIZE(listobject) != argcnt) {
    PyErr_SetString(PyExc_ValueError, "unpack list of wrong size");
    return -1;
  }
  for (i = argcnt; i--;) {
    PyObject *v = sp[i] = PyList_GET_ITEM(listobject, i);
    Py_INCREF(v);
  }
  return 0;
}

/* Logic for the raise statement.
   XXX a meta-implementation would be nice (e.g. to know at compile-time
   which exception class we got, so that we can know at compile-time which
   except: statements match) */
/* 3.7 ceval.c do_raise, except that it doesn't consume references to its
   arguments (psyco_generic_call can't express that), so it takes its own
   first. The reraise case doesn't return ceval's "don't add a traceback
   entry" flag, so Ni records one extra traceback entry for a bare raise. */
static void cimpl_do_raise(PyObject *exc, PyObject *cause) {
  PyObject *type = NULL, *value = NULL;

  if (exc == NULL) {
    /* Reraise: _PyErr_GetTopmostException isn't exported in 3.7 */
    _PyErr_StackItem *exc_info = PyThreadState_GET()->exc_info;
    PyObject *tb;
    while ((exc_info->exc_type == NULL || exc_info->exc_type == Py_None) &&
           exc_info->previous_item != NULL)
      exc_info = exc_info->previous_item;
    type = exc_info->exc_type;
    value = exc_info->exc_value;
    tb = exc_info->exc_traceback;
    if (type == Py_None || type == NULL) {
      PyErr_SetString(PyExc_RuntimeError, "No active exception to reraise");
      return;
    }
    Py_XINCREF(type);
    Py_XINCREF(value);
    Py_XINCREF(tb);
    PyErr_Restore(type, value, tb);
    return;
  }
  Py_INCREF(exc);
  Py_XINCREF(cause);

  if (PyExceptionClass_Check(exc)) {
    type = exc;
    value = _PyObject_CallNoArg(exc);
    if (value == NULL)
      goto raise_error;
    if (!PyExceptionInstance_Check(value)) {
      PyErr_Format(PyExc_TypeError,
                   "calling %R should have returned an instance of "
                   "BaseException, not %R",
                   type, Py_TYPE(value));
      goto raise_error;
    }
  } else if (PyExceptionInstance_Check(exc)) {
    value = exc;
    type = PyExceptionInstance_Class(exc);
    Py_INCREF(type);
  } else {
    Py_DECREF(exc);
    PyErr_SetString(PyExc_TypeError,
                    "exceptions must derive from BaseException");
    goto raise_error;
  }

  if (cause) {
    PyObject *fixed_cause;
    if (PyExceptionClass_Check(cause)) {
      fixed_cause = _PyObject_CallNoArg(cause);
      if (fixed_cause == NULL)
        goto raise_error;
      Py_DECREF(cause);
    } else if (PyExceptionInstance_Check(cause)) {
      fixed_cause = cause;
    } else if (cause == Py_None) {
      Py_DECREF(cause);
      fixed_cause = NULL;
    } else {
      PyErr_SetString(PyExc_TypeError,
                      "exception causes must derive from BaseException");
      goto raise_error;
    }
    PyException_SetCause(value, fixed_cause);
  }

  PyErr_SetObject(type, value);
  Py_DECREF(value);
  Py_DECREF(type);
  return;

raise_error:
  Py_XDECREF(value);
  Py_XDECREF(type);
  Py_XDECREF(cause);
}

static PyObject *cimpl_import_name(PyObject *globals, PyObject *name,
                                   PyObject *fromlist
#if VERYCONVOLUTED_IMPORT_NAME
                                   ,
                                   PyObject *fitharg
#endif
) {
  PyObject *w;
  PyObject *x = PyDict_GetItemString(psy_get_builtins(globals), "__import__");
  if (x == NULL) {
    PyErr_SetString(PyExc_ImportError, "__import__ not found");
    return NULL;
  }
#if VERYCONVOLUTED_IMPORT_NAME
  if (NiCompatInt_AsLong(fitharg) != -1 || PyErr_Occurred())
    w = PyTuple_Pack(5, name, globals, Py_None, fromlist, fitharg);
  else
#endif
    w = Py_BuildValue("(OOOO)", name, globals, Py_None, fromlist);
  if (w == NULL)
    return NULL;
  x = PyEval_CallObject(x, w);
  Py_DECREF(w);
  return x;
}

static int cimpl_import_all_from(PyObject *locals, PyObject *v) {
  PyObject *all = PyObject_GetAttrString(v, "__all__");
  PyObject *dict, *name, *value;
  int skip_leading_underscores = 0;
  int pos, err;

  if (all == NULL) {
    if (!PyErr_ExceptionMatches(PyExc_AttributeError))
      return -1; /* Unexpected error */
    PyErr_Clear();
    dict = PyObject_GetAttrString(v, "__dict__");
    if (dict == NULL) {
      if (!PyErr_ExceptionMatches(PyExc_AttributeError))
        return -1;
      PyErr_SetString(PyExc_ImportError,
                      "from-import-* object has no __dict__ and no __all__");
      return -1;
    }
    all = PyMapping_Keys(dict);
    Py_DECREF(dict);
    if (all == NULL)
      return -1;
    skip_leading_underscores = 1;
  }

  for (pos = 0, err = 0;; pos++) {
    name = PySequence_GetItem(all, pos);
    if (name == NULL) {
      if (!PyErr_ExceptionMatches(PyExc_IndexError))
        err = -1;
      else
        PyErr_Clear();
      break;
    }
    if (skip_leading_underscores && NiCompatStr_Check(name) &&
        NiCompatStr_AS_STRING(name)[0] == '_') {
      Py_DECREF(name);
      continue;
    }
    value = PyObject_GetAttr(v, name);
    if (value == NULL)
      err = -1;
    else
      err = PyDict_SetItem(locals, name, value);
    Py_DECREF(name);
    Py_XDECREF(value);
    if (err != 0)
      break;
  }
  Py_DECREF(all);
  return err;
}

/***************************************************************/
/***                         Main loop                         ***/
/***************************************************************/

static bool compute_and_raise_exception(PsycoObject *po) {
  /* &ERtPython is the case where the code that raised
     the Python exception is already written, e.g. when
     we called a function in the Python interpreter which
     raised an exception. */
  if (!PycException_Is(po, &ERtPython)) {
    /* In the other cases (virtual exception),
       compute and raise the exception now. */
    char args[4];
    args[3] = 0;
    if (po->pr.tb == NULL)
      args[2] = 'l';
    else {
      args[2] = 'v';
      consume_reference(po, po->pr.tb);
    }
    if (po->pr.val == NULL)
      args[1] = 'l';
    else {
      args[1] = 'v';
      consume_reference(po, po->pr.val);
    }
    args[0] = 'v';
    consume_reference(po, po->pr.exc);
    return (psyco_generic_call(po, PyErr_Restore, CfNoReturnValue, args,
                               po->pr.exc, po->pr.val, po->pr.tb) != NULL);
  } else
    return true;
}

static code_t *exit_function(PsycoObject *po) {
  vinfo_t **locals_plus;
  vinfo_t **pp;
  Source retsource;
  extra_assert(!po->pr.is_inlining);

  /* clear the stack and the locals */
  locals_plus = LOC_LOCALS_PLUS;
  for (pp = po->vlocals.items + po->vlocals.count; --pp >= locals_plus;)
    if (*pp != NULL) {
      vinfo_decref(*pp, po);
      *pp = NULL;
    }

  if (PycException_Is(po, &EReturn)) {
    /* load the return value */
    vinfo_t *retval = po->pr.val;
    extra_assert(retval != NULL);
    if (!compute_vinfo(retval, po))
      return NULL;
    /* return a new reference */
    consume_reference(po, retval);
    if (retval->array->count > 0) {
      array_delete(retval->array, po);
      retval->array = NullArray;
    }
    retsource = retval->source;
  } else {
    if (!compute_and_raise_exception(po))
      return NULL;
    clear_pseudo_exception(po);
    sk_incref(&psyco_skZero);
    retsource = CompileTime_NewSk(&psyco_skZero); /*to return NULL */
  }

  return psyco_finish_return(po, retsource);
}

/***************************************************************/
/***   the main loop of the interpreter/compiler.            ***/

/* psyco_pycompiler_mainloop() compiles the function,
   writing function entry and exit points if needed, and destroys 'po' when
   it is done */
DEFINEFN
code_t *psyco_pycompiler_mainloop(PsycoObject *po) {
  /* 'stack_a' is the Python stack base pointer */
  vinfo_t **stack_a = po->vlocals.items + po->pr.stack_base;
  int opcode = 0; /* Current opcode */
  int oparg = 0;  /* Current opcode argument, if any */
  code_t *code1;
  int saved_rec_limit;

  /* save and restore the current Python exception throughout compilation */
  PyObject *old_py_exc, *old_py_val, *old_py_tb;
  PyErr_Fetch(&old_py_exc, &old_py_val, &old_py_tb);

  /* hack: protection against obscure failures during compilation caused by
     the CPython recursion depth being very close to the limit */
  saved_rec_limit = Py_GetRecursionLimit();
  Py_SetRecursionLimit(saved_rec_limit + 50);

  if ((psyco_mp_flags(po->pr.merge_points) & MP_FLAGS_HAS_EXCEPT) != 0 &&
      (LOC_CONTINUATION->array == NullArray)) {
    /* for functions that have "try: except:" blocks, we reserve some room
       in the stack to store the previous tstate->curexc_xxx if an exception
       is raised and catched. This space plays the role of Python's
       PyFrameObject::f_exc_xxx fields. */
    psyco_emit_header(po, 3);
  }

  int at_jump_target = 0;
#if HAVE_NEXT_INSTR_POP
  if (po->pr.next_instr < -1) {
    /* there is a pending POP operation to be done: this is the target of a
       POP_JUMP_IF_xxx. With the pop done the state is the target's, so its
       merge point (often a loop head, for a jump back) is checked before
       the first instruction; otherwise the path would be compiled afresh
       up to the next merge point, every back-edge a new copy of the loop */
    POP_DECREF();
    po->pr.next_instr &= ~NEXT_INSTR_POP;
    at_jump_target = !is_respawning(po);
  }
#endif

  while (1) {
    /* 'co' is the code object we are interpreting/compiling */
    PyCodeObject *co = po->pr.co;
    unsigned char *bytecode = (unsigned char *)PyBytes_AS_STRING(co->co_code);
    vinfo_t *u, *v, /* temporary objects    */
        *w, *x;     /* popped off the stack */
    condition_code_t cc;
    /* 'next_instr' is the position in the byte-code of the next instr */
    int next_instr = po->pr.next_instr;
    mergepoint_t *mp =
        psyco_next_merge_point(po->pr.merge_points, next_instr + 1);

    if (at_jump_target) {
      at_jump_target = 0;
      mp = psyco_next_merge_point(po->pr.merge_points, next_instr);
      goto fine; /* the merge point check, before any instruction */
    }

    /* main loop */
    while (1) {

      extra_assert(!PycException_Occurred(po));
      psyco_assert_coherent(po); /* this test is expensive */

      /* are we potentially running out of stack space? */
      CHECK_STACK_SPACE();

      /* save 'next_instr' and 'respawn_cnt' */
      SAVE_NEXT_INSTR(next_instr); /* could be optimized, not needed in the
                                      case of an opcode that cannot set
                                      run-time conditions */


      opcode = bytecode[next_instr++];
      oparg = bytecode[next_instr++];
    dispatch_opcode:

      /* Main switch on opcode */

      /* !!IMPORTANT!!
         No operation with side-effects must be performed before we are
         sure the compilation of the current instruction succeeded!
         Indeed, if the compilation is interrupted by a C++ exception,
         it will be restarted by re-running psyco_pycompiler_mainloop()
         and this will restart the compilation of the instruction from
         the beginning. In particular, use POP() with care. Better use
         TOP() to get the arguments off the stack and call POP() at the
         end when you are sure everything when fine. */

      /* All opcodes found here must also be listed in mergepoints.c.
         Python code objects using a bytecode instruction not listed
         in mergepoints.c are never Psyco-ified. */

      switch (opcode) {

        /* case STOP_CODE: this is an error! */

      case POP_TOP:
        POP_DECREF();
        goto fine;

      case ROT_TWO:
        POP(v);
        POP(w);
        PUSH(v);
        PUSH(w);
        goto fine;

      case ROT_THREE:
        POP(v);
        POP(w);
        POP(x);
        PUSH(v);
        PUSH(x);
        PUSH(w);
        goto fine;

      case DUP_TOP:
        v = TOP();
        vinfo_incref(v);
        PUSH(v);
        goto fine;

      case DUP_TOP_TWO:
        v = NTOP(2);
        w = TOP();
        vinfo_incref(v);
        vinfo_incref(w);
        PUSH(v);
        PUSH(w);
        goto fine;

      case UNARY_POSITIVE:
        x = PsycoNumber_Positive(po, TOP());
        if (x == NULL)
          break;
        POP_DECREF();
        PUSH(x);
        goto fine;

      case UNARY_NEGATIVE:
        x = PsycoNumber_Negative(po, TOP());
        if (x == NULL)
          break;
        POP_DECREF();
        PUSH(x);
        goto fine;

      case UNARY_NOT:
        v = PsycoObject_IsTrue(po, TOP());
        cc = integer_NON_NULL(po, v);
        if (cc == CC_ERROR)
          break;

        cc = INVERT_CC(cc);
        /* turns 'cc' into a virtual Python boolean object */
        x = PsycoBool_FromCondition(po, cc);
        POP_DECREF();
        PUSH(x); /* consumes ref on 'x' */
        goto fine;

      case UNARY_INVERT:
        x = PsycoNumber_Invert(po, TOP());
        if (x == NULL)
          break;
        POP_DECREF();
        PUSH(x);
        goto fine;

      case BINARY_POWER:
        u = psyco_vi_None();
        x = PsycoNumber_Power(po, NTOP(2), NTOP(1), u);
        vinfo_decref(u, po);
        if (x == NULL)
          break;
        POP_DECREF();
        POP_DECREF();
        PUSH(x);
        goto fine;

#define BINARY_OPCODE(opcode, psycofn)                                         \
  case opcode:                                                                 \
    x = psycofn(po, NTOP(2), NTOP(1));                                         \
    if (x == NULL)                                                             \
      break;                                                                   \
    POP_DECREF();                                                              \
    POP_DECREF();                                                              \
    PUSH(x);                                                                   \
    goto fine

      BINARY_OPCODE(BINARY_MULTIPLY, PsycoNumber_Multiply);
      BINARY_OPCODE(BINARY_MODULO, PsycoNumber_Remainder);
      BINARY_OPCODE(BINARY_ADD, PsycoNumber_Add);
      BINARY_OPCODE(BINARY_SUBTRACT, PsycoNumber_Subtract);
      BINARY_OPCODE(BINARY_SUBSCR, PsycoObject_GetItem);
      BINARY_OPCODE(BINARY_LSHIFT, PsycoNumber_Lshift);
      BINARY_OPCODE(BINARY_RSHIFT, PsycoNumber_Rshift);
      BINARY_OPCODE(BINARY_AND, PsycoNumber_And);
      BINARY_OPCODE(BINARY_XOR, PsycoNumber_Xor);
      BINARY_OPCODE(BINARY_OR, PsycoNumber_Or);
      BINARY_OPCODE(BINARY_TRUE_DIVIDE, PsycoNumber_TrueDivide);
      BINARY_OPCODE(BINARY_FLOOR_DIVIDE, PsycoNumber_FloorDivide);
      BINARY_OPCODE(INPLACE_TRUE_DIVIDE, PsycoNumber_InPlaceTrueDivide);
      BINARY_OPCODE(INPLACE_FLOOR_DIVIDE, PsycoNumber_InPlaceFloorDivide);

      case LIST_APPEND:
        w = NTOP(1);
        v = NTOP(oparg + 1);
        if (!PsycoList_Append(po, v, w))
          break;
        POP_DECREF();
        goto fine;

      case INPLACE_POWER:
        u = psyco_vi_None();
        x = PsycoNumber_InPlacePower(po, NTOP(2), NTOP(1), u);
        vinfo_decref(u, po);
        if (x == NULL)
          break;
        POP_DECREF();
        POP_DECREF();
        PUSH(x);
        goto fine;

      BINARY_OPCODE(INPLACE_MULTIPLY, PsycoNumber_InPlaceMultiply);
      BINARY_OPCODE(INPLACE_MODULO, PsycoNumber_InPlaceRemainder);
      BINARY_OPCODE(INPLACE_ADD, PsycoNumber_InPlaceAdd);
      BINARY_OPCODE(INPLACE_SUBTRACT, PsycoNumber_InPlaceSubtract);
      BINARY_OPCODE(INPLACE_LSHIFT, PsycoNumber_InPlaceLshift);
      BINARY_OPCODE(INPLACE_RSHIFT, PsycoNumber_InPlaceRshift);
      BINARY_OPCODE(INPLACE_AND, PsycoNumber_InPlaceAnd);
      BINARY_OPCODE(INPLACE_XOR, PsycoNumber_InPlaceXor);
      BINARY_OPCODE(INPLACE_OR, PsycoNumber_InPlaceOr);

      case STORE_SUBSCR:
        w = NTOP(1);
        v = NTOP(2);
        u = NTOP(3);
        /* v[w] = u */
        if (!PsycoObject_SetItem(po, v, w, u))
          break;
        POP_DECREF();
        POP_DECREF();
        POP_DECREF();
        goto fine;

      case DELETE_SUBSCR:
        w = NTOP(1);
        v = NTOP(2);
        /* del v[w] */
        if (!PsycoObject_SetItem(po, v, w, NULL))
          break;
        POP_DECREF();
        POP_DECREF();
        goto fine;

      case BREAK_LOOP:
        PycException_Raise(po, vinfo_new(VirtualTime_New(&EBreak)), NULL);
        break;

      case CONTINUE_LOOP:
        PycException_Raise(po, vinfo_new(VirtualTime_New(&EContinue)),
                           vinfo_new(CompileTime_New(oparg)));
        break;

      case RAISE_VARARGS:
        /* 3.x: 0 reraise, 1 raise exc, 2 raise exc from cause */
        v = w = x = psyco_vi_Zero(); /* NULL; one reference, shared */
        switch (oparg) {
        case 2:
          v = NTOP(1); /* cause */
          w = NTOP(2); /* exc */
          psyco_generic_call(po, cimpl_do_raise, CfPyErrAlways, "vv", w, v);
          break;
        case 1:
          w = NTOP(1); /* exc */
          /* Fallthrough */
        case 0:
          psyco_generic_call(po, cimpl_do_raise, CfPyErrAlways, "vv", w, v);
          break;
        default:
          PycException_SetString(po, PyExc_SystemError,
                                 "bad RAISE_VARARGS oparg");
        }
        vinfo_decref(x, po);
        break;

        /*MISSING_OPCODE(LOAD_LOCALS);*/
        /*MISSING_OPCODE(WITH_CLEANUP);*/

      case RETURN_VALUE:
        POP(v);
        PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), v);
        break;

#ifdef RETURN_NONE
        /* This opcode was temporarily added to the Python 2.3 CVS.
           Now it is gone again, so the following will probably
           never be compiled into Psyco anyway. */
      case RETURN_NONE:
        v = psyco_vi_None();
        PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), v);
        break;
#endif

#ifdef NOP
      case NOP:
        goto fine;
#endif

        /*MISSING_OPCODE(YIELD_VALUE);*/
        /*MISSING_OPCODE(EXEC_STMT);*/

      case POP_BLOCK: {
        PyTryBlock *b = block_pop(po);
        while (STACK_LEVEL() > b->b_level) {
          POP(v);
          vinfo_decref(v, po);
        }
        goto fine;
      }

      case POP_EXCEPT: {
        PyTryBlock *b = block_pop(po);
        if (b->b_type != EXCEPT_HANDLER) {
          PycException_SetString(po, PyExc_SystemError,
                                 "popped block is not an except handler");
          break;
        }
        unwind_except_handler(po, b);
        goto fine;
      }

      case END_FINALLY:
        /* First make extra sure no exception is pending */
        if (PycException_Occurred(po))
          Py_FatalError("psyco: undetected exception "
                        "in END_FINALLY");
        POP(v);
        if (psyco_knowntobe(v, (long)Py_None)) {
          /* 'None' on the stack, it is the end of a finally
             block with no exception raised */
          vinfo_decref(v, po);
          goto fine;
        }
        if (Psyco_KnownType(v) == &PyTuple_Type && PsycoTuple_Load(v) == 3) {
          /* 'v' is a 3- tuple. As no real Python exception is
             a tuple object we are sure it comes from a
             finally block (see the SETUP_FINALLY stack unwind
             code at the end of pycompiler.c) */
          /* Re-raise the pseudo exception. This will re-raise
             exceptions like EReturn as well. */
          po->pr.exc = PsycoTuple_GET_ITEM(v, 0);
          PsycoTuple_GET_ITEM(v, 0) = NULL;
          po->pr.val = PsycoTuple_GET_ITEM(v, 1);
          PsycoTuple_GET_ITEM(v, 1) = NULL;
          po->pr.tb = PsycoTuple_GET_ITEM(v, 2);
          PsycoTuple_GET_ITEM(v, 2) = NULL;
          vinfo_decref(v, po);
          break;
        } else {
          /* end of an EXCEPT block, re-raise the exception
             stored in the stack. As in Python, no extra
             traceback is recorded, but instead of the
             WHY_RERAISE trick we simply record no traceback
             if the opcode is END_FINALLY. */
          po->pr.exc = v;
          POP(po->pr.val);
          POP(po->pr.tb);
          break;
        }

      case UNPACK_SEQUENCE: {
        int i;
        void *cimpl_unpack;
        PyTypeObject *vtp;

        v = TOP();
        vtp = Psyco_NeedType(po, v);
        if (vtp == NULL)
          break;

        if (PyType_TypeCheck(vtp, &PyTuple_Type)) {
          /* shortcut: is this a virtual tuple?
                       of the correct length? */
          if (PsycoTuple_Load(v) != oparg) {

            /* No, fall back to the default path:
               load the size, compare it with oparg,
               and if they match proceed by loading the
               tuple item by item into the stack. */
            vinfo_t *vsize;
            vsize = psyco_get_const(po, v, FIX_size);
            if (vsize == NULL)
              break;

            /* check the size */
            cc = integer_cmp_i(po, vsize, oparg, Py_NE);
            if (cc == CC_ERROR)
              break;
            if (runtime_condition_f(po, cc)) {
              PycException_SetString(po, PyExc_ValueError,
                                     "unpack tuple of wrong size");
              break;
            }

            /* make sure the tuple data is loaded */
            for (i = oparg; i--;) {
              w = psyco_get_nth_const(po, v, TUPLE_ob_item, i);
              if (w == NULL)
                break;
            }
            if (i >= 0)
              break;
          }
          /* copy the tuple items into the stack */
          POP(v);
          for (i = oparg; i--;) {
            w = PsycoTuple_GET_ITEM(v, i);
            vinfo_incref(w);
            PUSH(w);
            /* in case the tuple is freed while its items
               are still in use: */
            need_reference(po, w);
          }
          vinfo_decref(v, po);
          goto fine;
        }

        if (PyType_TypeCheck(vtp, &PyList_Type))
          cimpl_unpack = cimpl_unpack_list;
        else
          cimpl_unpack = cimpl_unpack_iterable;

        {
          vinfo_array_t *array = array_new(oparg);
          if (!psyco_generic_call(po, cimpl_unpack, CfCommonIntZeroOk, "vlA", v,
                                  oparg, array)) {
            array_release(array);
            break;
          }
          POP_DECREF();
          for (i = oparg; i--;)
            PUSH(array->items[i]);
          array_release(array);
          goto fine;
        }
      }

      case STORE_ATTR: {
        bool ok;
        w = vinfo_new(CompileTime_New(((long)GETNAMEV(oparg))));
        ok = PsycoObject_SetAttr(po, TOP(), w, NTOP(2)); /* v.w = u */
        vinfo_decref(w, po);
        if (!ok)
          break;
        POP_DECREF();
        POP_DECREF();
        goto fine;
      }

      case DELETE_ATTR: {
        bool ok;
        w = vinfo_new(CompileTime_New(((long)GETNAMEV(oparg))));
        ok = PsycoObject_SetAttr(po, TOP(), w, NULL); /* del v.w */
        vinfo_decref(w, po);
        if (!ok)
          break;
        POP_DECREF();
        goto fine;
      }

      case STORE_NAME:
        /* only for modules: we can only reach this point
           for code objects compiled from frames where
           f_locals == f_globals */
        /* fall through */
      case STORE_GLOBAL: {
        PyObject *w = GETNAMEV(oparg);
        mark_varying(po, w);
        if (!psyco_generic_call(po, PyDict_SetItem, CfCommonIntZeroOk, "vlv",
                                LOC_GLOBALS, w, TOP()))
          break;
        POP_DECREF();
        goto fine;
      }

      case DELETE_NAME:
        /* only for modules: we can only reach this point
           for code objects compiled from frames where
           f_locals == f_globals */
        /* fall through */
      case DELETE_GLOBAL: {
        PyObject *w = GETNAMEV(oparg);
        mark_varying(po, w);
        if (runtime_NON_NULL_f(po, psyco_generic_call(po, PyDict_DelItem,
                                                      CfReturnTypeInt, "vl",
                                                      LOC_GLOBALS, w))) {
          PycException_SetFormat(po, PyExc_NameError, GLOBAL_NAME_ERROR_MSG,
                                 NiCompatStr_AsString(w));
          break;
        }
        goto fine;
      }

      case LOAD_CONST:
        /* reference borrowed from the code object */
        v = vinfo_new(CompileTime_New((long)GETCONST(oparg)));
        PUSH(v);
        goto fine;

      case LOAD_NAME:
        /* only for modules: we can only reach this point
           for code objects compiled from frames where
           f_locals == f_globals */
        /* fall through */
      case LOAD_GLOBAL: {
        PyObject *namev = GETNAMEV(oparg);
        /* XXX tony: Add back a fast implementation of this for the case when
          globals is known at compile-time.

          This could use PyDictObject's ma_version_tag to test that the
          globals dict hasn't changed. The found value would be cached
          in the compiled code.

          It would need to respaswn if it changes to recompile with a different
          value, and it would need to deoptimise if it changed a lot.

          To be correct there would also need to be a way to detect
          shadowing of global variables, I think? */

        /* else { Globals dict is unknown at compile-time } */
        v = psyco_generic_call(po, cimpl_load_global, CfCommonNewRefPyObject,
                               "vl", LOC_GLOBALS, namev);
        if (v == NULL)
          break;
        PUSH(v);
        goto fine;
      }

      case LOAD_FAST:
        x = GETLOCAL(oparg);
        /* a local variable can only be unbound if its
           value is known to be (PyObject*)NULL. A run-time
           or virtual value is always non-NULL. */
        if (psyco_knowntobe(x, 0)) {
          PyObject *namev;
          namev = PyTuple_GetItem(co->co_varnames, oparg);
          PycException_SetFormat(po, PyExc_UnboundLocalError,
                                 UNBOUNDLOCAL_ERROR_MSG,
                                 NiCompatStr_AsString(namev));
          break;
        }
        vinfo_incref(x);
        PUSH(x);
        goto fine;

      case STORE_FAST:
        POP(v);
        SETLOCAL(oparg, v);
        goto fine;

      case DELETE_FAST:
        x = GETLOCAL(oparg);
        /* a local variable can only be unbound if its
           value is known to be (PyObject*)NULL. A run-time
           or virtual value is always non-NULL. */
        if (psyco_knowntobe(x, 0)) {
          PyObject *namev;
          namev = PyTuple_GetItem(co->co_varnames, oparg);
          PycException_SetFormat(po, PyExc_UnboundLocalError,
                                 UNBOUNDLOCAL_ERROR_MSG,
                                 NiCompatStr_AsString(namev));
          break;
        }

        u = psyco_vi_Zero();
        SETLOCAL(oparg, u);
        goto fine;

        /*MISSING_OPCODE(LOAD_CLOSURE);
          MISSING_OPCODE(LOAD_DEREF);
          MISSING_OPCODE(STORE_DEREF);*/

      case BUILD_TUPLE:
        v = PsycoTuple_New(oparg, STACK_POINTER() - oparg);
        while (oparg--)
          POP_DECREF();
        PUSH(v);
        goto fine;

      case BUILD_LIST:
        v = PsycoList_New(po, oparg, STACK_POINTER() - oparg);
        if (v == NULL)
          break;
        while (oparg--)
          POP_DECREF();
        PUSH(v);
        goto fine;

      case BUILD_MAP: {
        /* 3.5+: oparg key/value pairs on the stack, first pair deepest */
        int i;
        v = PsycoDict_NewPresized(po, oparg);
        if (v == NULL)
          break;
        for (i = oparg; i > 0; i--)
          if (!PsycoDict_SetItem(po, v, NTOP(2 * i), NTOP(2 * i - 1)))
            break;
        if (i > 0) {
          vinfo_decref(v, po);
          break;
        }
        for (i = 2 * oparg; i--;)
          POP_DECREF();
        PUSH(v);
        goto fine;
      }

      case BUILD_CONST_KEY_MAP: {
        /* oparg values, then a constant tuple of keys on top */
        int i;
        PyObject *keys;
        if (!is_compiletime(TOP()->source) ||
            !PyTuple_CheckExact(
                keys = (PyObject *)CompileTime_Get(TOP()->source)->value) ||
            PyTuple_GET_SIZE(keys) != oparg) {
          PycException_SetString(po, PyExc_SystemError,
                                 "bad BUILD_CONST_KEY_MAP keys argument");
          break;
        }
        v = PsycoDict_NewPresized(po, oparg);
        if (v == NULL)
          break;
        for (i = oparg; i > 0; i--)
          if (!PsycoDict_SetItemC(po, v, PyTuple_GET_ITEM(keys, oparg - i),
                                  NTOP(i + 1)))
            break;
        if (i > 0) {
          vinfo_decref(v, po);
          break;
        }
        for (i = oparg + 1; i--;)
          POP_DECREF();
        PUSH(v);
        goto fine;
      }

#ifdef STORE_MAP
      case STORE_MAP:
        w = NTOP(1);                         /* key */
        u = NTOP(2);                         /* value */
        v = NTOP(3);                         /* dict */
        if (!PsycoDict_SetItem(po, v, w, u)) /* v[w] = u */
          break;
        POP_DECREF();
        POP_DECREF();
        goto fine;
#endif

#ifdef MAP_ADD
      case MAP_ADD:
        w = NTOP(1);                         /* key */
        u = NTOP(2);                         /* value */
        v = NTOP(2 + oparg);                 /* dict */
        if (!PsycoDict_SetItem(po, v, w, u)) /* v[w] = u */
          break;
        POP_DECREF();
        POP_DECREF();
        goto fine;
#endif

        /*MISSING_OPCODE(BUILD_SET);
          MISSING_OPCODE(SET_ADD);*/

      case LOAD_ATTR:
        w = vinfo_new(CompileTime_New(((long)GETNAMEV(oparg))));
        x = PsycoObject_GetAttr(po, TOP(), w); /* v.w */
        vinfo_decref(w, po);
        if (x == NULL)
          break;
        POP_DECREF();
        PUSH(x);
        goto fine;

      case LOAD_METHOD:
        /* ceval pushes either (method, self) or (NULL, attribute). Ni always
           does the latter, with a compile-time NULL; the attribute is
           usually a virtual bound method, so nothing is lost. */
        w = vinfo_new(CompileTime_New(((long)GETNAMEV(oparg))));
        x = PsycoObject_GetAttr(po, TOP(), w);
        vinfo_decref(w, po);
        if (x == NULL)
          break;
        POP_DECREF();
        PUSH(psyco_vi_Zero());
        PUSH(x);
        goto fine;

      case COMPARE_OP: {
        condition_code_t cc = CC_ERROR;
        w = TOP();
        v = NTOP(2);
        switch (oparg) {

        case PyCmp_IS: /* pointer comparison */
          cc = integer_cmp(po, v, w, Py_EQ);
          break;

        case PyCmp_IS_NOT: /* pointer comparison */
          cc = integer_cmp(po, v, w, Py_NE);
          break;

        case PyCmp_IN:
        case PyCmp_NOT_IN:
          x = PsycoSequence_Contains(po, w, v);
          cc = integer_NON_NULL(po, x);
          if (oparg == PyCmp_NOT_IN && cc != CC_ERROR)
            cc = INVERT_CC(cc);
          break;

        case PyCmp_EXC_MATCH:
          x = psyco_generic_call(po, PyErr_GivenExceptionMatches,
                                 CfPure | CfReturnTypeInt, "vv", v, w);
          cc = integer_NON_NULL(po, x);
          break;

        default:
          x = PsycoObject_RichCompare(po, v, w, oparg);
          goto compare_done;
        }

        if (cc == CC_ERROR)
          break;

        /* turns 'cc' into a virtual Python boolean object */
        x = PsycoBool_FromCondition(po, cc);

      compare_done:
        if (x == NULL)
          break;
        POP_DECREF();
        POP_DECREF();
        PUSH(x); /* consumes ref on x */
        goto fine;
      }

      case IMPORT_NAME: {
        PyObject *w = GETNAMEV(oparg);
#if !VERYCONVOLUTED_IMPORT_NAME /* Python < 2.5 */
        x = psyco_generic_call(po, cimpl_import_name, CfCommonNewRefPyObject,
                               "vlv", LOC_GLOBALS, w, TOP());
        if (x == NULL)
          break;
        POP_DECREF();
#else
        v = TOP();
        u = NTOP(2);
        x = psyco_generic_call(po, cimpl_import_name, CfCommonNewRefPyObject,
                               "vlvv", LOC_GLOBALS, w, v, u);
        if (x == NULL)
          break;
        POP_DECREF();
        POP_DECREF();
#endif
        PUSH(x);
        goto fine;
      }

      case IMPORT_STAR: {
        /* only for modules: we can only reach this point
           for code objects compiled from frames where
           f_locals == f_globals */
        x = psyco_generic_call(po, cimpl_import_all_from,
                               CfReturnTypeInt | CfPyErrIfNonNull, "vv",
                               LOC_GLOBALS, TOP());
        if (x == NULL)
          break;
        vinfo_decref(x, po);
        POP_DECREF();
        goto fine;
      }

      case IMPORT_FROM: {
        PyObject *name = GETNAMEV(oparg);
        w = vinfo_new(CompileTime_New(((long)name)));
        v = TOP();
        x = PsycoObject_GetAttr(po, v, w);
        vinfo_decref(w, po);
        if (x == NULL) {
          extra_assert(PycException_Occurred(po));

          /* catch PyExc_AttributeError */
          v = PycException_Matches(po, PyExc_AttributeError);
          if (runtime_NON_NULL_t(po, v) == true) {
            PycException_SetFormat(po, PyExc_ImportError,
                                   "cannot import name %.230s",
                                   NiCompatStr_AsString(name));
          }
          break;
        }
        PUSH(x);
        goto fine;
      }

      case JUMP_FORWARD:
        JUMPBY(oparg);
        mp = psyco_next_merge_point(po->pr.merge_points, next_instr);
        goto fine;

#if HAVE_NEXT_INSTR_POP /* Python >= 2.7 */
      case JUMP_IF_TRUE_OR_POP:
      case JUMP_IF_FALSE_OR_POP:
      case POP_JUMP_IF_FALSE:
      case POP_JUMP_IF_TRUE:
#else
      case JUMP_IF_TRUE:
      case JUMP_IF_FALSE:
#endif
        /* This code is very different from the original
           interpreter's, because we generally do not know the
           outcome of PyObject_IsTrue(). In the case of JUMP_IF_xxx
           we must be prepared to have to compile the two possible
           paths. */
        cc = integer_NON_NULL(po, PsycoObject_IsTrue(po, TOP()));
        if (cc == CC_ERROR)
          break;
#if HAVE_NEXT_INSTR_POP
        if (opcode == JUMP_IF_FALSE_OR_POP || opcode == POP_JUMP_IF_FALSE)
#else
        if (opcode == JUMP_IF_FALSE)
#endif
          cc = INVERT_CC(cc);
        if ((int)cc < CC_TOTAL) {
          /* compile the beginning of the "if true" path */
          int current_instr = next_instr;
#if HAVE_NEXT_INSTR_POP
          JUMPTO(oparg);
          /* this is the case where we jump */
          if (opcode == POP_JUMP_IF_FALSE || opcode == POP_JUMP_IF_TRUE)
            next_instr |= NEXT_INSTR_POP;
#else
          JUMPBY(oparg);
#endif
          SAVE_NEXT_INSTR(next_instr);
          psyco_compile_cond(
              po, psyco_exact_merge_point(po->pr.merge_points, next_instr), cc);
          next_instr = current_instr;
        } else if (cc == CC_ALWAYS_TRUE) {
#if HAVE_NEXT_INSTR_POP
          JUMPTO(oparg); /* always jump */
          /* this is the case where we jump */
          if (opcode == POP_JUMP_IF_FALSE || opcode == POP_JUMP_IF_TRUE)
            POP_DECREF();
#else
          JUMPBY(oparg); /* always jump */
#endif
          mp = psyco_next_merge_point(po->pr.merge_points, next_instr);
          goto fine;
        } else
          ; /* never jump */
#if HAVE_NEXT_INSTR_POP
        /* this is the case where we don't jump */
        POP_DECREF();
#endif
        goto fine;

      case JUMP_ABSOLUTE:
        JUMPTO(oparg);
        mp = psyco_next_merge_point(po->pr.merge_points, next_instr);
        goto fine;

      case GET_ITER:
        x = PsycoObject_GetIter(po, TOP());
        if (x == NULL)
          break;
        POP_DECREF();
        PUSH(x);
        goto fine;

      case FOR_ITER:
        v = PsycoIter_Next(po, TOP());
        if (v != NULL) {
          /* iterator not exhausted */
          PUSH(v);
        } else {
          extra_assert(PycException_Occurred(po));

          /* catch PyExc_StopIteration */
          v = PycException_Matches(po, PyExc_StopIteration);
          if (runtime_NON_NULL_t(po, v) == true) {
            /* iterator ended normally */
            PycException_Clear(po);
            POP_DECREF();
            JUMPBY(oparg);
            mp = psyco_next_merge_point(po->pr.merge_points, next_instr);
          } else
            break; /* any other exception, or error in
                      runtime_NON_NULL_t() */
        }
        goto fine;

      case SETUP_LOOP:
      case SETUP_EXCEPT:
      case SETUP_FINALLY:
        block_setup(po, opcode, INSTR_OFFSET() + oparg, STACK_LEVEL());
        goto fine;

        /*MISSING_OPCODE(SETUP_WITH);*/

      case CALL_METHOD: {
        /* always the (NULL, attribute) form, see LOAD_METHOD */
        int i, stack_to_pop;
        extra_assert(psyco_knowntobe(NTOP(oparg + 2), 0));
        x = psyco_ext_do_calls(po, CALL_FUNCTION, oparg, STACK_POINTER(),
                               &stack_to_pop);
        if (x == NULL)
          break;
        for (i = stack_to_pop + 1; i--;) /* args, callable and the NULL */
          POP_DECREF();
        PUSH(x);
        goto fine;
      }

      case CALL_FUNCTION:
      case CALL_FUNCTION_KW:
      case CALL_FUNCTION_EX: {
        int i, stack_to_pop;
        if (opcode == CALL_FUNCTION_EX)
          x = psyco_ext_do_call_ex(po, oparg, STACK_POINTER(), &stack_to_pop);
        else
          x = psyco_ext_do_calls(po, opcode, oparg, STACK_POINTER(),
                                 &stack_to_pop);
        if (x == NULL)
          break;

        /* clean up the stack (remove args and func) */
        for (i = stack_to_pop; i--;)
          POP_DECREF();
        PUSH(x);
        goto fine;
      }

        /*MISSING_OPCODE(MAKE_CLOSURE);*/

      case MAKE_FUNCTION:
        x = PsycoFunction_NewWithQualName(po, NTOP(2), LOC_GLOBALS, NTOP(1));
        POP_DECREF();
        POP_DECREF();
        if (x == NULL)
          break;
        if (oparg & 0x08) {
          POP(x->array->items[iFUNC_CLOSURE]);
          if(!psyco_forking(po, x->array->items[iFUNC_CLOSURE]->array)) 
            break;
        }
        if (oparg & 0x04) {
          POP(x->array->items[iFUNC_ANNOTATIONS]);
          if(!psyco_forking(po, x->array->items[iFUNC_ANNOTATIONS]->array)) 
            break;
        }
        if (oparg & 0x02) {
          POP(x->array->items[iFUNC_KWDEFAULTS]);
          if(!psyco_forking(po, x->array->items[iFUNC_KWDEFAULTS]->array)) 
            break;
        }
        if (oparg & 0x01) {
          POP(x->array->items[iFUNC_DEFAULTS]);
          if(!psyco_forking(po, x->array->items[iFUNC_DEFAULTS]->array)) 
            break;
        }

        PUSH(x);
        goto fine;

      case BUILD_SLICE:
        if (oparg == 3) {
          w = NTOP(1);
          v = NTOP(2);
          u = NTOP(3);
        } else {
          w = psyco_vi_Zero();
          v = NTOP(1);
          u = NTOP(2);
        }
        x = psyco_generic_call(po, PySlice_New, CfCommonNewRefPyObject, "vvv",
                               u, v, w);
        if (oparg != 3)
          vinfo_decref(w, po);
        if (x == NULL)
          break;
        Psyco_AssertType(po, x, &PySlice_Type);
        if (oparg == 3)
          POP_DECREF(); /* w */
        POP_DECREF();   /* v */
        POP_DECREF();   /* u */
        PUSH(x);
        goto fine;

      case EXTENDED_ARG:
        opcode = bytecode[next_instr++];
        oparg = oparg << 8 | bytecode[next_instr++];;
        goto dispatch_opcode;

      default:
        fprintf(stderr, "XXX opcode: %d\n", opcode);
        Py_FatalError("unknown opcode");

      } /* switch (opcode) */

      /* Exit if an error occurred */
      if (PycException_Occurred(po))
        break;

    fine:
      extra_assert(!PycException_Occurred(po));
      extra_assert(mp ==
                   psyco_next_merge_point(po->pr.merge_points, next_instr));

      /* are we running out of space in the current code buffer? */
      if ((po->codelimit - po->code) < BUFFER_MARGIN) {
        if (is_respawning(po)) {
          /* when respawning, just forget everything we
             wrote so far and come back to the beginning
             again */
          PsycoObject_EmergencyCodeRoom(po);
        } else {
          /* normal case: save the current position, and
             stop compilation. When this point is reached
             at run-time, compilation will go on in a
             new buffer. */
          SAVE_NEXT_INSTR(next_instr);
          if (mp->bytecode_position != next_instr)
            mp = NULL;
          code1 = psyco_compile(po, mp, false);
          goto finished;
        }
      }

      /* mark merge points via a call to psyco_compile() */
      if (mp->bytecode_position == next_instr) {
        extra_assert(!is_respawning(po));
        SAVE_NEXT_INSTR(next_instr);
        /* simplify the po->vlocals array */
        if (!psyco_limit_nested_weight(po, &po->vlocals, NWI_NORMAL,
                                       NESTED_WEIGHT_END))
          break;
        psyco_delete_unused_vars(po, &mp->entries);
        code1 = psyco_compile(po, mp, true);
        if (code1 != NULL)
          goto finished;
        mp++;
      }
    } /* end of the main loop, exit if pseudo-exception */

    psyco_assert_coherent(po);

    /* check for the 'promotion' pseudo-exception.
       This is the only case in which the instruction
       causing the exception is restarted. */
    if (is_virtualtime(po->pr.exc->source) &&
        psyco_vsource_is_promotion(po->pr.exc->source)) {
      c_promotion_t *promotion =
          (c_promotion_t *)VirtualTime_Get(po->pr.exc->source);
      vinfo_t *promote_me = po->pr.val;
      /* NOTE: we assume that 'promote_me' is a member of the
         'po->vlocals' arrays, so that we can safely DECREF it
         now without actually releasing it. If this assumption
         is false, the psyco_finish_xxx() calls below will give
         unexpected results. */
      assert_array_contains_nonct(&po->vlocals, promote_me);
      clear_pseudo_exception(po);
      code1 = psyco_finish_promotion(po, promote_me, promotion->pflags);
      goto finished;
    }

    /* check for the 'inline' pseudo-exception. */
    if (PycException_Is(po, &EInline)) {
      vinfo_t *v;

      /* po->pr.val->array contains the new sub-frame that we
         should now start to compile, inlined into the parent frame */
      extra_assert(!po->pr.is_inlining);
      /* store po->vlocals away */
      v = psyco_save_inline_po(po);
      /* copy po->pr.val->array to po->vlocals */
      po = psyco_restore_inline_po(po, &po->pr.val->array);
      stack_a = po->vlocals.items + po->pr.stack_base;
      /* save the copy of the previous po->vlocals to LOC_INLINING */
      extra_assert(LOC_INLINING == NULL);
      LOC_INLINING = v;
      po->pr.is_inlining = true;
      /* mark this point as an inline frame entry */
      psyco_inline_enter(po);
      /* go on with compilation */
      clear_pseudo_exception(po);
      continue;
    }

    /* At this point, we got a real pseudo-exception. */

    if (PycException_IsPython(po) && opcode != END_FINALLY) {
      /* log traceback info for real Python exceptions,
         unless re-raised by END_FINALLY */
      PsycoTraceBack_Here(po, po->pr.next_instr);
    }

    /* All exceptions raised inside an inlined frame are passed to the
       parent (neither exception handlers nor loops are allowed in
       inlinable code objects). */
    if (po->pr.is_inlining) {
      /* unwind the inlined frame by restoring the parent one */
      vinfo_t *v = LOC_INLINING;
      LOC_INLINING = NULL;
      po = psyco_restore_inline_po(po, &v->array);
      stack_a = po->vlocals.items + po->pr.stack_base;
      vinfo_decref(v, po);
      po->pr.is_inlining = false;
      /* mark this point as an inline frame exit */
      psyco_inline_exit(po);

      if (PycException_Is(po, &EReturn)) {
        /* caught EReturn from the inlined frame */

        /* 'po' is now in the parent frame, pointing to the instruction
           (typically CALL_FUNCTION) which triggered the inline
           compilation in the first place.  We restart this instruction
           entierely.  With almost no code emitted we should reach the
           psyco_call_pyfunc() again, which will at that point return
           the expected result.  To this end we save the result in
           LOC_INLINING.  It might be exceptionally possible that the
           restarting CALL_FUNCTION does not reach psyco_call_pyfunc()
           again in some cases; this hopefully corresponds to cases
           that can never occur at run-time.  The waste of code is
           minimal. */
        LOC_INLINING = po->pr.val;
        po->pr.val = NULL;
        clear_pseudo_exception(po);
        need_reference(po, LOC_INLINING); /* see test5.makeSelection() */
        continue;
      }

      /* stack unwind goes on in the parent frame */
      if (PycException_IsPython(po) && compute_and_raise_exception(po)) {
        /* force the exception to be recorded on inner frame
           before we can record its traceback on the parent frame */
        clear_pseudo_exception(po);
        PycException_Raise(po, vinfo_new(VirtualTime_New(&ERtPython)), NULL);
        PsycoTraceBack_Here(po, po->pr.next_instr);
      }
    }

    /* Unwind the Python stack until we find a handler for
       the (pseudo) exception. You will recognize here
       ceval.c's stack unwinding code. */

    while (po->pr.iblock > 0) {
      PyTryBlock *b = block_pop(po);

      if (b->b_type == SETUP_LOOP && PycException_Is(po, &EContinue)) {
        /* For a continue inside a try block,
           don't pop the block for the loop. */
        int next_instr;
        block_setup(po, b->b_type, b->b_handler, b->b_level);
        JUMPTO(CompileTime_Get(po->pr.val->source)->value);
        clear_pseudo_exception(po);
        SAVE_NEXT_INSTR(next_instr);
        break;
      }

      if (b->b_type == EXCEPT_HANDLER) {
        /* leaving an except clause by any (pseudo) exception */
        unwind_except_handler(po, b);
        continue;
      }

      /* clear the stack up to b->b_level */
      while (STACK_LEVEL() > b->b_level) {
        POP_DECREF(); /* no NULLs here */
      }
      if (b->b_type == SETUP_LOOP && PycException_Is(po, &EBreak)) {
        int next_instr;
        clear_pseudo_exception(po);
        JUMPTO(b->b_handler);
        SAVE_NEXT_INSTR(next_instr);
        break;
      }
      if (b->b_type == SETUP_FINALLY) {
        /* SETUP_FINALLY always pushes on the stack either a
           single compile-time None object or three objects
           (exc, value, traceback). Unlike Python, the latter
           might represent a pseudo-exception like EReturn. */
        int next_instr;
        vinfo_t *exc_info = PsycoTuple_New(3, NULL);
        PycException_Fetch(po);
        PsycoTuple_SET_ITEM(exc_info, 0, po->pr.exc);
        PsycoTuple_SET_ITEM(exc_info, 1, po->pr.val);
        PsycoTuple_SET_ITEM(exc_info, 2, po->pr.tb);
        po->pr.exc = NULL;
        po->pr.val = NULL;
        po->pr.tb = NULL;
        PUSH(exc_info);
        JUMPTO(b->b_handler);
        SAVE_NEXT_INSTR(next_instr);
        break;
      }
      if (b->b_type == SETUP_EXCEPT && PycException_IsPython(po)) {
        /* as 3.7 ceval: an EXCEPT_HANDLER block over six stack items (see
           cimpl_push_exc_info). A virtual exception is raised for real
           first, so that the run-time code can fetch it. */
        int next_instr, i, handler = b->b_handler;
        vinfo_array_t *array;
        if (!compute_and_raise_exception(po))
          break;
        clear_pseudo_exception(po);
        block_setup(po, EXCEPT_HANDLER, -1, STACK_LEVEL()); /* reuses *b */
        array = array_new(6);
        if (psyco_generic_call(po, cimpl_push_exc_info, CfNoReturnValue, "A",
                               array) == NULL) {
          array_release(array);
          break;
        }
        for (i = 0; i < 6; i++)
          PUSH(array->items[i]);
        array_release(array);
        JUMPTO(handler);
        SAVE_NEXT_INSTR(next_instr);
        break;
      }
    } /* end of unwind stack */

    /* End the function if we still have a (pseudo) exception */
    if (PycException_Occurred(po)) {
      break;
    }
  }

  /* function return (either by RETURN or by exception). */
  while ((code1 = exit_function(po)) == NULL) {
    /* If LOC_EXCEPTION is still set after exit_function(), it
       means an exception was raised while handling the function
       return... In this case, we do a function return again,
       this time with the newly raised exception. XXX make sure
       this loop cannot be endless */
  }

finished:
#if ALL_CHECKS
  if (PyErr_Occurred()) {
    fprintf(stderr, "psyco: unexpected Python exception during compilation:\n");
    PyErr_WriteUnraisable(Py_None);
  }
#endif

  Py_SetRecursionLimit(saved_rec_limit);

  PyErr_Restore(old_py_exc, old_py_val, old_py_tb);
  return code1;
}
