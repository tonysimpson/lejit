/***************************************************************/
/***             Processor-specific execution                  ***/
/***************************************************************/

#ifndef _PROCESSOR_H
#define _PROCESSOR_H

#include "psyco.h"

/***************************************************************/
/*** Execution                                               ***/

/* executes a block of code. A new stack frame is created and
   the 'initial_stack' values are PUSHed into it. The number
   of values pushed are determined by the saved arguments_count of
   the 'codebuf'.
*/
struct stack_frame_info_s;
EXTERNFN PyObject *
psyco_processor_run(CodeBufferObject *codebuf, long initial_stack[],
                    struct stack_frame_info_s ***finfo);

#if LE_ENTERS_FUNCTIONS_FRAMELESS
/* the code a patched _PyFunction_Vectorcall jumps to (x64/iprocessor.c) */
EXTERNFN void *
le_function_gate_new(void *frameless, void ***original);
#endif

#define RUN_ARGC(codebuf)                                                 \
    ((int)((get_stack_depth(&((CodeBufferObject *)(codebuf))->snapshot) - \
            INITIAL_STACK_DEPTH - sizeof(long)) /                         \
           sizeof(long)))

/* check for signed integer multiplication overflow */
EXTERNVAR char (*psyco_int_mul_ovf)(long a, long b);

/* code that compiled code may 'call' with arguments in the SysV
   registers and the stack at any alignment: it aligns the stack and calls
   'fn' (made once, at start-up); the dealloc glue deallocates the
   object in RAX (x64/iprocessor.c) */
EXTERNFN code_t *
psyco_aligned_call_glue(void *fn);
EXTERNVAR code_t *psyco_dealloc_rax_glue;
/* the copy of a glue that also keeps XMM8-XMM14, for a call made while
   doubles are live in them */
EXTERNFN code_t *
psyco_glue_keeping_xmm(code_t *glue);

/* the return address given to a compiled callee whose frame object is
   kept (x64/iprocessor.c, frames.c:psyco_activation) */
EXTERNVAR code_t *psyco_return_barrier;

/* find the next stack frame
   ("next" = more recent in time = towards innermost frames) */
EXTERNFN struct stack_frame_info_s **
psyco_next_stack_frame(struct stack_frame_info_s **finfo);

#endif /* _PROCESSOR_H */
