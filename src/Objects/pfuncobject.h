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
EXTERNFN vinfo_t *
pfunction_simple_call(PsycoObject *po, vinfo_t *func, PyObject *f,
                      vinfo_t *arg, bool allow_inline);

/***************************************************************/
/* virtual functions.                                          */
/* 'fdefaults' may be NULL.                                    */
EXTERNFN vinfo_t *
PsycoFunction_NewWithQualName(PsycoObject *po, vinfo_t *codeobj,
                              vinfo_t *globals, vinfo_t *qualname);

/* SET_FUNCTION_ATTRIBUTE: the field of func that 'which' names (0 the
   closure, 1 the annotations, 2 the keyword defaults, 3 the defaults) set
   to value (borrowed). func is MAKE_FUNCTION's, still virtual unless
   something made it real. */
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

#endif /* _PSY_FUNCOBJECT_H */
