/***************************************************************/
/***          lejit._core's private functions for tests        ***/
/***************************************************************/

#ifndef _TESTING_H
#define _TESTING_H

#include "psyco.h"

/* the module's methods for tests and measurements (lejit.c) */
EXTERNVAR PyMethodDef le_testing_methods[];

/* tests/jit's count of the object allocator's allocations, called through
   ctypes */
EXTERNFN void
le_allocs_begin(void);
EXTERNFN long
le_allocs_end(void);
EXTERNFN long
le_allocs_asked(void);

#endif /* _TESTING_H */
