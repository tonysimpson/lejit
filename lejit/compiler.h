#ifndef LE_COMPILER_H
#define LE_COMPILER_H
#include <Python.h>
#include <frameobject.h>

typedef struct le_value_s LeValue;
typedef struct {
    PyObject_HEAD
    int s_lasti;
    PyCodeObject *s_co;
    LeValue *s_builtins;
    LeValue *s_globals;
    LeValue *s_locals;
    LeValue **s_array_base;
    LeValue **s_names;
    LeValue **s_consts;
    LeValue **s_fastlocals;
    LeValue **s_freevars;
    LeValue **s_valuestack;
    LeValue **s_stacktop;
} LeStateObject;

typedef struct {
    PyObject_HEAD
    PyObject *code_ref;
    void *entry_address; 
    long frozen_state_id;
} LeEntryPointObject;

LeStateObject *le_compiler__new(PyFrameObject *frame);
LeEntryPointObject *le_compiler__merge_or_modify_or_continue(LeStateObject *s);
LeEntryPointObject *le_compiler__entry_point_new(LeStateObject *s);
PyFrameObject *le_compiler__fake_frame_new(LeStateObject *s);
LeValue *le_compiler__call_direct(LeStateObject *s, int flags, void *func, char *args_format, ...);
int le_compiler__return(LeStateObject *s, LeValue *ret_val);

#define COMPILER_METHODS /* nothing */
int init_compiler(PyObject *module);
#endif /* LE_COMPILER_H */
