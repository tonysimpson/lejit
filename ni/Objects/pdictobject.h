 /***************************************************************/
/***            Psyco equivalent of dictobject.h               ***/
 /***************************************************************/

#ifndef _PSY_DICTOBJECT_H
#define _PSY_DICTOBJECT_H


#include "pobject.h"
#include "pabstract.h"

EXTERNFN vinfo_t* PsycoDict_New(PsycoObject* po);
EXTERNFN vinfo_t* PsycoDict_Copy(PsycoObject* po, vinfo_t* orig);
EXTERNFN bool PsycoDict_SetItem(PsycoObject* po, vinfo_t* vdict,
				vinfo_t* key, vinfo_t* vvalue);
EXTERNFN bool PsycoDict_SetItemC(PsycoObject* po, vinfo_t* vdict,
                                 PyObject* key, vinfo_t* vvalue);

#if HAVE_DICT_NEWPRESIZED
EXTERNFN vinfo_t* _PsycoDict_NewPresized(PsycoObject* po, long minused);
#else
#  define _PsycoDict_NewPresized(po, minused)     PsycoDict_New(po)
#endif


#endif /* _PSY_LISTOBJECT_H */
