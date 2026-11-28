/***************************************************************/
/***            Psyco equivalent of longobject.h               ***/
/***************************************************************/

#ifndef _PSY_LONGOBJECT_H
#define _PSY_LONGOBJECT_H

#include "pabstract.h"
#include "pobject.h"

#define PsycoLong_Check(tp) PyType_TypeCheck(tp, &PyLong_Type)

/* ob_size is the signed number of digits; ob_digit[0] is only there when
   ob_size != 0 (_PyLong_New(0) allocates no digit). Where ints have an
   lv_tag in its place, there is always a digit. */
#define LONG_ob_size FIX_signed_size
#if LE_HAS_LONG_VALUE
/* where ob_size was: (the digit count << 3) | the sign flags (0 positive,
   1 zero, 2 negative) */
#define LONG_lv_tag FIX_signed_size
_Static_assert(offsetof(PyLongObject, long_value.lv_tag) ==
                   offsetof(PyVarObject, ob_size),
               "LeJIT: lv_tag is where ob_size is");
#endif
#define LONG_ob_digit0 \
    DEF_ARRAY_FIELD(PyLongObject, LE_PyLongObject_digits, FIX_signed_size)
#define iLONG_OB_DIGIT0 FIELD_INDEX(LONG_ob_digit0)
/* the value as a machine word: a virtual int's, and a run-time int's once
   read (plongobject.c:plong_medium_value) or kept from when it was virtual;
   not a C field, so it is never read from memory (a field read the virtual
   int doesn't have makes psyco_internal_getfld compute it first) */
#define iLONG_VALUE FIELDS_TOTAL(LONG_ob_digit0)
#define LONG_TOTAL (iLONG_VALUE + 1)

/* not-yet-computed ints; computing calls PyLong_FromLong */
EXTERNVAR source_virtual_t psyco_computed_long;

/* not-yet-computed bools (pboolobject.h): virtual ints of type bool */
EXTERNVAR source_virtual_t psyco_computed_bool;

PSY_INLINE bool
PsycoLong_IsVirtual(vinfo_t *v)
{
    return v->source == VirtualTime_New(&psyco_computed_long) ||
           v->source == VirtualTime_New(&psyco_computed_bool);
}

/* !! consumes a reference to vlong. PsycoLong_FromLong() does not. */
PSY_INLINE vinfo_t *
PsycoLong_FROM_WORD(vinfo_t *vlong)
{
    vinfo_t *result = vinfo_new(VirtualTime_New(&psyco_computed_long));
    result->array = array_new(LONG_TOTAL);
    result->array->items[iOB_TYPE] =
        vinfo_new(CompileTime_New((long)(&PyLong_Type)));
    result->array->items[iLONG_VALUE] = vlong;
    return result;
}

/* MEDIUM_VALUE of an int that is one digit at run time, as a new reference
   in *pval: 1; 0 if it isn't one digit; -1 on error or promotion */
EXTERNFN int
PsycoLong_MediumValue(PsycoObject *po, vinfo_t *v, vinfo_t **pval);
/* a new reference to a word with the sign of the int: a virtual int's
   word, a real int's ob_size (or, where ints have an lv_tag, 1, 0 or -1);
   NULL on error */
EXTERNFN vinfo_t *
PsycoLong_Sign(PsycoObject *po, vinfo_t *v);
/* the run-time condition that a real int has at most n digits; CC_ERROR
   on error */
EXTERNFN condition_code_t
PsycoLong_SizeWithin(PsycoObject *po, vinfo_t *v, long n);
EXTERNFN vinfo_t *
PsycoNumber_AsSsize_t(PsycoObject *po, vinfo_t *v, PyObject *err);
/* PyLong_AsDouble's meta: a new reference to the double, NULL on error */
EXTERNFN vinfo_t *
PsycoLong_AsDouble(PsycoObject *po, vinfo_t *v);
EXTERNFN vinfo_t *
PsycoLong_FromUnsignedLong(PsycoObject *po, vinfo_t *v);
EXTERNFN vinfo_t *
PsycoLong_FromLong(PsycoObject *po, vinfo_t *v);
EXTERNFN vinfo_t *
PsycoLong_FromSsize_t(PsycoObject *po, vinfo_t *v);
EXTERNFN vinfo_t *
PsycoLong_FROM_LONG(PsycoObject *po, vinfo_t *v); /* consumes v */

#endif /* _PSY_LONGOBJECT_H */
