#ifndef _PSY_SLICEOBJECT_H
#define _PSY_SLICEOBJECT_H

#include "pabstract.h"
#include "pobject.h"

/* BUILD_SLICE: a virtual slice of start, stop and step (NULL for None).
   Takes new references to the three. */
EXTERNFN vinfo_t *PsycoSlice_New(vinfo_t *start, vinfo_t *stop, vinfo_t *step);

/* PySlice_Unpack for a virtual slice whose step is None and whose bounds
   are None or ints: start and stop as Py_ssize_t words, new references in
   *pstart and *pstop. Returns 1; 0 for anything else (the caller passes the
   slice on, which computes the real object); -1 on error or promotion. */
EXTERNFN int PsycoSlice_Unpack(PsycoObject *po, vinfo_t *slice,
                               vinfo_t **pstart, vinfo_t **pstop);

#endif /* _PSY_SLICEOBJECT_H */
