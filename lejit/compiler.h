#ifndef LE_COMPEVAL_H
#define LE_COMPEVAL_H

#include <Python.h>
#include <frameobject.h>
#include "encoder.h"
#include "writer.h"

typedef struct {
    PyObject_HEAD
    void *entry_address;
    PyObject* frozen_state;
} LeEntryPointObject;

LeEntryPointObject* LeCompiler_Compile(LeStateObject *c);


#define COMPEVAL_METHODS /* Nothing */
int init_compeval(PyObject *m);
#endif /* LE_COMPEVAL_H */
