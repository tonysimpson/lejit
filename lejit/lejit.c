#include <Python.h>
#include <frameobject.h>
#include "lejit.h"
#include "mergepoints.h"


PyObject *LeExc_Exception; 


const static char MODULE_NAME[] = "lejit";

static _PyFrameEvalFunction orig_eval_frame = NULL;

static PyObject* le_eval_frame(PyFrameObject *frame, int throwflag) {
    PyObject *result;
    result = orig_eval_frame(frame, throwflag);
    return result;
}

static PyObject *attach(PyObject *self, PyObject *args) {
    PyThreadState *tstate = PyThreadState_Get();
    if(orig_eval_frame == NULL) {
        orig_eval_frame = tstate->interp->eval_frame;
        tstate->interp->eval_frame = le_eval_frame;
    }
    Py_RETURN_NONE;
}


static PyMethodDef module_methods[] = {
    {"attach", attach, METH_NOARGS, "Start the JIT."},
    MERGEPOINTS_METHODS
    {NULL, NULL, 0, NULL}
};

static struct PyModuleDef module_def = {
   PyModuleDef_HEAD_INIT,
   MODULE_NAME,
   NULL,
   -1,
   module_methods
};

PyMODINIT_FUNC
PyInit_lejit(void) {
    PyObject* m = PyModule_Create(&module_def);
    LeExc_Exception = PyErr_NewException("lejit.Exception", NULL, NULL);
    if(LeExc_Exception == NULL) {
        return NULL;
    }
    if (m != NULL) {
        PyModule_AddObject(m, "Exception", LeExc_Exception);
        if(!init_mergepoints(m)) {
            Py_DECREF(m);
            return NULL;
        }
    }
    return m;
}

