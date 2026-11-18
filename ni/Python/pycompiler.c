/*
 * The bytecode compiler: a compile-time copy of ceval.c's main loop. Each
 * opcode case works on a stack of vinfo_t and calls the compile-time object
 * protocol (Objects/p*.c). Two things differ from ceval:
 *  - Restart. If an opcode needs a run-time value as a constant it raises the
 *    promotion pseudo-exception: compilation stops, a run-time switch is
 *    emitted, and the *same opcode is re-run from the start* once the value is
 *    known. So an opcode must do nothing irreversible before the last point
 *    where it can fail or promote (use TOP(), and POP() only at the end).
 *  - LOAD_GLOBAL is a constant behind a dict-version guard whose failure
 *    respawns (load_global_cached, D25); the block is split after each such
 *    site so that a respawn replay never re-decides one.
 *  - The simple opcodes (stack shuffles, LOAD_CONST/LOAD_FAST/STORE_FAST,
 *    unary, binary and in-place arithmetic, STORE_SUBSCR, GET_ITER,
 *    LOAD_ATTR/STORE_ATTR/DELETE_ATTR, BUILD_TUPLE/BUILD_LIST/BUILD_SLICE,
 *    LIST_APPEND, MAP_ADD) are generated into pycompiler_gen.h
 *    from CPython's ceval.c by tools/metac/gen_ceval.py (D26, D67).
 *  - After the switch come the merge-point checks, then the handling of
 *    pseudo-exceptions (promotion, EInline, EReturn/EBreak/EContinue, Python
 *    exceptions) with ceval-style unwinding of the block stack.
 * The block opcodes (SETUP_LOOP/EXCEPT/FINALLY) exist only up to 3.7. Some
 * cases keep Python 2 meanings (CALL_FUNCTION's oparg, read as na | nk << 8,
 * which is right only below 256 arguments;
 * doc/history/removed-mechanisms.md D2).
 * Exceptions: catching one follows 3.7 (an EXCEPT_HANDLER block over six
 * stack items, cimpl_push_exc_info), and a finally block entered by an
 * exception gets the same. A finally block entered by a pseudo-exception
 * (EReturn, EBreak, EContinue) differs from ceval: it gets a single 3-tuple,
 * which END_FINALLY tells apart from an exception type. The Py2 per-frame saved exception
 * (FINALIZE_FRAME_LOCALS, cimpl_finalize_frame_locals) is now never set.
 */
#include "pycompiler.h"
#include "../codemanager.h"
#include "../dispatcher.h"
#include "../mergepoints.h"
#include "../processor.h"
#include "../psyfunc.h"
#include "frames.h"
/* ni_frame_current, ni_frame_set_current (D75) */
#include "mechanism_frames_gen.h"
#include "../pycodegen.h"

#include "../Objects/pabstract.h"
#include "../Objects/pdictobject.h"
#include "../Objects/pfuncobject.h"
#include "../Objects/plistobject.h"
#include "../Objects/plongobject.h"
#include "../Objects/pboolobject.h"
#include "../Objects/ptupleobject.h"
#include "../Objects/psliceobject.h"

#include "../compat2to3.h"

#include "pycinternal.h"
#include <eval.h>
#include <ipyencoding.h>

#if NI_CODESTATS
#include <dlfcn.h>
#include <execinfo.h>
/* Promotion sites (pycompiler.h). Each site is a list
   [code, instr, opcode, pflags, stub bytes, (backtrace offsets in ni.so),
    entry kind, [values], parent, parent's value] and each value
   [description, ns, bytes, counter]; the parent is the site whose value was
   being compiled when this site was emitted, or -1, and the parent's value
   the index of that value in its [values], or -1,
   the counter being a long the promoted block increments each time it runs.
   Never reset: a site's id indexes this list for as long as its code lives. */
#define CS_PROMO_BT 8
static void *cs_promo_bt[CS_PROMO_BT];
static int cs_promo_nbt = 0;
static PyObject *cs_promo_sites = NULL;
static long *cs_promo_misses = NULL;
static PyObject *cs_promo_pending = NULL; /* the value being compiled */
static long cs_promo_t0, cs_promo_b0;
static long *cs_promo_last_counter = NULL;
DEFINEVAR int psyco_cs_promo_cur = -1;
static int cs_promo_vidx = -1; /* index of the value being compiled in its
                                  site's [values], for the copies */

DEFINEFN int psyco_cs_promo_site(PsycoObject *po, int pflags,
                                 long stub_bytes) {
  PyObject *bt, *rec;
  Py_ssize_t n;
  int i;
  Dl_info info;
  if (cs_promo_sites == NULL && (cs_promo_sites = PyList_New(0)) == NULL)
    OUT_OF_MEMORY();
  bt = PyTuple_New(cs_promo_nbt);
  for (i = 0; i < cs_promo_nbt; i++) {
    long off = -1;
    if (dladdr(cs_promo_bt[i], &info) && info.dli_fbase != NULL)
      off = (char *)cs_promo_bt[i] - (char *)info.dli_fbase;
    PyTuple_SET_ITEM(bt, i, PyLong_FromLong(off));
  }
  rec = Py_BuildValue("[OiiilNi[]ii]", po->pr.co, po->pr.next_instr,
                      (unsigned char)PyBytes_AS_STRING(
                          po->pr.co->co_code)[po->pr.next_instr],
                      pflags, stub_bytes, bt, psyco_stat_compile_kind,
                      psyco_cs_promo_cur, cs_promo_vidx);
  if (rec == NULL || PyList_Append(cs_promo_sites, rec) < 0)
    OUT_OF_MEMORY();
  Py_DECREF(rec);
  n = PyList_GET_SIZE(cs_promo_sites);
  cs_promo_misses = PyMem_Realloc(cs_promo_misses, n * sizeof(long));
  cs_promo_misses[n - 1] = 0;
  cs_promo_nbt = 0;
  return (int)(n - 1);
}

/* kind: 1 a PyObject*, 0 a raw word, -1 the megamorphic version */
DEFINEFN void psyco_cs_promo_begin(int site, long key, int kind) {
  PyObject *et, *ev, *tb, *d;
  PyErr_Fetch(&et, &ev, &tb);
  if (kind < 0)
    d = Py_BuildValue("(ss)", "MEGA", "");
  else if (kind == 0)
    d = Py_BuildValue("(sl)", "word", key);
  else {
    PyObject *o = (PyObject *)key;
    const char *detail = "";
    char buf[64];
    if (PyType_Check(o))
      detail = ((PyTypeObject *)o)->tp_name;
    else if (PyFunction_Check(o)) {
      PyObject *q = ((PyFunctionObject *)o)->func_qualname;
      detail = q && PyUnicode_Check(q) ? PyUnicode_AsUTF8(q) : "?";
    } else if (PyCode_Check(o))
      detail = PyUnicode_AsUTF8(((PyCodeObject *)o)->co_name);
    else if (PyCFunction_Check(o))
      detail = ((PyCFunctionObject *)o)->m_ml->ml_name;
    else if (PyBool_Check(o))
      detail = o == Py_True ? "True" : "False";
    else if (PyLong_CheckExact(o)) {
      snprintf(buf, sizeof(buf), "%ld", PyLong_AsLong(o));
      detail = buf;
    } else if (PyUnicode_CheckExact(o) && PyUnicode_GET_LENGTH(o) < 30)
      detail = PyUnicode_AsUTF8(o);
    else if (PyModule_Check(o))
      detail = PyModule_GetName(o);
    if (detail == NULL)
      detail = "?";
    d = Py_BuildValue("(ss)", Py_TYPE(o)->tp_name, detail);
  }
  PyErr_Clear();
  PyErr_Restore(et, ev, tb);
  Py_XDECREF(cs_promo_pending);
  cs_promo_pending = Py_BuildValue("[Nll]", d, (long)site, 0L);
  cs_promo_last_counter = NULL;
  cs_promo_t0 = psyco_stat_now_ns();
  cs_promo_b0 = psyco_stat_code_bytes;
  psyco_cs_promo_cur = site;
  cs_promo_vidx =
      (int)PyList_GET_SIZE(PyList_GET_ITEM(PyList_GET_ITEM(cs_promo_sites, site), 7));
}

DEFINEFN void psyco_cs_promo_end(void) {
  long site;
  PyObject *v = cs_promo_pending, *rec;
  psyco_cs_promo_cur = -1;
  cs_promo_vidx = -1;
  if (v == NULL)
    return;
  cs_promo_pending = NULL;
  site = PyLong_AsLong(PyList_GET_ITEM(v, 1));
  PyList_SetItem(v, 1, PyLong_FromLong(psyco_stat_now_ns() - cs_promo_t0));
  PyList_SetItem(v, 2, PyLong_FromLong(psyco_stat_code_bytes - cs_promo_b0));
  PyList_Append(v, PyLong_FromVoidPtr(cs_promo_last_counter));
  Py_DECREF(PyList_GET_ITEM(v, 3)); /* PyList_Append took its own */
  rec = PyList_GET_ITEM(cs_promo_sites, site);
  PyList_Append(PyList_GET_ITEM(rec, 7), v);
  Py_DECREF(v);
}

/* 'mov r11, &counter; inc qword [r11]' (13 bytes). Flags are dead here:
   ipromotion_finish spilled them before its compare, and R11 is never
   allocated. */
DEFINEFN code_t *psyco_cs_promo_counter(code_t *code) {
  long *c = PyMem_Malloc(sizeof(long));
  *c = 0;
  cs_promo_last_counter = c;
  code[0] = 0x49;
  code[1] = 0xBB;
  *(long *)(code + 2) = (long)c;
  code[10] = 0x49;
  code[11] = 0xFF;
  code[12] = 0x03;
  return code + 13;
}

DEFINEFN void psyco_cs_promo_miss(int site) { cs_promo_misses[site]++; }

/* ni._promostats(): the sites, each with its misses and its values' run
   counts filled in (copies; the counters keep counting) */
DEFINEFN PyObject *psyco_cs_promo_stats(void) {
  Py_ssize_t i, j;
  PyObject *r = PyList_New(0);
  if (cs_promo_sites == NULL)
    return r;
  for (i = 0; i < PyList_GET_SIZE(cs_promo_sites); i++) {
    PyObject *s = PyList_GET_ITEM(cs_promo_sites, i);
    PyObject *vals = PyList_GET_ITEM(s, 7);
    PyObject *nv = PyList_New(0);
    PyObject *ns;
    for (j = 0; j < PyList_GET_SIZE(vals); j++) {
      PyObject *v = PyList_GET_ITEM(vals, j);
      long *c = PyLong_AsVoidPtr(PyList_GET_ITEM(v, 3));
      PyObject *t = Py_BuildValue("(OOOl)", PyList_GET_ITEM(v, 0),
                                  PyList_GET_ITEM(v, 1), PyList_GET_ITEM(v, 2),
                                  c ? *c : -1L);
      PyList_Append(nv, t);
      Py_DECREF(t);
    }
    ns = Py_BuildValue("(OOOOOOONlOO)", PyList_GET_ITEM(s, 0),
                       PyList_GET_ITEM(s, 1), PyList_GET_ITEM(s, 2),
                       PyList_GET_ITEM(s, 3), PyList_GET_ITEM(s, 4),
                       PyList_GET_ITEM(s, 5), PyList_GET_ITEM(s, 6), nv,
                       cs_promo_misses[i], PyList_GET_ITEM(s, 8),
                       PyList_GET_ITEM(s, 9));
    PyList_Append(r, ns);
    Py_DECREF(ns);
  }
  return r;
}
#endif

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

DEFINEVAR PyObject *Psyco_Unregistered_Metas = NULL;

static void meta_not_registered(PyObject *module, char *meth_name,
                                const char *why) {
  PyObject *s = PyUnicode_FromFormat(
      "%s.%s: %s", module ? PyModule_GetName(module) : "?", meth_name, why);
  if (s == NULL) {
    PyErr_Clear();
    return;
  }
  if (Psyco_Unregistered_Metas == NULL)
    Psyco_Unregistered_Metas = PyList_New(0);
  if (Psyco_Unregistered_Metas == NULL ||
      PyList_Append(Psyco_Unregistered_Metas, s) < 0)
    PyErr_Clear();
#if ALL_CHECKS
  fprintf(stderr, "ni: no meta for %s\n", PyUnicode_AsUTF8(s));
#endif
  Py_DECREF(s);
}

DEFINEFN
PyCFunction Psyco_DefineModuleFn(PyObject *module, char *meth_name,
                                 int meth_flags, void *meta_fn) {
  PyCFunction f;
  PyObject *fobj = Psyco_GetModuleObject(module, meth_name, &PyCFunction_Type);
  if (fobj == NULL) {
    meta_not_registered(module, meth_name, "not a builtin function");
    return NULL;
  }

  if (PyCFunction_GET_FLAGS(fobj) != meth_flags) {
    /* the meta's arguments and its fallback follow meth_flags' convention,
       so it can't stand in for a function with another one */
    char why[64];
    snprintf(why, sizeof(why), "ml_flags are 0x%x, the meta's are 0x%x",
             PyCFunction_GET_FLAGS(fobj), meth_flags);
    meta_not_registered(module, meth_name, why);
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
    sk_mark_used(po, sk);
    *out = (PyObject *)sk->value;
    return 1;
  }
}

DEFINEFN
int psyco_atcompiletime_static_type(PsycoObject *po, vinfo_t *vi,
                                    PyObject **out) {
  if (!compute_vinfo(vi, po))
    return -1;
  if (is_runtime(vi->source)) {
    if (vi->source & RunTime_Megamorphic)
      return 0;
    PycException_Promote(po, vi, &psyco_static_type_promotion);
    return -1;
  } else {
    source_known_t *sk = CompileTime_Get(vi->source);
    sk_mark_used(po, sk);
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
#if NI_CODESTATS
  cs_promo_nbt = backtrace(cs_promo_bt, CS_PROMO_BT);
#endif
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
  /* val is NULL for PyErr_SetNone, e.g. MemoryError (Tismer, hg 643589a) */
  vval = val == NULL
             ? NULL
             : vinfo_new(CompileTime_NewSk(sk_new((long)val, SkFlagPyObj)));
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
  /* The real frame if the code has one (run by NiCode_Run). Otherwise a
     minimalistic frame object, working around the specific expectations
     of PyFrame_New(), which is the only reasonable way to create frame
     objects (because of free lists etc); cimpl_rt_traceback links it to
     its caller's (KNOWN_BUGS C11) */
  PyFrameObject *result = psyco_running_real_frame(code);
  ni_frame_t *back = ni_frame_current(tstate);
  if (result != NULL) {
    Py_INCREF(result);
    result->f_lasti = lasti;
    result->f_lineno = lineno;
    return result;
  }
  /* frame objects are not created in stack order with Psyco, so it's
     probably better not to create plain wrong chained lists */
  ni_frame_set_current(tstate, NULL);
  result = PyFrame_New(tstate, code, globals, NULL);
  ni_frame_set_current(tstate, back);
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
    /* the exception came out of the frame of the last entry: if that is an
       emulated one (a compiled callee), this frame is its f_back; an
       entry is recorded in every frame the exception passes through */
    PyObject *last = tstate->curexc_traceback;
    if (last != NULL && PyTraceBack_Check(last)) {
      PyFrameObject *callee = ((PyTracebackObject *)last)->tb_frame;
      if (callee->f_back == NULL && callee != f) {
        Py_INCREF(f);
        callee->f_back = f;
      }
    }
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
                       (long)po->pr.co, LOC_GLOBALS, (long)lasti, (long)lineno);
  } else {
    /* We only have a virtual-time exception (not set in Python),
       so we build po->pr.tb without actually setting it either */
    vinfo_t *tb = psyco_generic_call(
        po, cimpl_vt_traceback, CfCommonNewRefPyObjectNoError, "lvll",
        (long)po->pr.co, LOC_GLOBALS, (long)lasti, (long)lineno);
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


/* copy-on-write: snapshots and forked branches share the dict */
static void mark_varying(PsycoObject *po, PyObject *key) {
  PyObject *old = po->pr.changing_globals;
  PyObject *d;
  if (old != NULL && PyDict_GetItem(old, key) != NULL)
    return;
  d = old == NULL ? PyDict_New() : PyDict_Copy(old);
  if (d == NULL || PyDict_SetItem(d, key, Py_True))
    OUT_OF_MEMORY();
  Py_XDECREF(old);
  po->pr.changing_globals = d;
}

/* #define MISSING_OPCODE(opcode)					 */
/* 	case opcode:						 */
/* 		PycException_SetString(po, PyExc_PsycoError, */
/* 				  "opcode '" #opcode "' not implemented"); */
/* 		break */

DEFINEFN
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

/* Cached LOAD_GLOBAL (D25). The name is looked up now, as
   cimpl_load_global does at run time, and the value becomes a
   compile-time constant, guarded by the dicts' version tags (PEP 509:
   every change to a dict gives it a new tag, unique in the process). The
   guard compares the tags of globals and builtins (a name found in
   builtins also relies on its absence from globals) with expected tags
   kept in a per-site global_guard_t. A mismatch jumps to
   do_changed_global, which redoes the lookup: the same object (an
   unrelated write to the module) updates the expected tags and resumes;
   anything else respawns into a version where the name is varying and
   loaded at run time; a name not found is loaded at run time behind the
   same guard. The closure owns the value, so a constant that outlives a
   rebinding stays valid, and owns builtins, whose tag the guard reads; globals stays borrowed (D23): a new dict at its address
   has a new tag and is looked up. The guard slots are never freed. */
typedef struct {
  uint64_t globals_tag;
  uint64_t builtins_tag;
} global_guard_t;

#define DICT_ma_version_tag                                                    \
  FMUT(UNSIGNED_FIELD(PyDictObject, uint64_t, ma_version_tag, NO_PREV_FIELD))
#define GUARD_globals_tag                                                      \
  FMUT(UNSIGNED_FIELD(global_guard_t, uint64_t, globals_tag, NO_PREV_FIELD))
#define GUARD_builtins_tag                                                     \
  FMUT(UNSIGNED_FIELD(global_guard_t, uint64_t, builtins_tag, NO_PREV_FIELD))

typedef struct {
  PyObject *name;     /* owned */
  PyObject *value;    /* owned: the value compiled in, or NULL */
  PyObject *globals;  /* borrowed (D23) */
  PyObject *builtins; /* owned: the frame's (LOC_BUILTINS), which a new
                         globals['__builtins__'] doesn't change (F49) */
  global_guard_t *guard;
} changed_global_t;

static code_t *do_changed_global(changed_global_t *cg) {
  PyObject *x = PyDict_GetItem(cg->globals, cg->name);
  if (x == NULL)
    x = PyDict_GetItem(cg->builtins, cg->name);
  if (x != cg->value)
    return psyco_do_respawn(cg, sizeof(*cg));
  /* no real change */
  cg->guard->globals_tag = ((PyDictObject *)cg->globals)->ma_version_tag;
  cg->guard->builtins_tag = ((PyDictObject *)cg->builtins)->ma_version_tag;
  return psyco_dont_respawn(cg, sizeof(*cg));
}

/* the tag of 'vdict' differs from the guard's: a new run-time vinfo */
static vinfo_t *tag_changed(PsycoObject *po, vinfo_t *vdict, vinfo_t *vguard,
                            defield_t guard_field) {
  vinfo_t *x, *t1, *t2;
  t1 = psyco_get_field(po, vdict, DICT_ma_version_tag);
  if (t1 == NULL)
    return NULL;
  t2 = psyco_get_field(po, vguard, guard_field);
  if (t2 == NULL) {
    vinfo_decref(t1, po);
    return NULL;
  }
  x = integer_xor(po, t1, t2);
  vinfo_decref(t2, po);
  vinfo_decref(t1, po);
  return x;
}

/* Returns true with *result a new vinfo for the global, or with *result
   NULL if it isn't cached here (the caller loads it at run time); false
   on error. */
static bool load_global_cached(PsycoObject *po, PyObject *name,
                               vinfo_t **result) {
  static global_guard_t respawn_guard; /* written into trashed code only */
  PyObject *globals, *builtins, *value;
  global_guard_t *guard;
  vinfo_t *vguard, *vb, *x, *y, *z;
  condition_code_t cc;
  changed_global_t *cg;

  *result = NULL;
  if (!is_compiletime(LOC_GLOBALS->source) ||
      !is_compiletime(LOC_BUILTINS->source))
    return true;
  if (po->pr.changing_globals != NULL &&
      PyDict_GetItem(po->pr.changing_globals, name) != NULL)
    return true; /* known to change */
  globals = (PyObject *)CompileTime_Get(LOC_GLOBALS->source)->value;
  builtins = (PyObject *)CompileTime_Get(LOC_BUILTINS->source)->value;
  if (!PyDict_CheckExact(globals) || !PyDict_CheckExact(builtins))
    return true;

  /* A respawn replays compilation from the block's snapshot with the
     dicts as they are then, so a cached site must not be re-decided:
     - everything up to detect_respawn() is independent of what the dicts
       hold (both tags always guarded, the name looked up afterwards);
     - the block ends right after the site (split_after_global), so no
       later respawn point replays through it: later blocks start from a
       snapshot holding the constant. */
  if (is_respawning(po))
    guard = &respawn_guard;
  else {
    guard = PyMem_Malloc(sizeof(global_guard_t));
    if (guard == NULL)
      OUT_OF_MEMORY();
    guard->globals_tag = ((PyDictObject *)globals)->ma_version_tag;
    guard->builtins_tag = ((PyDictObject *)builtins)->ma_version_tag;
  }
  vguard = vinfo_new(CompileTime_New((long)guard));
  vb = vinfo_new(CompileTime_New((long)builtins));
  x = tag_changed(po, LOC_GLOBALS, vguard, GUARD_globals_tag);
  y = x == NULL ? NULL : tag_changed(po, vb, vguard, GUARD_builtins_tag);
  z = y == NULL ? NULL : integer_or(po, x, y);
  vinfo_xdecref(y, po);
  vinfo_xdecref(x, po);
  vinfo_decref(vb, po);
  vinfo_decref(vguard, po);
  if (z == NULL)
    return false;
  cc = integer_NON_NULL(po, z); /* consumes z */
  if (cc == CC_ERROR)
    return false;

  if (detect_respawn(po)) {
    /* the guard failed and the value really changed: from here on this
       name is loaded at run time */
    mark_varying(po, name);
    return true;
  }
  value = PyDict_GetItem(globals, name);
  if (value == NULL)
    value = PyDict_GetItem(builtins, name);
  cg = (changed_global_t *)psyco_prepare_respawn_ex(
      po, cc, &do_changed_global, sizeof(changed_global_t));
  po->pr.split_after_global = 1;
  if (cg != NULL) {
    cg->name = name;
    Py_INCREF(name);
    cg->value = value;
    Py_XINCREF(value);
    cg->globals = globals;
    cg->builtins = builtins;
    Py_INCREF(builtins);
    cg->guard = guard;
  }
  /* not found: a run-time load raises NameError; the guard catches a
     later definition */
  if (value != NULL)
    /* borrowed from cg (constants are never released, see sk_release) */
    *result = vinfo_new(CompileTime_NewSk(sk_new((long)value, SkFlagPyObj)));
  return true;
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
  /* ceval's stack holds its own reference to kwargs during the call, and
     callees rely on it: functools.partial keeps a dict whose refcount is 1
     instead of copying it. Compiled code holds none, so take one. */
  Py_XINCREF(kwargs);
  result = PyObject_Call(func, callargs, kwargs);
  Py_XDECREF(kwargs);
  Py_XDECREF(d);
  Py_XDECREF(t);
  return result;
}

/* CALL_FUNCTION_EX: f(*args) or f(*args, **kwargs). The stack, top down:
   [kwargs], args, callable. f(*tuple) goes straight to PsycoObject_Call,
   which can specialise the call; anything else is converted and called at
   run time, as ceval does. */
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
  if (t == &PyTuple_Type && zero != NULL) {
    /* f(*args): the call can be specialised. With **kwargs it goes to the
       run-time helper, which holds a reference to the dict as ceval does */
    result = PsycoObject_Call(po, func, callargs, kwargs);
    goto done;
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

#define GLOBAL_NAME_ERROR_MSG "name '%.200s' is not defined"
#define UNBOUNDLOCAL_ERROR_MSG                                                 \
  "local variable '%.200s' referenced before assignment"

static void unbound_local_error(PsycoObject *po, PyCodeObject *co, int oparg) {
  PyObject *namev = PyTuple_GetItem(co->co_varnames, oparg);
  PycException_SetFormat(po, PyExc_UnboundLocalError, UNBOUNDLOCAL_ERROR_MSG,
                         NiCompatStr_AsString(namev));
}

/* ceval.c:format_exc_unbound */
#define UNBOUNDFREE_ERROR_MSG                                                  \
  "free variable '%.200s' referenced before assignment in enclosing scope"

static void unbound_deref_error(PsycoObject *po, PyCodeObject *co, int oparg) {
  Py_ssize_t ncells = PyTuple_GET_SIZE(co->co_cellvars);
  if (oparg < ncells)
    PycException_SetFormat(
        po, PyExc_UnboundLocalError, UNBOUNDLOCAL_ERROR_MSG,
        NiCompatStr_AsString(PyTuple_GET_ITEM(co->co_cellvars, oparg)));
  else
    PycException_SetFormat(
        po, PyExc_NameError, UNBOUNDFREE_ERROR_MSG,
        NiCompatStr_AsString(PyTuple_GET_ITEM(co->co_freevars,
                                              oparg - ncells)));
}

#define CELL_ob_ref FMUT(DEF_FIELD(PyCellObject, PyObject *, ob_ref, OB_type))

/* ceval.c:import_from, as 3.7: the sys.modules fallback for circular
   relative imports, and an ImportError with name and path (F44) */
static PyObject *cimpl_import_from(PyObject *v, PyObject *name) {
  _Py_IDENTIFIER(__name__);
  PyObject *x, *fullmodname, *pkgname, *pkgpath, *pkgname_or_unknown, *errmsg;

  if (_PyObject_LookupAttr(v, name, &x) != 0)
    return x;
  pkgname = _PyObject_GetAttrId(v, &PyId___name__);
  if (pkgname == NULL)
    goto error;
  if (!PyUnicode_Check(pkgname)) {
    Py_CLEAR(pkgname);
    goto error;
  }
  fullmodname = PyUnicode_FromFormat("%U.%U", pkgname, name);
  if (fullmodname == NULL) {
    Py_DECREF(pkgname);
    return NULL;
  }
  x = PyImport_GetModule(fullmodname);
  Py_DECREF(fullmodname);
  if (x == NULL && !PyErr_Occurred())
    goto error;
  Py_DECREF(pkgname);
  return x;
error:
  pkgpath = PyModule_GetFilenameObject(v);
  if (pkgname == NULL) {
    pkgname_or_unknown = PyUnicode_FromString("<unknown module name>");
    if (pkgname_or_unknown == NULL) {
      Py_XDECREF(pkgpath);
      return NULL;
    }
  } else
    pkgname_or_unknown = pkgname;
  if (pkgpath == NULL || !PyUnicode_Check(pkgpath)) {
    PyErr_Clear();
    errmsg = PyUnicode_FromFormat(
        "cannot import name %R from %R (unknown location)", name,
        pkgname_or_unknown);
    PyErr_SetImportError(errmsg, pkgname, NULL);
  } else {
    errmsg = PyUnicode_FromFormat("cannot import name %R from %R (%S)", name,
                                  pkgname_or_unknown, pkgpath);
    PyErr_SetImportError(errmsg, pkgname, pkgpath);
  }
  Py_XDECREF(errmsg);
  Py_XDECREF(pkgname_or_unknown);
  Py_XDECREF(pkgpath);
  return NULL;
}

/* ceval.c:cmp_outcome, PyCmp_EXC_MATCH: -1 with a TypeError if w is not an
   exception class or a tuple of them (F43) */
static int cimpl_exc_match(PyObject *v, PyObject *w) {
  Py_ssize_t i;
  if (PyTuple_Check(w)) {
    for (i = 0; i < PyTuple_GET_SIZE(w); i++)
      if (!PyExceptionClass_Check(PyTuple_GET_ITEM(w, i)))
        goto bad;
  } else if (!PyExceptionClass_Check(w))
    goto bad;
  return PyErr_GivenExceptionMatches(v, w);
bad:
  PyErr_SetString(PyExc_TypeError, "catching classes that do not inherit "
                                   "from BaseException is not allowed");
  return -1;
}

static void cimpl_store_deref(PyObject *cell, PyObject *v) {
  PyObject *old = PyCell_GET(cell);
  Py_INCREF(v);
  PyCell_SET(cell, v);
  Py_XDECREF(old);
}

/* ceval.c BUILD_TUPLE_UNPACK_WITH_CALL, BUILD_TUPLE_UNPACK and
   BUILD_LIST_UNPACK, on a tuple of the parts; func is the function being
   called (for the WITH_CALL message, ceval.c:check_args_iterable) */
static PyObject *cimpl_build_unpack(PyObject *parts, PyObject *func,
                                    long opcode) {
  Py_ssize_t i;
  PyObject *sum = PyList_New(0), *r;
  if (sum == NULL)
    return NULL;
  for (i = 0; i < PyTuple_GET_SIZE(parts); i++) {
    PyObject *part = PyTuple_GET_ITEM(parts, i);
    PyObject *none_val = _PyList_Extend((PyListObject *)sum, part);
    if (none_val == NULL) {
      if (opcode == BUILD_TUPLE_UNPACK_WITH_CALL &&
          PyErr_ExceptionMatches(PyExc_TypeError) &&
          part->ob_type->tp_iter == NULL && !PySequence_Check(part)) {
        PyErr_Format(PyExc_TypeError,
                     "%.200s%.200s argument after * must be an iterable, "
                     "not %.200s",
                     PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                     part->ob_type->tp_name);
      }
      Py_DECREF(sum);
      return NULL;
    }
    Py_DECREF(none_val);
  }
  if (opcode == BUILD_LIST_UNPACK)
    return sum;
  r = PyList_AsTuple(sum);
  Py_DECREF(sum);
  return r;
}

/* ceval.c BUILD_MAP_UNPACK (func == NULL) and BUILD_MAP_UNPACK_WITH_CALL,
   on a tuple of the parts */
static PyObject *cimpl_build_map_unpack(PyObject *parts, PyObject *func) {
  Py_ssize_t i;
  PyObject *sum = PyDict_New();
  if (sum == NULL)
    return NULL;
  for (i = 0; i < PyTuple_GET_SIZE(parts); i++) {
    PyObject *arg = PyTuple_GET_ITEM(parts, i);
    if (func == NULL) {
      if (PyDict_Update(sum, arg) < 0) {
        if (PyErr_ExceptionMatches(PyExc_AttributeError))
          PyErr_Format(PyExc_TypeError, "'%.200s' object is not a mapping",
                       arg->ob_type->tp_name);
        Py_DECREF(sum);
        return NULL;
      }
    } else if (_PyDict_MergeEx(sum, arg, 2) < 0) {
      if (PyErr_ExceptionMatches(PyExc_AttributeError)) {
        PyErr_Format(PyExc_TypeError,
                     "%.200s%.200s argument after ** "
                     "must be a mapping, not %.200s",
                     PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                     arg->ob_type->tp_name);
      } else if (PyErr_ExceptionMatches(PyExc_KeyError)) {
        PyObject *exc, *val, *tb;
        PyErr_Fetch(&exc, &val, &tb);
        if (val && PyTuple_Check(val) && PyTuple_GET_SIZE(val) == 1) {
          PyObject *key = PyTuple_GET_ITEM(val, 0);
          if (!PyUnicode_Check(key))
            PyErr_Format(PyExc_TypeError,
                         "%.200s%.200s keywords must be strings",
                         PyEval_GetFuncName(func), PyEval_GetFuncDesc(func));
          else
            PyErr_Format(PyExc_TypeError,
                         "%.200s%.200s got multiple "
                         "values for keyword argument '%U'",
                         PyEval_GetFuncName(func), PyEval_GetFuncDesc(func),
                         key);
          Py_XDECREF(exc);
          Py_XDECREF(val);
          Py_XDECREF(tb);
        } else
          PyErr_Restore(exc, val, tb);
      }
      Py_DECREF(sum);
      return NULL;
    }
  }
  return sum;
}

/* ceval.c BUILD_SET_UNPACK, on a tuple of the parts */
static PyObject *cimpl_build_set_unpack(PyObject *parts) {
  Py_ssize_t i;
  PyObject *sum = PySet_New(NULL);
  if (sum == NULL)
    return NULL;
  for (i = 0; i < PyTuple_GET_SIZE(parts); i++)
    if (_PySet_Update(sum, PyTuple_GET_ITEM(parts, i)) < 0) {
      Py_DECREF(sum);
      return NULL;
    }
  return sum;
}

/* ceval.c:special_lookup */
static PyObject *special_lookup(PyObject *o, _Py_Identifier *id) {
  PyObject *res = _PyObject_LookupSpecial(o, id);
  if (res == NULL && !PyErr_Occurred()) {
    PyErr_SetObject(PyExc_AttributeError, id->object);
    return NULL;
  }
  return res;
}

/* ceval.c SETUP_WITH up to the block setup: out[0] gets the bound
   __exit__, out[1] __enter__'s result. Unlike ceval, mgr stays on the stack
   (one reference more) while __enter__ runs. */
static int cimpl_setup_with(PyObject *mgr, PyObject *out[]) {
  _Py_IDENTIFIER(__exit__);
  _Py_IDENTIFIER(__enter__);
  PyObject *enter = special_lookup(mgr, &PyId___enter__), *exit, *res;
  if (enter == NULL)
    return -1;
  exit = special_lookup(mgr, &PyId___exit__);
  if (exit == NULL) {
    Py_DECREF(enter);
    return -1;
  }
  res = _PyObject_CallNoArg(enter);
  Py_DECREF(enter);
  if (res == NULL) {
    Py_DECREF(exit);
    return -1;
  }
  out[0] = exit;
  out[1] = res;
  return 0;
}

/* WITH_CLEANUP_START's call */
static PyObject *cimpl_call_exit(PyObject *exit_func, PyObject *exc,
                                 PyObject *val, PyObject *tb) {
  PyObject *stack[3];
  stack[0] = exc;
  stack[1] = val;
  stack[2] = tb;
  return _PyObject_FastCall(exit_func, stack, 3);
}

/* ceval.c LOAD_BUILD_CLASS */
static PyObject *cimpl_load_build_class(PyObject *builtins) {
  _Py_IDENTIFIER(__build_class__);
  PyObject *bc;
  if (PyDict_CheckExact(builtins)) {
    bc = _PyDict_GetItemId(builtins, &PyId___build_class__);
    if (bc == NULL) {
      PyErr_SetString(PyExc_NameError, "__build_class__ not found");
      return NULL;
    }
    Py_INCREF(bc);
  } else {
    PyObject *build_class_str = _PyUnicode_FromId(&PyId___build_class__);
    if (build_class_str == NULL)
      return NULL;
    bc = PyObject_GetItem(builtins, build_class_str);
    if (bc == NULL) {
      if (PyErr_ExceptionMatches(PyExc_KeyError))
        PyErr_SetString(PyExc_NameError, "__build_class__ not found");
      return NULL;
    }
  }
  return bc;
}

/* ceval.c DELETE_DEREF: 1 if the cell was already empty (the caller
   raises ceval's error) */
static int cimpl_delete_deref(PyObject *cell) {
  PyObject *oldobj = PyCell_GET(cell);
  if (oldobj == NULL)
    return 1;
  PyCell_SET(cell, NULL);
  Py_DECREF(oldobj);
  return 0;
}

static PyObject *cimpl_load_global(PyObject *globals, PyObject *builtins,
                                   PyObject *w) {
  PyObject *x;
  RECLIMIT_SAFE_ENTER();
  x = PyDict_GetItem(globals, w);
  if (x == NULL) {
    x = PyDict_GetItem(builtins, w);
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

/* ceval.c:unpack_iterable (argcntafter -1: no starred target), returning
   0 or -1 and filling sp[] upwards: the first item in sp[0], which the
   caller pushes last, so that it is on top as in ceval */
static int unpack_iterable(PyObject *v, int argcnt, int argcntafter,
                           PyObject **sp) {
  int i = 0, j = 0;
  Py_ssize_t ll = 0;
  PyObject *it; /* iter(v) */
  PyObject *w;
  PyObject *l = NULL; /* variable list */

  extra_assert(v != NULL);

  it = PyObject_GetIter(v);
  if (it == NULL) {
    if (PyErr_ExceptionMatches(PyExc_TypeError) &&
        v->ob_type->tp_iter == NULL && !PySequence_Check(v))
      PyErr_Format(PyExc_TypeError, "cannot unpack non-iterable %.200s object",
                   v->ob_type->tp_name);
    return -1;
  }

  for (; i < argcnt; i++) {
    w = PyIter_Next(it);
    if (w == NULL) {
      /* Iterator done, via error or exhaustion. */
      if (!PyErr_Occurred()) {
        if (argcntafter == -1)
          PyErr_Format(PyExc_ValueError,
                       "not enough values to unpack (expected %d, got %d)",
                       argcnt, i);
        else
          PyErr_Format(PyExc_ValueError,
                       "not enough values to unpack "
                       "(expected at least %d, got %d)",
                       argcnt + argcntafter, i);
      }
      goto Error;
    }
    *sp++ = w;
  }

  if (argcntafter == -1) {
    /* We better have exhausted the iterator now. */
    w = PyIter_Next(it);
    if (w == NULL) {
      if (PyErr_Occurred())
        goto Error;
      Py_DECREF(it);
      return 0;
    }
    Py_DECREF(w);
    PyErr_Format(PyExc_ValueError, "too many values to unpack (expected %d)",
                 argcnt);
    goto Error;
  }

  l = PySequence_List(it);
  if (l == NULL)
    goto Error;
  *sp++ = l;
  i++;

  ll = PyList_GET_SIZE(l);
  if (ll < argcntafter) {
    PyErr_Format(PyExc_ValueError,
                 "not enough values to unpack (expected at least %d, got %zd)",
                 argcnt + argcntafter, argcnt + ll);
    goto Error;
  }

  /* Pop the "after-variable" args off the list. */
  for (j = argcntafter; j > 0; j--, i++)
    *sp++ = PyList_GET_ITEM(l, ll - j);
  /* Resize the list. */
  Py_SIZE(l) = ll - argcntafter;
  Py_DECREF(it);
  return 0;

Error:
  for (; i > 0; i--)
    Py_DECREF(*--sp);
  Py_XDECREF(it);
  return -1;
}

static int cimpl_unpack_iterable(PyObject *v, long argcnt, PyObject **sp) {
  return unpack_iterable(v, (int)argcnt, -1, sp);
}

/* UNPACK_EX's oparg: the targets before the star, and after it << 8 */
static int cimpl_unpack_ex(PyObject *v, long oparg, PyObject **sp) {
  return unpack_iterable(v, (int)(oparg & 0xFF), (int)(oparg >> 8), sp);
}

static int cimpl_unpack_list(PyObject *listobject, long argcnt,
                             PyObject **sp) {
  int i;
  extra_assert(PyList_Check(listobject));

  if (PyList_GET_SIZE(listobject) != argcnt)
    return cimpl_unpack_iterable(listobject, argcnt, sp); /* the error */
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
/* 3.7 ceval.c do_raise's reraise, with ceval's traceback rule: a reraised
   exception gets no entry for this frame (ceval goes to exception_unwind),
   so the unwind loop records none (KNOWN_BUGS F18), and the RuntimeError
   for no active exception gets its entry here instead (ceval's error:) */
static void cimpl_reraise(PyCodeObject *code, PyObject *globals, int lasti,
                          int lineno) {
  /* _PyErr_GetTopmostException isn't exported in 3.7 */
  _PyErr_StackItem *exc_info = PyThreadState_GET()->exc_info;
  PyObject *type, *value, *tb;
  while ((exc_info->exc_type == NULL || exc_info->exc_type == Py_None) &&
         exc_info->previous_item != NULL)
    exc_info = exc_info->previous_item;
  type = exc_info->exc_type;
  value = exc_info->exc_value;
  tb = exc_info->exc_traceback;
  if (type == Py_None || type == NULL) {
    PyErr_SetString(PyExc_RuntimeError, "No active exception to reraise");
    cimpl_rt_traceback(code, globals, lasti, lineno);
    return;
  }
  Py_XINCREF(type);
  Py_XINCREF(value);
  Py_XINCREF(tb);
  PyErr_Restore(type, value, tb);
}

/* 3.7 ceval.c do_raise with an exception (the reraise is cimpl_reraise),
   except that it doesn't consume references to its arguments
   (psyco_generic_call can't express that), so it takes its own first */
static void cimpl_do_raise(PyObject *exc, PyObject *cause) {
  PyObject *type = NULL, *value = NULL;

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

static PyObject *cimpl_import_name(PyObject *globals, PyObject *builtins,
                                   PyObject *name, PyObject *fromlist
#if VERYCONVOLUTED_IMPORT_NAME
                                   ,
                                   PyObject *fitharg
#endif
) {
  PyObject *w;
  PyObject *x = PyDict_GetItemString(builtins, "__import__");
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

/* FORMAT_VALUE as ceval: 'which' is oparg & FVC_MASK, spec may be NULL */
static PyObject *cimpl_format_value(PyObject *value, PyObject *spec,
                                    long which) {
  PyObject *conv = NULL, *result;
  switch (which) {
  case FVC_STR:
    conv = PyObject_Str(value);
    break;
  case FVC_REPR:
    conv = PyObject_Repr(value);
    break;
  case FVC_ASCII:
    conv = PyObject_ASCII(value);
    break;
  default:
    Py_INCREF(value);
    conv = value;
  }
  if (conv == NULL)
    return NULL;
  if (PyUnicode_CheckExact(conv) && spec == NULL)
    return conv;
  result = PyObject_Format(conv, spec);
  Py_DECREF(conv);
  return result;
}

/* BUILD_STRING as ceval, for up to four parts or a tuple of them (n < 0) */
static PyObject *cimpl_build_string(long n, PyObject *a, PyObject *b,
                                    PyObject *c, PyObject *d) {
  PyObject *items[4] = {a, b, c, d};
  PyObject *empty = PyUnicode_New(0, 0), *str;
  if (empty == NULL)
    return NULL;
  if (n < 0)
    str = _PyUnicode_JoinArray(empty, &PyTuple_GET_ITEM(a, 0),
                               PyTuple_GET_SIZE(a));
  else
    str = _PyUnicode_JoinArray(empty, items, n);
  Py_DECREF(empty);
  return str;
}

/* min_max (bltinmodule.c) without key or default, one item at a time */
static PyObject *cimpl_minmax_step(PyObject *best, PyObject *item, long op) {
  if (best != NULL) {
    int c = PyObject_RichCompareBool(item, best, (int)op);
    if (c < 0)
      return NULL;
    if (c == 0) {
      Py_INCREF(best);
      return best;
    }
  }
  Py_INCREF(item);
  return item;
}

static PyObject *cimpl_minmax_result(PyObject *best, long is_min) {
  if (best == NULL) {
    PyErr_Format(PyExc_ValueError, "%s() arg is an empty sequence",
                 is_min ? "min" : "max");
    return NULL;
  }
  Py_INCREF(best);
  return best;
}

static void cimpl_generator_stop(void) {
  PyObject *exc, *val, *tb;
  if (!PyErr_ExceptionMatches(PyExc_StopIteration))
    return;
  PyErr_Fetch(&exc, &val, &tb);
  PyErr_NormalizeException(&exc, &val, &tb);
  if (tb != NULL)
    PyException_SetTraceback(val, tb);
  Py_DECREF(exc);
  Py_XDECREF(tb);
  PyErr_SetString(PyExc_RuntimeError, "generator raised StopIteration");
  /* as _PyErr_ChainExceptions: the new exception's cause and context */
  {
    PyObject *e2, *v2, *t2;
    PyErr_Fetch(&e2, &v2, &t2);
    PyErr_NormalizeException(&e2, &v2, &t2);
    Py_INCREF(val);
    PyException_SetCause(v2, val);
    PyException_SetContext(v2, val);
    PyErr_Restore(e2, v2, t2);
  }
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
    /* PEP 479, as gen_send_ex: StopIteration leaving a generator (a fused
       generator expression) becomes RuntimeError */
    if ((po->pr.co->co_flags & CO_GENERATOR) &&
        !psyco_generic_call(po, cimpl_generator_stop, CfNoReturnValue, ""))
      return NULL;
    clear_pseudo_exception(po);
    sk_incref(&psyco_skZero);
    retsource = CompileTime_NewSk(&psyco_skZero); /*to return NULL */
  }

  return psyco_finish_return(po, retsource);
}

/***************************************************************/
/***   the main loop of the interpreter/compiler.            ***/

#if NI_CODESTATS
/* Code size per instruction (tools/codesize.py, ni._codestats()): for each
   (code object, instruction offset), [opcode, times compiled, bytes, the
   copies as (address, bytes, how the compiler
   was entered: codemanager.h NI_ENTER_*, the promotion site or -1, the
   index of that site's value or -1)]. The
   bytes are those emitted from the start of one instruction to the start of
   the next, so an instruction's error paths and the end of a block count
   with it; replays and code written into other buffers in between
   (respawn and promotion stubs) don't. */
DEFINEVAR PyObject *psyco_codestats = NULL;
DEFINEVAR long psyco_codestats_lost = 0; /* bytes in a jump to a new buffer */

static void codestats_flush(PsycoObject *po) {
  if (po->cs_mark != NULL) {
    long n = (char *)po->code - (char *)po->cs_mark;
    PyObject *key = Py_BuildValue("(Oi)", po->cs_co, po->cs_instr);
    PyObject *e = key ? PyDict_GetItem(psyco_codestats, key) : NULL;
    Py_XDECREF(key);
    if (e != NULL && n >= 0 && n < 65536) {
      PyObject *b = PyLong_FromLong(PyLong_AsLong(PyList_GET_ITEM(e, 2)) + n);
      PyObject *copy = Py_BuildValue("(ky#iii)", (unsigned long)po->cs_mark,
                                     (char *)po->cs_mark, (Py_ssize_t)n,
                                     psyco_stat_compile_kind,
                                     psyco_cs_promo_cur, cs_promo_vidx);
      if (b != NULL)
        PyList_SetItem(e, 2, b);
      if (copy != NULL) {
        PyList_Append(PyList_GET_ITEM(e, 3), copy); /* (address, bytes) */
        Py_DECREF(copy);
      }
    } else
      psyco_codestats_lost++;
    po->cs_mark = NULL;
  }
}

static void codestats_begin(PsycoObject *po, int instr, int opcode) {
  PyObject *key, *e;
  codestats_flush(po);
  if (is_respawning(po))
    return;
  if (psyco_codestats == NULL && (psyco_codestats = PyDict_New()) == NULL)
    OUT_OF_MEMORY();
  key = Py_BuildValue("(Oi)", po->pr.co, instr);
  if (key == NULL)
    OUT_OF_MEMORY();
  e = PyDict_GetItem(psyco_codestats, key);
  if (e == NULL) {
    e = Py_BuildValue("[iii[]]", opcode, 0, 0);
    if (e == NULL || PyDict_SetItem(psyco_codestats, key, e) < 0)
      OUT_OF_MEMORY();
    Py_DECREF(e);
  }
  Py_DECREF(key);
  PyList_SetItem(e, 1, PyLong_FromLong(PyLong_AsLong(PyList_GET_ITEM(e, 1)) + 1));
  po->cs_mark = po->code;
  po->cs_co = po->pr.co;
  po->cs_instr = instr;
}
#endif

/* The eval_breaker test after a call (KNOWN_BUGS C24, D45, D53), once its
   result is pushed: only if the call emitted a C call that isn't known to
   be brief and non-blocking (po->blocking_call, set by psyco_generic_call
   unless psyfunc.c:psyco_cannot_block accepts the function). A call that
   was inlined, folded, made virtual or made directly into compiled code
   emits none: that code tests at its own loop heads and after its own
   calls. The test is part of the CALL_*, not of the next instruction: a
   merge point there is matched before the next instruction is compiled,
   and code compiled after a call that needed no test would then serve one
   that did. None when the next instruction tests anyway (a loop head), or
   is one ceval skips, after which mergepoints.c marks a test that is
   always made. False with an exception set. */
static bool breaker_after_call(PsycoObject *po, int next_instr) {
  PyObject *code = po->pr.co->co_code;
  if (!po->blocking_call)
    return true;
  if (next_instr >= PyBytes_GET_SIZE(code) ||
      BREAKER_SKIPS((unsigned char)PyBytes_AS_STRING(code)[next_instr]) ||
      psyco_breaker_check_at(po->pr.merge_points, next_instr))
    return true;
  return psyco_check_eval_breaker(po);
}

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


#if NI_CODESTATS
      codestats_begin(po, next_instr, bytecode[next_instr]);
#endif
      opcode = bytecode[next_instr++];
      oparg = bytecode[next_instr++];
      /* at loop heads and after calls (KNOWN_BUGS C24) */
      if (psyco_breaker_check_at(po->pr.merge_points, po->pr.next_instr) &&
          !psyco_check_eval_breaker(po))
        break;
      po->blocking_call = 0;
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

      /* the simple opcodes, generated from ceval.c (D26, D67) */
#include "pycompiler_gen.h"

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
          psyco_generic_call(po, cimpl_do_raise, CfPyErrAlways, "vv", w, v);
          break;
        case 0:
          psyco_generic_call(po, cimpl_reraise, CfPyErrAlways, "lvll",
                             (long)co, LOC_GLOBALS, (long)po->pr.next_instr,
                             (long)PyCode_Addr2Line(co, po->pr.next_instr));
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
        if (co->co_flags & CO_GENERATOR) {
          /* a fused generator expression: return the consumer's result */
          int slot = GENEXPR_SLOT_MODE(co);
          long mode = CompileTime_Get(po->vlocals.items[slot]->source)->value;
          vinfo_decref(v, po);
          if (mode == GENEXPR_ANY)
            v = psyco_vi_Py_False();
          else if (mode == GENEXPR_ALL)
            v = psyco_vi_Py_True();
          else if (mode == GENEXPR_MIN || mode == GENEXPR_MAX) {
            v = psyco_generic_call(po, cimpl_minmax_result,
                                   CfCommonNewRefPyObject, "vl",
                                   po->vlocals.items[slot + 1],
                                   (long)(mode == GENEXPR_MIN));
            if (v == NULL)
              break;
          } else {
            v = po->vlocals.items[slot + 1];
            vinfo_incref(v);
          }
        }
        PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), v);
        break;

      case FORMAT_VALUE: {
        /* f-strings: the value (and spec) stay on the stack until done */
        int which = oparg & FVC_MASK;
        int spec = (oparg & FVS_MASK) == FVS_HAVE_SPEC;
        u = spec ? NTOP(1) : NULL;
        v = spec ? NTOP(2) : NTOP(1);
        if (which == FVC_NONE && !spec &&
            Psyco_KnownType(v) == &PyUnicode_Type) {
          goto fine; /* the value itself, as ceval */
        }
        w = psyco_vi_Zero();
        x = psyco_generic_call(po, cimpl_format_value, CfCommonNewRefPyObject,
                               "vvl", v, spec ? u : w, (long)which);
        vinfo_decref(w, po);
        if (x == NULL)
          break;
        Psyco_AssertType(po, x, &PyUnicode_Type);
        if (spec)
          POP_DECREF();
        POP_DECREF();
        PUSH(x);
        goto fine;
      }

      case BUILD_STRING: {
        int i;
        vinfo_t *parts[4], *t = NULL;
        if (oparg <= 4) {
          w = psyco_vi_Zero();
          for (i = 0; i < 4; i++)
            parts[i] = i < oparg ? NTOP(oparg - i) : w;
          x = psyco_generic_call(po, cimpl_build_string,
                                 CfCommonNewRefPyObject, "lvvvv", (long)oparg,
                                 parts[0], parts[1], parts[2], parts[3]);
          vinfo_decref(w, po);
        } else {
          t = PsycoTuple_New(oparg, STACK_POINTER() - oparg);
          x = psyco_generic_call(po, cimpl_build_string,
                                 CfCommonNewRefPyObject, "lvlll", -1L, t, 0L, 0L,
                                 0L);
          vinfo_decref(t, po);
        }
        if (x == NULL)
          break;
        Psyco_AssertType(po, x, &PyUnicode_Type);
        while (oparg--)
          POP_DECREF();
        PUSH(x);
        goto fine;
      }

      case YIELD_VALUE: {
        /* only in a generator expression fused with its consumer
           (GENEXPR_*): do the consumer's step, then push the value send()
           would have, for the POP_TOP that follows */
        int slot = GENEXPR_SLOT_MODE(co);
        long mode = CompileTime_Get(po->vlocals.items[slot]->source)->value;
        vinfo_t *acc = po->vlocals.items[slot + 1];
        v = TOP();
        if (mode == GENEXPR_LIST) {
          if (!PsycoList_Append(po, acc, v))
            break;
        } else if (mode == GENEXPR_SET) {
          if (!psyco_generic_call(po, PySet_Add, CfCommonIntZeroOk, "vv", acc,
                                  v))
            break;
        } else if (mode == GENEXPR_SUM) {
          /* as builtin_sum's generic loop (its fast paths give the same
             results as + on ints and floats) */
          if (Psyco_KnownType(acc) != NULL)
            x = PsycoNumber_Add(po, acc, v);
          else
            x = psyco_generic_call(po, PyNumber_Add, CfCommonNewRefPyObject,
                                   "vv", acc, v);
          if (x == NULL)
            break;
          po->vlocals.items[slot + 1] = x;
          vinfo_decref(acc, po);
        } else if (mode == GENEXPR_MIN || mode == GENEXPR_MAX) {
          x = psyco_generic_call(po, cimpl_minmax_step, CfCommonNewRefPyObject,
                                 "vvl", acc, v,
                                 (long)(mode == GENEXPR_MIN ? Py_LT : Py_GT));
          if (x == NULL)
            break;
          po->vlocals.items[slot + 1] = x;
          vinfo_decref(acc, po);
        } else {
          cc = integer_NON_NULL(po, PsycoObject_IsTrue(po, v));
          if (cc == CC_ERROR)
            break;
          if (mode == GENEXPR_ALL)
            cc = INVERT_CC(cc);
          if (runtime_condition_f(po, cc)) {
            PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)),
                               mode == GENEXPR_ANY ? psyco_vi_Py_True()
                                                   : psyco_vi_Py_False());
            break;
          }
        }
        POP_DECREF();
        PUSH(psyco_vi_None());
        goto fine;
      }

#ifdef RETURN_NONE
        /* This opcode was temporarily added to the Python 2.3 CVS.
           Now it is gone again, so the following will probably
           never be compiled into Psyco anyway. */
      case RETURN_NONE:
        v = psyco_vi_None();
        PycException_Raise(po, vinfo_new(VirtualTime_New(&EReturn)), v);
        break;
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
            if (runtime_condition_f(po, cc))
              goto unpack_generic; /* which raises ceval's error */

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
        unpack_generic:
          cimpl_unpack = cimpl_unpack_iterable;

        {
          vinfo_array_t *array = array_new(oparg);
          if (!psyco_generic_call(po, cimpl_unpack, CfCommonIntZeroOk, "vlA", v,
                                  (long)oparg, array)) {
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

      case UNPACK_EX: {
        int i, before = oparg & 0xFF, total = 1 + (oparg & 0xFF) + (oparg >> 8);
        vinfo_array_t *array = array_new(total);
        if (!psyco_generic_call(po, cimpl_unpack_ex, CfCommonIntZeroOk, "vlA",
                                TOP(), (long)oparg, array)) {
          array_release(array);
          break;
        }
        Psyco_AssertType(po, array->items[before], &PyList_Type);
        POP_DECREF();
        for (i = total; i--;)
          PUSH(array->items[i]);
        array_release(array);
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

      case LOAD_NAME:
        /* only for modules: we can only reach this point
           for code objects compiled from frames where
           f_locals == f_globals */
        /* fall through */
      case LOAD_GLOBAL: {
        PyObject *namev = GETNAMEV(oparg);
        if (!load_global_cached(po, namev, &v))
          break;
        if (v != NULL) {
          PUSH(v);
          goto fine;
        }
        /* not cached: globals unknown, the name varying or not found */
        v = psyco_generic_call(po, cimpl_load_global, CfCommonNewRefPyObject,
                               "vvl", LOC_GLOBALS, LOC_BUILTINS, namev);
        if (v == NULL)
          break;
        PUSH(v);
        goto fine;
      }

      case DELETE_FAST:
        x = GETLOCAL(oparg);
        /* a local variable can only be unbound if its
           value is known to be (PyObject*)NULL. A run-time
           or virtual value is always non-NULL. */
        if (psyco_knowntobe(x, 0)) {
          unbound_local_error(po, co, oparg);
          break;
        }

        u = psyco_vi_Zero();
        SETLOCAL(oparg, u);
        goto fine;

      /* Cells and free variables: the frame's own cells, which CPython made
         before the frame reached Ni (PsycoObject_FromCode maps them as
         borrowed words). Calls to closure code from compiled code are
         still real calls (psyfunc.c:fncall_init), so no cell is ever made
         here. A cell can change under any call (nonlocal in a callee), so
         its contents are read each time (FMUT). */
      case LOAD_CLOSURE:
        x = GETLOCAL(co->co_nlocals + oparg);
        vinfo_incref(x);
        PUSH(x);
        goto fine;

      case LOAD_DEREF: {
        condition_code_t cc;
        x = psyco_get_field(po, GETLOCAL(co->co_nlocals + oparg), CELL_ob_ref);
        if (x == NULL)
          break;
        cc = integer_cmp_i(po, x, 0, Py_NE);
        if (cc == CC_ERROR) {
          vinfo_decref(x, po);
          break;
        }
        if (!runtime_condition_t(po, cc)) {
          vinfo_decref(x, po);
          unbound_deref_error(po, co, oparg);
          break;
        }
        need_reference(po, x);
        PUSH(x);
        goto fine;
      }

      case STORE_DEREF:
        if (!psyco_generic_call(po, cimpl_store_deref, CfNoReturnValue, "vv",
                                GETLOCAL(co->co_nlocals + oparg), NTOP(1)))
          break;
        POP_DECREF();
        goto fine;

      case DELETE_DEREF: {
        int r = runtime_NON_NULL_f(
            po, psyco_generic_call(po, cimpl_delete_deref, CfReturnTypeInt,
                                   "v", GETLOCAL(co->co_nlocals + oparg)));
        if (r < 0)
          break;
        if (r) {
          unbound_deref_error(po, co, oparg);
          break;
        }
        goto fine;
      }

      /* class statements: a run-time load each time (classes made in a
         function are a rare, slow path anyway) */
      case LOAD_BUILD_CLASS:
        v = psyco_generic_call(po, cimpl_load_build_class,
                               CfCommonNewRefPyObject, "v", LOC_BUILTINS);
        if (v == NULL)
          break;
        PUSH(v);
        goto fine;

      /* f(a, *b), (*a, *b), [*a, *b]: parts that are tuples with known
         items make a virtual tuple; anything else is ceval's code */
      case BUILD_TUPLE_UNPACK_WITH_CALL:
      case BUILD_TUPLE_UNPACK:
      case BUILD_LIST_UNPACK: {
        int i, n = 0, known = opcode != BUILD_LIST_UNPACK;
        for (i = oparg; known && i > 0; i--) {
          int k = PsycoTuple_Load(NTOP(i));
          if (k < 0)
            known = 0;
          n += k;
        }
        if (known) {
          vinfo_t **items = (vinfo_t **)PyMem_Malloc((n + 1) * sizeof(*items));
          int m = 0;
          if (items == NULL) {
            PycException_SetString(po, PyExc_MemoryError, "");
            break;
          }
          for (i = oparg; i > 0; i--) {
            int j, k = PsycoTuple_Load(NTOP(i));
            for (j = 0; j < k; j++)
              items[m++] = PsycoTuple_GET_ITEM(NTOP(i), j);
          }
          v = PsycoTuple_New(n, items);
          PyMem_Free(items);
        } else {
          vinfo_t *fn = opcode == BUILD_TUPLE_UNPACK_WITH_CALL
                            ? NTOP(oparg + 1) : NULL;
          w = PsycoTuple_New(oparg, STACK_POINTER() - oparg);
          if (fn == NULL)
            fn = psyco_vi_Zero();
          else
            vinfo_incref(fn);
          v = psyco_generic_call(po, cimpl_build_unpack,
                                 CfCommonNewRefPyObject, "vvl", w, fn,
                                 (long)opcode);
          vinfo_decref(fn, po);
          vinfo_decref(w, po);
          if (v == NULL)
            break;
          Psyco_AssertType(po, v, opcode == BUILD_LIST_UNPACK ? &PyList_Type
                                                               : &PyTuple_Type);
        }
        while (oparg--)
          POP_DECREF();
        PUSH(v);
        goto fine;
      }

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

      /* {**a, **b}, f(**a, k=1), {*a, *b}: ceval's code at run time */
      case BUILD_MAP_UNPACK:
      case BUILD_MAP_UNPACK_WITH_CALL:
      case BUILD_SET_UNPACK:
        w = PsycoTuple_New(oparg, STACK_POINTER() - oparg);
        if (opcode == BUILD_SET_UNPACK) {
          v = psyco_generic_call(po, cimpl_build_set_unpack,
                                 CfCommonNewRefPyObject, "v", w);
        } else {
          /* the function being called is under the positional tuple */
          vinfo_t *fn = opcode == BUILD_MAP_UNPACK_WITH_CALL
                            ? NTOP(oparg + 2) : NULL;
          if (fn == NULL)
            fn = psyco_vi_Zero();
          else
            vinfo_incref(fn);
          v = psyco_generic_call(po, cimpl_build_map_unpack,
                                 CfCommonNewRefPyObject, "vv", w, fn);
          vinfo_decref(fn, po);
        }
        vinfo_decref(w, po);
        if (v == NULL)
          break;
        Psyco_AssertType(po, v, opcode == BUILD_SET_UNPACK ? &PySet_Type
                                                            : &PyDict_Type);
        while (oparg--)
          POP_DECREF();
        PUSH(v);
        goto fine;

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

      case BUILD_SET: {
        /* as ceval: a new set, the items added deepest first */
        int i;
        v = psyco_generic_call(po, PySet_New, CfCommonNewRefPyObject, "l",
                               0L);
        if (v == NULL)
          break;
        Psyco_AssertType(po, v, &PySet_Type);
        for (i = oparg; i > 0; i--)
          if (!psyco_generic_call(po, PySet_Add, CfCommonIntZeroOk, "vv", v,
                                  NTOP(i)))
            break;
        if (i > 0) {
          vinfo_decref(v, po);
          break;
        }
        while (oparg--)
          POP_DECREF();
        PUSH(v);
        goto fine;
      }

      case SET_ADD:
        w = NTOP(1);
        v = NTOP(oparg + 1);
        if (!psyco_generic_call(po, PySet_Add, CfCommonIntZeroOk, "vv", v, w))
          break;
        POP_DECREF();
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
          x = psyco_generic_call(po, cimpl_exc_match,
                                 CfCommonIntBoolOrError, "vv", v, w);
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
                               "vvlv", LOC_GLOBALS, LOC_BUILTINS, w, TOP());
        if (x == NULL)
          break;
        POP_DECREF();
#else
        v = TOP();
        u = NTOP(2);
        x = psyco_generic_call(po, cimpl_import_name, CfCommonNewRefPyObject,
                               "vvlvv", LOC_GLOBALS, LOC_BUILTINS, w, v, u);
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
        x = psyco_generic_call(po, cimpl_import_from, CfCommonNewRefPyObject,
                               "vl", TOP(), (long)GETNAMEV(oparg));
        if (x == NULL)
          break;
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
          Psyco_ForgetClasses(po); /* IsTrue may have run code (F55) */
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

      /* with: as 3.7 ceval, a SETUP_FINALLY block over the bound __exit__.
         Its handler starts with WITH_CLEANUP_START, entered in one of
         Ni's three finally states (see the unwind loop): None on top (the
         body ended), a 3-tuple (EReturn, EBreak, EContinue), or an
         exception's six items under an EXCEPT_HANDLER block. */
      case SETUP_WITH: {
        vinfo_array_t *array = array_new(2);
        if (!psyco_generic_call(po, cimpl_setup_with, CfCommonIntZeroOk, "vA",
                                TOP(), array)) {
          array_release(array);
          break;
        }
        POP_DECREF();
        PUSH(array->items[0]);
        block_setup(po, SETUP_FINALLY, INSTR_OFFSET() + oparg, STACK_LEVEL());
        PUSH(array->items[1]);
        array_release(array);
        goto fine;
      }

      case WITH_CLEANUP_START: {
        PyTryBlock *b = po->pr.iblock > 0
                            ? &po->pr.blockstack[(int)po->pr.iblock - 1]
                            : NULL;
        vinfo_t **sp = STACK_POINTER();
        if (b != NULL && b->b_type == EXCEPT_HANDLER &&
            STACK_LEVEL() == b->b_level + 6) {
          /* an exception: exit(type, value, tb), then ceval's shift of
             the saved exception down over __exit__'s slot, with NULL
             where the type was */
          x = psyco_generic_call(po, cimpl_call_exit, CfCommonNewRefPyObject,
                                 "vvvv", sp[-7], sp[-1], sp[-2], sp[-3]);
          if (x == NULL)
            break;
          vinfo_decref(sp[-7], po);
          sp[-7] = sp[-6];
          sp[-6] = sp[-5];
          sp[-5] = sp[-4];
          sp[-4] = psyco_vi_Zero();
          b->b_level--;
          u = sp[-1];
          vinfo_incref(u);
          PUSH(u);
          PUSH(x);
          goto fine;
        }
        /* None or a pseudo-exception's 3-tuple: exit(None, None, None),
           and __exit__'s slot is dropped */
        extra_assert(psyco_knowntobe(TOP(), (long)Py_None) ||
                     Psyco_KnownType(TOP()) == &PyTuple_Type);
        x = psyco_generic_call(po, cimpl_call_exit, CfCommonNewRefPyObject,
                               "vlll", NTOP(2), (long)Py_None, (long)Py_None,
                               (long)Py_None);
        if (x == NULL)
          break;
        POP(v);
        POP_DECREF();
        PUSH(v);
        PUSH(psyco_vi_None());
        PUSH(x);
        goto fine;
      }

      case WITH_CLEANUP_FINISH:
        /* TOP is __exit__'s result, SECOND the exception type or None */
        if (!psyco_knowntobe(NTOP(2), (long)Py_None)) {
          cc = integer_NON_NULL(po, PsycoObject_IsTrue(po, TOP()));
          if (cc == CC_ERROR)
            break;
          if (runtime_condition_f(po, cc)) {
            /* a true result silences the exception: what END_FINALLY
               does in ceval for WHY_SILENCED, after which it sees None */
            PyTryBlock *b;
            POP_DECREF();
            POP_DECREF();
            b = block_pop(po);
            extra_assert(b->b_type == EXCEPT_HANDLER);
            unwind_except_handler(po, b);
            PUSH(psyco_vi_None());
            goto fine;
          }
        }
        POP_DECREF();
        POP_DECREF();
        goto fine;

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
        if (!breaker_after_call(po, next_instr))
          break;
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
        if (!breaker_after_call(po, next_instr))
          break;
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
      /* before the next merge point compares known types (F55) */
      Psyco_ForgetClasses(po);
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

      /* end the block after a guarded site (load_global_cached,
         pobject.c): a new buffer, contiguous, with its own snapshot, kept
         at this instruction so that a branch respawned from the site's
         checks can join it (vcompiler.c:psyco_compile_join) */
      if (po->pr.split_after_global) {
        po->pr.split_after_global = 0;
        if (!is_respawning(po) && mp->bytecode_position != next_instr) {
          SAVE_NEXT_INSTR(next_instr);
          code1 = psyco_compile_join(
              po, psyco_join_point(po->pr.merge_points, next_instr));
          if (code1 != NULL)
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
      Psyco_ForgetClasses(po); /* the arguments' types (F55) */
      /* go on with compilation */
      clear_pseudo_exception(po);
      continue;
    }

    /* At this point, we got a real pseudo-exception. */

    if (PycException_IsPython(po) && opcode != END_FINALLY &&
        !(opcode == RAISE_VARARGS && oparg == 0)) {
      /* log traceback info for real Python exceptions, unless re-raised
         by END_FINALLY or a bare raise (cimpl_reraise) */
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
      if ((b->b_type == SETUP_EXCEPT || b->b_type == SETUP_FINALLY) &&
          PycException_IsPython(po)) {
        /* as 3.7 ceval: an EXCEPT_HANDLER block over six stack items (see
           cimpl_push_exc_info), for a finally block too, so that the
           exception is the handled one inside it (KNOWN_BUGS F20);
           END_FINALLY re-raises it from the top three. A virtual exception
           is raised for real first, so that the run-time code can fetch
           it. */
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
      if (b->b_type == SETUP_FINALLY) {
        /* a pseudo-exception (EReturn, EBreak, EContinue) entering a
           finally block: unlike Python, it is pushed as a single 3-tuple,
           which END_FINALLY tells apart from an exception type and
           re-raises */
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
    } /* end of unwind stack */

    /* End the function if we still have a (pseudo) exception */
    if (PycException_Occurred(po)) {
      break;
    }
    /* a handler is reached without passing 'fine' (F55). Not at the top
       of the main loop: an instruction restarted after a promotion would
       forget the promoted type again, for ever */
    Psyco_ForgetClasses(po);
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

#if NI_CODESTATS
  codestats_flush(po);
#endif
  Py_SetRecursionLimit(saved_rec_limit);

  PyErr_Restore(old_py_exc, old_py_val, old_py_tb);
  return code1;
}
