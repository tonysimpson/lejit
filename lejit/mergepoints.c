#include <Python.h>
#include <opcode.h>
#include <structmember.h>

#include <stddef.h>

#include "opcode_names.h"
#include "lejit.h"
#include "mergepoints.h"

static PyObject*
LeMergePoint_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    LeMergePoint *self;
    int bytecode_offset;
    PyObject *entrypoints = NULL;
    if (!PyArg_ParseTuple(args, "i|O!", &bytecode_offset, &PyList_Type, &entrypoints)) {
        return NULL;
    }
    self = (LeMergePoint*)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->bytecode_offset = bytecode_offset;
    if (entrypoints == NULL) {
        entrypoints = PyList_New(0);
        if (entrypoints == NULL) {
            return NULL;
        }
    }
    self->entrypoints = entrypoints;
    return (PyObject*)self;
}

static PyObject* 
LeMergePoint_repr(PyObject *o) {
    LeMergePoint *self = (LeMergePoint*)o;
    return PyUnicode_FromFormat("MergePoint(%d, %R)", self->bytecode_offset, self->entrypoints);
}

static void
LeMergePoint_dealloc(PyObject* self) {
    Py_DECREF(((LeMergePoint*)self)->entrypoints);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyMemberDef LeMergePoint_members[] = {
    {"bytecode_offset", T_INT, offsetof(LeMergePoint, bytecode_offset), READONLY},
    {"entrypoints", T_OBJECT, offsetof(LeMergePoint, entrypoints), READONLY},
    {NULL}
};

static PyTypeObject LeMergePoint_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "lejit.MergePoint",             /* tp_name */
    sizeof(LeMergePoint),             /* tp_basicsize */
    0,                              /* tp_itemsize */
    LeMergePoint_dealloc,             /* tp_dealloc */
    0,                              /* tp_print */
    0,                              /* tp_getattr */
    0,                              /* tp_setattr */
    0,                              /* tp_reserved */
    LeMergePoint_repr,                /* tp_repr */
    0,                              /* tp_as_number */
    0,                              /* tp_as_sequence */
    0,                              /* tp_as_mapping */
    0,                              /* tp_hash  */
    0,                              /* tp_call */
    0,                              /* tp_str */
    0,                              /* tp_getattro */
    0,                              /* tp_setattro */
    0,                              /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,             /* tp_flags */
    "Merge Point",                  /* tp_doc */
    0,                              /* tp_traverse */
    0,                              /* tp_clear */
    0,                              /* tp_richcompare */
    0,                              /* tp_weaklistoffset */
    0,                              /* tp_iter */
    0,                              /* tp_iternext */
    0,                              /* tp_methods */
    LeMergePoint_members,             /* tp_members */
    0,                              /* tp_getset */
    0,                              /* tp_base */
    0,                              /* tp_dict */
    0,                              /* tp_descr_get */
    0,                              /* tp_descr_set */
    0,                              /* tp_dictoffset */
    0,                              /* tp_init */
    0,                              /* tp_alloc */
    LeMergePoint_new,                  /* tp_new */
};

static PyObject*
LeMergePoints_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    LeMergePoints *self;
    PyObject *mergepoints = NULL;
    if (!PyArg_ParseTuple(args, "|O!", &PyList_Type, &mergepoints)) {
        return NULL;
    }
    self = (LeMergePoints*)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    if (mergepoints == NULL) {
        mergepoints = PyList_New(0);
        if (mergepoints == NULL) {
            return NULL;
        }
    }
    self->mergepoints = mergepoints;
    return (PyObject*)self;
}

static PyObject* 
LeMergePoints_repr(PyObject *o) {
    LeMergePoints *self = (LeMergePoints*)o;
    return PyUnicode_FromFormat("LeMergePoints(%R)", self->mergepoints);
}

static void
LeMergePoints_dealloc(PyObject* self) {
    Py_DECREF(((LeMergePoints*)self)->mergepoints);
    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyMemberDef LeMergePoints_members[] = {
    {"mergepoints", T_OBJECT, offsetof(LeMergePoints, mergepoints), READONLY},
    {NULL}
};

static PyTypeObject LeMergePoints_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "lejit.MergePoints",            /* tp_name */
    sizeof(LeMergePoints),            /* tp_basicsize */
    0,                              /* tp_itemsize */
    (destructor)LeMergePoints_dealloc,/* tp_dealloc */
    0,                              /* tp_print */
    0,                              /* tp_getattr */
    0,                              /* tp_setattr */
    0,                              /* tp_reserved */
    LeMergePoints_repr,               /* tp_repr */
    0,                              /* tp_as_number */
    0,                              /* tp_as_sequence */
    0,                              /* tp_as_mapping */
    0,                              /* tp_hash  */
    0,                              /* tp_call */
    0,                              /* tp_str */
    0,                              /* tp_getattro */
    0,                              /* tp_setattro */
    0,                              /* tp_as_buffer */
    Py_TPFLAGS_DEFAULT,             /* tp_flags */
    "Merge Point",                  /* tp_doc */
    0,                              /* tp_traverse */
    0,                              /* tp_clear */
    0,                              /* tp_richcompare */
    0,                              /* tp_weaklistoffset */
    0,                              /* tp_iter */
    0,                              /* tp_iternext */
    0,                              /* tp_methods */
    LeMergePoints_members,            /* tp_members */
    0,                              /* tp_getset */
    0,                              /* tp_base */
    0,                              /* tp_dict */
    0,                              /* tp_descr_get */
    0,                              /* tp_descr_set */
    0,                              /* tp_dictoffset */
    0,                              /* tp_init */
    0,                              /* tp_alloc */
    LeMergePoints_new,                /* tp_new */
};

PyObject *mergepoints_build(PyCodeObject *co) {
    PyObject *list = PyList_New(0);
    PyObject *mp = PyObject_CallFunction((PyObject*)&LeMergePoint_Type, "i", 0);
    _Py_CODEUNIT *instr = (_Py_CODEUNIT*)PyBytes_AS_STRING(co->co_code);
    const int num_opcodes = (PyBytes_GET_SIZE(co->co_code) / sizeof(_Py_CODEUNIT));
    int index = 0;
    int opcode, oparg;
    while (index < num_opcodes) {
        opcode = instr[index] & 0xFF;
        oparg  = instr[index] >> 8;
        switch(opcode) {
            case LOAD_CONST:
            case LOAD_FAST:
            case RETURN_VALUE:
                break;
            default:
                goto unsupported_op;
        }
        index++;
    }
    if (mp == NULL) 
        return NULL;
    if(PyList_Append(list, mp) == -1) {
        Py_DECREF(mp);
        return NULL;
    }
    return PyObject_CallFunction((PyObject*)&LeMergePoints_Type, "O", list);
unsupported_op:
    return PyErr_Format(LeExc_Exception, "Unsupported OpCode: index %d, name %s, opcode %d, oparg %d", index, LE_OPCODE_NAME(opcode), opcode, oparg);
}

PyObject *mergepoints_find_entry_points(PyObject *mergepoints,
                                             int bytecode_offset) {
    Py_RETURN_NONE;
}

PyObject *Le_MergePointsBuild(PyObject *self, PyObject *args) {
    PyCodeObject *co;
    if(!PyArg_ParseTuple(args, "O!", &PyCode_Type, &co)) {
        return NULL;
    }
    return mergepoints_build(co);
}

PyObject *LeExc_UnsupportedOpError;

int init_mergepoints(PyObject *m) {
    if (PyType_Ready(&LeMergePoint_Type) < 0)
        return -1;
    if (PyType_Ready(&LeMergePoints_Type) < 0)
        return -1;
    PyModule_AddObject(m, "MergePoint", (PyObject*)&LeMergePoint_Type);
    PyModule_AddObject(m, "MergePoints", (PyObject*)&LeMergePoints_Type);
    if(!(LeExc_UnsupportedOpError = PyErr_NewException("lejit.UnsupportedOpError", LeExc_Exception, NULL))) {
        return -1;
    }
    PyModule_AddObject(m, "UnsupportedOpError", LeExc_UnsupportedOpError);
    PyObject *co_to_mp = PyDict_New();
    if (co_to_mp == NULL) {
        return -1;
    }
    PyModule_AddObject(m, "co_to_mp", co_to_mp);
    return 0;
}

