/***************************************************************/
/***               Detection of Python features                ***/
/***************************************************************/

#ifndef _PYVER_H
#define _PYVER_H

#include <Python.h>

#define PSYCO_VERSION_HEX 0x010600f0 /* 1.6 */

/*****************************************************************/
/***   Detects differences between Python versions             ***/

/* Note: not all features can be automatically detected; in some cases
   we just assume that the feature is present or not based on some
   other feature that has been introduced roughly at the same time.
   This may need fixes to compile with some intermediary Python
   versions. */

#define PSYCO_CAN_CALL_UNICODE                                                 \
  0 /* prevent references to PyUnicode_Xxx                                     \
       functions causing potential linker                                      \
       errors because of UCS2/UCS4 name                                        \
       mangling */

#define HAVE_arrayobject_allocated (PY_VERSION_HEX >= 0x02040000) /* 2.4 */
#define VERYCONVOLUTED_IMPORT_NAME (PY_VERSION_HEX >= 0x02050000) /* 2.5 */

#if HAVE_LONG_LONG && !defined(PY_LONG_LONG)
#define PY_LONG_LONG LONG_LONG /* Python < 2.3 */
#endif

/* for extra fun points, let's try to emulate Python's ever-changing behavior
   (but not too hard; you can still tell the difference in Python < 2.5 if
   you use -sys.maxint-1 as the lower bound of a slice, provided you inspect
   it with a custom __getslice__() */
#if PY_VERSION_HEX < 0x02030000
#define LARGE_NEG_LONG_AS_SLICE_INDEX 0 /* 2.2 */
#elif PY_VERSION_HEX < 0x02050000
#define LARGE_NEG_LONG_AS_SLICE_INDEX (-INT_MAX) /* 2.3, 2.4 */
#else
#define LARGE_NEG_LONG_AS_SLICE_INDEX (-INT_MAX - 1) /* 2.5 */
#endif

#define HAVE_NEGATIVE_IDS (PY_VERSION_HEX < 0x02050000) /* Python < 2.5 */

#define MATHMODULE_USES_METH_O (PY_VERSION_HEX >= 0x02060000) /* Py >= 2.6 */

#define T_UINT_READS_AS_SIGNED                                                 \
  (PY_VERSION_HEX < 0x02050000) /* Python < 2.5                                \
                                 */

#define HAVE_DICT_NEWPRESIZED (PY_VERSION_HEX >= 0x02060000) /* Py >= 2.6 */

#if (PY_VERSION_HEX >= 0x02070000) /* Python >= 2.7 */
#define HAVE_NEXT_INSTR_POP 1
#define NEXT_INSTR_POP (-INT_MAX - 1)
#else
#define HAVE_NEXT_INSTR_POP 0
#endif

#define HAVE_PEEK_LIST_APPEND (PY_VERSION_HEX >= 0x02070000) /* Py >= 2.7 */

#endif /* _PYVER_H */
