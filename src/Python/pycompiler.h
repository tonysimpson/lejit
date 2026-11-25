/***************************************************************/
/***          Language-dependent part of the compiler          ***/
/***************************************************************/

#ifndef _PYCOMPILER_H
#define _PYCOMPILER_H

#include "../codemanager.h"
#include "../dispatcher.h"
#include "../processor.h"
#include "../vcompiler.h"
#include "pycheader.h"
#include "pyver.h"
#include "v_mem_mov.h"
#include "../mechanisms/exc.h"

/*#define MAX3(a,b,c)  ((a)>(b)?((a)>(c)?(a):(c)):(b)>(c)?(b):(c))*/

/*****************************************************************/
/***   Common constants                                        ***/

/* the following known values have the SkFlagFixed set */
#define DEF_SK_AND_VI(name)                                   \
    EXTERNVAR source_known_t psyco_sk##name;                  \
    PSY_INLINE vinfo_t *psyco_vi_##name(void)                 \
    {                                                         \
        sk_incref(&psyco_sk##name);                           \
        return vinfo_new(CompileTime_NewSk(&psyco_sk##name)); \
    }

DEF_SK_AND_VI(Zero)           /* known value 0 */
DEF_SK_AND_VI(One)            /* known value 1 */
DEF_SK_AND_VI(None)           /* known value 'Py_None'  */
DEF_SK_AND_VI(Py_False)       /* known value 'Py_False' */
DEF_SK_AND_VI(Py_True)        /* known value 'Py_True'  */
DEF_SK_AND_VI(NotImplemented) /* 'Py_NotImplemented' */

/* the macro defines psyco_vi_None(), psyco_vi_Zero(),
   psyco_vi_One() and psyco_vi_NotImplemented(). */

#undef DEF_SK_AND_VI

/*****************************************************************/
/***   Compile-time Pseudo exceptions                          ***/

/*****
 * A pseudo-exception is the compile-time equivalent of a Python exception.
 * They are encoded in the fields 'exc' and 'val' of pyc_data_t. For real
 * Python the translation is immediate: 'exc' describes the PyObject*
 * pointing to the Python exception class, and 'val' is the associated value.
 *
 * For the other pseudo-exceptions, like special events breaking the Python
 * main loop (returns, break, continue), 'exc' is a virtual value using
 * one of the following non-computable virtual sources.
 *
 * 'ERtPython' is particular: it is the virtual equivalent of "the exception
 * currently set at run-time". Use PycException_Fetch() to emit the actual
 * call to PyErr_Fetch().
 */
EXTERNVAR source_virtual_t ERtPython; /* Exception raised by Python */
EXTERNVAR source_virtual_t EReturn;   /* 'return' statement */
EXTERNVAR source_virtual_t EBreak;    /* 'break' statement */
EXTERNVAR source_virtual_t EContinue; /* 'continue' statement */
EXTERNVAR source_virtual_t EInline; /* inline a frame inside a parent frame */

/* Check whether a pseudo-exception is currently set */
PSY_INLINE bool
PycException_Occurred(PsycoObject *po)
{
    return po->pr.exc != NULL;
}

/* raise an arbitrary pseudo-exception (consumes the references) */
EXTERNFN void
PycException_Clear(PsycoObject *po);

/* globals['__builtins__'] as a dict, as frameobject.c finds a new frame's
   builtins; borrowed */
EXTERNFN PyObject *
psy_get_builtins(PyObject *globals);
#if ALL_CHECKS
/* failed guards of cached globals, for lejit._core._stats() */
EXTERNVAR long psyco_stat_global_fails;
#endif
PSY_INLINE void
PycException_Raise(PsycoObject *po, vinfo_t *exc, vinfo_t *val)
{
    if (PycException_Occurred(po))
        PycException_Clear(po);
    po->pr.exc = exc;
    po->pr.val = val;
}
PSY_INLINE void
PycException_Restore(PsycoObject *po, vinfo_t *exc, vinfo_t *val, vinfo_t *tb)
{
    PycException_Raise(po, exc, val);
    po->pr.tb = tb;
}

/* for Python exceptions detected at compile-time */
EXTERNFN void
PycException_SetString(PsycoObject *po, PyObject *e, const char *text);
EXTERNFN void
PycException_SetFormat(PsycoObject *po, PyObject *e, const char *fmt, ...);
/* PycException_SetFormat for a message naming the types t1 and t2 (NULL
   for none): fmt's arguments are s (left out if NULL), then their names */
EXTERNFN void
PycException_SetTypeFormat(PsycoObject *po, PyObject *e, const char *fmt,
                           const char *s, PyTypeObject *t1, PyTypeObject *t2);
/* consumes a reference on 'v': */
EXTERNFN void
PycException_SetObject(PsycoObject *po, PyObject *e, PyObject *v);
/* consumes a reference on 'v': */
EXTERNFN void
PycException_SetVInfo(PsycoObject *po, PyObject *e, vinfo_t *v);
/* takes its own references (pycompiler.c) */
EXTERNFN void
PycException_Reraise(PsycoObject *po, vinfo_t *exc, vinfo_t *val, vinfo_t *tb);
/* checking for the Python class of an exception */
EXTERNFN vinfo_t *
PycException_Matches(PsycoObject *po, PyObject *e);

PSY_INLINE bool
PycException_Is(PsycoObject *po, source_virtual_t *sv)
{
    return po->pr.exc->source == VirtualTime_New(sv);
}
PSY_INLINE bool
PycException_IsPython(PsycoObject *po)
{
    Source src = po->pr.exc->source;
    if (is_virtualtime(src)) {
        return !(src == VirtualTime_New(&EReturn) ||
                 src == VirtualTime_New(&EBreak) ||
                 src == VirtualTime_New(&EContinue) ||
                 psyco_vsource_is_promotion(src));
    }
    else
        return true;
}

/* fetch a Python exception set at compile-time (that is, now) and turn into
   a pseudo-exception (typically to be re-raised at run-time). */
EXTERNFN void
psyco_virtualize_exception(PsycoObject *po);

/* fetch a Python exception set at run-time (that is, a ERtPython) and turn
   into a pseudo-exception. This is a no-op if !PycException_Is(po,
   &ERtPython). */
EXTERNFN void
PycException_Fetch(PsycoObject *po);

/*****************************************************************/
/***   Promotion                                               ***/

/* Raise a pseudo-exception meaning "promote 'vi' from run-time to
   compile-time". If 'promotion->fs' is not NULL, only promote if the
   run-time value turns out to be in the given set.
*/
EXTERNFN void
PycException_Promote(PsycoObject *po, vinfo_t *vi, c_promotion_t *promotion);

#if LE_CODESTATS
/* Promotion sites, for lejit._core._promostats(): the site
   (code object, instruction, who asked: a backtrace), the bytes of its stub,
   each value seen with the code and time its compile took, how often its
   code ran (a counter at the start of each promoted block, this build only)
   and how often the C dispatcher was entered (inline cache misses). */
EXTERNVAR int psyco_cs_promo_cur; /* site being compiled from, or -1 */
EXTERNFN void
psyco_codestats_delete(PsycoObject *po); /* PsycoObject_Delete */
EXTERNFN int
psyco_cs_promo_site(PsycoObject *po, int pflags, long stub_bytes);
EXTERNFN void
psyco_cs_promo_begin(int site, long key, int kind);
EXTERNFN void
psyco_cs_promo_end(void);
EXTERNFN code_t *
psyco_cs_promo_counter(code_t *code);
EXTERNFN void
psyco_cs_promo_miss(int site);
#define LE_CS_PROMO_COUNTER(code) psyco_cs_promo_counter(code)
#else
#define LE_CS_PROMO_COUNTER(code) (code)
#endif

/* A powerful function: it appears to return the value of the
   variable 'vi', even if it is a run-time variable. Implemented
   by raising a EPromotion exception if needed. Returns -1 in
   this case; use PycException_Occurred() to know if it is really
   an exception or a plain normal -1. Its callers fetch values that can't
   be -1 (pointers), so for them -1 always means "stop: pending". */
PSY_INLINE long
psyco_atcompiletime(PsycoObject *po, vinfo_t *vi)
{
    if (!compute_vinfo(vi, po))
        return -1;
    if (is_runtime(vi->source)) {
        PycException_Promote(po, vi, &psyco_nonfixed_promotion);
        return -1;
    }
    else {
        source_known_t *sk = CompileTime_Get(vi->source);
        sk_mark_used(po, sk);
        return sk->value;
    }
}
/* the same if the value to promote is itself a PyObject* which can be
   used as key in the look-up dictionary. Returns NULL to stop: its callers
   fetch objects (code, types) that are never NULL themselves. */
PSY_INLINE PyObject *
psyco_pyobj_atcompiletime(PsycoObject *po, vinfo_t *vi)
{
    if (!compute_vinfo(vi, po))
        return NULL;
    if (is_runtime(vi->source)) {
        PycException_Promote(po, vi, &psyco_nonfixed_pyobj_promotion);
        return NULL;
    }
    else {
        source_known_t *sk = CompileTime_Get(vi->source);
        sk_mark_used(po, sk);
        return (PyObject *)sk->value;
    }
}
/* the same, by 'promotion': Psyco_NeedType's, for a site with no generic
   path (psyco_interp_pyobj_promotion, psyco_fused_pyobj_promotion) */
PSY_INLINE PyObject *
psyco_pyobj_atcompiletime_by(PsycoObject *po, vinfo_t *vi,
                             c_promotion_t *promotion)
{
    if (!compute_vinfo(vi, po))
        return NULL;
    if (is_runtime(vi->source)) {
        PycException_Promote(po, vi, promotion);
        return NULL;
    }
    else {
        source_known_t *sk = CompileTime_Get(vi->source);
        sk_mark_used(po, sk);
        return (PyObject *)sk->value;
    }
}
/* whether the frame being compiled can go on in ceval
   (pycompiler.c:resume_in_ceval): not a fused generator or a coroutine,
   nor while a finally block holds a pseudo-exception's 3-tuple */
EXTERNFN bool
psyco_can_go_interpreted(PsycoObject *po);
/* what the generator being compiled does at a yield (GENEXPR_*,
   pycheader.h), or 0 if it is not a generator's code */
EXTERNFN long
psyco_generator_mode(PsycoObject *po);
#if LE_RESUMES_ON_FRAME
/* a resumed generator's frame word i, as a virtual value its frame still
   holds (pycompiler.c says how it is read); which word a value still is,
   or -1; and the frame, which the resumable body is given */
EXTERNFN vinfo_t *
psyco_frame_word(int i);
EXTERNFN int
psyco_frame_word_index(vinfo_t *v);
EXTERNFN vinfo_t *
psyco_frame_words_frame(PsycoObject *po);
#endif
#if LE_RESUMES_COROUTINES
EXTERNFN int
psyco_trace_yield(PyFrameObject *frame, PyObject *value);
#endif
/* the same again, detecting megamorphic sites: if many different run-time
   values keep showing up, return 0.  If successfully promoted, return 1.
   In case of exception, return -1. */
/*EXTERNFN int psyco_atcompiletime_mega(PsycoObject* po, vinfo_t *vi,
                                        long *out);*/
EXTERNFN int
psyco_pyobj_atcompiletime_mega(PsycoObject *po, vinfo_t *vi, PyObject **out);
/* the same, but only a static type is promoted (psyco_static_type_promotion):
   returns 0 for anything else at run time. A value that is already
   compile-time is returned as it is, whatever it is. */
EXTERNFN int
psyco_atcompiletime_static_type(PsycoObject *po, vinfo_t *vi, PyObject **out);

/* lazy comparison. Returns true if 'vi' is non-NULL, compile-time, and has the
   given value, and false otherwise. */
PSY_INLINE bool
psyco_knowntobe(vinfo_t *vi, long value)
{
    return vi != NULL && is_compiletime(vi->source) &&
           CompileTime_Get(vi->source)->value == value;
}

/* like psyco_knowntobe but tests for NULL */
PSY_INLINE bool
psyco_known_NULL(vinfo_t *vi)
{
    return vi != NULL && is_compiletime(vi->source) &&
           CompileTime_Get(vi->source)->value == 0;
}

/*****************************************************************/
/***   Exception utilities                                     ***/
/*****************************************************************/

/* Psyco meta-equivalent of PyErr_Occurred(). Not to be confused with
   PycException_Occurred(), which tells whether a Psyco-level exception
   is currently set. */
PSY_INLINE vinfo_t *
psyco_PyErr_Occurred(PsycoObject *po)
{
    if (PycException_Occurred(po) && PycException_IsPython(po)) {
        return psyco_vi_One();
    }
    else {
        /* inline PyErr_Occurred */
        vinfo_t *vtstate;
        vinfo_t *vcurexc;
        /* XXX Can we get _PyThreadState_Current to make this quicker ? */
        vtstate =
            psyco_generic_call(po, PyThreadState_Get, CfReturnTypePtr, "");
        vcurexc = v_mem_mov(po, NULL, vtstate, NULL, LE_EXC_OCCURRED_OFFSET,
                            V_MEM_MOV_SCALE_POINTER, false);
        vinfo_decref(vtstate, po);
        return vcurexc;
    }
}

/*****************************************************************/
/***   Meta functions                                          ***/

/* Each C function of the Python interpreter might be associated to a
   "meta" function from Psyco with the same signature but 'vinfo_t*' for
   the arguments and return value. The idea is that when such a meta
   function exists, Psyco can invoke it at compile-time to do (a part of)
   what the Python interpreter would do at run-time only. The code of the
   meta function typically ressembles that of the original function enough
   that we might dream about a language in which we never have to write
   the two versions (where Psyco's version could be derived automatically
   from the standard version).
*/

EXTERNVAR PyObject *Psyco_Meta_Dict; /* key is an int holding the
                                        address of the C function, value is
                                        an int holding the address
                                        of the corresponding Psyco function. */
EXTERNFN void
Psyco_DefineMeta(void *c_function, void *psyco_function);
PSY_INLINE void *
Psyco_Lookup(void *c_function)
{
    PyObject *value;
    PyObject *key = PyLong_FromLong((long)c_function);
    if (key == NULL || LE_FAIL(LE_FAIL_LOOKUP))
        OUT_OF_MEMORY();
    value = PyDict_GetItem(Psyco_Meta_Dict, key);
    Py_DECREF(key);
    if (value != NULL)
        return (void *)PyLong_AS_LONG(value);
    else
        return NULL;
}

/*** To map the content of a Python module to meta-implementations. ***/
/* Returns the module object (refcount *incremented*) or NULL if
   not found. It also prints some infos in verbose mode. */
EXTERNFN PyObject *
Psyco_DefineMetaModule(char *modulename);
/* Returns an object from a module (refcount *incremented*) or NULL
   if not found. In verbose mode, 'not found' errors are printed. */
EXTERNFN PyObject *
Psyco_GetModuleObject(PyObject *module, char *name,
                      PyTypeObject *expected_type);
/* Maps a built-in function object from a module to a meta-implementation.
   Returns a pointer to the C function itself, or NULL if the module has no
   such builtin or its ml_flags aren't meth_flags; then nothing is
   registered, and "module.name: why" goes in Psyco_Unregistered_Metas
   (lejit._core._unregistered_metas(); printed in checked builds). */
EXTERNVAR PyObject *Psyco_Unregistered_Metas;
EXTERNFN PyCFunction
Psyco_DefineModuleFn(PyObject *module, char *meth_name, int meth_flags,
                     void *meta_fn);
/* The same for a meta given positional arguments only, as every
   METH_FASTCALL meta is (pmethodobject.c), of a builtin of either fastcall
   convention: *meth_flags gets the builtin's own, for the meta's fallback. */
EXTERNFN PyCFunction
Psyco_DefineModuleFast(PyObject *module, char *meth_name, int *meth_flags,
                       void *meta_fn);
/* The same for a method of a static type: its PyMethodDef found through
   the method descriptor in tp's dict ("Type.name: why" when not). */
EXTERNFN PyCFunction
Psyco_DefineMethodFn(PyTypeObject *tp, char *meth_name, int meth_flags,
                     void *meta_fn);
/* Same as above, but also alternatively accepts a callable type object
   and maps it to meta_type_new. Returns NULL in this case. */
EXTERNFN PyCFunction
Psyco_DefineModuleC(PyObject *module, char *meth_name, int meth_flags,
                    void *meta_fn, void *meta_type_new);

/* the general-purpose calling routine: it looks for a meta implementation of
   'c_function' and call it if found; if not found, it encode a run-time call
   to 'c_function'. The 'flags' and 'arguments' are as in psyco_generic_call().
   The remaining arguments are given to the meta function or encoded in the
   run-time call; they should be compatible with the description given in
   'arguments'.
   This is a bit tricky, with one version of the macro per number of arguments,
   because the C processor is too limited and we want to avoid handling
   functions with '...' arguments all around
*/
#define Psyco_META1(po, c_function, flags, arguments, a1) \
    Psyco_Meta1x(po, c_function, flags, arguments, (long)(a1))
#define Psyco_META2(po, c_function, flags, arguments, a1, a2) \
    Psyco_Meta2x(po, c_function, flags, arguments, (long)(a1), (long)(a2))
#define Psyco_META3(po, c_function, flags, arguments, a1, a2, a3)          \
    Psyco_Meta3x(po, c_function, flags, arguments, (long)(a1), (long)(a2), \
                 (long)(a3))

#define Psyco_flag_META1 (condition_code_t) Psyco_META1
#define Psyco_flag_META2 (condition_code_t) Psyco_META2
#define Psyco_flag_META3 (condition_code_t) Psyco_META3

EXTERNFN vinfo_t *
Psyco_Meta1x(PsycoObject *po, void *c_function, int flags,
             const char *arguments, long a1);
EXTERNFN vinfo_t *
Psyco_Meta2x(PsycoObject *po, void *c_function, int flags,
             const char *arguments, long a1, long a2);
EXTERNFN vinfo_t *
Psyco_Meta3x(PsycoObject *po, void *c_function, int flags,
             const char *arguments, long a1, long a2, long a3);

/******************************************************************/
/*** pyc_data_t implementation and snapshots for the dispatcher ***/

/* construction for non-frozen snapshots */
EXTERNFN void
pyc_data_build(PsycoObject *po, PyObject *merge_points);
PSY_INLINE void
pyc_data_release(pyc_data_t *pyc)
{
    vinfo_xdecref(pyc->val, NULL);
    vinfo_xdecref(pyc->exc, NULL);
    vinfo_xdecref(pyc->tb, NULL);
    Py_XDECREF(pyc->changing_globals);
}
PSY_INLINE void
pyc_data_duplicate(pyc_data_t *target, pyc_data_t *source)
{
    memcpy(target, source, sizeof(pyc_data_t));
    target->exc = NULL;
    target->val = NULL;
    target->tb = NULL;
    Py_XINCREF(target->changing_globals);
}

/* construction for frozen snapshots */
PSY_INLINE size_t
frozen_size(pyc_data_t *pyc)
{
    return offsetof(pyc_data_t, blockstack) + pyc->iblock * sizeof(le_block_t);
}
PSY_INLINE void
frozen_copy(pyc_data_t *target, pyc_data_t *source)
{
    memcpy(target, source, frozen_size(source));
    Py_XINCREF(target->changing_globals);
}

#endif /* _PYCOMPILER_H */
