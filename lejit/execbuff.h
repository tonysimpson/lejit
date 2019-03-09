#ifndef LE_WRITER_H
#define LE_WRITER_H

#include <Python.h>
#include <stdbool.h>

typedef unsigned char mm_mem_t;

typedef struct mm_block_s mm_block_t;

struct mm_block_s {
    mm_mem_t *end;
    mm_block_t *next;
    mm_block_t *prev;
    bool free;
};

typedef struct {
    PyObject_HEAD
    mm_block_t *start;
    void *buffer;
    size_t size;
    unsigned short block_alignment;
} LeMemoryManagerObject;

typedef struct {
    PyObject_HEAD
    LeMemoryManagerObject* mm;
    PyObject **held_refs; /* NULL or a NULL term array */
    mm_block_t *block;
} LeCodeObject;

LeMemoryManagerObject* LeMemoryManager_New(short block_alignment, size_t size);
LeCodeObject* LeCodeObject_New(LeMemoryManagerObject *mm, size_t min_size, int relative, mm_mem_t *location);
int LeCodeObject_Resize(LeCodeObject *self, size_t new_size);
int LeCodeObject_AddReference(LeCodeObject *self, PyObject *ref);
int LeCodeObject_RemoveReference(LeCodeObject *self, PyObject *ref);

#define WRITER_METHODS /* nothing */
int init_writer(PyObject *m);
#endif /* LE_WRITER_H */
