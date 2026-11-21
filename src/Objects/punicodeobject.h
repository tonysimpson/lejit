#ifndef _PSY_UNICODEOBJECT_H
#define _PSY_UNICODEOBJECT_H

#include "pabstract.h"
#include "pobject.h"

/* s.split() or s.split(sep), not yet built: a virtual list that
   PyUnicode_Split makes when anything else reads it. */
EXTERNFN bool
PsycoUnicode_IsSplitList(vinfo_t *v);

/* iter() of such a list that only the caller holds: a virtual sequence
   iterator over it that scans the string as FOR_ITER asks for items
   (PsycoUnicode_SplitNext), so the list is made only if the iterator is
   forced. */
EXTERNFN vinfo_t *
PsycoUnicode_SplitIter(PsycoObject *po, vinfo_t *list);

/* the next item of such an iterator 'it' over 'list', as piter_iternext
   gives (NULL with StopIteration set when it ends) */
EXTERNFN vinfo_t *
PsycoUnicode_SplitNext(PsycoObject *po, vinfo_t *it, vinfo_t *list);

/* a key for a dict probe: a pending lower() or upper() is built with its
   hash set in the same pass; anything else is left as it is. false on an
   error. */
EXTERNFN bool
PsycoUnicode_ForceKey(PsycoObject *po, vinfo_t *v);

/* A str key not built, as the copies of lookdict_unicode and
   lookdict_unicode_nodummy read it (the str_key representation): its
   characters, ASCII, and its length. */
typedef struct {
    const Py_UCS1 *data;
    Py_ssize_t length;
} le_strkey_t;

/* a view or a case map: true with (borrowed) its root, start and length,
   and 'map' -1 (a view), 0 (lower) or 1 (upper) */
EXTERNFN bool
PsycoUnicode_KeyParts(vinfo_t *v, vinfo_t **root, vinfo_t **start,
                      vinfo_t **len, long *map);

/* at run time, such a key of root[start:start + len] with its hash as
   unicode_hash would give it, the characters of a case map written into
   'buf' (LE_KEY_BUF bytes); 0 where it can't be read so (root not compact
   ASCII, a case map longer than the buffer) */
#define LE_KEY_BUF 128
EXTERNFN int
PsycoUnicode_KeyChars(le_strkey_t *k, Py_hash_t *hash, char *buf,
                      PyObject *root, Py_ssize_t start, Py_ssize_t len,
                      long map);

/* FORMAT_VALUE of 'n', an exact int, with no conversion and no spec, for
   an f-string's BUILD_STRING: 1 with *part its str() not made yet, 0 if
   it is not made (an int too long), -1 on an error or promotion */
EXTERNFN int
PsycoUnicode_DecimalPart(PsycoObject *po, vinfo_t *n, vinfo_t **part);

/* BUILD_STRING of the n items from 'items' when one is such a part: 1
   with *result the str, 0 if not, -1 on an error */
EXTERNFN int
PsycoUnicode_BuildParts(PsycoObject *po, vinfo_t **items, int n,
                        vinfo_t **result);

#endif /* _PSY_UNICODEOBJECT_H */
