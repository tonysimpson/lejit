#ifndef LE_MERGEPOINTS_H
#define LE_MERGEPOINTS_H

#include <Python.h>
#include <stdbool.h>

extern PyObject *LeExc_UnsupportedOpError;

/* MergePoints are used to find and track places where the compiler can 
   start, pause and resume compilation. Each MergePoint has a collection 
   of entrypoints which are keys into a dict of compressed compiler state. */
typedef struct {
    PyObject_HEAD
    PyObject *entrypoints;
    int bytecode_offset;
} LeMergePoint;

typedef struct {
    PyObject_HEAD
    PyCodeObject *codeobject;
    PyObject *mergepoints;
} LeMergePoints;

/* Build MergePoints from PyCodeObject. Raises LeExc_UnsupportedOpError if a 
   codeobject is not compilable by lejit */
PyObject *mergepoints_build(PyCodeObject *codeobject);

/* Search the for the entrypoint matching bytecode_offset */
PyObject *mergepoints_find_entry_points(PyObject *mergepoints, 
                                             int bytecode_offset);

/* All the following required for construction and initialisation of the 
   module. */
PyObject *Le_MergePointsBuild(PyObject *self, PyObject *args);
bool init_mergepoints(PyObject *module);
#define MERGEPOINTS_METHODS \
    {"merge_points_build", Le_MergePointsBuild, METH_VARARGS, \
     "Build MergePoints from code object. Raises UnsupportedOpError" \
     " if a codeobject is not compilable by lejit"}, 

#endif /* LE_MERGEPOINTS_H */
