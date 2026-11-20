#ifndef _PSY_BOOLOBJECT_H
#define _PSY_BOOLOBJECT_H
#include "../psyco.h"
#include "pobject.h"
#include "pabstract.h"
#include "plongobject.h"

/* Virtual bools: a bool is an int (same layout, PyBool_Type), so a virtual
   bool is a virtual int of type bool whose word is 0 or 1; the int metas
   (plongobject.c) work on it. Computing it calls PyBool_FromLong. */
EXTERNVAR source_virtual_t psyco_computed_bool;

/* !! consumes a reference to v01, which must be 0 or 1 */
PSY_INLINE vinfo_t *PsycoBool_FROM_WORD(vinfo_t *v01) {
  vinfo_t *result = vinfo_new(VirtualTime_New(&psyco_computed_bool));
  result->array = array_new(LONG_TOTAL);
  result->array->items[iOB_TYPE] =
      vinfo_new(CompileTime_New((long)(&PyBool_Type)));
  result->array->items[iLONG_VALUE] = v01;
  return result;
}

PSY_INLINE vinfo_t *PsycoBool_FromCondition(PsycoObject *po,
                                            condition_code_t cc) {
  vinfo_t *vc = psyco_vinfo_condition(po, cc);
  return vc != NULL ? PsycoBool_FROM_WORD(vc) : NULL;
}

/* PyBool_FromLong's meta: True for any non-zero v (borrowed); a constant
   v gives the constant bool */
EXTERNFN vinfo_t *PsycoBool_FromLong(PsycoObject *po, vinfo_t *v);
/* the same for a v known to be 0 or 1: v is the bool's word */
EXTERNFN vinfo_t *PsycoBool_FromZeroOrOne(PsycoObject *po, vinfo_t *v);

#endif /* _PSY_BOOLOBJECT_H */
