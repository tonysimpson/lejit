#ifndef LE_COMPILER_H
#define LE_COMPILER_H
#include <Python.h>
#include <frameobject.h>
#include "codebuffer.h"

typedef struct le_value_s LeValue;
typedef struct le_state_object_s LeStateObject;
typedef struct {
    PyObject_HEAD
    LeCodeBuffer *code;
    void *entry_address; 
    long frozen_state_reference;
} LeEntryPointObject;

LeEntryPointObject *le_compiler__merge_or_modify_or_continue(LeStateObject *s);
LeEntryPointObject *le_compiler__entry_point_new(LeStateObject *s);
PyFrameObject *le_compiler__fake_frame_new(LeStateObject *s);
LeValue *le_compiler__call_direct(LeStateObject *s, int flags, void *func, char *args_format, ...);
int le_compiler__return(LeStateObject *s, LeValue *ret_val);

#endif /* LE_COMPILER_H */
