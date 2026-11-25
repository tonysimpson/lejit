#ifndef _PSY_CFFI_H
#define _PSY_CFFI_H

#include "../Objects/pabstract.h"

/* the call ml(vself, *vargs) of a builtin with no keywords, called
   directly if ml is a function of a cffi API-mode module; NULL with no
   exception set where the builtin's own call is to be made
   (Modules/pcffi.c) */
EXTERNFN vinfo_t *
PsycoCffi_ApiCall(PsycoObject *po, PyMethodDef *ml, vinfo_t *vself,
                  vinfo_t *vargs);
/* whether an object of type tp is a cffi cdata, which may be a function
   pointer this file can call */
EXTERNFN bool
PsycoCffi_IsCData(PyTypeObject *tp);
/* the call vf(*vargs, **kw) of a cdata, called directly if it is a
   function pointer; NULL with no exception set where cffi's own call is
   to be made */
EXTERNFN vinfo_t *
PsycoCffi_Call(PsycoObject *po, vinfo_t *vf, vinfo_t *vargs, vinfo_t *kw);

#endif /* _PSY_CFFI_H */
