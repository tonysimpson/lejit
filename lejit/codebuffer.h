#ifndef LE_CODEBUFFER_H
#define LE_CODEBUFFER_H

typedef struct {
    PyObject_HEAD
    Py_ssize_t pos;
    unsigned char *buffer;
    Py_ssize_t len;
    PyObject held_references;
    PyObject linked_buffers;
} LeCodeBuffer;

LeCodeBuffer_NewMain();
LeCodeBuffer_NewAux();

#endif /* LE_CODEBUFFER_H */
