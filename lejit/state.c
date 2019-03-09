#include "state.h"

static inline int
state_array_length(PyCodeObject *co) {
    #define BUILTIN_GLOBALS_LOCALS_LEN 3
    return BUILTIN_GLOBALS_LOCALS_LEN + 
        PyList_Size(co->co_consts) + 
        PyList_Size(co->co_names) +
        co->co_nlocals + 
        PyTuple_GET_SIZE(co->co_cellvars) + 
        PyTuple_GET_SIZE(co->co_freevars) + 
        co->co_stacksize;
}

static void
state_dealloc(LeStateObject* self) {
    int i;
    int array_len = state_array_length(self->s_code);
    Py_XDECREF(self->s_encoder);
    Py_XDECREF(self->s_code);
    /* everything else is stored in the array */
    if (self->s_array_base != NULL) {
        for(i = 0; i < array_len; i++) {
            Py_XDECREF(self->s_array_base[i]);
        }
        PyMem_Free(self->s_array_base);
    }
    PyObject_Del((PyObject*)self);
}

static int
state_traverse(LeStateObject *self, visitproc visit, void *arg)
{
    int i;
    int array_len = state_array_length(self->s_code);
    Py_VISIT(self->s_code);
    for(i = 0; i < array_len; i++) {
        Py_VISIT(self->s_array_base[i]);
    }
    return 0;
}

static int
state_clear(LeStateObject *self)
{
    int i;
    int array_len = state_array_length(self->s_code);
    Py_CLEAR(self->s_code);
    for(i = 0; i < array_len; i++) {
        Py_CLEAR(self->s_array_base[i]);
    }
    return 0;
}

static PyTypeObject LeState_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lejit.State",
    .tp_basicsize = sizeof(LeStateObject),
    .tp_dealloc = (destructor)state_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_traverse = (traverseproc)state_travers,
    .tp_clear = (inquiry)state_clear,
    .tp_doc = "State"
};

int LeState_GetLineNumber(LeStateObject *s) {
    return PyCode_Addr2Line(s->s_code, s->s_lasti);
}

LeStateObject *LeState_FromFrame(PyFrameObject *f, LeEncoderObject *e) {
    int i, j;
    int array_len = state_array_length(f->f_code);
    LeStateObject *s;
    LeValue **iter;
    s = PyObject_GC_New(LeStateObject, &LeState_Type);
    if (s == NULL)
        return NULL;
    s->s_lasti = f->f_lasti;
    Py_INCREF(e);
    s->s_encoder = e;
    Py_INCREF(w);
    Py_INCREF(f->f_code);
    s->s_code = f->f_code;
    s-s_array_base = PyMem_Calloc(state_array_length(f->f_code), sizeof(LeValue*));
    if (s-s_array_base == NULL) {
        Py_DECREF(s);
        PyErr_SetString(PyExc_MemoryError, "Could not allocate state array");
        return NULL;
    }
    iter = s-s_array_base;
    s->s_builtins = iter++;
    s->s_globals = iter++;
    s->s_locals = iter++;
    *s->s_builtins = le_value_ct(f->f_builtins);
    *s->s_gloabls = le_value_ct(f->f_globals);
    if (f->f_locals != NULL) {
        if (f->f_locals == f->f_globals) {
            Py_INCREF(*s->s_globals);
            *s->s_locals = *s->s_globals;
        }
        else {
            *s->s_locals = le_value_ct(f->f_locals);
        }
    }
    s->consts = iter;
    for(i = 0; i < PyList_Size(s->s_code->co_consts); i++) {
        *iter++ = le_value_ct(PyList_GET_ITEM(s->s_code->co_consts, i));
    }
    s->names = iter;
    for(i = 0; i < PyList_Size(s->s_code->co_names); i++) {
        *iter++ = le_value_ct(PyList_GET_ITEM(s->s_code->co_names, i));
    }
    s->s_fastlocals = iter;
    for(i = 0; i < s->s_code->co_nlocals; i++) {
        *iter++ = le_value_rt_from_value(f->f_localsplus[i]);
    }
    /* NOTE: parent cell vars are child free vars */
    /* TODO: optimise free/cell vars */
    s->s_cellvars = iter;
    for(j = 0; j < PyTuple_Size(s->s_code->co_cellvars); j++, i++) {
         *iter++ = le_value_rt_from_value(f->f_localsplus[i]);
    }
    s->s_freevars = iter;
    iter += PyTuple_Size(s->s_code->co_freevars);
    for(j = 0; j < PyTuple_Size(s->s_code->co_freevars); j++, i++) {
         *iter++ = le_value_rt_from_value(f->f_localsplus[i]);
    }
    s->s_valuestack = s->s_stacktop = iter;
    return s;
}

int init_state(PyObject *m) {
    if (PyType_Ready(&LeState_Type) < 0)
        return -1;
    PyModule_AddObject(m, "State", (PyObject*)&LeState_Type);
    return 0;
}

