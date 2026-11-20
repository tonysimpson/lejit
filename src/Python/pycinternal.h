 /***************************************************************/
/***         Includes Python internal headers                  ***/
 /***************************************************************/


#ifndef _PYCINTERNAL_H
#define _PYCINTERNAL_H

#include <opcode.h>
/* and, as values no instruction has, the opcodes Ni names that this
   CPython lacks (tools/features/features.py) */
#include "ni_features_gen.h"

/* Ni keeps every position in the bytecode in bytes (merge points, the
   compiler's next_instr, a block's handler). A jump's argument is converted
   where it is read, and a position where CPython's frame takes it
   (f_lasti, b_handler), in instructions where the source counts them. */
#define NI_JUMP_BYTES(a)                                                       \
  ((a) * (NI_HAS_JUMP_IN_INSTRUCTIONS ? (int)sizeof(_Py_CODEUNIT) : 1))
/* -1, before the first instruction, is -1 in either unit */
static inline int ni_lasti_of(int offset) {
  return NI_HAS_LASTI_IN_INSTRUCTIONS && offset > 0
             ? offset / (int)sizeof(_Py_CODEUNIT)
             : offset;
}


/* COMPARE_OP's opargs after the rich comparisons, as macros with the
   values of opcode.h's enum cmp_op */

#define  PyCmp_IN		6
#define  PyCmp_NOT_IN		7
#define  PyCmp_IS		8
#define  PyCmp_IS_NOT		9
#define  PyCmp_EXC_MATCH	10


#endif /* _PYCINTERNAL_H */
