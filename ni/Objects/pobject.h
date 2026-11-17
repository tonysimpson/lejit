 /***************************************************************/
/***             Psyco equivalent of object.h                  ***/
 /***************************************************************/

#ifndef _PSY_OBJECT_H
#define _PSY_OBJECT_H


#include "../Python/pycompiler.h"


/*#define OB_REFCOUNT         never directly manipulated*/
#define OB_type      DEF_FIELD(PyObject, PyTypeObject*, ob_type, NO_PREV_FIELD)
#define FIX_size     NONNEG_FIELD(PyVarObject,     Py_ssize_t, ob_size, OB_type)
#define VAR_size     FMUT(FIX_size)
#define FIX_signed_size DEF_FIELD(PyVarObject,     Py_ssize_t, ob_size, OB_type)
#define VAR_signed_size FMUT(FIX_signed_size)

#define iOB_TYPE   FIELD_INDEX(OB_type)
#define iFIX_SIZE  FIELD_INDEX(FIX_size)
#define iVAR_SIZE  FIELD_INDEX(VAR_size)


/* common type checkers, rewritten because in Psyco we manipulate type
   objects directly and Python's usual macros insist on taking a regular
   PyObject* whose type is checked. */
# define PyType_TypeCheck(tp1, tp)  	\
	((tp1) == (tp) || PyType_IsSubtype((tp1), (tp)))

#define PsycoIter_Check(tp) \
     ((tp)->tp_iternext != NULL && \
      (tp)->tp_iternext != &_PyObject_NextNotImplemented)

/* A heap type's slots change whenever a class attribute that defines one
   is set or deleted (KNOWN_BUGS F50). A static type's never change. */
PSY_INLINE bool Psyco_SlotsMutable(PyTypeObject* tp) {
	return (tp->tp_flags & Py_TPFLAGS_HEAPTYPE) != 0;
}

/* A slot of a heap type, at 'slot' (an address inside the type), for code
   specialised on it: *value is the slot as Ni first saw it, and compiled
   code checks that it still is. 1 on that path, 0 on the path where it
   changed (the caller then calls what CPython calls, which reads the slot
   at run time), -1 on error. */
EXTERNFN int Psyco_HeapSlot(PsycoObject* po, void* slot, void** value);

/* the same for any type's slot: a static type's is read now */
PSY_INLINE int Psyco_TypeSlot(PsycoObject* po, PyTypeObject* tp, void* slot,
			      void** value) {
	if (!Psyco_SlotsMutable(tp)) {
		*value = *(void**)slot;
		return 1;
	}
	return Psyco_HeapSlot(po, slot, value);
}

/* PyType_IsSubtype(a, b) as Psyco_TypeSlot reads a slot: a heap type's
   mro changes with a __bases__ assignment, so a's tp_mro is recorded as
   first seen (and held), *result comes from it, and compiled code checks
   that it is still a's: 1, 0 on the path where it changed, -1. */
EXTERNFN int Psyco_TypeIsSubtype(PsycoObject* po, PyTypeObject* a,
				 PyTypeObject* b, bool* result);

#define PsycoSequence_Check(tp) \
	((tp)->tp_as_sequence && (tp)->tp_as_sequence->sq_item != NULL)


/* Return the type of an object, or NULL in case of exception (typically
   a promotion exception). */
EXTERNFN PyTypeObject* Psyco_NeedType(PsycoObject* po, vinfo_t* vi);

PSY_INLINE PyTypeObject* Psyco_FastType(vinfo_t* vi) {
	/* fast version.  Only call this when you know the type has
	   already been loaded by a previous Psyco_NeedType() */
	vinfo_t* vtp = vinfo_getitem(vi, iOB_TYPE);
	if (vtp == NULL) {
		PyObject* o = (PyObject*) CompileTime_Get(vi->source)->value;
		return o->ob_type;
	}
	else {
		return (PyTypeObject*) CompileTime_Get(vtp->source)->value;
	}
}

/* Is the given object is of the given type (or a subtype of it) ?
   Returns -1 in case of error or if promotion is requested. */
PSY_INLINE int Psyco_VerifyType(PsycoObject* po, vinfo_t* vi, PyTypeObject* tp) {
	PyTypeObject* vtp = Psyco_NeedType(po, vi);
	if (vtp == NULL)
		return -1;
	return PyType_TypeCheck(vtp, tp);
}
/* Return the type that the object is known to be of, or NULL if unknown.
   Never fails. */
EXTERNFN PyTypeObject* Psyco_KnownType(vinfo_t* vi);

/* Use this to assert the type of an object. Do not use unless you are
   sure about it! (e.g. don't use this for integer-computing functions
   if they might return a long in case of overflow) */
PSY_INLINE void Psyco_AssertType(PsycoObject* po, vinfo_t* vi, PyTypeObject* tp) {
	psyco_assert_field(po, vi, OB_type, (long) tp);
}

/* Same as integer_non_null() but assumes we are testing a PyObject* pointer,
   so that only compile-time NULLs or run-time pointers from which we have
   not read anything yet can be NULL. Virtual-time pointers are assumed never
   to be NULL. */
PSY_INLINE condition_code_t object_non_null(PsycoObject* po, vinfo_t* v) {
	if (is_virtualtime(v->source) || v->array != NullArray)
		return CC_ALWAYS_TRUE;
	else
		return integer_non_null(po, v);
}


/* The following functions are Psyco implementations of common functions
   of the standard interpreter. */
EXTERNFN vinfo_t* PsycoObject_IsTrue(PsycoObject* po, vinfo_t* vi);
EXTERNFN vinfo_t* PsycoObject_Repr(PsycoObject* po, vinfo_t* vi);

/* Note: DelAttr() is SetAttr() with 'v' set to NULL (and not some vinfo_t
   that would happend to contain zero). */
EXTERNFN vinfo_t* PsycoObject_GetAttr(PsycoObject* po, vinfo_t* o,
                                      vinfo_t* attr_name);
EXTERNFN bool PsycoObject_SetAttr(PsycoObject* po, vinfo_t* o,
                                  vinfo_t* attr_name, vinfo_t* v);
/* inline attribute stores that missed (called the C helper), and misses
   that changed their (class, name) cache: ni._stats() */
EXTERNVAR long psyco_stat_store_misses;
EXTERNVAR long psyco_stat_store_type_changes;
EXTERNVAR long psyco_stat_store_refreshes;
/* failed load guards, and those that re-armed some sites (D60) */
EXTERNVAR long psyco_stat_load_type_changes;
EXTERNVAR long psyco_stat_load_repairs;
#if NI_CODESTATS
/* load guards (QUESTIONS 41, tools/loadguards.py): see pobject.c */
EXTERNVAR long psyco_cs_load[8];
EXTERNVAR bool psyco_cs_load_respawn;
#endif
EXTERNFN vinfo_t* PsycoObject_GenericGetAttr(PsycoObject* po, vinfo_t* obj,
                                             vinfo_t* vname);

EXTERNFN vinfo_t* PsycoObject_RichCompare(PsycoObject* po, vinfo_t* v,
					  vinfo_t* w, int op);
/* Objects/object.c:PyObject_Hash; the result is a Py_hash_t word */
EXTERNFN vinfo_t* PsycoObject_Hash(PsycoObject* po, vinfo_t* v);

EXTERNFN vinfo_t* PsycoObject_RichCompareBool(PsycoObject* po,
                                              vinfo_t* v,
                                              vinfo_t* w, int op);


/* a quick way to specify the type of the object returned by an operation
   when it is known, without having to go into all the details of the
   operation itself (be careful, you must be *sure* of the return type): */

#define DEF_KNOWN_RET_TYPE_1(cname, op, flags, knowntype)	\
    DEF_KNOWN_RET_TYPE_internal(cname, knowntype,		\
				(PsycoObject* po, vinfo_t* v1),	\
				(po, op, flags, "v", v1))
#define DEF_KNOWN_RET_TYPE_2(cname, op, flags, knowntype)			\
    DEF_KNOWN_RET_TYPE_internal(cname, knowntype,				\
				(PsycoObject* po, vinfo_t* v1, vinfo_t* v2),	\
				(po, op, flags, "vv", v1, v2))
#define DEF_KNOWN_RET_TYPE_3(cname, op, flags, knowntype)			\
    DEF_KNOWN_RET_TYPE_internal(cname, knowntype,				\
				(PsycoObject* po, vinfo_t* v1, vinfo_t* v2,	\
					vinfo_t* v3),				\
				(po, op, flags, "vvv", v1, v2, v3))
#define DEF_KNOWN_RET_TYPE_4(cname, op, flags, knowntype)			\
    DEF_KNOWN_RET_TYPE_internal(cname, knowntype,				\
				(PsycoObject* po, vinfo_t* v1, vinfo_t* v2,	\
					vinfo_t* v3, vinfo_t* v4),		\
				(po, op, flags, "vvvv", v1, v2, v3, v4))

#define DEF_KNOWN_RET_TYPE_internal(cname, knowntype, fargs, gargs)	\
static vinfo_t* cname  fargs  {						\
	vinfo_t* result = psyco_generic_call  gargs ;			\
	if (result != NULL && !IS_NOTIMPLEMENTED(result)) {		\
		Psyco_AssertType(po, result, knowntype);		\
	}								\
	return result;							\
}

/* 'true' unless 'x' is exactly the special 'not implemented' value
   built by psyco_generic_call with CfPyErrNotImplemented */
#define IS_IMPLEMENTED(x)	((x) == NULL || !IS_NOTIMPLEMENTED(x))

/* is 'x' the special 'not implemented' value? */
#define IS_NOTIMPLEMENTED(x)	\
	((x)->source == CompileTime_NewSk(&psyco_skNotImplemented))


/* for dispatcher.c */
EXTERNFN vinfo_t* Psyco_SafelyDeleteVar(PsycoObject* po, vinfo_t* vi);

/* 3.7's struct _dictkeysobject (Objects/dict-common.h), which isn't in the
   installed headers; the entries follow the indices. */
typedef struct {
  Py_ssize_t dk_refcnt;
  Py_ssize_t dk_size;
  void *dk_lookup;
  Py_ssize_t dk_usable;
  Py_ssize_t dk_nentries;
  char dk_indices[];
} ni_dictkeys_t;
typedef struct {
  Py_hash_t me_hash;
  PyObject *me_key;
  PyObject *me_value;
} ni_dictentry_t;

/* Inline attribute stores (D38): a cache per (class, name) says where the
   name sits in keys of one size; it holds no reference to any keys. The
   store emitted by x64/ipyencoding.c:psyco_inline_store_begin reads it at
   run time and checks it against the live keys of the dict it stores
   into, so a wrong cache only costs a miss. */
typedef struct {
  Py_ssize_t dk_size; /* size of the keys this was computed for; 0: none */
  Py_ssize_t kword;   /* entries[ix].me_key in such keys, in words */
  Py_ssize_t ix;      /* the name's index in them */
  PyObject *name;     /* interned; held by the table's key */
  uint64_t *version;  /* &ni_dict_version (D35) */
  long refreshes;     /* misses that changed the cache */
} store_cache_t;


#endif /* _PSY_OBJECT_H */
