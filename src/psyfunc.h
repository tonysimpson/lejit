#ifndef _PSYFUNC_H
#define _PSYFUNC_H

#include "psyco.h"
#include <compile.h> /* for PyCodeObject */

/* Encode a call to the given Python function, compiling it as needed. */
/* 'vfunc' is the function object itself (compile-time or virtual), called
   as such when the call can't be compiled; never inlined unless
   'allow_inline' */
EXTERNFN vinfo_t *
psyco_call_pyfunc(PsycoObject *po, vinfo_t *vfunc, PyCodeObject *co,
                  vinfo_t *vglobals, vinfo_t *vdefaults, vinfo_t *arg_tuple,
                  int recursion, bool allow_inline);
/* the same for a run-time function whose closure code 'co' is known */
EXTERNFN vinfo_t *
psyco_call_closure(PsycoObject *po, vinfo_t *vfunc, PyCodeObject *co,
                   vinfo_t *arg_tuple);

/* emits the test of whether a trace or profile function is set in this
   thread; true on its rare path, -1 with an exception set in 'po' */
EXTERNFN int
psyco_tracing_on(PsycoObject *po);

/* generators fused with their consumer (pycheader.h GENEXPR_*) */
EXTERNFN bool
psyco_genexpr_fusable(PsycoObject *po, vinfo_t *v);
/* psyco_genexpr_fusable, and not while tracing nor once its fusing has
   stopped (emits those tests): 1 to fuse, 0 for the consumer's own call,
   -1 with an exception */
EXTERNFN int
psyco_genexpr_fuse(PsycoObject *po, vinfo_t *v);
EXTERNFN vinfo_t *
psyco_call_genexpr(PsycoObject *po, vinfo_t *vgen, int mode, vinfo_t *acc);
/* whether the await being compiled runs the body of the pending coroutine
   'v' as a call, emitting the tests: 1 to, 0 not to, -1 with an
   exception */
EXTERNFN int
psyco_coro_start(PsycoObject *po, vinfo_t *v);

/* The argument binding of ceval.c:_PyEval_EvalCodeWithName, translated
   into psyfunc.c: run while compiling a call, it binds the values in 'args',
   'defs' and 'closure' into the frame's locals, 'f_localsplus' (all NULL on
   entry), taking a reference for each, and makes the code's own cells (only
   if 'make_cells': psyfunc.c:bind_cells). -1 where CPython's binding needs
   what the compiler can't do (a wrong count, **kwargs): the caller releases
   the locals and makes the real call; -2 on an error. The vinfo_t **
   parameters are the values bound; the others are the compiler's own. The
   C function's keyword parameters are left out: a call with keywords is the
   real one (pfunction_call), and a keyword-only parameter with no argument
   is left to the real call, which has the defaults. */
EXTERNFN int
_PsycoEval_EvalCodeWithName(PsycoObject *po, PyObject *_co,
                            vinfo_t **f_localsplus, vinfo_t **args,
                            Py_ssize_t argcount, vinfo_t **defs,
                            Py_ssize_t defcount, vinfo_t **closure,
                            bool make_cells);

/* for pycompiler.c */
EXTERNFN vinfo_t *
psyco_save_inline_po(PsycoObject *po);
EXTERNFN PsycoObject *
psyco_restore_inline_po(PsycoObject *po, vinfo_array_t **a);

EXTERNFN
void
psycofunction_init(void);

#endif /* _PSYFUNC_H */
