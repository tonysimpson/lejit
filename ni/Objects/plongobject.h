/***************************************************************/
/***            Psyco equivalent of longobject.h               ***/
/***************************************************************/

#ifndef _PSY_LONGOBJECT_H
#define _PSY_LONGOBJECT_H

#include "pabstract.h"
#include "pobject.h"

#define PsycoLong_Check(tp) PyType_TypeCheck(tp, &PyLong_Type)

/* ob_size is the signed number of digits; ob_digit[0] is only there when
   ob_size != 0 (_PyLong_New(0) allocates no digit) */
#define LONG_ob_size FIX_signed_size
#define LONG_ob_digit0                                                         \
  UNSIGNED_FIELD(PyLongObject, digit, ob_digit[0], FIX_signed_size)
#define iLONG_OB_DIGIT0 FIELD_INDEX(LONG_ob_digit0)
/* virtual ints only: the value as a machine word; not a C field, so it is
   never read from memory (a field read the virtual int doesn't have makes
   psyco_internal_getfld compute it first) */
#define iLONG_VALUE FIELDS_TOTAL(LONG_ob_digit0)
#define LONG_TOTAL (iLONG_VALUE + 1)

/* not-yet-computed ints; computing calls PyLong_FromLong */
EXTERNVAR source_virtual_t psyco_computed_long;

PSY_INLINE bool PsycoLong_IsVirtual(vinfo_t *v) {
  return v->source == VirtualTime_New(&psyco_computed_long);
}

/* !! consumes a reference to vlong. PsycoLong_FromLong() does not. */
PSY_INLINE vinfo_t *PsycoLong_FROM_WORD(vinfo_t *vlong) {
  vinfo_t *result = vinfo_new(VirtualTime_New(&psyco_computed_long));
  result->array = array_new(LONG_TOTAL);
  result->array->items[iOB_TYPE] =
      vinfo_new(CompileTime_New((long)(&PyLong_Type)));
  result->array->items[iLONG_VALUE] = vlong;
  return result;
}

EXTERNFN vinfo_t *PsycoLong_AsLong(PsycoObject *po, vinfo_t *v);
EXTERNFN bool PsycoLong_AsDouble(PsycoObject *po, vinfo_t *v, vinfo_t **vd);
EXTERNFN vinfo_t *PsycoLong_FromUnsignedLong(PsycoObject *po, vinfo_t *v);
EXTERNFN vinfo_t *PsycoLong_FromLong(PsycoObject *po, vinfo_t *v);
EXTERNFN vinfo_t *PsycoLong_FROM_LONG(PsycoObject *po, vinfo_t *v); /* consumes v */

#endif /* _PSY_LONGOBJECT_H */
