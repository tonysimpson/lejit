 /***************************************************************/
/***         Includes Python internal headers                  ***/
 /***************************************************************/


#ifndef _PYCINTERNAL_H
#define _PYCINTERNAL_H

#include <opcode.h>


/* COMPARE_OP's opargs after the rich comparisons, as macros with the
   values of opcode.h's enum cmp_op */

#define  PyCmp_IN		6
#define  PyCmp_NOT_IN		7
#define  PyCmp_IS		8
#define  PyCmp_IS_NOT		9
#define  PyCmp_EXC_MATCH	10


#endif /* _PYCINTERNAL_H */
