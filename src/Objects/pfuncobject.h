/***************************************************************/
/***            Psyco equivalent of funcobject.h               ***/
/***************************************************************/

#ifndef _PSY_FUNCOBJECT_H
#define _PSY_FUNCOBJECT_H

#include "pabstract.h"
#include "pobject.h"

#define FUNC_code CPY_FIELD(PyFunctionObject, func_code, OB_type)
#define FUNC_globals CPY_FIELD(PyFunctionObject, func_globals, FUNC_code)
#define FUNC_defaults CPY_FIELD(PyFunctionObject, func_defaults, FUNC_globals)
#define FUNC_kwdefaults \
    CPY_FIELD(PyFunctionObject, func_kwdefaults, FUNC_defaults)
#define FUNC_closure CPY_FIELD(PyFunctionObject, func_closure, FUNC_kwdefaults)
#define FUNC_annotations \
    CPY_FIELD(PyFunctionObject, func_annotations, FUNC_closure)
#define FUNC_qualname \
    CPY_FIELD(PyFunctionObject, func_qualname, FUNC_annotations)

#define iFUNC_CODE FIELD_INDEX(FUNC_code)
#define iFUNC_GLOBALS FIELD_INDEX(FUNC_globals)
#define iFUNC_DEFAULTS FIELD_INDEX(FUNC_defaults)
#define iFUNC_KWDEFAULTS FIELD_INDEX(FUNC_kwdefaults)
#define iFUNC_CLOSURE FIELD_INDEX(FUNC_closure)
#define iFUNC_ANNOTATIONS FIELD_INDEX(FUNC_annotations)
#define iFUNC_QUALNAME FIELD_INDEX(FUNC_qualname)
#define FUNC_TOTAL FIELDS_TOTAL(FUNC_qualname)

EXTERNFN vinfo_t *
pfunction_call(PsycoObject *po, vinfo_t *func, vinfo_t *arg, vinfo_t *kw);
/* PyFunction_Type's tp_call as compiled code calls it */
EXTERNFN ternaryfunc
le_function_tp_call(void);
#ifdef Py_TPFLAGS_HAVE_VECTORCALL
/* the vectorcall function a new function has (psy_funcobject_init) */
EXTERNVAR vectorcallfunc le_usual_vectorcall;
#endif
/* a call of the function 'f' (promoted, 'func' at compile time), with
   'kw' NULL or as psyfunc.c:psyco_call_pyfunc takes it */
/* the real call of a Python function, as compiled code makes it */
EXTERNFN vinfo_t *
PsycoFunction_RealCall(PsycoObject *po, vinfo_t *vfunc, vinfo_t *arg,
                       vinfo_t *kw, bool as_direct);
EXTERNFN vinfo_t *
pfunction_simple_call(PsycoObject *po, vinfo_t *func, PyObject *f,
                      vinfo_t *arg, vinfo_t *kw, bool allow_inline);

/***************************************************************/
/* virtual functions.                                          */
/* 'fdefaults' may be NULL.                                    */
EXTERNFN vinfo_t *
PsycoFunction_NewWithQualName(PsycoObject *po, vinfo_t *codeobj,
                              vinfo_t *globals, vinfo_t *qualname);

/* SET_FUNCTION_ATTRIBUTE: the field of func that 'which' names (0 the
   closure, 1 the annotations, 2 the keyword defaults, 3 the defaults, 4
   __annotate__) set to value (borrowed). func is MAKE_FUNCTION's, still
   virtual unless something made it real; __annotate__ makes it real. */
EXTERNFN bool
PsycoFunction_SetField(PsycoObject *po, vinfo_t *func, int which,
                       vinfo_t *value);

/* A generator called but not yet run (a generator expression, or a
   generator function psyfunc.c:psyco_call_pyfunc has bound the arguments
   of): the function, its positional arguments with the defaults the call
   used, the globals and the code (a fixed constant). A consumer that knows
   it (sum, any, all, min, max, list, tuple, set, str.join) runs its code
   fused (psyfunc.c:psyco_call_genexpr); anything else makes the real
   generator by calling the function with those arguments. Delaying is safe
   because creating a generator runs no code. A coroutine awaited as soon
   as it is made is one too (psyfunc.c:psyco_coro_start). */
#define iGENEXPR_FUNC (iOB_TYPE + 1)
#define iGENEXPR_ARGS (iOB_TYPE + 2)
#define iGENEXPR_GLOBALS (iOB_TYPE + 3)
#define iGENEXPR_CODE (iOB_TYPE + 4)
EXTERNFN vinfo_t *
PsycoGenexpr_New(vinfo_t *vfunc, vinfo_t *arg_tuple, vinfo_t *vglobals,
                 PyCodeObject *co);
EXTERNFN bool
PsycoGenexpr_Check(vinfo_t *v);
/* A generator compiled into the for loop that steps it
   (psyfunc.c:psyco_gen_inline_next): its code (a fixed constant), where
   it is (a fixed constant: where it paused, INLGEN_RUNNING while its code
   is compiled into the loop, or INLGEN_ENDED), the function its frame
   names (as LOC_INLINE_FUNC has it), then, while it is paused, its frame's
   values, laid out by psyfunc.c:inlgen_word. Only the loop's stack holds
   it, and nothing makes it real. A pending generator becomes one in
   place. */
#define iINLGEN_CODE (iOB_TYPE + 1)
#define iINLGEN_PLACE (iOB_TYPE + 2)
#define iINLGEN_FUNC (iOB_TYPE + 3)
#define iINLGEN_WORDS (iOB_TYPE + 4)
#define INLGEN_RUNNING (-1)
#define INLGEN_ENDED (-2)
EXTERNFN void
PsycoInlineGen_FromPending(PsycoObject *po, vinfo_t *v, vinfo_t *func,
                           int nwords);
EXTERNFN bool
PsycoInlineGen_Check(vinfo_t *v);
EXTERNFN long
PsycoInlineGen_Place(vinfo_t *v);
EXTERNFN void
PsycoInlineGen_SetPlace(PsycoObject *po, vinfo_t *v, long place);

#if LE_RESUMES_GENERATORS_DIRECTLY
/* A virtual generator: one compiled code has started stepping
   (psyfunc.c:gen_send), its paused state in a record (Python/genrecord.c,
   a run-time reference) in place of the arguments. Anything else makes it
   escape, into the real generator, the same object each time. A pending
   generator becomes one in place, keeping its function, type and code. */
#define iVGEN_RECORD iGENEXPR_ARGS
EXTERNFN void
PsycoVgen_FromPending(PsycoObject *po, vinfo_t *v, vinfo_t *rec);
EXTERNFN bool
PsycoVgen_Check(vinfo_t *v);
#define LE_VGEN_CHECK(v) PsycoVgen_Check(v)
#else
#define LE_VGEN_CHECK(v) false
#endif

#endif /* _PSY_FUNCOBJECT_H */
