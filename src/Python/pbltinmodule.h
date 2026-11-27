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

#if LE_HAS_SUM_COMPENSATED
/* sum() where it compensates float sums, as builtin_sum_impl's phases
   (GENEXPR_SUM...): its start ('start' its argument, or NULL for 0), one
   item's step (*phase, *acc and *comp replaced; false on error, leaving
   them) and its result (a new reference) */
EXTERNFN bool
psyco_sum_start(PsycoObject *po, vinfo_t *start, int *phase, vinfo_t **acc,
                vinfo_t **comp);
EXTERNFN bool
psyco_sum_step(PsycoObject *po, int *phase, vinfo_t **acc, vinfo_t **comp,
               vinfo_t *item);
EXTERNFN vinfo_t *
psyco_sum_result(PsycoObject *po, int phase, vinfo_t *acc, vinfo_t *comp);
#else
/* one item of a sum() where float sums aren't compensated: acc + item as
   builtin_sum_impl gives it, a new reference */
EXTERNFN vinfo_t *
psyco_sum_add(PsycoObject *po, vinfo_t *acc, vinfo_t *item);
#endif

#endif /* _PSY_BLTINMODULE_H */
