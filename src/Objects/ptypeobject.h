/***************************************************************/
/***            Psyco equivalent of typeobject.c's slots       ***/
/***************************************************************/

#ifndef _PSY_TYPEOBJECT_H
#define _PSY_TYPEOBJECT_H

#include "pobject.h"
#include "pabstract.h"

/* type_call(tp, varg, vkw) for a heap class tp known now */
EXTERNFN vinfo_t *
psyco_heap_type_call(PsycoObject *po, PyTypeObject *tp, vinfo_t *vtype,
                     vinfo_t *varg, vinfo_t *vkw);

#endif /* _PSY_TYPEOBJECT_H */
