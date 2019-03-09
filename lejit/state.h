#ifndef LE_STATE_H
#define LE_STATE_H

#include <Python.h>
#include "encoder.h"
#include "value.H"

typedef struct {
    PyObject_HEAD
    int s_lasti;
    LeEncoderStateObject *s_encoder;
    PyCodeObject *s_code;
    LeValue **s_array_base;
    LeValue **s_builtins;
    LeValue **s_globals;
    LeValue **s_locals;
    LeValue **s_consts;
    LeValue **s_names;
    LeValue **s_fastlocals;
    LeValue **s_cellvars;
    LeValue **s_freevars;
    LeValue **s_valuestack;
    LeValue **s_stacktop;
} LeStateObject;

LeStateObject *LeState_FromFrame(PyFrameObject *f, LeEncoderObject *encoder);
int LeState_GetLineNumber(LeStateObject *s);

#define STATE_METHODS /* Nothing */
int init_state(PyObject *m);
#endif /* LE_STATE_H */
