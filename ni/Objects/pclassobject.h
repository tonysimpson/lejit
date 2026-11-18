/***************************************************************/
/***            Psyco equivalent of classobject.h              ***/
/***************************************************************/

#ifndef _PSY_CLASSOBJECT_H
#define _PSY_CLASSOBJECT_H

#include "pabstract.h"
#include "pobject.h"

/* Instance methods: METHOD_im_func, METHOD_im_self and their indices are
   in virtual_fields_gen.h (pobject.h) */
EXTERNFN vinfo_t *pinstancemethod_call(PsycoObject *po, vinfo_t *methobj,
                                       vinfo_t *arg, vinfo_t *kw);

/***************************************************************/
/***   Virtual-time object builder                           ***/

/* not-yet-computed instance method objects. Usually not computed at all,
   but if it needs be, will call PyMethod_New(). */
EXTERNVAR source_virtual_t psyco_computed_method;

/* PyMethod_New(func, self), virtual; new references to both (generated) */
EXTERNFN
vinfo_t *PsycoMethod_New(vinfo_t *func, vinfo_t *self);

#endif /* _PSY_CLASSOBJECT_H */
