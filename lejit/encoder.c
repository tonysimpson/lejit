#include "compiler.h"


LeStateObject *le_compiler__new(PyFrameObject *frame) {
    return NULL;
}

LeEntryPointObject *le_compiler__merge_or_modify_or_continue(LeStateObject *s) {
    return NULL;
}

LeEntryPointObject *le_compiler__entry_point_new(LeStateObject *s) {
    return NULL;
}

PyFrameObject *le_compiler__fake_frame_new(LeStateObject *s) {
    return NULL;
}

LeValue *le_compiler__call_direct(LeStateObject *s, int flags, void *func, char *args_format, ...) {
    return NULL;
}

int le_compiler__return(LeStateObject *s, LeValue *ret_val) {
    return 0;
}

int PyState_GetLineNumber(LeStateObject *s) {
    return PyCode_Addr2Line(s->s_code, s->s_lasti);
}

int init_compiler(PyObject *module) {
    return 0;
}
