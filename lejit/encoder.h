#ifndef LE_COMPILER_H
#define LE_COMPILER_H
#include <Python.h>

typedef struct {
    PyObject_HEAD
} LeValue;

typedef struct {
    PyObject_HEAD
} LeEncoderStateObject;

LeValue *encoder_calldirect(LeStateObject *s, int flags, void *func, char *args_format, ...);
int encoder_return(LeStateObject *s, LeValue *ret_val);

#define COMPILER_METHODS /* nothing */
int init_encoder(PyObject *module);
#endif /* LE_COMPILER_H */
