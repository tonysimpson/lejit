/***************************************************************/
/***         Includes Python internal headers                  ***/
/***************************************************************/

#ifndef _PYCINTERNAL_H
#define _PYCINTERNAL_H

#include <opcode.h>
/* and, as values no instruction has, the opcodes LeJIT names that this
   CPython lacks (tools/features/features.py) */
#include "le_features_gen.h"
#if LE_HAS_OPCODE_CALL_INTRINSIC_1
/* CALL_INTRINSIC_1's and CALL_INTRINSIC_2's arguments */
#define Py_BUILD_CORE
#include "internal/pycore_intrinsics.h"
#undef Py_BUILD_CORE
#endif
#if LE_HAS_LAZY_IMPORT_REIFY_LINKABLE
/* a module's dict may hold a lazy import's proxy (PyLazyImport_Type),
   which a global's load replaces with the module (_PyLazyImport_Reify) */
#define Py_BUILD_CORE
#include "internal/pycore_lazyimportobject.h"
#undef Py_BUILD_CORE
#endif

/* LeJIT keeps every position in the bytecode in bytes (merge points, the
   compiler's next_instr, a block's handler). A jump's argument is converted
   where it is read, and a position where CPython's frame takes it
   (f_lasti, b_handler), in instructions where the source counts them. */
#define LE_JUMP_BYTES(a) \
    ((a) * (LE_HAS_JUMP_IN_INSTRUCTIONS ? (int)sizeof(_Py_CODEUNIT) : 1))
/* -1, before the first instruction, is -1 in either unit */
static inline int
le_lasti_of(int offset)
{
    return LE_HAS_LASTI_IN_INSTRUCTIONS && offset > 0
               ? offset / (int)sizeof(_Py_CODEUNIT)
               : offset;
}

#if LE_HAS_EXCEPTION_TABLE
/* the type of an except handler's block in the compiler's block stack, as
   a value no opcode has: where handlers are in an exception table, CPython
   has no block stack */
#define EXCEPT_HANDLER (-1)
#endif

/* COMPARE_OP's opargs after the rich comparisons, as macros with the
   values of opcode.h's enum cmp_op */

#define PyCmp_IN 6
#define PyCmp_NOT_IN 7
#define PyCmp_IS 8
#define PyCmp_IS_NOT 9
#define PyCmp_EXC_MATCH 10

#endif /* _PYCINTERNAL_H */
