#ifndef _PSY_ARRAY_H
#define _PSY_ARRAY_H

#include "../Objects/pabstract.h"

/* the first time an exact array.array of type tp is subscripted: checks
   array's layout against it and, if it is the expected one, gives array's
   subscript slots their metas (Modules/parray.c) */
EXTERNFN void
PsycoArray_Seen(PyTypeObject *tp);

#endif /* _PSY_ARRAY_H */
