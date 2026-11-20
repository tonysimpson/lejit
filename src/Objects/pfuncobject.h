/***************************************************************/
/***            Psyco equivalent of funcobject.h               ***/
/***************************************************************/

#ifndef _PSY_FUNCOBJECT_H
#define _PSY_FUNCOBJECT_H

#include "pabstract.h"
#include "pobject.h"

#define FUNC_code CPY_FIELD(PyFunctionObject, func_code, OB_type)
#define FUNC_globals \
    CPY_FIELD(PyFunctionObject, func_globals, FUNC_code)
#define FUNC_defaults \
    CPY_FIELD(PyFunctionObject, func_defaults, FUNC_globals)
#define FUNC_kwdefaults \
    CPY_FIELD(PyFunctionObject, func_kwdefaults, FUNC_defaults)
#define FUNC_closure \
    CPY_FIELD(PyFunctionObject, func_closure, FUNC_kwdefaults)
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
EXTERNFN ternaryfunc ni_function_tp_call(void);
EXTERNFN vinfo_t *
pfunction_simple_call(PsycoObject *po, vinfo_t *func, PyObject *f,
                      vinfo_t *arg, bool allow_inline);

/***************************************************************/
/* virtual functions.                                          */
/* 'fdefaults' may be NULL.                                    */
EXTERNFN vinfo_t *
PsycoFunction_NewWithQualName(PsycoObject *po, vinfo_t *codeobj,
                              vinfo_t *globals, vinfo_t *qualname);

/* A generator expression called but not yet run: the virtual function and
   its argument tuple. A consumer that knows it (sum, any, all, list, tuple,
   set) runs its code fused (psyfunc.c:psyco_call_genexpr); anything else
   makes the real generator. Delaying is safe because creating a generator
   runs no code. */
#define iGENEXPR_FUNC (iOB_TYPE + 1)
#define iGENEXPR_ARGS (iOB_TYPE + 2)
EXTERNFN vinfo_t *PsycoGenexpr_New(vinfo_t *vfunc, vinfo_t *arg_tuple);
EXTERNFN bool PsycoGenexpr_Check(vinfo_t *v);

#endif /* _PSY_FUNCOBJECT_H */
