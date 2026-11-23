/*
 * Python-aware emitters: Py_INCREF/Py_DECREF (calling tp_dealloc when the
 * count reaches zero) and the prologue and epilogue for a function's frame
 * locals. This is CPython object layout and C-API policy living in the
 * backend; in a tidied design it belongs in a generated Python layer.
 * The frame locals are the words of Python 2's per-frame exception state
 * (f_exc_*), which nothing sets or reads.
 */
/***************************************************************/
/***     Processor- and language-dependent code producers      ***/
/***************************************************************/

#ifndef _IPYENCODING_H
#define _IPYENCODING_H

#include "../Objects/pobject.h"
#include "../dispatcher.h"
#include "../processor.h"
#include "iencoding.h"

/* emit the equivalent of the Py_INCREF() macro */
/* the PyObject* is stored in the register 'rg' */
/* XXX if Py_REF_DEBUG is set (Python debug mode), the
       following will not properly update _Py_RefTotal.
       Don't trust _Py_RefTotal with Psyco.     */
#define INC_OB_REFCNT(rg)           \
    do {                            \
        NEED_CC_REG(rg);            \
        INC_OB_REFCNT_internal(rg); \
    } while (0)
/* same as above, preserving the cc */
#define INC_OB_REFCNT_CC(rg)              \
    do {                                  \
        bool _save_ccreg = HAS_CCREG(po); \
        if (_save_ccreg)                  \
            PUSH_CC();                    \
        INC_OB_REFCNT_internal(rg);       \
        if (_save_ccreg)                  \
            POP_CC();                     \
    } while (0)
#if LE_HAS_IMMORTAL_OBJECTS
_Static_assert(offsetof(PyObject, ob_refcnt) == 0,
               "LeJIT: the refcount is at the object's address");
#if LE_HAS_REFCOUNT_32
/* as Py_INCREF: the 32-bit count goes up by one, unless it is
   _Py_IMMORTAL_INITIAL_REFCNT or more (an immortal's) */
_Static_assert(_Py_IMMORTAL_INITIAL_REFCNT == 0xC0000000,
               "LeJIT: an immortal's count is 3 << 30");
#define INC_OB_REFCNT_internal(rg)       \
    do {                                 \
        CMP_I32_DA((int)0xC0000000, rg); \
        BEGIN_SHORT_COND_JUMP(8, CC_NB); \
        ADD_DA_I8(rg, 1);                \
        END_SHORT_JUMP(8);               \
    } while (0)
/* a mortal's count, which Py_DECREF changes as 32 bits */
#define LE_SUB_OB_REFCNT(rg) SUB_DA_I8(rg, 1)
#else
/* as Py_INCREF: the count's low 32 bits go up by one, unless they are all
   ones (an immortal), which the carry puts back */
#define INC_OB_REFCNT_internal(rg) \
    do {                           \
        ADD_DA_I8(rg, 1);          \
        SBB_DA_I8(rg, 0);          \
    } while (0)
/* a mortal's count, whose high 32 bits are 0 */
#define LE_SUB_OB_REFCNT(rg) SUB_A_I8(rg, 1)
#endif

/* Py_INCREF() for a compile-time-known 'pyobj': nothing for an immortal,
   which stays one */
#define INC_KNOWN_OB_REFCNT(pyobj)                         \
    do {                                                   \
        if (!_Py_IsImmortal(pyobj)) {                      \
            NEED_CC();                                     \
            MOV_R_I(REG_TRANSIENT_1, &(pyobj)->ob_refcnt); \
            INC_OB_REFCNT_internal(REG_TRANSIENT_1);       \
        }                                                  \
    } while (0)

/* Py_DECREF() for a compile-time 'pyobj' assuming counter cannot reach zero */
#define DEC_KNOWN_OB_REFCNT_NZ(pyobj)                      \
    do {                                                   \
        if (!_Py_IsImmortal(pyobj)) {                      \
            NEED_CC();                                     \
            MOV_R_I(REG_TRANSIENT_1, &(pyobj)->ob_refcnt); \
            DEC_OB_REFCNT_NZ_internal(REG_TRANSIENT_1);    \
        }                                                  \
    } while (0)

/* like DEC_OB_REFCNT() but assume the reference counter cannot reach zero */
#define DEC_OB_REFCNT_NZ(rg)           \
    do {                               \
        NEED_CC_REG(rg);               \
        DEC_OB_REFCNT_NZ_internal(rg); \
    } while (0)
/* as Py_DECREF: an immortal (its count's low 32 bits negative) is left
   alone */
#define DEC_OB_REFCNT_NZ_internal(rg)   \
    do {                                \
        CMP_I8_DA(0, rg);               \
        BEGIN_SHORT_COND_JUMP(9, CC_S); \
        LE_SUB_OB_REFCNT(rg);           \
        END_SHORT_JUMP(9);              \
    } while (0)
#else
#define INC_OB_REFCNT_internal(rg)                             \
    do {                                                       \
        if (CPY_OFFSET(PyObject, ob_refcnt) == 0) {            \
            ADD_A_I8(rg, 1);                                   \
        }                                                      \
        else {                                                 \
            ADD_O8_I8(rg, CPY_OFFSET(PyObject, ob_refcnt), 1); \
        }                                                      \
    } while (0)

/* Py_INCREF() for a compile-time-known 'pyobj' */
#define INC_KNOWN_OB_REFCNT(pyobj)                     \
    do {                                               \
        NEED_CC();                                     \
        MOV_R_I(REG_TRANSIENT_1, &(pyobj)->ob_refcnt); \
        ADD_A_I8(REG_TRANSIENT_1, 1);                  \
    } while (0)

/* Py_DECREF() for a compile-time 'pyobj' assuming counter cannot reach zero */
#define DEC_KNOWN_OB_REFCNT_NZ(pyobj)                  \
    do {                                               \
        NEED_CC();                                     \
        MOV_R_I(REG_TRANSIENT_1, &(pyobj)->ob_refcnt); \
        SUB_A_I8(REG_TRANSIENT_1, 1);                  \
    } while (0)

/* like DEC_OB_REFCNT() but assume the reference counter cannot reach zero */
#define DEC_OB_REFCNT_NZ(rg) \
    do {                     \
        NEED_CC_REG(rg);     \
        SUB_A_I8(rg, 1);     \
    } while (0)

#endif

/* internal utilities for the macros below */
EXTERNFN code_t *
decref_dealloc_calling(code_t *code, PsycoObject *po, reg_t rg, destructor fn);

/* the equivalent of Py_DECREF(): it calls tp_dealloc itself where that is
   all _Py_Dealloc would do, and _Py_Dealloc where it does more */
#if defined(Py_TRACE_REFS) || LE_HAS_DEALLOC_HOOKS
#define DEC_OB_REFCNT(rg) \
    (code = decref_dealloc_calling(code, po, rg, _Py_Dealloc))
#else
#define DEC_OB_REFCNT(rg) (code = decref_dealloc_calling(code, po, rg, NULL))
#endif

/* the equivalent of Py_DECREF() when we know the type of the object
   (assuming that tp_dealloc never changes for a given type) */
#if defined(Py_TRACE_REFS) || LE_HAS_DEALLOC_HOOKS
#define DEC_OB_REFCNT_T(rg, type) \
    (code = decref_dealloc_calling(code, po, rg, _Py_Dealloc))
#else
#define DEC_OB_REFCNT_T(rg, type) \
    (code = decref_dealloc_calling(code, po, rg, (type)->tp_dealloc))
#endif

/***************************************************************/
/***   generic reference counting functions                  ***/

/* emit Py_INCREF(v) for run-time v */
PSY_INLINE void
psyco_incref_rt(PsycoObject *po, vinfo_t *v)
{
    reg_t rg;
    BEGIN_CODE
    RTVINFO_IN_REG(v);
    rg = RUNTIME_REG(v);
    INC_OB_REFCNT(rg);
    END_CODE
}

/* emit Py_INCREF(v) for non-virtual v */
PSY_INLINE void
psyco_incref_nv(PsycoObject *po, vinfo_t *v)
{
    if (!is_compiletime(v->source))
        psyco_incref_rt(po, v);
    else {
        BEGIN_CODE
        INC_KNOWN_OB_REFCNT((PyObject *)CompileTime_Get(v->source)->value);
        END_CODE
    }
}

/* a dealloc can run a __del__, except these types' */
PSY_INLINE void
decref_may_run_python(PsycoObject *po, PyTypeObject *tp)
{
    if (!(tp == &PyUnicode_Type || tp == &PyBool_Type || tp == &PyFloat_Type ||
          tp == &PyLong_Type || tp == Py_None->ob_type || tp == &PyRange_Type))
        po->pr.class_unchecked = 1;
}

/* emit Py_DECREF(v) for run-time v. Used by vcompiler.c when releasing a
   run-time vinfo_t holding a reference to a Python object. */
PSY_INLINE void
psyco_decref_rt(PsycoObject *po, vinfo_t *v)
{
    PyTypeObject *tp = Psyco_KnownType(v);
    reg_t rg;
    decref_may_run_python(po, tp);
    BEGIN_CODE
    RTVINFO_IN_REG(v);
    rg = RUNTIME_REG(v);
    if (tp != NULL)
        DEC_OB_REFCNT_T(rg, tp);
    else
        DEC_OB_REFCNT(rg);
    END_CODE
}

/* emit Py_XDECREF(v) for run-time v, a word LeJIT holds no reference to that
   may be NULL at run time: the old content of a field the C code has just
   overwritten, whose reference it now releases (STORE_DEREF's cell). The
   register load and the cc save are made before the NULL test, so both
   paths leave the compiler's state the same. */
PSY_INLINE void
psyco_xdecref_rt(PsycoObject *po, vinfo_t *v)
{
    PyTypeObject *tp = Psyco_KnownType(v);
    reg_t rg;
    decref_may_run_python(po, tp);
    BEGIN_CODE
    RTVINFO_IN_REG(v);
    rg = RUNTIME_REG(v);
    NEED_CC_REG(rg);
    TEST_R_R(rg, rg);
    BEGIN_SHORT_COND_JUMP(1, CC_E);
    if (tp != NULL)
        DEC_OB_REFCNT_T(rg, tp);
    else
        DEC_OB_REFCNT(rg);
    END_SHORT_JUMP(1);
    END_CODE
}

/* emit Py_DECREF(o) for a compile-time o */
PSY_INLINE void
psyco_decref_c(PsycoObject *po, PyObject *o)
{
    BEGIN_CODE
    DEC_KNOWN_OB_REFCNT_NZ(o);
    END_CODE
}

/* to store a new reference to a Python object into a memory structure,
   use psyco_put_field() or psyco_put_field_array() to store the value
   proper and then one of the following two functions to adjust the
   reference counter: */

/* normal case */
EXTERNFN void
decref_create_new_ref(PsycoObject *po, vinfo_t *w);

/* if 'w' is supposed to be freed soon, this function tries (if possible)
   to move an eventual Python reference owned by 'w' to the memory
   structure.  This avoids a Py_INCREF()/Py_DECREF() pair.
   Returns 'true' if the reference was successfully transfered;
   'false' does not mean failure. */
EXTERNFN bool
decref_create_new_lastref(PsycoObject *po, vinfo_t *w);

/* called by psyco_emit_header() */
#define INITIALIZE_FRAME_LOCALS(nframelocal)                              \
    do {                                                                  \
        if (RBP_IS_RESERVED) {                                            \
            PUSH_R(REG_X64_RBP);                                          \
            psyco_inc_stackdepth(po);                                     \
            MOV_R_R(REG_X64_RBP, REG_X64_RSP);                            \
        }                                                                 \
        STACK_CORRECTION(sizeof(long) * ((nframelocal) - 1));             \
        PUSH_IMMED(0); /* the catcher word (pycompiler.c:catcher_word) */ \
    } while (0)

#define WRITE_FRAME_EPILOGUE(retval, nframelocal)                             \
    do {                                                                      \
        /* load the return value into EAX for regular functions, EBX for      \
           functions with a prologue */                                       \
        if (retval != SOURCE_DUMMY) {                                         \
            reg_t rg = nframelocal > 0 ? REG_ANY_CALLEE_SAVED                 \
                                       : REG_FUNCTIONS_RETURN;                \
            LOAD_REG_FROM(retval, rg);                                        \
        }                                                                     \
                                                                              \
        if (nframelocal > 0) {                                                \
            /* psyco_emit_header() was used; first clear the stack only up to \
               and not including the frame-local data */                      \
            int framelocpos =                                                 \
                getstack(LOC_CONTINUATION->array->items[0]->source);          \
            STACK_CORRECTION(framelocpos - po->stack_depth);                  \
            po->stack_depth = framelocpos;                                    \
                                                                              \
            MOV_R_R(REG_FUNCTIONS_RETURN, REG_ANY_CALLEE_SAVED);              \
        }                                                                     \
    } while (0)

/* retpos is position in stack of the return address pushed by call */
/* the stack must be cleared up this */
#define FUNCTION_RET(retpos)                                        \
    do {                                                            \
        int post_return_stack_depth = 0;                            \
        int pre_return_stack_correction = retpos - po->stack_depth; \
        int return_depth = sizeof(long) + INITIAL_STACK_DEPTH;      \
        if (RBP_IS_RESERVED) {                                      \
            pre_return_stack_correction += sizeof(long);            \
        }                                                           \
        STACK_CORRECTION(pre_return_stack_correction);              \
        po->stack_depth += pre_return_stack_correction;             \
        if (RBP_IS_RESERVED) {                                      \
            POP_R(REG_X64_RBP);                                     \
            psyco_dec_stackdepth(po);                               \
        }                                                           \
        post_return_stack_depth = po->stack_depth - return_depth;   \
        if (post_return_stack_depth >= 0x8000) {                    \
            POP_R(REG_TRANSIENT_1);                                 \
            STACK_CORRECTION(-post_return_stack_depth);             \
            PUSH_R(REG_TRANSIENT_1);                                \
            po->stack_depth = return_depth;                         \
            RET();                                                  \
        }                                                           \
        else {                                                      \
            RET_N(post_return_stack_depth);                         \
        }                                                           \
        po->stack_depth = 0;                                        \
    } while (0)

/* The inline attribute store into a split instance dict, or into a
   managed dict's values, behind a type guard emitted before it. Hand-allocated
   in the caller-saved registers, which hold no tracked values, so the
   compiler's state is the same on every path through it.
   psyco_inline_store_begin emits the checks and the store, then the miss path:
   'call miss_glue' (miss_glue(obj, value, cache) in RDI, RSI, RDX, returning
   an int, nonzero on error) and 'test eax, eax'; the caller then emits the
   error check (runtime_condition_f with CC_NE) and calls
   psyco_inline_store_end, which points the store's exits past that check. */
typedef enum {
    STORE_GC_NONE, /* the value's type is known and not GC */
    STORE_GC_DICT, /* it is known and GC: miss if the dict is untracked */
    STORE_GC_FULL, /* unknown: miss if the dict is untracked and it is GC */
} store_gc_t;
/* the inline store's jumps to its end: a third skips an immortal's decref */
#define STORE_DONE_JUMPS (2 + LE_HAS_IMMORTAL_OBJECTS)
typedef struct {
    vinfo_t *obj, *value; /* run-time or compile-time */
    long dictoffset;      /* the type's tp_dictoffset, > 0, or with 'keys'
                             the offset of the instance's values pointer */
    void *keys; /* a managed dict's class keys (never replaced), whose
                   values are at the values pointer, which has none if
                   the dict was made; NULL: the dict is at dictoffset */
    store_cache_t *cache;
    store_gc_t gc;
    store_gc_t dict_gc; /* as gc, for a store into a combined dict */
    bool fill; /* inline the store into an empty slot: a dict's next one */
    code_t *miss_glue;
    code_t *done[STORE_DONE_JUMPS]; /* for psyco_inline_store_end to point */
} inline_store_t;
EXTERNFN void
psyco_inline_store_begin(PsycoObject *po, inline_store_t *s);
EXTERNFN void
psyco_inline_store_end(PsycoObject *po, inline_store_t *s);

/* The inline attribute load from an instance dict, behind a type guard
   emitted before it, hand-allocated like the store. Its result, a
   borrowed pointer or NULL, is the vinfo returned. 'absent' is the
   load expected to find nothing (a class attribute exists): it runs the
   dict's own probe sequence (lookdict_split's) and is done at an empty
   index slot; otherwise the name is read from the dict's values at the
   index the cache gives, if the keys hold it there. With 'keys' (a
   managed dict's), that is the only form, and keys whose entry count is
   the cache's 'nentries' don't hold the name. Anything else calls
   miss_glue(dict, cache), or with 'keys' miss_glue(obj, cache), which
   returns the same. Nothing depends on which keys a dict has when the
   code is emitted. */
typedef struct {
    vinfo_t *obj;         /* run-time or compile-time */
    long dictoffset;      /* as inline_store_t's */
    void *keys;           /* as inline_store_t's */
    store_cache_t *cache; /* (class, name), shared with the stores */
    Py_hash_t hash;       /* the name's */
    bool absent;
    code_t *miss_glue;
} inline_load_t;
EXTERNFN vinfo_t *
psyco_inline_load(PsycoObject *po, inline_load_t *s);

/* a type guard: the condition code for "tp's tp_flags or tp_version_tag
   differs from *flags or *tag" */
EXTERNFN condition_code_t
psyco_type_guard(PsycoObject *po, PyTypeObject *tp, unsigned long *flags,
                 unsigned long *tag);

/* The shape of dict keys (Objects/pobject.h:le_dict_shape) as emitted
   code compares it with an immediate, which a patch may rewrite */
#if LE_HAS_DICT_KEYS_KIND
typedef uint16_t shape_imm_t;
#else
typedef dword_t shape_imm_t;
#endif

/* A cached global's guard (Python/pycompiler.c:load_global_cached): the
   condition code for "changed". With 'entry', the keys of globals must
   have the shape 'shape' and hold name and value at the key 'offset'
   bytes from their start and the word after it, both written into the
   code, where the at_ fields say.
   With 'absent', the name must be absent from globals and the version tag
   of builtins tags[1], written into the code where *patch says. Otherwise the
   version tags of globals and builtins must be tags[0] and tags[1], read
   from 'tags'. All forms change the compiler's state in the same way, so
   a replay may emit any. */
typedef struct {
    shape_imm_t *at_shape; /* in the code */
    dword_t *at_key, *at_value;
} global_patch_t;
/* The name absent from globals, as the dict's own probe walks it: keys of
   the shape 'shape', and the 'n' distinct index slots it walks before the
   first empty one holding what they held at compile time, each a dummy or an
   entry whose hash differs from the name's in its low 32 bits. Then any lookup
   of the name in such keys stops at that empty slot. */
#define GLOBAL_PROBE_MAX 4
/* room for the longest absent guard, and a jump */
#define GLOBAL_ABSENT_CODE_MAX 256
typedef struct {
    long shape;
    int width; /* of an index: 1, 2 or 4 bytes */
    int n;
    bool hashed; /* the hash is in the entry, else in its key, a str */
    long slot[GLOBAL_PROBE_MAX + 1]; /* from the keys; the last is empty */
    long index[GLOBAL_PROBE_MAX];    /* what each slot holds */
    long hash_at[GLOBAL_PROBE_MAX];  /* an entry's me_hash, or its key,
                                        from the keys */
    int32_t hash[GLOBAL_PROBE_MAX];  /* its low 32 bits */
} global_absent_t;
/* false if the probe isn't one this guard can show */
EXTERNFN bool
psyco_global_absent(PyObject *globals, PyObject *name, global_absent_t *a);
/* Where an absent guard is: the code from 'start' up to 'end', where the
   code goes on with flags clear if the name is still absent, and 'fresh',
   NULL or the memory the guard was moved to when shown again for keys it
   didn't fit. */
typedef struct {
    code_t *start, *end, *fresh;
} global_absent_patch_t;
EXTERNFN condition_code_t
psyco_global_guard(PsycoObject *po, PyObject *globals, PyObject *builtins,
                   PyObject *name, PyObject *value, global_patch_t *entry,
                   long shape, long offset, const global_absent_t *absent,
                   global_absent_patch_t *patch, uint64_t *tags);
/* An absent guard shown again for the keys globals have now and builtins'
   tag now (the keys resized, a name inserted into the slots it walked, or
   builtins written), so that it passes again while the name stays absent:
   rewritten in place where it fits, else in fresh memory that the start
   jumps to. A probe that can't be shown is replaced by globals' tag, so
   the guard passes until globals or builtins are written. */
EXTERNFN void
psyco_global_absent_rearm(global_absent_patch_t *patch, PyObject *globals,
                          PyObject *builtins, PyObject *name);
/* the entry moved: rewrites the code's shape and displacements (with no
   at_value, the shape and key's) */
EXTERNFN void
psyco_global_guard_move(global_patch_t *entry, long shape, long offset);
/* jumps to 'stub' unless the keys of globals have the shape 'shape' and
   hold the name 'offset' bytes from their start, both written into the
   code where check's at_shape and at_key say */
EXTERNFN void
psyco_global_stale(PsycoObject *po, PyObject *globals, PyObject *name,
                   global_patch_t *check, long shape, long offset,
                   code_t *stub);

/* A global loaded at run time from the entry of globals that holds the
   name, the shape and offset written into the code as the guard's entry
   form: keys of that shape holding the name there give its value, increfed;
   anything else calls miss_glue(arg), which returns a new reference or
   NULL, and may move the entry. The result is in a register, tested
   (zero: the miss's error), and not yet owned: the caller checks the
   condition CC_E, then adds the reference. */
EXTERNFN vinfo_t *
psyco_global_load(PsycoObject *po, PyObject *globals, PyObject *name,
                  global_patch_t *entry, long shape, long offset,
                  code_t *miss_glue, void *arg);

EXTERNFN void
function_return(PsycoObject *po, Source retval, int nframelocal, long retpos);

#endif /* _IPYENCODING_H */
