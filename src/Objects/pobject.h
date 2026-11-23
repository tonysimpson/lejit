/***************************************************************/
/***             Psyco equivalent of object.h                  ***/
/***************************************************************/

#ifndef _PSY_OBJECT_H
#define _PSY_OBJECT_H

#include "../Python/pycompiler.h"

/*#define OB_REFCOUNT         never directly manipulated*/
#define OB_type CPY_FIELD(PyObject, ob_type, NO_PREV_FIELD)
#define FIX_size CPY_NONNEG_FIELD(PyVarObject, ob_size, OB_type)
#define VAR_size FMUT(FIX_size)
#define FIX_signed_size CPY_FIELD(PyVarObject, ob_size, OB_type)
#define VAR_signed_size FMUT(FIX_signed_size)

#define iOB_TYPE FIELD_INDEX(OB_type)
#define iFIX_SIZE FIELD_INDEX(FIX_size)
#define iVAR_SIZE FIELD_INDEX(VAR_size)

/* the field descriptors of the virtual types: TUPLE_ob_item, SLICE_*,
   METHOD_* */
#define TUPLE_ob_item FARRAY(CPY_FIELD(PyTupleObject, ob_item, FIX_size))
#define iTUPLE_OB_ITEM FIELD_INDEX(TUPLE_ob_item)

#define SLICE_start CPY_FIELD(PySliceObject, start, OB_type)
#define iSLICE_START FIELD_INDEX(SLICE_start)
#define SLICE_stop CPY_FIELD(PySliceObject, stop, SLICE_start)
#define iSLICE_STOP FIELD_INDEX(SLICE_stop)
#define SLICE_step CPY_FIELD(PySliceObject, step, SLICE_stop)
#define iSLICE_STEP FIELD_INDEX(SLICE_step)
#define SLICE_TOTAL FIELDS_TOTAL(SLICE_step)

#define METHOD_im_func CPY_FIELD(PyMethodObject, im_func, OB_type)
#define iMETHOD_IM_FUNC FIELD_INDEX(METHOD_im_func)
#define METHOD_im_self CPY_FIELD(PyMethodObject, im_self, METHOD_im_func)
#define iMETHOD_IM_SELF FIELD_INDEX(METHOD_im_self)
#define METHOD_TOTAL FIELDS_TOTAL(METHOD_im_self)

/* common type checkers, rewritten because in Psyco we manipulate type
   objects directly and Python's usual macros insist on taking a regular
   PyObject* whose type is checked. */
#define PyType_TypeCheck(tp1, tp) \
    ((tp1) == (tp) || PyType_IsSubtype((tp1), (tp)))

/* A type whose attributes type_setattro lets be set: a heap type, or where
   CPython has the flag, a type without Py_TPFLAGS_IMMUTABLETYPE (which
   PyType_Ready gives every static type, and an extension's heap type may
   ask for, as array's does). */
PSY_INLINE bool
Psyco_TypeMutable(PyTypeObject *tp)
{
#if LE_HAS_IMMUTABLE_TYPE_FLAG
    return (tp->tp_flags & Py_TPFLAGS_IMMUTABLETYPE) == 0;
#else
    return (tp->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0;
#endif
}

/* A mutable type's slots change whenever a class attribute that defines
   one is set or deleted. An immutable type's never change. */
PSY_INLINE bool
Psyco_SlotsMutable(PyTypeObject *tp)
{
    return Psyco_TypeMutable(tp);
}

/* object_set_class changes an object's type only between mutable types,
   or between subtypes of ModuleType (which is static): only such a known
   type can go stale. */
PSY_INLINE bool
Psyco_ClassMutable(PyTypeObject *tp)
{
    return Psyco_TypeMutable(tp) || PyType_IsSubtype(tp, &PyModule_Type);
}

/* Whether a call this instruction made, which may have run Python code
   (po->pr.class_unchecked), may have changed what was read of a or b when
   compiling: a mutable one's class, or a class's slots. The instruction
   forgets the known types only when it ends, so what is left of it is
   CPython's function, which reads them again. */
PSY_INLINE bool
Psyco_TypesStale(PsycoObject *po, PyTypeObject *a, PyTypeObject *b)
{
    return po->pr.class_unchecked &&
           (Psyco_ClassMutable(a) || Psyco_ClassMutable(b));
}

/* After code that may assign __class__ (po->pr.class_unchecked), forgets
   the run-time values' known types that can change, and the constants'.
   Called between instructions and before a direct call. */
EXTERNFN void
Psyco_ForgetClasses(PsycoObject *po);

/* Before a run-time value's type is promoted: if it was forgotten and
   vi->type_hint holds it, a guard against it, which leaves the type known
   on the likely path. False on error. */
EXTERNFN bool
Psyco_TypeHintGuard(PsycoObject *po, vinfo_t *vi);

/* A constant object whose class can be assigned: its
   type is not read when compiling, but checked at run time
   (Psyco_ConstantType). */
PSY_INLINE bool
Psyco_ConstantClassMutable(vinfo_t *vi)
{
    PyObject *o = (PyObject *)CompileTime_Get(vi->source)->value;
    return o != NULL && Psyco_ClassMutable(Py_TYPE(o));
}

/* The type of such a constant, as psyco_get_field(OB_type) returns it:
   emits the run-time checks and sets *tp to the type they passed for.
   1, or -1 on error. */
EXTERNFN int
Psyco_ConstantType(PsycoObject *po, vinfo_t *vi, PyTypeObject **tp);

/* At the start of a block (dispatcher.c:fpo_build): a snapshot keeps no
   constant's known fields, so the constants' checked types are dropped
   from the state that compiles the block too. */
EXTERNFN void
Psyco_ForgetConstantClasses(PsycoObject *po);
/* the same for one constant 'v' with known fields, and what is under it
   (dispatcher.c:fz_compress meets every constant); psyco_stat_forgotten_types
   counts the types either drops, in checked builds */
EXTERNFN void
Psyco_ForgetConstantClass(PsycoObject *po, vinfo_t *v);
#if ALL_CHECKS
EXTERNVAR long psyco_stat_forgotten_types;
#endif

/* A slot of a heap type, at 'slot' (an address inside the type), for code
   specialised on it: *value is the slot as LeJIT first saw it, and compiled
   code checks that it still is. 1 on that path, 0 on the path where it
   changed (the caller then calls what CPython calls, which reads the slot
   at run time), -1 on error. */
EXTERNFN int
Psyco_HeapSlot(PsycoObject *po, void *slot, void **value);

/* the same for any type's slot: a static type's is read now */
PSY_INLINE int
Psyco_TypeSlot(PsycoObject *po, PyTypeObject *tp, void *slot, void **value)
{
    if (!Psyco_SlotsMutable(tp)) {
        *value = *(void **)slot;
        return 1;
    }
    return Psyco_HeapSlot(po, slot, value);
}

/* _PyType_Lookup(tp, name) on a heap type, as typeobject.c's slot
   functions find a special method: *descr (borrowed, NULL for none) is what
   LeJIT first saw for 'key' (an address of tp's, distinct per name), held by
   the record, so a respawn's replay decides as the compile did; compiled
   code checks the version tag behind a guard shared with the sites of
   (tp, name, *descr). 1 on the path where it passes, 0 on the path where
   the type changed: the caller then calls cimpl_type_lookup_changed (which
   re-arms the guard if the type gives *descr again) and the CPython
   function. -1 on error. */
EXTERNFN int
Psyco_TypeLookup(PsycoObject *po, PyTypeObject *tp, void *key, PyObject *name,
                 PyObject **descr);
EXTERNFN void
cimpl_type_lookup_changed(PyTypeObject *tp, PyObject *name, PyObject *descr);

/* A guard for a message made now that names tp, a mutable type: a
   condition code, true where tp has changed since. */
EXTERNFN condition_code_t
Psyco_TypeNameChanged(PsycoObject *po, PyTypeObject *tp);

/* PyType_IsSubtype(a, b) as Psyco_TypeSlot reads a slot: a heap type's
   mro changes with a __bases__ assignment, so a's tp_mro is recorded as
   first seen (and held), *result comes from it, and compiled code checks
   that it is still a's: 1, 0 on the path where it changed, -1. */
EXTERNFN int
Psyco_TypeIsSubtype(PsycoObject *po, PyTypeObject *a, PyTypeObject *b,
                    bool *result);

#define PsycoSequence_Check(tp) \
    ((tp)->tp_as_sequence && (tp)->tp_as_sequence->sq_item != NULL)

/* Return the type of an object, or NULL in case of exception (typically
   a promotion exception). */
EXTERNFN PyTypeObject *
Psyco_NeedType(PsycoObject *po, vinfo_t *vi);

PSY_INLINE PyTypeObject *
Psyco_FastType(vinfo_t *vi)
{
    /* fast version.  Only call this when you know the type has
       already been loaded by a previous Psyco_NeedType() */
    vinfo_t *vtp = vinfo_getitem(vi, iOB_TYPE);
    if (vtp == NULL) {
        PyObject *o = (PyObject *)CompileTime_Get(vi->source)->value;
        extra_assert(!Psyco_ClassMutable(o->ob_type));
        return o->ob_type;
    }
    else {
        return (PyTypeObject *)CompileTime_Get(vtp->source)->value;
    }
}

/* Is the given object is of the given type (or a subtype of it) ?
   Returns -1 in case of error or if promotion is requested. */
PSY_INLINE int
Psyco_VerifyType(PsycoObject *po, vinfo_t *vi, PyTypeObject *tp)
{
    PyTypeObject *vtp = Psyco_NeedType(po, vi);
    if (vtp == NULL)
        return -1;
    return PyType_TypeCheck(vtp, tp);
}
/* Return the type that the object is known to be of, or NULL if unknown.
   Never fails. */
EXTERNFN PyTypeObject *
Psyco_KnownType(vinfo_t *vi);

/* Use this to assert the type of an object. Do not use unless you are
   sure about it! (e.g. don't use this for integer-computing functions
   if they might return a long in case of overflow) */
PSY_INLINE void
Psyco_AssertType(PsycoObject *po, vinfo_t *vi, PyTypeObject *tp)
{
    psyco_assert_field(po, vi, OB_type, (long)tp);
}

/* Same as integer_non_null() but assumes we are testing a PyObject* pointer,
   so that only compile-time NULLs or run-time pointers from which we have
   not read anything yet can be NULL. Virtual-time pointers are assumed never
   to be NULL. */
PSY_INLINE condition_code_t
object_non_null(PsycoObject *po, vinfo_t *v)
{
    if (is_virtualtime(v->source) || v->array != NullArray)
        return CC_ALWAYS_TRUE;
    else
        return integer_non_null(po, v);
}

/* The following functions are Psyco implementations of common functions
   of the standard interpreter. */
EXTERNFN vinfo_t *
PsycoObject_IsTrue(PsycoObject *po, vinfo_t *vi);

/* Note: DelAttr() is SetAttr() with 'v' set to NULL (and not some vinfo_t
   that would happend to contain zero). */
EXTERNFN vinfo_t *
PsycoObject_GetAttr(PsycoObject *po, vinfo_t *o, vinfo_t *attr_name);
EXTERNFN bool
PsycoObject_SetAttr(PsycoObject *po, vinfo_t *o, vinfo_t *attr_name,
                    vinfo_t *v);
#if ALL_CHECKS
/* inline attribute stores that missed (called the C helper), and misses
   that changed their (class, name) cache: lejit._core._stats() */
EXTERNVAR long psyco_stat_store_misses;
EXTERNVAR long psyco_stat_store_type_changes;
EXTERNVAR long psyco_stat_store_refreshes;
/* failed load guards, and those that re-armed some sites */
EXTERNVAR long psyco_stat_load_type_changes;
EXTERNVAR long psyco_stat_load_repairs;
#endif
#if LE_CODESTATS
/* load guards: see pobject.c */
EXTERNVAR long psyco_cs_load[8];
EXTERNVAR bool psyco_cs_load_respawn;
#endif
EXTERNFN vinfo_t *
PsycoObject_GenericGetAttr(PsycoObject *po, vinfo_t *obj, vinfo_t *vname);

EXTERNFN vinfo_t *
PsycoObject_RichCompare(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op);
/* Objects/object.c:PyObject_Hash; the result is a Py_hash_t word */
EXTERNFN vinfo_t *
PsycoObject_Hash(PsycoObject *po, vinfo_t *v);

EXTERNFN vinfo_t *
PsycoObject_RichCompareBool(PsycoObject *po, vinfo_t *v, vinfo_t *w, int op);

/* a quick way to specify the type of the object returned by an operation
   when it is known, without having to go into all the details of the
   operation itself (be careful, you must be *sure* of the return type): */

#define DEF_KNOWN_RET_TYPE_1(cname, op, flags, knowntype)         \
    DEF_KNOWN_RET_TYPE_internal(cname, knowntype,                 \
                                (PsycoObject * po, vinfo_t * v1), \
                                (po, op, flags, "v", v1))
#define DEF_KNOWN_RET_TYPE_2(cname, op, flags, knowntype)                 \
    DEF_KNOWN_RET_TYPE_internal(                                          \
        cname, knowntype, (PsycoObject * po, vinfo_t * v1, vinfo_t * v2), \
        (po, op, flags, "vv", v1, v2))
#define DEF_KNOWN_RET_TYPE_3(cname, op, flags, knowntype)             \
    DEF_KNOWN_RET_TYPE_internal(                                      \
        cname, knowntype,                                             \
        (PsycoObject * po, vinfo_t * v1, vinfo_t * v2, vinfo_t * v3), \
        (po, op, flags, "vvv", v1, v2, v3))
#define DEF_KNOWN_RET_TYPE_4(cname, op, flags, knowntype)                   \
    DEF_KNOWN_RET_TYPE_internal(cname, knowntype,                           \
                                (PsycoObject * po, vinfo_t * v1,            \
                                 vinfo_t * v2, vinfo_t * v3, vinfo_t * v4), \
                                (po, op, flags, "vvvv", v1, v2, v3, v4))

#define DEF_KNOWN_RET_TYPE_internal(cname, knowntype, fargs, gargs) \
    static vinfo_t *cname fargs                                     \
    {                                                               \
        vinfo_t *result = psyco_generic_call gargs;                 \
        if (result != NULL && !IS_NOTIMPLEMENTED(result)) {         \
            Psyco_AssertType(po, result, knowntype);                \
        }                                                           \
        return result;                                              \
    }

/* 'true' unless 'x' is exactly the special 'not implemented' value
   built by psyco_generic_call with CfPyErrNotImplemented */
#define IS_IMPLEMENTED(x) ((x) == NULL || !IS_NOTIMPLEMENTED(x))

/* is 'x' the special 'not implemented' value? */
#define IS_NOTIMPLEMENTED(x) \
    ((x)->source == CompileTime_NewSk(&psyco_skNotImplemented))

/* le_dictkeys_t and le_dictentry_t, the dict keys and entries that aren't
   in the installed headers, are copied from the source into psyco.h;
   the entries follow the indices. */

/* Inline attribute stores and loads: a cache per (class, name) says where
   the name sits in keys of one size; it holds no reference to any keys.
   The code emitted by x64/ipyencoding.c (psyco_inline_store_begin,
   psyco_inline_load) reads it at run time and checks it against the live
   keys of the dict it uses, so a wrong cache only costs a miss. */
typedef struct {
    Py_ssize_t shape;  /* of the keys this was computed for (le_dict_shape);
                          0: none */
    Py_ssize_t kword;  /* entries[ix].me_key in such keys, in words */
    Py_ssize_t ix;     /* the name's index in them */
    PyObject *name;    /* interned; held by the table's key */
    uint64_t *version; /* &le_dict_version */
    long refreshes;    /* misses that changed the cache */
#if LE_HAS_MANAGED_VALUES_POINTER
    Py_ssize_t nentries; /* where the keys are a managed dict's class keys,
                            which only grow: their entry count when the
                            name was last found absent; -1: never */
#endif
} store_cache_t;

/* the slots of the keys' hash table */
PSY_INLINE Py_ssize_t
le_dict_size(le_dictkeys_t *k)
{
#if LE_HAS_DICT_KEYS_KIND
    return (Py_ssize_t)1 << k->dk_log2_size;
#else
    return k->dk_size;
#endif
}

/* the entries hold their key's hash (le_dictentry_t), or not
   (le_dictunicodeentry_t: every key is an exact str, which keeps its
   hash) */
PSY_INLINE bool
le_dict_hashed(le_dictkeys_t *k)
{
#if LE_HAS_DICT_KEYS_KIND
    return k->dk_kind == DICT_KEYS_GENERAL;
#else
    return true;
#endif
}

/* the entries of dict keys, after their indices (dictobject.c:DK_ENTRIES) */
PSY_INLINE char *
le_dict_entries(le_dictkeys_t *k)
{
#if LE_HAS_DICT_KEYS_KIND
    return k->dk_indices + ((size_t)1 << k->dk_log2_index_bytes);
#else
    Py_ssize_t ixsize = k->dk_size <= 0xff         ? 1
                        : k->dk_size <= 0xffff     ? 2
                        : k->dk_size <= 0xffffffff ? 4
                                                   : 8;
    return k->dk_indices + k->dk_size * ixsize;
#endif
}

/* entry ix's key, its value in the next word */
_Static_assert(offsetof(le_dictentry_t, me_value) ==
                   offsetof(le_dictentry_t, me_key) + sizeof(PyObject *),
               "an entry's value follows its key");
PSY_INLINE PyObject **
le_dict_key_slot(le_dictkeys_t *k, Py_ssize_t ix)
{
#if LE_HAS_DICT_KEYS_KIND
    _Static_assert(
        offsetof(le_dictunicodeentry_t, me_value) ==
            offsetof(le_dictunicodeentry_t, me_key) + sizeof(PyObject *),
        "an entry's value follows its key");
    if (!le_dict_hashed(k))
        return &((le_dictunicodeentry_t *)le_dict_entries(k))[ix].me_key;
#endif
    return &((le_dictentry_t *)le_dict_entries(k))[ix].me_key;
}

/* the hash of entry ix's key */
PSY_INLINE Py_hash_t
le_dict_hash(le_dictkeys_t *k, Py_ssize_t ix)
{
    if (!le_dict_hashed(k))
        return ((PyASCIIObject *)*le_dict_key_slot(k, ix))->hash;
    return ((le_dictentry_t *)le_dict_entries(k))[ix].me_hash;
}

/* What keys of the same shape share: the size, and so where the entries
   are, and their layout. Emitted code compares it (x64/ipyencoding.c,
   KEYS_SHAPE_CMP): dk_size, or dk_log2_index_bytes and dk_kind as one
   word, the index bytes growing with the size. Never -1. */
PSY_INLINE long
le_dict_shape(le_dictkeys_t *k)
{
#if LE_HAS_DICT_KEYS_KIND
    return k->dk_log2_index_bytes | (long)k->dk_kind << 8;
#else
    return k->dk_size;
#endif
}

/* ma_version_tag for the dict stores LeJIT makes itself: CPython's own
   counter never reaches 2^63, where this one starts, so tags from the two
   never meet. Where a tag's low 8 bits say which watchers watch the dict,
   LeJIT stores only into a dict none watch (le_dict_watched), and steps
   its counter as CPython does, past those bits. */
EXTERNVAR uint64_t le_dict_version;
#if LE_HAS_DICT_WATCHER_BITS
#define LE_DICT_WATCHER_MASK 0xff
#define LE_DICT_VERSION_STEP 0x100
#else
#define LE_DICT_WATCHER_MASK 0
#define LE_DICT_VERSION_STEP 1
#endif

/* a watcher watches the dict: a store must go through CPython, which sends
   the watchers the event */
PSY_INLINE bool
le_dict_watched(PyDictObject *mp)
{
    return (mp->LE_DICT_TAG_FIELD & LE_DICT_WATCHER_MASK) != 0;
}

#endif /* _PSY_OBJECT_H */
