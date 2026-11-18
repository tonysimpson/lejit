#ifndef _PSYFUNC_H
#define _PSYFUNC_H

#include "psyco.h"
#include <compile.h> /* for PyCodeObject */

/* Encode a call to the given Python function, compiling it as needed. */
/* 'vfunc' is the function object itself (compile-time or virtual), called
   as such when the call can't be compiled */
EXTERNFN vinfo_t *psyco_call_pyfunc(PsycoObject *po, vinfo_t *vfunc,
                                    PyCodeObject *co,
                                    vinfo_t *vglobals, vinfo_t *vdefaults,
                                    vinfo_t *arg_tuple, int recursion);
/* the same for a run-time function whose closure code 'co' is known */
EXTERNFN vinfo_t *psyco_call_closure(PsycoObject *po, vinfo_t *vfunc,
                                     PyCodeObject *co, vinfo_t *arg_tuple);

/* generator expressions fused with their consumer (pycheader.h GENEXPR_*) */
EXTERNFN bool psyco_genexpr_fusable(vinfo_t *v);
/* psyco_genexpr_fusable, and not while tracing (emits that test, KNOWN_BUGS
   C23): 1 to fuse, 0 for the consumer's own call, -1 with an exception */
EXTERNFN int psyco_genexpr_fuse(PsycoObject *po, vinfo_t *v);
EXTERNFN vinfo_t *psyco_call_genexpr(PsycoObject *po, vinfo_t *vgen, int mode,
                                     vinfo_t *acc);

/* for pycompiler.c */
EXTERNFN vinfo_t *psyco_save_inline_po(PsycoObject *po);
EXTERNFN PsycoObject *psyco_restore_inline_po(PsycoObject *po,
                                              vinfo_array_t **a);

EXTERNFN
void psycofunction_init();

#endif /* _PSYFUNC_H */
