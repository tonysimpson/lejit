#ifndef LE_FRAME_H
#define LE_FRAME_H

#include <Python.h>
#include "mergepoints.h"
#include "object.h"

/* LeCompilerFrame is used by the compiler to track the state of the 
   frame being compiled. */

typedef struct {
    LeValue *exc; /* runtime exception. Runtime NULL or an active 
                      exception */
    LeValue *buildins; /* builtins module. Never NULL */
    LeValue *globals; /* globals dict. Never NULL */
    LeValue *locals; /* locals dict. May be NULL if frame does not have a 
                         locals dict, will be set if frame is unwound. */
    LeValue *fastlocals_plus; /* fast locals plus stack starts here */
} le_stack_head;

typedef union {
    le_stack_head head;
    LeValue *entries[4]; /* atleast 4 for above head but fastlocals_plus is 
                             variable size */
} le_stack;

typedef struct {
    PyObject_HEAD
    int next_i; /* index of next instruction to compile */
    le_stack stack; 
} LeFrame;

PyObject *framestate_from_frame(PyFrameObject *frame);

#endif /* LE_FRAME_H */
