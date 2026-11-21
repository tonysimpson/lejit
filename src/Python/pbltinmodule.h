/***************************************************************/
/***               interface to bltinmodule.c                  ***/
/***************************************************************/

#ifndef _PSY_BLTINMODULE_H
#define _PSY_BLTINMODULE_H

#include "../Objects/pobject.h"

/* Objects/abstract.c:PyObject_IsInstance's meta (by hand, in
   pbltinmodule.c): 0 or 1, the answer at compile time where it is known,
   else the real call's; NULL on error */
EXTERNFN vinfo_t *
PsycoObject_IsInstance(PsycoObject *po, vinfo_t *inst, vinfo_t *cls);

#endif /* _PSY_BLTINMODULE_H */
