/*
 * The generic object protocol at compile time, plus the two primitives that
 * everything else uses to learn types: Psyco_NeedType (promote the type to a
 * constant; may raise the promotion pseudo-exception) and Psyco_VerifyType
 * (true / false / error-or-promotion). Psyco_SafelyDeleteVar decides which dead
 * locals can be dropped without a run-time decref, using a hard-coded list of
 * types that have no __del__. GenericGetAttr is specialised on the type
 * (see its comment); it is what reaches the tp_descr_get metas in
 * pdescrobject.c and pfuncobject.c.
 */
#include "pobject.h"
#include "../compat2to3.h"
#include "../pycodegen.h"
#include "../codemanager.h"
#include "pboolobject.h"
#include "pstringobject.h"

DEFINEFN
PyTypeObject *Psyco_NeedType(PsycoObject *po, vinfo_t *vi) {
  if (is_compiletime(vi->source)) {
    /* optimization for the type of a known object */
    PyObject *o = (PyObject *)CompileTime_Get(vi->source)->value;
    return o->ob_type;
  } else {
    vinfo_t *vtp = psyco_get_const(po, vi, OB_type);
    if (vtp == NULL)
      return NULL;
    return (PyTypeObject *)psyco_pyobj_atcompiletime(po, vtp);
  }
}

DEFINEFN
PyTypeObject *Psyco_KnownType(vinfo_t *vi) {
  if (is_compiletime(vi->source)) {
    PyObject *o = (PyObject *)CompileTime_Get(vi->source)->value;
    return o->ob_type;
  } else {
    vinfo_t *vtp = vinfo_getitem(vi, iOB_TYPE);
    if (vtp != NULL && is_compiletime(vtp->source))
      return (PyTypeObject *)CompileTime_Get(vtp->source)->value;
    else
      return NULL;
  }
}

DEFINEFN
vinfo_t *PsycoObject_IsTrue(PsycoObject *po, vinfo_t *vi) {
  PyTypeObject *tp;
  tp = (PyTypeObject *)Psyco_NeedType(po, vi);
  if (tp == NULL)
    return NULL;
  /* XXX tony: Need some way to check Py_True/Py_False quickly */
  /* XXX tony: Should make sure result is 0 or 1 like cPython */
  if (tp == Py_None->ob_type)
    return psyco_vi_Zero();
  else if (tp->tp_as_number != NULL && tp->tp_as_number->nb_bool != NULL)
    return Psyco_META1(po, tp->tp_as_number->nb_bool, CfCommonInquiry, "v", vi);
  else if (tp->tp_as_mapping != NULL && tp->tp_as_mapping->mp_length != NULL)
    return Psyco_META1(po, tp->tp_as_mapping->mp_length, CfCommonPySSizeTResult,
                       "v", vi);
  else if (tp->tp_as_sequence != NULL && tp->tp_as_sequence->sq_length != NULL)
    return Psyco_META1(po, tp->tp_as_sequence->sq_length,
                       CfCommonPySSizeTResult, "v", vi);
  else
    return psyco_vi_One();
}

DEFINEFN
vinfo_t *PsycoObject_Repr(PsycoObject *po, vinfo_t *vi) {
  /* XXX implement me */
  vinfo_t *vstr =
      psyco_generic_call(po, PyObject_Repr, CfCommonNewRefPyObject, "v", vi);
  if (vstr == NULL)
    return NULL;

  /* the result is a string */
  Psyco_AssertType(po, vstr, &NiCompatStr_Type);
  return vstr;
}

static vinfo_t *generic_getattr(PsycoObject *po, vinfo_t *obj, vinfo_t *vname,
                                void *generic);

/* The type of 'obj' as a constant, promoted for at most MEGAMORPHIC_MAX
   types at a site: 1 (*tp set), 0 (megamorphic: stay generic), or -1. A
   heap type made a constant stays alive with the code, so the bound also
   limits how many classes a site keeps. */
static int need_type_mega(PsycoObject *po, vinfo_t *obj, PyTypeObject **tp) {
  vinfo_t *vtp;
  if (is_compiletime(obj->source)) {
    *tp = Py_TYPE((PyObject *)CompileTime_Get(obj->source)->value);
    return 1;
  }
  vtp = psyco_get_const(po, obj, OB_type);
  if (vtp == NULL)
    return -1;
  return psyco_pyobj_atcompiletime_mega(po, vtp, (PyObject **)tp);
}

DEFINEFN
vinfo_t *PsycoObject_GetAttr(PsycoObject *po, vinfo_t *o, vinfo_t *attr_name) {
  PyTypeObject *tp;
  int r;
  if (!is_compiletime(attr_name->source) ||
      !PyUnicode_CheckExact(
          (PyObject *)CompileTime_Get(attr_name->source)->value))
    goto generic;
  r = need_type_mega(po, o, &tp);
  if (r < 0)
    return NULL;
  /* a heap type's tp_getattro can change (a new __getattr__), and a respawn
     replays this with the type as it is then: so don't test it here, before
     generic_getattr's guard, but after it */
  if (r > 0 && ((tp->tp_flags & Py_TPFLAGS_HEAPTYPE) ||
                tp->tp_getattro == PyObject_GenericGetAttr))
    return generic_getattr(po, o, attr_name, PyObject_GetAttr);
generic:
  return psyco_generic_call(po, PyObject_GetAttr, CfCommonNewRefPyObject, "vv",
                            o, attr_name);
}

DEFINEFN
bool PsycoObject_SetAttr(PsycoObject *po, vinfo_t *o, vinfo_t *vattrname,
                         vinfo_t *v) {
  PyObject *name;
  PyTypeObject *tp;
  vinfo_t *vresult;
  /* as in PsycoObject_GenericGetAttr() we don't try to analyse
     a non-constant vattrname */
  if (!is_compiletime(vattrname->source))
    goto generic;

  tp = (PyTypeObject *)Psyco_NeedType(po, o);
  if (tp == NULL)
    return false;

  name = (PyObject *)CompileTime_Get(vattrname->source)->value;
  if (!NiCompatStr_Check(name)) {
#ifdef IS_PY3K
    /* The Unicode to string conversion is done here because the
       existing tp_setattro slots expect a string object as name
       and we wouldn't want to break those. */
    if (PyUnicode_Check(name)) {
      goto generic;
    } else {
#endif
      PycException_SetFormat(po, PyExc_TypeError,
                             "attribute name must be string, not '%.200s'",
                             Py_TYPE(name)->tp_name);
      return false;
#ifdef IS_PY3K
    }
#endif
  } else
    Py_INCREF(name);

  NiCompatStr_InternInPlace(&name);
  if (tp->tp_setattro != NULL) {
    vresult = Psyco_META3(po, tp->tp_setattro, CfCommonIntZeroOk,
                          v ? "vlv" : "vll", o, name, v);
    Py_DECREF(name);
    return vresult != NULL;
  }
  if (tp->tp_setattr != NULL) {
    vresult =
        Psyco_META3(po, tp->tp_setattr, CfCommonIntZeroOk, v ? "vlv" : "vll", o,
                    (long)NiCompatStr_AS_STRING(name), v);
    Py_DECREF(name);
    return vresult != NULL;
  }
  Py_DECREF(name);

generic:
  /* fall-back implementation */
  return psyco_generic_call(po, PyObject_SetAttr, CfCommonIntZeroOk,
                            v ? "vvv" : "vvl", o, vattrname, v) != NULL;
}

/* PyObject_GenericGetAttr specialised on the type (A9). The steps are
   those of 3.7's _PyObject_GenericGetAttrWithDict: the type's attribute
   (descr) is looked up now, and is only valid while the type is unchanged,
   which a run-time check of its version tag guards (every change to a type
   or a base, including a new __getattr__, clears
   Py_TPFLAGS_VALID_VERSION_TAG; a new tag is assigned at the next lookup).
   A failed guard takes the generic path. Then, at run time: a data
   descriptor's __get__; else the instance __dict__; else a descriptor's
   __get__ (a function gives a virtual bound method, so a method call
   becomes a direct call); else the attribute itself.

   Respawns (dispatcher.h) replay compilation with the type as it is then,
   so, as for load_global_cached:
   - nothing before the guard depends on the type's mutable state (the
     expected tag is read from a per-site attr_guard_t, never freed);
   - the block ends right after the site (split_after_global), so the only
     replays through it are for its own conditions: the guard (the type
     changed: generic), or one just after a passed guard, with the type as
     the guard saw it. For that, only descriptors whose __get__ runs no
     Python code are specialised (a property getter could change the
     class and then raise, which is a respawn).
   - Only descriptors of built-in types: a heap descriptor type could gain
     or lose __get__/__set__ without the guard seeing it.
   - The C descriptor metas don't check the instance's type, as
     descr_check does, so they are used only when it is a subtype of the
     descriptor's d_type.
   - The compiled code holds a reference to descr (never released), so a
     bound method made from it stays valid if the class changes later.
   Known gap: an instance __dict__ key with a Python __eq__ that changes the
   class during the lookup (KNOWN_BUGS F34). */
typedef struct {
  unsigned long flags; /* tp_flags as compiled; 0 (never valid) if unusable */
  unsigned long tag;
} attr_guard_t;

/* The guard is inline: the type changed if tp_flags (which include
   Py_TPFLAGS_VALID_VERSION_TAG) or tp_version_tag differ from the guard's.
   All four are run-time loads, so a replay with an unset guard emits the
   same conditions. */
#define TYPE_flags                                                             \
  FMUT(UNSIGNED_FIELD(PyTypeObject, unsigned long, tp_flags, NO_PREV_FIELD))
#define TYPE_version_tag                                                       \
  FMUT(UNSIGNED_FIELD(PyTypeObject, unsigned int, tp_version_tag,              \
                      NO_PREV_FIELD))
#define AGUARD_flags                                                           \
  FMUT(UNSIGNED_FIELD(attr_guard_t, unsigned long, flags, NO_PREV_FIELD))
#define AGUARD_tag                                                             \
  FMUT(UNSIGNED_FIELD(attr_guard_t, unsigned long, tag, NO_PREV_FIELD))

static vinfo_t *field_xor(PsycoObject *po, vinfo_t *a, defield_t fa,
                          vinfo_t *b, defield_t fb) {
  vinfo_t *x, *y, *r;
  x = psyco_get_field(po, a, fa);
  if (x == NULL)
    return NULL;
  y = psyco_get_field(po, b, fb);
  if (y == NULL) {
    vinfo_decref(x, po);
    return NULL;
  }
  r = integer_xor(po, x, y);
  vinfo_decref(y, po);
  vinfo_decref(x, po);
  return r;
}

/* the type differs from the guard's: a condition code, or CC_ERROR */
static condition_code_t type_changed(PsycoObject *po, PyTypeObject *tp,
                                     attr_guard_t *guard) {
  vinfo_t *vtp = vinfo_new(CompileTime_New((long)tp));
  vinfo_t *vg = vinfo_new(CompileTime_New((long)guard));
  vinfo_t *x, *y = NULL, *z = NULL;
  x = field_xor(po, vtp, TYPE_flags, vg, AGUARD_flags);
  if (x != NULL)
    y = field_xor(po, vtp, TYPE_version_tag, vg, AGUARD_tag);
  if (y != NULL)
    z = integer_or(po, x, y);
  vinfo_xdecref(y, po);
  vinfo_xdecref(x, po);
  vinfo_decref(vg, po);
  vinfo_decref(vtp, po);
  if (z == NULL)
    return CC_ERROR;
  return integer_NON_NULL(po, z); /* consumes z */
}


/* borrowed, or NULL with no exception set (PyDict_GetItem) */
static PyObject *cimpl_instance_dict_get(PyObject *obj, PyObject *name) {
  PyObject **dictptr = _PyObject_GetDictPtr(obj);
  if (dictptr == NULL || *dictptr == NULL)
    return NULL;
  return PyDict_GetItem(*dictptr, name);
}

/* the instance dict's entry for 'name', whose hash is known: borrowed, or
   NULL. Like PyDict_GetItem, errors from a key's __eq__ are dropped; the
   compiled code has no exception pending here, so nothing is saved. */
static PyObject *cimpl_dict_get_known(PyObject *dict, PyObject *name,
                                      Py_hash_t hash) {
  PyObject *v = _PyDict_GetItem_KnownHash(dict, name, hash);
  if (v == NULL && PyErr_Occurred())
    PyErr_Clear();
  return v;
}

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

/* Where to find 'name' in an instance dict that uses the class's shared
   keys (PEP 412): at *ix, while the dict uses *keys and, if *n >= 0, the
   keys still hold *n entries. A name not in the keys is NULL at the next
   free index, since a dict's values array covers all usable entries and a
   shared table only grows. Takes a reference to the keys. Only when some
   dict uses them (dk_refcnt > 1): before that, the first instance may still
   grow them, and CPython re-shares the grown keys only if the class holds
   the sole reference. */
static bool shared_key_slot(PyTypeObject *tp, PyObject *name,
                            ni_dictkeys_t **keys, Py_ssize_t *ix,
                            Py_ssize_t *n) {
  ni_dictkeys_t *k = (ni_dictkeys_t *)((PyHeapTypeObject *)tp)->ht_cached_keys;
  Py_ssize_t i, ixsize;
  ni_dictentry_t *entries;
  if (k == NULL || k->dk_refcnt < 2)
    return false;
  ixsize = k->dk_size <= 0xff ? 1 : k->dk_size <= 0xffff ? 2
                                  : k->dk_size <= 0xffffffff ? 4 : 8;
  entries = (ni_dictentry_t *)(k->dk_indices + k->dk_size * ixsize);
  for (i = 0; i < k->dk_nentries; i++)
    if (entries[i].me_key == name)
      break;
  if (i < k->dk_nentries)
    *n = -1;
  else if (k->dk_usable > 0)
    *n = i;
  else
    return false;
  *ix = i;
  *keys = k;
  k->dk_refcnt++; /* held by the compiled code */
  return true;
}

/* the expected keys, compared at run time: a dict matches if it uses
   'keys' and (nentries ^ n) & mask is 0 */
typedef struct {
  ni_dictkeys_t *keys;
  Py_ssize_t n;
  Py_ssize_t mask;
} keys_guard_t;

#define KGUARD_keys                                                            \
  FMUT(DEF_FIELD(keys_guard_t, ni_dictkeys_t *, keys, NO_PREV_FIELD))
#define KGUARD_n FMUT(DEF_FIELD(keys_guard_t, Py_ssize_t, n, NO_PREV_FIELD))
#define KGUARD_mask                                                            \
  FMUT(DEF_FIELD(keys_guard_t, Py_ssize_t, mask, NO_PREV_FIELD))

#define INSTANCE_dict FMUT(DEF_ARRAY(PyObject *, 0))
#define DICT_ma_keys                                                           \
  FMUT(DEF_FIELD(PyDictObject, PyDictKeysObject *, ma_keys, NO_PREV_FIELD))
#define DICT_ma_values                                                         \
  FMUT(DEF_FIELD(PyDictObject, PyObject **, ma_values, NO_PREV_FIELD))
#define VALUES_item FMUT(DEF_ARRAY(PyObject *, 0))
#define KEYS_nentries                                                          \
  FMUT(DEF_FIELD(ni_dictkeys_t, Py_ssize_t, dk_nentries, NO_PREV_FIELD))

/* Whether to emit the inline path for 'tp', in a way that a replay repeats
   (see psyco_block_serial). CPython never gives a class shared keys again
   once it drops them. So: yes while it has them; no if Ni never saw it with
   them; otherwise yes only in blocks older than the moment Ni first saw
   them gone, which includes every block compiled while it had them. A dead
   type's address reused by a new class only costs a needless inline path
   (or misses one). */
static bool keys_inline(PsycoObject *po, PyTypeObject *tp) {
  /* type address -> None while it has shared keys, then the first block
     serial after Ni saw them gone */
  static PyObject *seen = NULL;
  PyObject *key, *d;
  bool r;
  if (seen == NULL && (seen = PyDict_New()) == NULL)
    OUT_OF_MEMORY();
  key = PyLong_FromVoidPtr(tp);
  if (key == NULL)
    OUT_OF_MEMORY();
  d = PyDict_GetItem(seen, key);
  if (((PyHeapTypeObject *)tp)->ht_cached_keys != NULL) {
    if (d != Py_None && PyDict_SetItem(seen, key, Py_None) < 0)
      OUT_OF_MEMORY();
    r = true;
  } else if (d == NULL) {
    r = false;
  } else {
    if (d == Py_None) {
      d = PyLong_FromUnsignedLong(psyco_next_serial);
      if (d == NULL || PyDict_SetItem(seen, key, d) < 0)
        OUT_OF_MEMORY();
      Py_DECREF(d);
    }
    r = psyco_block_serial(po) < PyLong_AsUnsignedLong(d);
  }
  Py_DECREF(key);
  return r;
}

/* (keys ^ g->keys) | ((keys->dk_nentries ^ g->n) & g->mask) */
static vinfo_t *keys_mismatch(PsycoObject *po, vinfo_t *vk, vinfo_t *vg) {
  vinfo_t *a, *b, *c, *r = NULL;
  c = psyco_get_field(po, vg, KGUARD_keys);
  if (c == NULL)
    return NULL;
  a = integer_xor(po, vk, c);
  vinfo_decref(c, po);
  if (a == NULL)
    return NULL;
  b = field_xor(po, vk, KEYS_nentries, vg, KGUARD_n);
  if (b != NULL) {
    c = psyco_get_field(po, vg, KGUARD_mask);
    if (c != NULL) {
      vinfo_t *m = integer_and(po, b, c);
      vinfo_decref(c, po);
      if (m != NULL) {
        r = integer_or(po, a, m);
        vinfo_decref(m, po);
      }
    }
    vinfo_decref(b, po);
  }
  vinfo_decref(a, po);
  return r;
}

/* The instance's __dict__ entry for 'name': a borrowed pointer or NULL.
   For a heap type whose instances share keys (PEP 412) this is inline: the
   dict at tp_dictoffset, one check that it uses the keys compiled in (and,
   for a name not in them, that they have no new entries), then the value
   at shared_key_slot's index (a dict using shared keys is always split).
   Otherwise, or when a check fails, a lookup with the name's hash computed
   now (cimpl_dict_get_known); no dict at all is "not found". As for the type guard, a
   replay may find different keys, so the code emitted must not depend on
   them: the expected values are run-time loads from a per-site
   keys_guard_t (never freed), and without usable keys it expects an
   impossible pointer and fails into the helper. The keys are held for
   good, like descriptors (D30). */
static vinfo_t *instance_dict_get(PsycoObject *po, vinfo_t *obj,
                                  PyTypeObject *tp, PyObject *name) {
  static keys_guard_t respawn_guard; /* read by trashed code only */
  keys_guard_t *guard = &respawn_guard;
  ni_dictkeys_t *keys;
  Py_ssize_t ix = 0, n;
  vinfo_t *vd, *vk, *vg, *vv, *v, *x;
  condition_code_t cc;
  bool shared;
  if (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE) || tp->tp_dictoffset <= 0)
    goto helper;
  shared = keys_inline(po, tp);
  if (shared && !is_respawning(po)) {
    guard = PyMem_Malloc(sizeof(keys_guard_t));
    if (guard == NULL)
      OUT_OF_MEMORY();
    if (shared_key_slot(tp, name, &keys, &ix, &n)) {
      guard->keys = keys;
      guard->n = n < 0 ? 0 : n;
      guard->mask = n < 0 ? 0 : -1;
    } else {
      guard->keys = (ni_dictkeys_t *)1; /* matches no dict */
      guard->n = guard->mask = 0;
    }
  }
  vd = psyco_get_field_offset(po, obj, INSTANCE_dict, tp->tp_dictoffset);
  if (vd == NULL)
    return NULL;
  cc = integer_non_null(po, vd);
  if (cc == CC_ERROR || !runtime_condition_t(po, cc)) {
    vinfo_decref(vd, po);
    if (cc == CC_ERROR)
      return NULL;
    return psyco_vi_Zero(); /* no dict yet: not found */
  }
  if (!shared)
    goto known_hash;
  vk = psyco_get_field(po, vd, DICT_ma_keys);
  if (vk == NULL) {
    vinfo_decref(vd, po);
    return NULL;
  }
  vg = vinfo_new(CompileTime_New((long)guard));
  x = keys_mismatch(po, vk, vg);
  vinfo_decref(vg, po);
  vinfo_decref(vk, po);
  if (x == NULL) {
    vinfo_decref(vd, po);
    return NULL;
  }
  cc = integer_NON_NULL(po, x); /* consumes x */
  if (cc == CC_ERROR) {
    vinfo_decref(vd, po);
    return NULL;
  }
  if (runtime_condition_f(po, cc))
    goto known_hash;
  vv = psyco_get_field(po, vd, DICT_ma_values);
  vinfo_decref(vd, po);
  if (vv == NULL)
    return NULL;
  v = psyco_get_field_offset(po, vv, VALUES_item, ix * sizeof(PyObject *));
  vinfo_decref(vv, po);
  return v;

known_hash:
  v = psyco_generic_call(po, cimpl_dict_get_known, CfReturnTypePtr, "vll", vd,
                         (long)name, (long)PyObject_Hash(name));
  vinfo_decref(vd, po);
  return v;

helper:
  return psyco_generic_call(po, cimpl_instance_dict_get, CfReturnTypePtr, "vl",
                            obj, (long)name);
}

/* descriptor types whose __get__ is C code that runs no Python code */
static bool c_descriptor(PyTypeObject *dtp) {
  return dtp == &PyMemberDescr_Type || dtp == &PyMethodDescr_Type ||
         dtp == &PyGetSetDescr_Type || dtp == &PyClassMethodDescr_Type;
}
static bool safe_descriptor(PyTypeObject *dtp) {
  return c_descriptor(dtp) || dtp == &PyFunction_Type ||
         dtp == &PyStaticMethod_Type || dtp == &PyClassMethod_Type ||
         dtp->tp_descr_get == NULL;
}

/* 'generic' is the function the compiled code calls when the type has
   changed: PyObject_GetAttr from PsycoObject_GetAttr, because the change
   may have given the type a __getattr__ or __getattribute__. */
static vinfo_t *generic_getattr(PsycoObject *po, vinfo_t *obj, vinfo_t *vname,
                                void *generic) {
  static attr_guard_t respawn_guard;
  PyTypeObject *tp;
  PyObject *name, *descr;
  descrgetfunc f = NULL;
  attr_guard_t *guard;
  vinfo_t *v;
  condition_code_t cc;
  int r;

  if (!is_compiletime(vname->source))
    goto generic;
  name = (PyObject *)CompileTime_Get(vname->source)->value;
  if (!PyUnicode_CheckExact(name))
    goto generic;
  r = need_type_mega(po, obj, &tp);
  if (r < 0)
    return NULL;
  /* a static type's tp_getattro never changes; a heap type's is checked
     after the guard */
  if (r == 0 || (!(tp->tp_flags & Py_TPFLAGS_HEAPTYPE) &&
                 tp->tp_getattro != PyObject_GenericGetAttr))
    goto generic;

  if (is_respawning(po))
    guard = &respawn_guard;
  else {
    guard = PyMem_Malloc(sizeof(attr_guard_t));
    if (guard == NULL)
      OUT_OF_MEMORY();
    if (tp->tp_dict != NULL)
      _PyType_Lookup(tp, name); /* assigns the version tag */
    guard->tag = tp->tp_version_tag;
    guard->flags = PyType_HasFeature(tp, Py_TPFLAGS_VALID_VERSION_TAG) &&
                           tp->tp_getattro == PyObject_GenericGetAttr
                       ? tp->tp_flags
                       : 0;
  }
  cc = type_changed(po, tp, guard);
  if (cc == CC_ERROR)
    return NULL;
  po->pr.split_after_global = 1;
  if (runtime_condition_f(po, cc))
    goto generic;

  /* the type is as the guard saw it */
  if (tp->tp_getattro != PyObject_GenericGetAttr || tp->tp_dict == NULL)
    goto generic;
  descr = _PyType_Lookup(tp, name); /* borrowed */
  if (descr != NULL) {
    PyTypeObject *dtp = Py_TYPE(descr);
    if ((dtp->tp_flags & Py_TPFLAGS_HEAPTYPE) || !safe_descriptor(dtp))
      goto generic;
    if (c_descriptor(dtp) && !PyType_IsSubtype(tp, PyDescr_TYPE(descr)))
      goto generic;
    f = dtp->tp_descr_get;
    Py_INCREF(descr); /* held by the compiled code */
  }

  if (f != NULL && PyDescr_IsData(descr))
    return Psyco_META3(po, f, CfCommonNewRefPyObject, "lvl", descr, obj, tp);

  if (tp->tp_dictoffset != 0) {
    bool found;
    v = instance_dict_get(po, obj, tp, name);
    if (v == NULL)
      return NULL;
    cc = integer_non_null(po, v);
    if (cc == CC_ERROR) {
      vinfo_decref(v, po);
      return NULL;
    }
    /* an attribute shadowing a method is rare; one without a class
       attribute is likely in the instance */
    found = descr != NULL ? runtime_condition_f(po, cc)
                          : runtime_condition_t(po, cc);
    if (found) {
      need_reference(po, v);
      return v;
    }
    vinfo_decref(v, po);
  }

  if (f != NULL)
    return Psyco_META3(po, f, CfCommonNewRefPyObject, "lvl", descr, obj, tp);
  if (descr != NULL) {
    Py_INCREF(descr);
    return vinfo_new(CompileTime_NewSk(sk_new((long)descr, SkFlagPyObj)));
  }
  /* not found: the generic call raises AttributeError */
generic:
  return psyco_generic_call(po, generic, CfCommonNewRefPyObject, "vv", obj,
                            vname);
}

DEFINEFN
vinfo_t *PsycoObject_GenericGetAttr(PsycoObject *po, vinfo_t *obj,
                                    vinfo_t *vname) {
  return generic_getattr(po, obj, vname, PyObject_GenericGetAttr);
}

/* Map rich comparison operators to their swapped version, e.g. LT --> GT */
static int swapped_op[] = {Py_GT, Py_GE, Py_EQ, Py_NE, Py_LT, Py_LE};

static vinfo_t* try_rich_compare(PsycoObject* po, vinfo_t* v, vinfo_t* w, int op)
{
  bool swap;
  PyTypeObject* vtp;
  PyTypeObject* wtp;
  richcmpfunc fv;
  richcmpfunc fw;
  vinfo_t* res;

  vtp = (PyTypeObject*)Psyco_NeedType(po, v);
  if (vtp == NULL) return NULL;
  wtp = (PyTypeObject*) Psyco_NeedType(po, w);
  if (wtp == NULL) return NULL;
  fv = vtp->tp_richcompare;
  fw = wtp->tp_richcompare;

  swap = (vtp != wtp &&
          PyType_IsSubtype(wtp, vtp) &&
          fw != NULL);
  if (swap) {
          res = Psyco_META3(po, fw, CfCommonCheckNotImplemented,
                            "vvl", w, v, swapped_op[op]);
          if (IS_IMPLEMENTED(res))
                  return res;   /* 'res' might be NULL */
          vinfo_decref(res, po);
  }
  if (fv != NULL) {
          res = Psyco_META3(po, fv, CfCommonCheckNotImplemented,
                            "vvl", v, w, op);
          if (IS_IMPLEMENTED(res))
                  return res;
          vinfo_decref(res, po);
  }
  if (!swap && fw != NULL) {
          return Psyco_META3(po, fw, CfCommonCheckNotImplemented,
                              "vvl", w, v, swapped_op[op]);
  }
  return psyco_vi_NotImplemented();
}

DEFINEFN vinfo_t *PsycoObject_RichCompare(PsycoObject *po, vinfo_t *v,
                                          vinfo_t *w, int op) {
  vinfo_t *res = try_rich_compare(po, v, w, op);
  if (IS_IMPLEMENTED(res)) return res;
        vinfo_decref(res, po);
  return psyco_generic_call(po, PyObject_RichCompare, CfCommonNewRefPyObject,
                            "vvl", v, w, (long)op);
}

DEFINEFN
vinfo_t *PsycoObject_RichCompareBool(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                                     int op) {
  /* The result is a temporary, and only values in the frame can be
     promoted, so if its type isn't known its truth is a run-time call.
     Unlike CPython there is no v == w shortcut for Py_EQ/Py_NE. */
  vinfo_t *diff;
  vinfo_t *result = PsycoObject_RichCompare(po, v, w, op);
  if (result == NULL)
    return NULL;
  if (Psyco_KnownType(result) != NULL)
    diff = PsycoObject_IsTrue(po, result);
  else
    diff = psyco_generic_call(po, PyObject_IsTrue, CfCommonInquiry, "v",
                              result);
  vinfo_decref(result, po);
  return diff;
}

DEFINEFN
vinfo_t *PsycoObject_Hash(PsycoObject *po, vinfo_t *v) {
  /* -1 is never a hash: it means an error */
  PyTypeObject *tp = Psyco_NeedType(po, v);
  if (tp == NULL)
    return NULL;
  if (tp->tp_hash == NULL) /* a type not yet ready: CPython readies it */
    return psyco_generic_call(po, PyObject_Hash,
                              CfReturnTypeLong | CfPyErrIfMinus1, "v", v);
  return Psyco_META1(po, tp->tp_hash, CfReturnTypeLong | CfPyErrIfMinus1, "v",
                     v);
}

static vinfo_t *collect_undeletable_vars(vinfo_t *vi, vinfo_t *link) {
  int i;
  PyTypeObject *tp;
  switch (gettime(vi->source)) {

  case RunTime:
    if (vi->tmp != NULL || (vi->source & RunTime_NoRef) != 0)
      break; /* already seen or not holding a ref */
    tp = Psyco_KnownType(vi);
    if (tp &&
        (tp == &NiCompatStr_Type || tp == &PyBool_Type || tp == &PyFloat_Type ||
         tp == &PyLong_Type || tp == Py_None->ob_type || tp == &PyRange_Type))
      break; /* known safe type */
    /* make a linked list of results */
    vi->tmp = link;
    link = vi;
    break;

  case VirtualTime:
    for (i = vi->array->count; i--;)
      if (vi->array->items[i] != NULL)
        link = collect_undeletable_vars(vi->array->items[i], link);
    break;

  default:
    break;
  }
  return link;
}

DEFINEFN
vinfo_t *Psyco_SafelyDeleteVar(PsycoObject *po, vinfo_t *vi) {
  vinfo_t *result;
  vinfo_t *head;
  vinfo_t *queue = (vinfo_t *)1;
  vinfo_t *p;
  int count;
  vi->tmp = NULL;
  clear_tmp_marks(vi->array);

  head = collect_undeletable_vars(vi, queue);
  count = 0;
  for (p = head; p != queue; p = p->tmp) {
    vinfo_array_t *a = p->array;
    p->array = NullArray;
    array_delete(a, po);
    count++;
  }
  if (count == 0) {
    result = psyco_vi_Zero();
  } else if (count == 1) {
    result = head;
    vinfo_incref(result);
  } else {
    result = vinfo_new(VirtualTime_New(&psyco_vsource_not_important));
    count += iOB_TYPE + 1;
    result->array = array_new(count);
    for (p = head; p != queue; p = p->tmp) {
      vinfo_incref(p);
      result->array->items[--count] = p;
    }
  }
  return result;
}

INITIALIZATIONFN
void psy_object_init(void) {
  /* associate the Python implementation of some functions with
     the one from Psyco */
  Psyco_DefineMeta(PyObject_GenericGetAttr, PsycoObject_GenericGetAttr);
}
