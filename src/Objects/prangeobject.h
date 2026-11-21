#ifndef _PSY_RANGEOBJECT_H
#define _PSY_RANGEOBJECT_H

#include "pabstract.h"
#include "pobject.h"

/* range(args) for 1 to 3 int arguments, as a virtual range. Returns 1 with
   a new reference in *result, 0 if the arguments aren't ones it handles
   (the caller falls back to the real call), -1 on error or promotion. */
EXTERNFN int
PsycoRange_New(PsycoObject *po, vinfo_t **args, int n, vinfo_t **result);

#endif /* _PSY_RANGEOBJECT_H */
