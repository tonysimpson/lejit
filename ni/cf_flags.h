#ifndef _CF_FLAGS_H
#define _CF_FLAGS_H

/***************************************************************************
 * Flags used by psyco_generic_call, generic_call_check and generic_call_ct
 ***************************************************************************/
/* if the C function has no side effect it can be called at compile-time
   if all its arguments are compile-time. Use CfPure in this case. */
#define CfPure              0x10
/* if the C function returns a new reference to a PyObject */
#define CfNewRef            0x20
/* if the C function does not return anything
   or if you are not interested in getting the result in a vinfo_t.
   psyco_generic_call() returns VINFO_OK (unless there is an error)
   in this case. */
#define CfNoReturnValue     0x40
/* If the c function result after error checking is always positive */
#define CfNotNegative       0x80
/* the call needs no eval_breaker test after it, as for a function
   psyco_cannot_block() accepts: for a helper that calls one */
#define CfNoBlock           0x1000
/* the C function runs no Python code, so the known types stay: an
   instance dict lookup by name (a str-subclass key's __eq__ is not
   allowed for) */
#define CfNoPython          0x2000

#define VINFO_OK ((vinfo_t*)1)

/************************
 * Function return types
 ************************/
#define CfReturnTypeMask    0x07
/* if the C function is declared as void */
#define CfReturnTypeVoid    0x00
/* if the C function returns an int */
#define CfReturnTypeInt     0x01
/* if the C function returns a long */
#define CfReturnTypeLong    0x02
/* if the c function returns a ssize_t or equivelent native sized type */
#define CfReturnTypeNative  0x03
/* if the c function returns a double (float64) */
#define CfReturnTypeDouble   0x04
/* if the C function returns a pointer or a PyObject* */
#define CfReturnTypePtr     0x07
/* All the above types are signed add this flag to make them unsigned */
#define CfReturnUnsigned    0x08

/*******************************
 * Error detection and handling
 *******************************/
#define CfPyErrMask           0xF00
/* some extra flags recognized by psyco_generic_call(). They tell how the
   Python C function signals errors. They are ignored by the Psyco_METAX()
   macros if a meta-implementation is found, as meta-implementations always
   signal errors by returning either (vinfo_t*)NULL or CC_ERROR. */
#define CfPyErrDontCheck      0x000 /* default: no check */
#define CfPyErrIfNull         0x100 /* a return == 0 (or NULL) means an error */
#define CfPyErrIfNonNull      0x200 /* a return != 0 means an error */
#define CfPyErrIfNeg          0x300 /* a return < 0 means an error */
#define CfPyErrIfMinus1       0x400 /* only -1 means an error */
#define CfPyErrCheck          0x500 /* always check with PyErr_Occurred() */
#define CfPyErrCheckMinus1    0x600 /* use PyErr_Occurred() if return is -1 */
#define CfPyErrCheckNeg       0x700 /* use PyErr_Occurred() if return is < 0 */
#define CfPyErrNotImplemented 0x800 /* test for a Py_NotImplemented result */
#define CfPyErrIterNext       0x900 /* specially for tp_iternext slots */
#define CfPyErrAlways         0xA00 /* always set an exception */
#define CfPyErrCheckNull      0xB00 /* use PyErr_Occurred() if return is NULL;
                                       a NULL without one is psyco_vi_Zero() */
/* Note: CfPyErrNotImplemented means that the C function may return
   Py_NotImplemented, and this is checked; if true, then Psyco_METAX()
   returns exactly 'psyco_viNotImplemented', and not just a possibly run-time
   vinfo_t* containing Py_NotImplemented. Meta-implementations always return
   exactly 'psyco_viNotImplemented'. */

/**********************
 * Common combinations
 **********************/
#define CfCommonNewRefPyObject          (CfReturnTypePtr|CfNewRef|CfPyErrIfNull)
#define CfCommonCheckNotImplemented     (CfReturnTypePtr|CfNewRef|CfPyErrNotImplemented)
#define CfCommonIterNext                (CfReturnTypePtr|CfNewRef|CfPyErrIterNext)
#define CfCommonNewRefPyObjectNoError   (CfReturnTypePtr|CfNewRef)
#define CfCommonPySSizeTResult          (CfReturnTypeNative|CfNotNegative|CfPyErrCheckNeg)
#define CfCommonIntBoolOrError          (CfReturnTypeInt|CfNotNegative|CfPyErrCheckNeg)
#define CfCommonInquiry                 (CfReturnTypeInt|CfNotNegative|CfPyErrCheckNeg)
#define CfCommonIntZeroOk               (CfNoReturnValue|CfReturnTypeInt|CfPyErrIfNonNull)
#endif /* _CF_FLAGS_H */


