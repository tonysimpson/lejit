#include <structmember.h>
#include <sys/mman.h>

static mm_block_t*
mm_block_new(mm_mem_t *end,  bool free, mm_block_t *prev, mm_block_t *next) {
    mm_block_t *block = malloc(sizeof(mm_block_t));
    if (block != NULL) {
        block->end = end;
        block->next = next;
        block->prev = prev;
        block->free = free;
    } else {
        PyErr_SetString(PyExc_MemoryError, "Could not allocate mm_block_t");
    }
    return block;
}

static void
mm_block_free(mm_block_t *block) {
    free(block);
}

static mm_block_t*
mm_block_insert_before(mm_block_t *other, mm_mem_t *end, bool free) {
    mm_block_t *block = mm_block_new(end, free, other->prev, other);
    if (block != NULL) {
        other->prev->next = block;
        other->prev = block;
    }
    return block;
}

/* makes a block free==true and merges free blocks either side  */
static void 
mm_block_release(mm_block_t *block) {
    if (block->prev->free && block->next->free) {
        block->prev->prev->next = block->next;
        block->next->prev = block->prev->prev;
        mm_block_free(block->prev);
        mm_block_free(block);
        return;
    }
    block->free = true;
    if (block->prev->free) {
        mm_block_t *prev = block->prev;
        prev->prev->next = block;
        block->prev = prev->prev;
        mm_block_free(prev);
        return;
    }
    if (block->next->free) {
        block->next->prev = block->prev;
        block->prev->next = block->next;
        mm_block_free(block);
    }
}

inline static size_t
get_size(mm_block_t *block) {
    if (block->prev != NULL) {
        return ((size_t)block->end) - ((size_t)block->prev->end);
    }
    return 0;
}

static void
code_dealloc(LeCodeObject* self) {
    mm_block_release(self->block);
    if(self->held_refs != NULL) {
        PyObject **iter = self->held_refs;
        while(*iter != NULL) {
            Py_DECREF(*iter);
            iter++;
        }
        PyMem_Free(self->held_refs);
    }
    Py_DECREF(self->mm);
    PyObject_Del((PyObject*)self);
}

int
LeCodeObject_Resize(LeCodeObject *self, size_t new_size) {
    mm_block_t *block = self->block;
    size_t size = get_size(block);
    if (new_size == size)
        return 1;
    if (new_size < size) {
        /* if next block is free we enlarge it towards us if not we insert a new free block in the gap */
        if (!block->next->free) {
            if(mm_block_insert_before(block->next, block->end - (size - new_size), true) == NULL) {
                return -1;
            }
        }
        block->end -= (size - new_size);
        return 1;
    }
    if (block->next->free) {
        size_t diff = (new_size - size);
        size_t free_size = get_size(block->next);
        if (free_size > diff) {
            block->end += diff;
            return 1;
        }
        if (free_size == diff) {
            block->end += diff;
            block->next = block->next->next;
            mm_block_free(block->next->prev);
            block->next->prev = block;
            return 1;
        }
    }
    return 0;
}

static PyObject*
code_resize(LeCodeObject *self, PyObject *args) {
    size_t new_size;
    int res;
    if (!PyArg_ParseTuple(args, "k", &new_size))
        return NULL;
    res = LeCodeObject_Resize(self, new_size);
    return res == -1 ? NULL : PyBool_FromLong(res);
}

static inline size_t 
held_refs_len(PyObject ** l) {
    size_t len = 0;
    if (l != NULL) {
        while(*l != NULL) {
            len += 1;
            l++;
        }
    }
    return len;
}

int
LeCodeObject_AddReference(LeCodeObject *self, PyObject *ref) {
    size_t len = held_refs_len(self->held_refs);
    self->held_refs = PyMem_Realloc(self->held_refs, sizeof(PyObject*) * (len + 2));
    if (self->held_refs == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Could not add reference to code object, realloc failed");
        return -1;
    }
    Py_INCREF(ref);
    self->held_refs[len] = ref;
    self->held_refs[len+1] = NULL;
    return 0;
}

static PyObject*
code_add_ref(LeCodeObject *self, PyObject *args) {
    PyObject *ref;
    if (!PyArg_ParseTuple(args, "O", &ref))
        return NULL;
    if(LeCodeObject_AddReference(self, ref) != 0)
        return NULL;
    Py_RETURN_NONE;
}

int
LeCodeObject_RemoveReference(LeCodeObject *self, PyObject *ref) {
    PyObject **iter, **iter2;
    if (self->held_refs == NULL)
        goto value_error;
    if (self->held_refs[1] == NULL) {
        if (self->held_refs[0] != ref)
            goto value_error;
        PyMem_Free(self->held_refs);
        self->held_refs = NULL;
        Py_DECREF(ref);
        return 0;
    }
    iter = self->held_refs;
    while(*iter != NULL) {
        if (*iter == ref)
            goto found;
        iter++;
    }
value_error:
    PyErr_SetString(PyExc_ValueError, "Ref not held by code");
    return -1;
found:
    {
        size_t len = held_refs_len(self->held_refs);
        self->held_refs = PyMem_Realloc(self->held_refs, sizeof(PyObject*) * (len + 1));
        if (self->held_refs == NULL) {
            PyErr_SetString(PyExc_MemoryError, "Could not remove reference, realloc failed");
            return -1;
        }
        iter = self->held_refs;
        while(*iter != NULL) {
            if (*iter == ref) {
                iter2 = iter;
                iter++;
                while(*iter2 != NULL) {
                    *iter2 = *iter;
                    iter++;
                    iter2++;
                }
                Py_DECREF(ref);
                return 0;
            }
            iter++;
        }
        PyErr_SetString(PyExc_SystemError, "Remove ref logic failure");
        return -1;
    }
}

static PyObject*
code_remove_ref(LeCodeObject *self, PyObject *args) {
    PyObject *ref;
    if (!PyArg_ParseTuple(args, "O", &ref))
        return NULL;
    if(LeCodeObject_RemoveReference(self, ref) != 0)
        return NULL;
    Py_RETURN_NONE;
}

static PyMethodDef code_methods[] = {
    {"resize", (PyCFunction)code_resize, METH_VARARGS, "Resize the code block"}, 
    {"add_ref", (PyCFunction)code_add_ref, METH_VARARGS, "Add a reference"}, 
    {"remove_ref", (PyCFunction)code_remove_ref, METH_VARARGS, "Remove a reference"}, 
    {NULL}
};

static PyMemberDef code_members[] = {
    {"mm", T_OBJECT, offsetof(LeCodeObject, mm), READONLY},
    {NULL}
};

static PyObject*
code_get_start(LeCodeObject* self, void *closure) {
    return PyLong_FromUnsignedLong((unsigned long)self->block->prev->end);
}

static PyObject*
code_get_end(LeCodeObject* self, void *closure) {
    return PyLong_FromUnsignedLong((unsigned long)self->block->end);
}

static PyObject*
code_get_size(LeCodeObject* self, void *closure) {
    return PyLong_FromUnsignedLong((unsigned long)self->block->end - (unsigned long)self->block->prev->end);
}

static PyObject*
code_get_held_refs(LeCodeObject* self, void *closure) {
    size_t i;
    size_t len = held_refs_len(self->held_refs);
    PyObject *tuple = PyTuple_New(len);
    if (tuple == NULL)
        return NULL;
    for (i = 0; i < len; i++) {
        PyObject *o = self->held_refs[i];
        Py_INCREF(o);
        PyTuple_SET_ITEM(tuple, i, o);
    }
    return tuple;
}

static PyGetSetDef code_getset[] = {
    {"start", (getter)code_get_start, NULL, "code start", NULL},
    {"end", (getter)code_get_end, NULL, "code end", NULL},
    {"size", (getter)code_get_size, NULL, "code size", NULL},
    {"held_refs", (getter)code_get_held_refs, NULL, "References held by code", NULL},
    {NULL}
};

static PyTypeObject LeCode_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lejit.Code",
    .tp_basicsize = sizeof(LeCodeObject),
    .tp_dealloc = (destructor)code_dealloc,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Code",
    .tp_methods = code_methods,
    .tp_members = code_members,
    .tp_getset = code_getset
};

static PyTypeObject LeMemoryManager_Type;

LeMemoryManagerObject*
LeMemoryManager_New(short block_alignment, size_t size) {
    LeMemoryManagerObject *self;
    self = PyObject_New(LeMemoryManagerObject, &LeMemoryManager_Type);
    if (self == NULL)
        return NULL;
    self->start = NULL;
    self->size = size;
    self->block_alignment = block_alignment;
    self->buffer = mmap(NULL, self->size, PROT_EXEC|PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (self->buffer == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Could not allocate buffer");
        Py_DECREF((PyObject*)self);
        return NULL;
    }
    if ((self->start = mm_block_new((mm_mem_t*)self->buffer, false,  NULL, NULL)) == NULL) {
        Py_DECREF((PyObject*)self);
        return NULL;
    }
    if ((self->start->next = mm_block_new(self->start->end + size, true, self->start, NULL)) == NULL) {
        mm_block_free(self->start);
        Py_DECREF((PyObject*)self);
        return NULL;
    }
    return self;
}

static PyObject*
memory_manager_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    short block_alignment;
    size_t size;
    if (!PyArg_ParseTuple(args, "Hk", &block_alignment, &size))
        return NULL;
    return (PyObject*)LeMemoryManager_New(block_alignment, size);
}

static PyObject*
memory_manager_repr(PyObject *o) {
    LeMemoryManagerObject *self = (LeMemoryManagerObject*)o;
    return PyUnicode_FromFormat("MemoryManager(block_alignment=%u, "
                                 "size=%zu)", 
                                 self->block_alignment, 
                                 self->size);
}

static void
memory_manager_dealloc(PyObject* self) {
    mm_block_t *next, *cur;
    LeMemoryManagerObject *o = (LeMemoryManagerObject*)self;
    munmap(o->buffer, o->size);
    next = o->start;
    while(next != NULL) {
        cur = next;
        next = cur->next;
        mm_block_free(cur);
    }
    PyObject_Del((PyObject*)self);
}

static mm_block_t*
aquire_block_first_free_split(LeMemoryManagerObject *mm, size_t min_size) {
    mm_block_t *cur = mm->start->next;
    while (cur != NULL) {
        size_t size = get_size(cur);
        if (cur->free && size >= min_size) {
            if (size > min_size * 3) {
                /* insert the aquired block in the middle of the free block - 
                   creates 3 blocks from 1 */
                mm_block_t *prev;
                mm_mem_t *start = (cur->end - (size / 2)) - (min_size / 2);
                prev = mm_block_insert_before(cur, start, true);
                if (prev == NULL)
                    return NULL;
                cur = mm_block_insert_before(cur, start + min_size, false);
                if (cur == NULL) {
                    mm_block_release(prev);
                }
                return cur;
            }
            cur->free = false;
            return cur;
        }
        cur = cur->next;
    }
    PyErr_SetString(PyExc_MemoryError, "No free blocks of sufficient size");
    return NULL;
}

static mm_block_t*
aquire_block(LeMemoryManagerObject *mm, size_t min_size, int relative, mm_mem_t *location) {
    return aquire_block_first_free_split(mm, min_size);
}

LeCodeObject *
LeCodeObject_New(LeMemoryManagerObject *mm, size_t min_size, int relative, mm_mem_t *location) {
    LeCodeObject *self;
    mm_block_t *block = aquire_block(mm, min_size, relative, location);
    self = PyObject_New(LeCodeObject, &LeCode_Type);
    if (self != NULL) {
        Py_INCREF(mm);
        self->mm = mm;
        self->block = block;
        self->held_refs = NULL;
    }
    else {
        mm_block_release(block);
    }
    return self;
}

static PyObject*
new_code(LeMemoryManagerObject *self, PyObject *args) {
    size_t min_size;
    int relative;
    mm_mem_t *location;
    if (!PyArg_ParseTuple(args, "kik", &min_size, &relative, &location))
        return NULL;
    return (PyObject*)LeCodeObject_New(self, min_size, relative, location);
}

static PyMethodDef memory_manager_methods[] = {
    {"new_code", (PyCFunction)new_code, METH_VARARGS, ""},
    {NULL}
};

static PyMemberDef memory_manager_members[] = {
    {"block_alignment", T_SHORT, offsetof(LeMemoryManagerObject, block_alignment), READONLY},
    {"size", T_ULONG, offsetof(LeMemoryManagerObject, size), READONLY},
    {NULL}
};

static PyObject*
memory_manager_get_blocks(LeMemoryManagerObject* self, void *closure) {
    PyObject *list = PyList_New(0);
    mm_block_t *cur = self->start;
    while (cur != NULL) {
        PyObject *tuple = PyTuple_New(2);
        PyTuple_SET_ITEM(tuple, 0, PyBool_FromLong(cur->free));
        PyTuple_SET_ITEM(tuple, 1, PyLong_FromUnsignedLong((unsigned long)cur->end));
        PyList_Append(list, tuple);
        cur = cur->next;
    }
    return list;
}

static PyGetSetDef memory_manager_getset[] = {
    {"blocks", (getter)memory_manager_get_blocks, NULL, "block layout as tuples", NULL},
    {NULL}
};

static PyTypeObject LeMemoryManager_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "lejit.MemoryManager",
    .tp_basicsize = sizeof(LeMemoryManagerObject),
    .tp_dealloc = memory_manager_dealloc,
    .tp_repr = memory_manager_repr,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "Memory Manager",
    .tp_methods = memory_manager_methods,
    .tp_members = memory_manager_members,
    .tp_getset = memory_manager_getset,
    .tp_new = memory_manager_new,
};

int init_writer(PyObject *m) {
    if (PyType_Ready(&LeMemoryManager_Type) < 0)
        return -1;
    if (PyType_Ready(&LeCode_Type) < 0)
        return -1;
    PyModule_AddObject(m, "MemoryManager", (PyObject*)&LeMemoryManager_Type);
    return 0;
}

