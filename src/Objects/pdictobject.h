/***************************************************************/
/***            Psyco equivalent of dictobject.h               ***/
/***************************************************************/

#ifndef _PSY_DICTOBJECT_H
#define _PSY_DICTOBJECT_H

#include "pobject.h"
#include "pabstract.h"

EXTERNFN vinfo_t *
PsycoDict_New(PsycoObject *po);
EXTERNFN bool
PsycoDict_SetItemC(PsycoObject *po, vinfo_t *vdict, PyObject *key,
                   vinfo_t *vvalue);

/* The exact-str key probe (pdictobject.c), for pycompiler.c's fused get
   then store: StrKey says whether the probe serves d[k] at compile time;
   'record' makes a get record the slot it finds, 'fused' makes a store
   reuse it; StrForget clears it. */
EXTERNFN bool
PsycoDict_StrKey(vinfo_t *mp, vinfo_t *key);
EXTERNFN vinfo_t *
PsycoDict_StrGetItem(PsycoObject *po, vinfo_t *mp, vinfo_t *key, bool record);
EXTERNFN vinfo_t *
PsycoDict_StrGet(PsycoObject *po, vinfo_t *mp, vinfo_t *key, vinfo_t *dflt);
EXTERNFN bool
PsycoDict_StrSetItem(PsycoObject *po, vinfo_t *mp, vinfo_t *key,
                     vinfo_t *value, bool fused);
EXTERNFN bool
PsycoDict_StrForget(PsycoObject *po);

#if LE_HAS_PYDICT_NEWPRESIZED
EXTERNFN vinfo_t *
_PsycoDict_NewPresized(PsycoObject *po, long minused);
#else
#define _PsycoDict_NewPresized(po, minused) PsycoDict_New(po)
#endif

#endif /* _PSY_LISTOBJECT_H */
