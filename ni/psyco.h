/***************************************************************/
/***                 Global Psyco definitions                ***/
/***************************************************************/

#ifndef _PSYCO_H
#define _PSYCO_H


#include <Python.h>
#include <structmember.h>   /* for offsetof() */
#include <stdint.h>
#include "compat2to3.h"

typedef int8_t byte_t;
typedef int16_t word_t;
typedef int32_t dword_t;
typedef int64_t qword_t;

#if INTPTR_MAX == INT32_MAX
    typedef dword_t stackitem_t;
#elif INTPTR_MAX == INT64_MAX
    typedef int64_t qword_t;
    typedef qword_t stackitem_t;
#else
    #error "Environment not 32 or 64-bit."
#endif

/*****************************************************************/
/***   Various customizable parameters (use your compilers'    ***/
/***   option to override them, e.g. -DXXX=value in gcc)       ***/
/* set to 0 to disable all debugging checks and output */
#ifndef NI_TRACE
#define NI_TRACE 0
#endif

/* define to 1 to count code size per instruction (tools/codesize.py) */
#ifndef NI_CODESTATS
#define NI_CODESTATS 0
#endif

/* define to 1 for extra assert()'s */
#ifndef ALL_CHECKS
#define ALL_CHECKS 0
#endif

/*  define to inline the most common functions in the produced code
    (should be enabled unless you want to trade code size for speed) */
#ifndef INLINE_COMMON_FUNCTIONS
# define INLINE_COMMON_FUNCTIONS 1
#endif

/*****************************************************************/
/* Size of buffer to allocate when emitting code.
   Can be as large as you like (most OSes will not actually allocate
   RAM pages before they are actually used). We no longer perform
   any realloc() on this; a single allocated code region is reused
   for all code buffers until it is exhausted. There are BUFFER_MARGIN
   unused bytes at the end, so BIG_BUFFER_SIZE has better be large to
   minimize this effect.
   Linux note: I've seen in my version of glibc's malloc() that it
   uses mmap for sizes >= 128k, and that it will refuse the use mmap
   more than 1024 times, which means that if you allocate blocks of
   128k you cannot allocate more than 128M in total.
   Note that Psyco will usually allocate and fill two buffers in
   parallel, by the way vcompiler.c works. However, it occasionally
   needs more; codemanager.c can handle any number of parallely-growing
   buffers. There is a safeguard in vcompiler.c to put an upper bound
   on this number (currently should not exceed 4).
   In debugging mode, we use a small size to stress the buffer-
   continuation coding routines. */
#ifndef BIG_BUFFER_SIZE
# define BIG_BUFFER_SIZE 0x100000
#endif

/* A safety margin for occasional overflows: we might write a few
   instructions too much before we realize we wrote past 'codelimit'.
   XXX carefully check that it is impossible to overflow by more
   We need more than 128 bytes because of the way conditional jumps
   are emitted; see pycompiler.c.
   The END_CODE macro triggers a silent buffer change if space is
   getting very low -- less than GUARANTEED_MINIMUM */
#ifndef BUFFER_MARGIN
# define BUFFER_MARGIN 1024
#endif

/* When emitting code, all called functions can assume that they
   have at least this amount of room to write their code. If they
   might need more, they have to allocate new buffers and write a
   jump to these from the original code (jumps can be done in less
   than GUARANTEED_MINIMUM bytes). */
#ifndef GUARANTEED_MINIMUM
# define GUARANTEED_MINIMUM 64
#endif


#ifndef ALL_STATIC
#define ALL_STATIC  0   /* 1: one unit, all static (setup.py NI_ALL_STATIC) */
#endif

#if ALL_STATIC
# define EXTERNVAR   static
# define EXTERNFN    static
# define DEFINEVAR   static
# define DEFINEFN    static
# define INITIALIZATIONFN  PSY_INLINE
#else
# define EXTERNVAR   extern
# define EXTERNFN
# define DEFINEVAR
# define DEFINEFN
# define INITIALIZATIONFN  DEFINEFN
#endif

#define psyco_assert(x) ((void)((x) || psyco_fatal_msg("assertion failed")))
#define psyco_fatal_msg(msg)  psyco_fatal_error(msg, __FILE__, __LINE__)
EXTERNFN int psyco_fatal_error(char* msg, char* filename, int lineno);

#if ALL_CHECKS
# define MALLOC_CHECK_    2  /* GCC malloc() checks */
# define extra_assert(x)  psyco_assert(x)
#else
# define extra_assert(x)  (void)0  /* nothing */
#endif

#define RECLIMIT_SAFE_ENTER()  PyThreadState_GET()->recursion_depth--
#define RECLIMIT_SAFE_LEAVE()  PyThreadState_GET()->recursion_depth++

#if INLINE_COMMON_FUNCTIONS
# define PSY_INLINE	__inline static
#else
# define PSY_INLINE	static
#endif

#ifndef bool
typedef int bool;
#endif
#ifndef false
# define false   0
#endif
#ifndef true
# define true    1
#endif

#ifndef PyObject_TypeCheck
# define PyObject_TypeCheck(o,t)   ((o)->ob_type == (t))
#endif


typedef unsigned char code_t;

typedef struct vinfo_s vinfo_t;             /* defined in compiler.h */
typedef struct vinfo_array_s vinfo_array_t; /* defined in compiler.h */
typedef struct PsycoObject_s PsycoObject;   /* defined in compiler.h */
typedef struct FrozenPsycoObject_s FrozenPsycoObject; /* def in dispatcher.h */
typedef struct CodeBufferObject_s CodeBufferObject;  /* def in codemanager.h */
typedef struct global_entries_s global_entries_t;  /* def in dispatcher.h */
typedef struct mergepoint_s mergepoint_t;   /* defined in mergepoint.h */
typedef struct stack_frame_info_s stack_frame_info_t; /* def in pycompiler.h */

EXTERNVAR PyObject* PyExc_PsycoError;
EXTERNVAR PyObject* CPsycoModule;
/* What Ni keeps per code object, in its co_extra slot (PEP 523): found by
   identity, not by value as a dict keyed by code objects would be (equal
   code objects compare co_consts recursively; KNOWN_BUGS C13). */
typedef struct {
  PyObject* merge_points;  /* capsule, Py_None if unsupported, NULL if not
                              built yet; owned */
  PyObject* entry_point;   /* codebuf, Py_None if compiling failed, NULL if
                              not tried; owned */
  PyObject* entry_globals; /* the globals it was compiled for: borrowed, so
                              maybe dead and reused (vcompiler.c says why
                              that is safe) */
  PyObject* entry_builtins; /* and its f_builtins (KNOWN_BUGS F49): borrowed,
                               the compiled code owns it */
  long serial;             /* never reused, unlike the code object's
                              address (psyfunc.c:psyco_save_inline_po) */
} ni_code_extra_t;
/* the record for 'co', made on first use; NULL with an exception set */
EXTERNFN ni_code_extra_t* ni_code_extra(PyCodeObject* co);

/* moved here from vcompiler.h because needed by numerous header files.
   See vcompiler.h for comments */
typedef bool (*compute_fn_t)(PsycoObject* po, vinfo_t* vi);
typedef PyObject* (*direct_compute_fn_t)(vinfo_t* vi, char* data);
typedef struct {
  compute_fn_t compute_fn;
  direct_compute_fn_t direct_compute_fn;
  long pyobject_mask;
  signed char nested_weight[2];
} source_virtual_t;


#define psyco_inc_stackdepth(po) do {\
    po->stack_depth += sizeof(stackitem_t);\
} while(0)

#define psyco_dec_stackdepth(po) do {\
    po->stack_depth -= sizeof(stackitem_t);\
} while(0)


/* to display code object names */
#define PyCodeObject_NAME(co)   (co->co_name ? NiCompatStr_AS_STRING(co->co_name)  \
                                 : "<anonymous code object>")


/* defined in pycompiler.c */
#define GLOBAL_ENTRY_POINT	psyco_pycompiler_mainloop
EXTERNFN code_t* psyco_pycompiler_mainloop(PsycoObject* po);


/* XXX no handling of out-of-memory conditions. We have to define precisely
   what should occur in various cases, like when we run out of memory in the
   middle of writing code, when the beginning is already executing. When
   should we report the exception? */
EXTERNFN void psyco_out_of_memory(char *filename, int lineno);
#define OUT_OF_MEMORY()      psyco_out_of_memory(__FILE__, __LINE__)

/* Thread-specific state */
EXTERNFN PyObject* psyco_thread_dict(void);

/* Getting data from the _psyco module */
EXTERNFN PyObject* need_cpsyco_obj(char* name);

/* defined in dispatcher.c */
EXTERNFN void PsycoObject_EmergencyCodeRoom(PsycoObject* po);

#if NI_TRACE
/* Calls hooked by nianalyser to collect compiler behaviour */
EXTERNFN void ni_trace_begin_simple_code(code_t *code);
EXTERNFN void ni_trace_end_simple_code(code_t *code);
EXTERNFN void ni_trace_begin_code(PsycoObject* po);
EXTERNFN void ni_trace_end_code(PsycoObject* po);
EXTERNFN void ni_trace_jump(code_t *location, code_t *target);
EXTERNFN void ni_trace_jump_update(code_t *location, code_t *target);
EXTERNFN void ni_trace_jump_reg(code_t *location, int reg_target);
EXTERNFN void ni_trace_jump_cond(code_t *location, code_t *not_taken, code_t *taken);
EXTERNFN void ni_trace_jump_cond_update(code_t *location, code_t *not_taken, code_t *taken);
EXTERNFN void ni_trace_jump_cond_reg(code_t *location, code_t *not_taken, int reg_taken);
EXTERNFN void ni_trace_call(code_t *location, code_t *call_target);
EXTERNFN void ni_trace_call_reg(code_t *location, int reg_call_target);
EXTERNFN void ni_trace_return(code_t *location, int stack_adjust);
EXTERNFN void ni_trace_unsupported_opcode(PyCodeObject *co, int bytecode_index);
EXTERNFN void ni_trace_run_fail(PyCodeObject *co, char *reason);
EXTERNFN void ni_trace_unify(PsycoObject* po, CodeBufferObject *match);
#else
#define ni_trace_begin_simple_code(code) do {} while(0)
#define ni_trace_end_simple_code(code) do {} while(0)
#define ni_trace_begin_code(po) do {} while(0)
#define ni_trace_end_code(po) do {} while(0)
#define ni_trace_jump(location, target) do {} while(0)
#define ni_trace_jump_update(location, target) do {} while(0)
#define ni_trace_jump_reg(location, target) do {} while(0)
#define ni_trace_jump_cond(location, not_taken, taken) do {} while(0)
#define ni_trace_jump_cond_update(location, not_taken, taken) do {} while(0)
#define ni_trace_jump_cond_reg(location, not_taken, taken) do {} while(0)
#define ni_trace_call(location, call_target) do {} while(0)
#define ni_trace_call_reg(location, reg_call_target) do {} while(0)
#define ni_trace_return(location, stack_adjust) do {} while(0)
#define ni_trace_unsupported_opcode(co, bytecode_index) do {} while(0)
#define ni_trace_run_fail(co, reason) do {} while(0)
#define ni_trace_unify(po, match) do {} while (0)
#endif

#define BEGIN_CODE do {\
    code_t* code = po->code;\
    ni_trace_begin_code(po);
#define END_CODE\
    po->code = code;\
    ni_trace_end_code(po);\
    if (code >= po->codelimit) {\
        PsycoObject_EmergencyCodeRoom(po);\
    }\
} while(0);

#endif /* _PSYCO_H */
