/***************************************************************/
/***          Psyco Function objects (a.k.a. proxies)          ***/
/***************************************************************/

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

/* generator expressions fused with their consumer (pycheader.h GENEXPR_*) */
EXTERNFN bool psyco_genexpr_fusable(vinfo_t *v);
EXTERNFN vinfo_t *psyco_call_genexpr(PsycoObject *po, vinfo_t *vgen, int mode,
                                     vinfo_t *acc);

/* for pycompiler.c */
EXTERNFN vinfo_t *psyco_save_inline_po(PsycoObject *po);
EXTERNFN PsycoObject *psyco_restore_inline_po(PsycoObject *po,
                                              vinfo_array_t **a);

/* Psyco proxies for Python functions. Calling a proxy has the same effect
   as calling the function it has been built from, except that the function
   is compiled first. As proxies are real Python objects, calling them is
   the only way to go from Python's base level to Psyco's meta-level.
   Note that (unlike in previous versions of Psyco) proxies should not be
   seen by user Python code. Use _psyco.proxycode() to build a proxy and
   emcompass it in a code object. */
typedef struct {
  PyObject_HEAD PyCodeObject
      *psy_code;          /*                                     */
  PyObject *psy_globals;  /*  same as in Python function object  */
  PyObject *psy_defaults; /*                                     */
  int psy_recursion; /* # levels to automatically compile called functions */
  PyObject *psy_fastcall; /* cache mapping arg count to code bufs */
} PsycoFunctionObject;

EXTERNVAR PyTypeObject PsycoFunction_Type;

#define PsycoFunction_Check(op) PyObject_TypeCheck(op, &PsycoFunction_Type)

EXTERNFN PsycoFunctionObject *
psyco_PsycoFunction_NewEx(PyCodeObject *code, PyObject *globals,
                          PyObject *defaults, /* or NULL */
                          int rec);

PSY_INLINE bool is_proxycode(PyCodeObject *code) {
  return PyTuple_Size(code->co_consts) > 1 &&
         PsycoFunction_Check(PyTuple_GET_ITEM(code->co_consts, 1));
}

EXTERNFN
void psycofunction_init();

#endif /* _PSYFUNC_H */
