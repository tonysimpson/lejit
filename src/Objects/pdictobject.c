#include "pdictobject.h"
#include "pmethodobject.h"
#include "ptupleobject.h"
#include "punicodeobject.h"
/* LE_GC_IS_TRACKED */
#include "../mechanisms/store_gc.h"

/* ma_used is read as its low 32 bits (psyco.h's layouts) */
#define DICT_ma_used FMUT(CPY_FIELD(PyDictObject, ma_used, OB_type))
#define iDICT_MA_USED FIELD_INDEX(DICT_ma_used)

DEFINEFN
vinfo_t *
PsycoDict_New(PsycoObject *po)
{
    /* XXX no virtual dicts yet */
    vinfo_t *v =
        psyco_generic_call(po, PyDict_New, CfCommonNewRefPyObject, "");
    if (v == NULL)
        return NULL;

    /* the result is a dict */
    Psyco_AssertType(po, v, &PyDict_Type);
    return v;
}

#if LE_HAS_PYDICT_NEWPRESIZED
DEFINEFN
vinfo_t *
_PsycoDict_NewPresized(PsycoObject *po, long minused)
{
    if (minused > 5) {
        vinfo_t *v = psyco_generic_call(po, _PyDict_NewPresized,
                                        CfCommonNewRefPyObject, "l", minused);
        if (v == NULL)
            return NULL;
        /* the result is a dict */
        Psyco_AssertType(po, v, &PyDict_Type);
        return v;
    }
    else
        return PsycoDict_New(po);
}
#endif

DEFINEFN
bool
PsycoDict_SetItemC(PsycoObject *po, vinfo_t *vdict, PyObject *key,
                   vinfo_t *vvalue)
{
    return psyco_generic_call(po, PyDict_SetItem, CfCommonIntZeroOk, "vlv",
                              vdict, (long)key, vvalue) != NULL;
}

/* the dict slots and methods translated from dictobject.c */
/* the C functions, for the fallbacks: set by the registration */
static PyCFunction cfunc_dict_get;
static PyCFunction cfunc_dict_setdefault;

/* dictobject.c's macros for the keys' layout, which it keeps to itself */
#define DKIX_EMPTY (-1)
#define DKIX_DUMMY (-2)
#define DKIX_ERROR (-3)
#define DK_SIZE(dk) ((dk)->dk_size)
#define DK_IXSIZE(dk)                \
    (DK_SIZE(dk) <= 0xff         ? 1 \
     : DK_SIZE(dk) <= 0xffff     ? 2 \
     : DK_SIZE(dk) <= 0xffffffff ? 4 \
                                 : sizeof(int64_t))
#define DK_ENTRIES(dk)     \
    ((PyDictKeyEntry *)(&( \
        (int8_t *)((dk)->dk_indices))[DK_SIZE(dk) * DK_IXSIZE(dk)]))
#define DK_MASK(dk) (DK_SIZE(dk) - 1)

static PyObject *
cimpl_dict_get_impl(PyDictObject *self, PyObject *key, PyObject *default_value)
{
    PyObject *val = NULL;
    Py_hash_t hash;
    Py_ssize_t ix;
    if (!PyUnicode_CheckExact(key) ||
        (hash = ((PyASCIIObject *)key)->hash) == -1) {
        hash = PyObject_Hash(key);
        if (hash == -1)
            return NULL;
    }
    ix = (self->ma_keys->dk_lookup)(self, key, hash, &val);
    if (ix == DKIX_ERROR)
        return NULL;
    if (ix == DKIX_EMPTY || val == NULL) {
        val = default_value;
    }
    Py_INCREF(val);
    return val;
}

static Py_ssize_t
cimpl_dictkeys_get_index(const PyDictKeysObject *keys, Py_ssize_t i)
{
    Py_ssize_t s = DK_SIZE(keys);
    Py_ssize_t ix;
    if (s <= 0xff) {
        const int8_t *indices = (const int8_t *)(keys->dk_indices);
        ix = indices[i];
    }
    else if (s <= 0xffff) {
        const int16_t *indices = (const int16_t *)(keys->dk_indices);
        ix = indices[i];
    }
    else if (s > 0xffffffff) {
        const int64_t *indices = (const int64_t *)(keys->dk_indices);
        ix = indices[i];
    }
    else {
        const int32_t *indices = (const int32_t *)(keys->dk_indices);
        ix = indices[i];
    }
    assert(ix >= DKIX_DUMMY);
    return ix;
}

static PyObject *
cimpl_dict_setdefault_impl(PyDictObject *self, PyObject *key,
                           PyObject *default_value)
{
    PyObject *val;
    val = PyDict_SetDefault((PyObject *)self, key, default_value);
    Py_XINCREF(val);
    return val;
}

static int
cimpl_unicode_eq_str_key(PyObject *aa, const le_strkey_t *bb)
{
    PyUnicodeObject *a = (PyUnicodeObject *)aa;
    const le_strkey_t *b = bb;
    if (PyUnicode_READY(a) == -1) {
        Py_UNREACHABLE();
    }
    if (PyUnicode_GET_LENGTH(a) != (b->length))
        return 0;
    if (PyUnicode_GET_LENGTH(a) == 0)
        return 1;
    if (PyUnicode_KIND(a) != PyUnicode_1BYTE_KIND)
        return 0;
    return memcmp(PyUnicode_1BYTE_DATA(a), (b->data),
                  PyUnicode_GET_LENGTH(a) * PyUnicode_KIND(a)) == 0;
}

static Py_ssize_t
cimpl_lookdict_unicode_str_key(PyDictObject *mp, const le_strkey_t *key,
                               Py_hash_t hash, PyObject **value_addr)
{
    PyDictKeyEntry *ep0 = DK_ENTRIES(mp->ma_keys);
    size_t mask = DK_MASK(mp->ma_keys);
    size_t perturb = (size_t)hash;
    size_t i = (size_t)hash & mask;
    for (;;) {
        Py_ssize_t ix = cimpl_dictkeys_get_index(mp->ma_keys, i);
        if (ix == DKIX_EMPTY) {
            *value_addr = NULL;
            return DKIX_EMPTY;
        }
        if (ix >= 0) {
            PyDictKeyEntry *ep = &ep0[ix];
            if (ep->me_hash == hash &&
                cimpl_unicode_eq_str_key(ep->me_key, key)) {
                *value_addr = ep->me_value;
                return ix;
            }
        }
        perturb >>= 5;
        i = mask & (i * 5 + perturb + 1);
    }
    Py_UNREACHABLE();
}

static Py_ssize_t
cimpl_lookdict_unicode_nodummy_str_key(PyDictObject *mp,
                                       const le_strkey_t *key, Py_hash_t hash,
                                       PyObject **value_addr)
{
    PyDictKeyEntry *ep0 = DK_ENTRIES(mp->ma_keys);
    size_t mask = DK_MASK(mp->ma_keys);
    size_t perturb = (size_t)hash;
    size_t i = (size_t)hash & mask;
    for (;;) {
        Py_ssize_t ix = cimpl_dictkeys_get_index(mp->ma_keys, i);
        if (ix == DKIX_EMPTY) {
            *value_addr = NULL;
            return DKIX_EMPTY;
        }
        PyDictKeyEntry *ep = &ep0[ix];
        if (ep->me_hash == hash && cimpl_unicode_eq_str_key(ep->me_key, key)) {
            *value_addr = ep->me_value;
            return ix;
        }
        perturb >>= 5;
        i = mask & (i * 5 + perturb + 1);
    }
    Py_UNREACHABLE();
}

static vinfo_t *
dict_get(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
dict_length(PsycoObject *po, vinfo_t *);
static vinfo_t *
dict_setdefault(PsycoObject *po, vinfo_t *, vinfo_t *);

static vinfo_t *
dict_get(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    int _t1;
    vinfo_t *_t2;
    vinfo_t *_t3;
    vinfo_t *default_value;
    vinfo_t *key;
    vinfo_t *return_value;
    _t1 = PsycoTuple_Load(vargs);
    if (_t1 < 1 || _t1 > 2) {
        return PsycoCFunction_CallFast(po, cfunc_dict_get, METH_FASTCALL, self,
                                       vargs);
    }
    if (_t1 == 1) {
        key = PsycoTuple_GET_ITEM(vargs, 0);
        default_value = psyco_vi_None();
        _t2 =
            psyco_generic_call(po, cimpl_dict_get_impl, CfCommonNewRefPyObject,
                               "vvv", self, key, default_value);
        return_value = _t2;
        vinfo_decref(default_value, po);
        return return_value;
    }
    else {
        key = PsycoTuple_GET_ITEM(vargs, 0);
        default_value = PsycoTuple_GET_ITEM(vargs, 1);
        _t3 =
            psyco_generic_call(po, cimpl_dict_get_impl, CfCommonNewRefPyObject,
                               "vvv", self, key, default_value);
        return_value = _t3;
        return return_value;
    }
}

static vinfo_t *
dict_length(PsycoObject *po, vinfo_t *mp)
{
    vinfo_t *_t1;
    _t1 = psyco_get_field(po, mp, DICT_ma_used);
    if (_t1 == NULL) {
        return NULL;
    }
    return _t1;
}

static vinfo_t *
dict_setdefault(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    int _t1;
    vinfo_t *default_value;
    vinfo_t *key;
    vinfo_t *return_value;
    _t1 = PsycoTuple_Load(vargs);
    if (_t1 < 1 || _t1 > 2) {
        return PsycoCFunction_CallFast(po, cfunc_dict_setdefault,
                                       METH_FASTCALL, self, vargs);
    }
    if (_t1 == 1) {
        key = PsycoTuple_GET_ITEM(vargs, 0);
        default_value = psyco_vi_None();
        return_value = psyco_generic_call(po, cimpl_dict_setdefault_impl,
                                          CfCommonNewRefPyObject, "vvv", self,
                                          key, default_value);
        vinfo_decref(default_value, po);
        return return_value;
    }
    else {
        key = PsycoTuple_GET_ITEM(vargs, 0);
        default_value = PsycoTuple_GET_ITEM(vargs, 1);
        return_value = psyco_generic_call(po, cimpl_dict_setdefault_impl,
                                          CfCommonNewRefPyObject, "vvv", self,
                                          key, default_value);
        return return_value;
    }
}

static void
gen_dictobject_init(void)
{
    Psyco_DefineMeta(PyDict_Type.tp_as_mapping->mp_length, dict_length);
}
static void
gen_dictobject_methods_init(void)
{
    cfunc_dict_get =
        Psyco_DefineMethodFn(&PyDict_Type, "get", METH_FASTCALL, &dict_get);
    cfunc_dict_setdefault = Psyco_DefineMethodFn(
        &PyDict_Type, "setdefault", METH_FASTCALL, &dict_setdefault);
}

/* The exact-str key probe of d[k], k in d and d[k] = v. A dict whose keys
   are all exact str looks a key up with lookdict_unicode or
   lookdict_unicode_nodummy (dk_lookup), which compare by identity, hash
   and bytes, run no Python and cannot fail. The metas below take an exact
   dict and a key that may be a str; at run time, for an exact str key and
   one of those two lookups, the helpers call it directly with the str's
   own hash (PyObject_Hash computes and caches it if not yet known, as
   dictobject.c does), and otherwise CPython's own function, so a str
   subclass or a key with its own __eq__ anywhere in the dict takes
   CPython's path. A store over a key found is insertdict's own
   replacement; a new key is _PyDict_SetItem_KnownHash's. d.get is the
   translated meta's copy of dict_get_impl, which already calls dk_lookup
   with the cached hash, except in a fused get then store. A view or a
   case map key is probed without being built (below); where it must be
   built, a case map is built with its hash (PsycoUnicode_ForceKey), which
   the probe then reads. */
static dict_lookup_func lookdict_unicode_fn, lookdict_unicode_nodummy_fn;
static binaryfunc dict_subscript_fn;
static objobjargproc dict_ass_sub_fn;
static _PyCFunctionFast dict_get_fn;

/* ix (>= 0 found, DKIX_EMPTY not), or -2: CPython's function, or -3: an
   error */
static Py_ssize_t
str_probe(PyDictObject *mp, PyObject *key, Py_hash_t *phash, PyObject **pvalue)
{
    dict_lookup_func f = mp->ma_keys->dk_lookup;
    Py_hash_t hash;
    if (!PyUnicode_CheckExact(key) ||
        (f != lookdict_unicode_nodummy_fn && f != lookdict_unicode_fn))
        return -2;
    hash = ((PyASCIIObject *)key)->hash;
    if (hash == -1 && (hash = PyObject_Hash(key)) == -1)
        return -3;
    *phash = hash;
    return f(mp, key, hash, pvalue);
}

/* The slot the get of a fused get then store found (pycompiler.c:
   dict_str_fusion), good while 'mp' has these keys and this version tag:
   the tag names one state of one dict, so the slot is the one a new probe
   for the same key would find. Every such get writes it or clears it, and
   the key is held from the get to the store; any record that names the
   key at the store was therefore written while that key object was alive,
   for that key. Code run between the two (an operator's Python) may write
   its own, which the store then fails to match.
   A key not built is named by its root, start, length and map instead
   (start -1 for a key object): the root is held from the get to the
   store too, and equal parts are equal strings, so the same slot. */
static struct {
    PyDictObject *mp;
    PyObject *key;
    Py_ssize_t start, len;
    long map;
    PyDictKeysObject *keys;
    uint64_t tag;
    Py_ssize_t ix;
} probed;

static void
remember_parts(PyDictObject *mp, PyObject *key, Py_ssize_t start,
               Py_ssize_t len, long map, Py_ssize_t ix)
{
    probed.mp = mp;
    probed.key = key;
    probed.start = start;
    probed.len = len;
    probed.map = map;
    probed.keys = mp->ma_keys;
    probed.tag = mp->ma_version_tag;
    probed.ix = ix;
}

static void
remember(PyDictObject *mp, PyObject *key, Py_ssize_t ix)
{
    remember_parts(mp, key, -1, 0, 0, ix);
}

static bool
recorded(PyDictObject *mp, PyObject *key, Py_ssize_t start, Py_ssize_t len,
         long map)
{
    return probed.mp == mp && probed.key == key && probed.start == start &&
           (start < 0 || (probed.len == len && probed.map == map)) &&
           probed.keys == mp->ma_keys && probed.tag == mp->ma_version_tag;
}

static void
cimpl_dictstr_forget(void)
{
    probed.mp = NULL;
}

/* _PyObject_GC_MAY_BE_TRACKED */
#define MAY_BE_TRACKED(o) \
    (PyObject_IS_GC(o) && (!PyTuple_CheckExact(o) || LE_GC_IS_TRACKED(o)))

/* insertdict's path for a key found at ix of a combined table; 'key'
   NULL for a key not built (a str, which the GC never tracks) */
static int
store_found(PyDictObject *mp, PyObject *key, Py_ssize_t ix, PyObject *value)
{
    le_dictentry_t *ep = &le_dict_entries((le_dictkeys_t *)mp->ma_keys)[ix];
    PyObject *old = ep->me_value;
    Py_XINCREF(key);
    Py_INCREF(value);
    if (!LE_GC_IS_TRACKED(mp) &&
        ((key != NULL && MAY_BE_TRACKED(key)) || MAY_BE_TRACKED(value)))
        PyObject_GC_Track(mp);
    ep->me_value = value;
    mp->ma_version_tag = ++le_dict_version;
    Py_XDECREF(old); /* may run any code */
    Py_XDECREF(key);
    return 0;
}

/* 'record': the get of a fused pair */
static PyObject *
cimpl_dictstr_getitem(PyDictObject *mp, PyObject *key, long record)
{
    PyObject *value;
    Py_hash_t hash;
    Py_ssize_t ix;
    if (record)
        probed.mp = NULL;
    ix = str_probe(mp, key, &hash, &value);
    if (ix == -2)
        return dict_subscript_fn((PyObject *)mp, key);
    if (ix == -3)
        return NULL;
    if (ix < 0) {
        _PyErr_SetKeyError(key);
        return NULL;
    }
    if (record)
        remember(mp, key, ix);
    Py_INCREF(value);
    return value;
}

/* d.get(k) (nargs 1) and d.get(k, x) (2), the get of a fused pair */
static PyObject *
cimpl_dictstr_get(PyDictObject *mp, PyObject *key, PyObject *dflt, long nargs)
{
    PyObject *value;
    Py_hash_t hash;
    Py_ssize_t ix;
    probed.mp = NULL;
    ix = str_probe(mp, key, &hash, &value);
    if (ix == -2) {
        PyObject *args[2] = {key, dflt};
        return dict_get_fn((PyObject *)mp, args, nargs);
    }
    if (ix == -3)
        return NULL;
    if (ix < 0)
        value = nargs == 2 ? dflt : Py_None;
    else
        remember(mp, key, ix);
    Py_INCREF(value);
    return value;
}

static int
cimpl_dictstr_contains(PyDictObject *mp, PyObject *key)
{
    PyObject *value;
    Py_hash_t hash;
    Py_ssize_t ix = str_probe(mp, key, &hash, &value);
    if (ix == -2)
        return PyDict_Contains((PyObject *)mp, key);
    if (ix == -3)
        return -1;
    return ix >= 0;
}

/* 'fused': the store of a fused pair */
static int
cimpl_dictstr_setitem(PyDictObject *mp, PyObject *key, PyObject *value,
                      long fused)
{
    PyObject *old;
    Py_hash_t hash;
    Py_ssize_t ix;
    if (fused) {
        if (recorded(mp, key, -1, 0, 0)) {
            probed.mp = NULL;
            return store_found(mp, key, probed.ix, value);
        }
        probed.mp = NULL;
    }
    ix = str_probe(mp, key, &hash, &old);
    if (ix == -2)
        return PyDict_SetItem((PyObject *)mp, key, value);
    if (ix == -3)
        return -1;
    if (ix < 0)
        return _PyDict_SetItem_KnownHash((PyObject *)mp, key, value, hash);
    return store_found(mp, key, ix, value);
}

/* ---- a key not built: a view or a case map (punicodeobject.c) ----

   d[k], k in d, d.get(k[, x]) and d[k] = v with k a view or a case map
   probe with its characters through the copies of
   lookdict_unicode and lookdict_unicode_nodummy, which are CPython's but
   for comparing and hashing those characters instead of an object's, so
   no key object is made. A key is built only where one must exist:
   inserted by a store, held by a KeyError, or where the probe can't be
   used (another lookup function, a root not compact ASCII, a case map
   too long for the buffer). There the helper returns 'key needed', having
   done nothing, and the compiled code takes a branch, made only when
   first taken (runtime_condition_f), that forces the key and calls the
   object-key helper. So the virtual becomes CPython's object wherever
   one exists, and the key a store inserts is the str any later use of
   the variable sees, as in CPython. */

/* 'key needed': a value no dict holds, as nothing else can reach it */
static PyObject key_needed = {.ob_refcnt = 1, .ob_type = &PyBaseObject_Type};

/* ix, DKIX_EMPTY, or -2: the key is needed */
static Py_ssize_t
key_probe(PyDictObject *mp, PyObject *root, Py_ssize_t start, Py_ssize_t len,
          long map, PyObject **pvalue)
{
    dict_lookup_func f = mp->ma_keys->dk_lookup;
    le_strkey_t k;
    Py_hash_t hash;
    char buf[LE_KEY_BUF];
    if (f != lookdict_unicode_nodummy_fn && f != lookdict_unicode_fn)
        return -2;
    if (!PsycoUnicode_KeyChars(&k, &hash, buf, root, start, len, map))
        return -2;
    if (f == lookdict_unicode_nodummy_fn)
        return cimpl_lookdict_unicode_nodummy_str_key(mp, &k, hash, pvalue);
    return cimpl_lookdict_unicode_str_key(mp, &k, hash, pvalue);
}

static PyObject *
needed(void)
{
    Py_INCREF(&key_needed);
    return &key_needed;
}

/* d[k]; 'record': the get of a fused pair. A missing key's KeyError
   holds the key, so needs it. */
static PyObject *
cimpl_dictkey_getitem(PyDictObject *mp, PyObject *root, Py_ssize_t start,
                      Py_ssize_t len, long map, long record)
{
    PyObject *value;
    Py_ssize_t ix;
    if (record)
        probed.mp = NULL;
    ix = key_probe(mp, root, start, len, map, &value);
    if (ix < 0)
        return needed();
    if (record)
        remember_parts(mp, root, start, len, map, ix);
    Py_INCREF(value);
    return value;
}

/* d.get(k) (dflt None) and d.get(k, dflt); 'record': the get of a fused
   pair */
static PyObject *
cimpl_dictkey_get(PyDictObject *mp, PyObject *root, Py_ssize_t start,
                  Py_ssize_t len, long map, PyObject *dflt, long record)
{
    PyObject *value;
    Py_ssize_t ix;
    if (record)
        probed.mp = NULL;
    ix = key_probe(mp, root, start, len, map, &value);
    if (ix == -2)
        return needed();
    if (ix < 0)
        value = dflt;
    else if (record)
        remember_parts(mp, root, start, len, map, ix);
    Py_INCREF(value);
    return value;
}

/* 1 or 0, or 2: the key is needed */
static int
cimpl_dictkey_contains(PyDictObject *mp, PyObject *root, Py_ssize_t start,
                       Py_ssize_t len, long map)
{
    PyObject *value;
    Py_ssize_t ix = key_probe(mp, root, start, len, map, &value);
    return ix == -2 ? 2 : ix >= 0;
}

/* 0 stored over the key found, or 1: the key is needed (to insert it);
   'fused': the store of a fused pair */
static int
cimpl_dictkey_setitem(PyDictObject *mp, PyObject *root, Py_ssize_t start,
                      Py_ssize_t len, long map, PyObject *value, long fused)
{
    PyObject *old;
    Py_ssize_t ix;
    if (fused) {
        if (recorded(mp, root, start, len, map)) {
            probed.mp = NULL;
            return store_found(mp, NULL, probed.ix, value);
        }
        probed.mp = NULL;
    }
    ix = key_probe(mp, root, start, len, map, &old);
    if (ix < 0)
        return 1;
    return store_found(mp, NULL, ix, value);
}

/* r, a helper's result, or NULL with *need set on the branch where it is
   'sentinel' (r released); NULL with *need false on an error */
static vinfo_t *
unless_needed(PsycoObject *po, vinfo_t *r, long sentinel, bool *need)
{
    condition_code_t cc;
    *need = false;
    if (r == NULL)
        return NULL;
    cc = integer_cmp_i(po, r, sentinel, Py_EQ);
    if (cc == CC_ERROR) {
        vinfo_decref(r, po);
        return NULL;
    }
    if (runtime_condition_f(po, cc)) {
        vinfo_decref(r, po);
        *need = true;
        return NULL;
    }
    return r;
}

#define PROBE_FLAGS (CfNoPython | CfNoBlock)

/* each: the probe of k, a view or a case map; NULL (or false) with *need
   set where the key must be built first */
static vinfo_t *
key_getitem(PsycoObject *po, vinfo_t *mp, vinfo_t *k, bool record, bool *need)
{
    vinfo_t *root, *start, *len;
    long map;
    PsycoUnicode_KeyParts(k, &root, &start, &len, &map);
    return unless_needed(
        po,
        psyco_generic_call(po, cimpl_dictkey_getitem,
                           CfCommonNewRefPyObjectNoError | PROBE_FLAGS,
                           "vvvvll", mp, root, start, len, map, (long)record),
        (long)&key_needed, need);
}

static vinfo_t *
key_get(PsycoObject *po, vinfo_t *mp, vinfo_t *k, vinfo_t *dflt, bool record,
        bool *need)
{
    vinfo_t *root, *start, *len, *r, *none = NULL;
    long map;
    PsycoUnicode_KeyParts(k, &root, &start, &len, &map);
    if (dflt == NULL)
        dflt = none = psyco_vi_None();
    r = psyco_generic_call(
        po, cimpl_dictkey_get, CfCommonNewRefPyObjectNoError | PROBE_FLAGS,
        "vvvvlvl", mp, root, start, len, map, dflt, (long)record);
    vinfo_xdecref(none, po);
    return unless_needed(po, r, (long)&key_needed, need);
}

static vinfo_t *
key_contains(PsycoObject *po, vinfo_t *mp, vinfo_t *k, bool *need)
{
    vinfo_t *root, *start, *len;
    long map;
    PsycoUnicode_KeyParts(k, &root, &start, &len, &map);
    return unless_needed(
        po,
        psyco_generic_call(po, cimpl_dictkey_contains,
                           CfReturnTypeInt | CfNotNegative | PROBE_FLAGS,
                           "vvvvl", mp, root, start, len, map),
        2, need);
}

static bool
key_setitem(PsycoObject *po, vinfo_t *mp, vinfo_t *k, vinfo_t *value,
            bool fused, bool *need)
{
    vinfo_t *root, *start, *len, *r;
    long map;
    PsycoUnicode_KeyParts(k, &root, &start, &len, &map);
    /* the old value's decref may run any code */
    r = unless_needed(
        po,
        psyco_generic_call(po, cimpl_dictkey_setitem,
                           CfReturnTypeInt | CfNotNegative, "vvvvlvl", mp,
                           root, start, len, map, value, (long)fused),
        1, need);
    vinfo_xdecref(r, po);
    return r != NULL;
}

static bool
not_built(vinfo_t *k)
{
    vinfo_t *root, *start, *len;
    long map;
    return PsycoUnicode_KeyParts(k, &root, &start, &len, &map);
}

DEFINEFN
bool
PsycoDict_StrKey(vinfo_t *mp, vinfo_t *key)
{
    PyTypeObject *tk = Psyco_KnownType(key);
    return dict_get_fn != NULL && Psyco_KnownType(mp) == &PyDict_Type &&
           (tk == NULL || tk == &PyUnicode_Type);
}

DEFINEFN
vinfo_t *
PsycoDict_StrGetItem(PsycoObject *po, vinfo_t *mp, vinfo_t *key, bool record)
{
    if (not_built(key)) {
        bool need;
        vinfo_t *r = key_getitem(po, mp, key, record, &need);
        if (!need)
            return r;
    }
    if (!PsycoUnicode_ForceKey(po, key))
        return NULL;
    return psyco_generic_call(po, cimpl_dictstr_getitem,
                              CfCommonNewRefPyObject, "vvl", mp, key,
                              (long)record);
}

DEFINEFN
vinfo_t *
PsycoDict_StrGet(PsycoObject *po, vinfo_t *mp, vinfo_t *key, vinfo_t *dflt)
{
    if (not_built(key)) {
        bool need;
        vinfo_t *r = key_get(po, mp, key, dflt, true, &need);
        if (!need)
            return r;
    }
    if (!PsycoUnicode_ForceKey(po, key))
        return NULL;
    if (dflt == NULL)
        return psyco_generic_call(po, cimpl_dictstr_get,
                                  CfCommonNewRefPyObject, "vvll", mp, key, 0L,
                                  1L);
    return psyco_generic_call(po, cimpl_dictstr_get, CfCommonNewRefPyObject,
                              "vvvl", mp, key, dflt, 2L);
}

DEFINEFN
bool
PsycoDict_StrSetItem(PsycoObject *po, vinfo_t *mp, vinfo_t *key,
                     vinfo_t *value, bool fused)
{
    if (not_built(key)) {
        bool need, ok = key_setitem(po, mp, key, value, fused, &need);
        if (!need)
            return ok;
        fused = false; /* the helper cleared the record */
    }
    if (!PsycoUnicode_ForceKey(po, key))
        return false;
    return psyco_generic_call(po, cimpl_dictstr_setitem, CfCommonIntZeroOk,
                              "vvvl", mp, key, value, (long)fused) != NULL;
}

DEFINEFN
bool
PsycoDict_StrForget(PsycoObject *po)
{
    return psyco_generic_call(po, cimpl_dictstr_forget,
                              CfNoReturnValue | CfNoPython | CfNoBlock,
                              "") != NULL;
}

static vinfo_t *
pdict_subscript(PsycoObject *po, vinfo_t *mp, vinfo_t *key)
{
    if (!PsycoDict_StrKey(mp, key))
        return psyco_generic_call(po, dict_subscript_fn,
                                  CfCommonNewRefPyObject, "vv", mp, key);
    return PsycoDict_StrGetItem(po, mp, key, false);
}

static vinfo_t *
pdict_contains(PsycoObject *po, vinfo_t *mp, vinfo_t *key)
{
    if (!PsycoDict_StrKey(mp, key))
        return psyco_generic_call(po, PyDict_Contains, CfCommonIntBoolOrError,
                                  "vv", mp, key);
    if (not_built(key)) {
        bool need;
        vinfo_t *r = key_contains(po, mp, key, &need);
        if (!need)
            return r;
    }
    if (!PsycoUnicode_ForceKey(po, key))
        return NULL;
    return psyco_generic_call(po, cimpl_dictstr_contains,
                              CfCommonIntBoolOrError, "vv", mp, key);
}

/* value NULL: del d[k] */
static vinfo_t *
pdict_ass_sub(PsycoObject *po, vinfo_t *mp, vinfo_t *key, vinfo_t *value)
{
    if (value == NULL)
        return psyco_generic_call(po, dict_ass_sub_fn, CfCommonIntZeroOk,
                                  "vvl", mp, key, (long)NULL);
    if (!PsycoDict_StrKey(mp, key))
        return psyco_generic_call(po, dict_ass_sub_fn, CfCommonIntZeroOk,
                                  "vvv", mp, key, value);
    return PsycoDict_StrSetItem(po, mp, key, value, false) ? VINFO_OK : NULL;
}

/* d.get(k[, x]): the translated meta, after a pending case map key is
   built with its hash */
static vinfo_t *
pdict_get(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    int n = PsycoTuple_Load(vargs);
    vinfo_t *key = n == 1 || n == 2 ? PsycoTuple_GET_ITEM(vargs, 0) : NULL;
    if (key != NULL && not_built(key) && dict_get_fn != NULL &&
        Psyco_KnownType(self) == &PyDict_Type) {
        bool need;
        vinfo_t *r = key_get(po, self, key,
                             n == 2 ? PsycoTuple_GET_ITEM(vargs, 1) : NULL,
                             false, &need);
        if (!need)
            return r;
    }
    if (key != NULL && !PsycoUnicode_ForceKey(po, key))
        return NULL;
    return dict_get(po, self, vargs);
}

/* lookdict_unicode_nodummy is the lookup of a new dict of str keys, and
   lookdict_unicode that of one which has lost a key; a key of another
   type gives a third. dict.get's C function serves a fused get's other
   keys. Without all of them the probe is not used. */
static void
define_str_probes(void)
{
    PyObject *d = PyDict_New(), *e = PyDict_New(), *get;
    dict_lookup_func a = NULL, b = NULL, c = NULL;
    if (d != NULL && PyDict_SetItemString(d, "a", Py_None) == 0) {
        a = ((PyDictObject *)d)->ma_keys->dk_lookup;
        if (PyDict_DelItemString(d, "a") == 0)
            b = ((PyDictObject *)d)->ma_keys->dk_lookup;
    }
    if (e != NULL && PyDict_SetItem(e, Py_None, Py_None) == 0)
        c = ((PyDictObject *)e)->ma_keys->dk_lookup;
    Py_XDECREF(d);
    Py_XDECREF(e);
    PyErr_Clear();
    get = PyDict_GetItemString(le_type_dict(&PyDict_Type), "get");
    if (a == NULL || b == NULL || c == NULL || a == b || c == a || c == b ||
        get == NULL || Py_TYPE(get) != &PyMethodDescr_Type ||
        ((PyMethodDescrObject *)get)->d_method->ml_flags != METH_FASTCALL)
        return;
    lookdict_unicode_nodummy_fn = a;
    lookdict_unicode_fn = b;
    dict_get_fn =
        (_PyCFunctionFast)(void (*)(void))((PyMethodDescrObject *)get)
            ->d_method->ml_meth;
    dict_subscript_fn = PyDict_Type.tp_as_mapping->mp_subscript;
    dict_ass_sub_fn = PyDict_Type.tp_as_mapping->mp_ass_subscript;
    Psyco_DefineMeta(dict_subscript_fn, pdict_subscript);
    Psyco_DefineMeta(dict_ass_sub_fn, pdict_ass_sub);
    Psyco_DefineMeta(PyDict_Type.tp_as_sequence->sq_contains, pdict_contains);
}

INITIALIZATIONFN
void
psy_dictobject_init(void)
{
    gen_dictobject_init();
    gen_dictobject_methods_init();
    define_str_probes();
    if (cfunc_dict_get != NULL)
        Psyco_DefineMeta(cfunc_dict_get, pdict_get);
}
