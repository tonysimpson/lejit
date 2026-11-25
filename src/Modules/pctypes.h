#ifndef _PSY_CTYPES_H
#define _PSY_CTYPES_H

#include "../Objects/pabstract.h"

/* whether an object of type tp, whose tp_call is 'call', is a ctypes
   function pointer this file can call (Modules/pctypes.c) */
EXTERNFN bool
PsycoCtypes_IsFunction(PyTypeObject *tp, ternaryfunc call);
/* the call vf(*vargs, **kw) of such a function, called directly; NULL
   with no exception set where ctypes' own call is to be made */
EXTERNFN vinfo_t *
PsycoCtypes_Call(PsycoObject *po, PyTypeObject *tp, vinfo_t *vf,
                 vinfo_t *vargs, vinfo_t *kw);

/* the call ml(*vargs) of a builtin, if it is ctypes' byref(obj): a
   virtual result, made only if it escapes; NULL with no exception set
   where the builtin's own call is to be made */
EXTERNFN vinfo_t *
PsycoCtypes_Byref(PsycoObject *po, PyMethodDef *ml, vinfo_t *vargs);

#endif /* _PSY_CTYPES_H */
