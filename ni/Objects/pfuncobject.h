/***************************************************************/
/***            Psyco equivalent of funcobject.h               ***/
/***************************************************************/

#ifndef _PSY_FUNCOBJECT_H
#define _PSY_FUNCOBJECT_H

#include "pabstract.h"
#include "pobject.h"

#define FUNC_code DEF_FIELD(PyFunctionObject, PyObject *, func_code, OB_type)
#define FUNC_globals \
    DEF_FIELD(PyFunctionObject, PyObject *, func_globals, FUNC_code)
#define FUNC_defaults \
    DEF_FIELD(PyFunctionObject, PyObject *, func_defaults, FUNC_globals)
#define FUNC_kwdefaults \
    DEF_FIELD(PyFunctionObject, PyObject *, func_kwdefaults, FUNC_defaults)
#define FUNC_closure \
    DEF_FIELD(PyFunctionObject, PyObject *, func_closure, FUNC_kwdefaults)
#define FUNC_annotations \
    DEF_FIELD(PyFunctionObject, PyObject *, func_annotations, FUNC_closure)
#define FUNC_qualname \
    DEF_FIELD(PyFunctionObject, PyObject *, func_qualname, FUNC_annotations)

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
EXTERNFN vinfo_t *
pfunction_simple_call(PsycoObject *po, vinfo_t *func, PyObject *f,
                      vinfo_t *arg, bool allow_inline);

/***************************************************************/
/* virtual functions.                                          */
/* 'fdefaults' may be NULL.                                    */
EXTERNFN vinfo_t *
PsycoFunction_NewWithQualName(PsycoObject *po, vinfo_t *codeobj,
                              vinfo_t *globals, vinfo_t *qualname);

#endif /* _PSY_FUNCOBJECT_H */
