/***************************************************************/
/***                 Global Psyco definitions                ***/
/***************************************************************/

#ifndef _PSYCO_H
#define _PSYCO_H


#include <Python.h>
#include <structmember.h>   /* for offsetof() */
#include <stdint.h>
/* the NI_HAS_ features of the CPython source built for */
#include "ni_features_gen.h"
/* offsets, sizes and private structs of the CPython fields Ni relies on
   (vcompiler.h:CPY_FIELD), each checked at compile time against the
   field's type */
#include <stddef.h>

#define NI_OFS_PyObject__ob_refcnt offsetof(PyObject, ob_refcnt)
#define NI_SIZE2_PyObject__ob_refcnt 3
#define NI_UNSIGNED_PyObject__ob_refcnt 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyObject *)0)->ob_refcnt), Py_ssize_t),
               "Ni: PyObject.ob_refcnt is not Py_ssize_t");
_Static_assert(sizeof(((PyObject *)0)->ob_refcnt) == 8,
               "Ni: PyObject.ob_refcnt: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyObject *)0)->ob_refcnt) == 1,
               "Ni: PyObject.ob_refcnt: Ni reads it as an integer");
_Static_assert(((__typeof__(((PyObject *)0)->ob_refcnt))-1 < 0) == 1,
               "Ni: PyObject.ob_refcnt: Ni reads it as signed");
#define NI_OFS_PyObject__ob_type offsetof(PyObject, ob_type)
#define NI_SIZE2_PyObject__ob_type 3
#define NI_UNSIGNED_PyObject__ob_type 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyObject *)0)->ob_type), struct _typeobject *),
               "Ni: PyObject.ob_type is not struct _typeobject *");
_Static_assert(sizeof(((PyObject *)0)->ob_type) == 8,
               "Ni: PyObject.ob_type: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyObject *)0)->ob_type) == 5,
               "Ni: PyObject.ob_type: Ni reads it as a pointer");

#define NI_OFS_PyVarObject__ob_size offsetof(PyVarObject, ob_size)
#define NI_SIZE2_PyVarObject__ob_size 3
#define NI_UNSIGNED_PyVarObject__ob_size 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyVarObject *)0)->ob_size), Py_ssize_t),
               "Ni: PyVarObject.ob_size is not Py_ssize_t");
_Static_assert(sizeof(((PyVarObject *)0)->ob_size) == 8,
               "Ni: PyVarObject.ob_size: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyVarObject *)0)->ob_size) == 1,
               "Ni: PyVarObject.ob_size: Ni reads it as an integer");
_Static_assert(((__typeof__(((PyVarObject *)0)->ob_size))-1 < 0) == 1,
               "Ni: PyVarObject.ob_size: Ni reads it as signed");

#define NI_OFS_PyTypeObject__tp_dealloc offsetof(PyTypeObject, tp_dealloc)
#define NI_SIZE2_PyTypeObject__tp_dealloc 3
#define NI_UNSIGNED_PyTypeObject__tp_dealloc 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTypeObject *)0)->tp_dealloc), destructor),
               "Ni: PyTypeObject.tp_dealloc is not destructor");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_dealloc) == 8,
               "Ni: PyTypeObject.tp_dealloc: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyTypeObject *)0)->tp_dealloc) == 5,
               "Ni: PyTypeObject.tp_dealloc: Ni reads it as a pointer");
#define NI_OFS_PyTypeObject__tp_flags offsetof(PyTypeObject, tp_flags)
#define NI_SIZE2_PyTypeObject__tp_flags 3
#define NI_UNSIGNED_PyTypeObject__tp_flags 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTypeObject *)0)->tp_flags), unsigned long),
               "Ni: PyTypeObject.tp_flags is not unsigned long");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_flags) == 8,
               "Ni: PyTypeObject.tp_flags: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyTypeObject *)0)->tp_flags) == 1,
               "Ni: PyTypeObject.tp_flags: Ni reads it as an integer");
_Static_assert(((__typeof__(((PyTypeObject *)0)->tp_flags))-1 < 0) == 0,
               "Ni: PyTypeObject.tp_flags: Ni reads it as unsigned");
#define NI_OFS_PyTypeObject__tp_version_tag \
    offsetof(PyTypeObject, tp_version_tag)
#define NI_SIZE2_PyTypeObject__tp_version_tag 2
#define NI_UNSIGNED_PyTypeObject__tp_version_tag 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTypeObject *)0)->tp_version_tag),
                   unsigned int),
               "Ni: PyTypeObject.tp_version_tag is not unsigned int");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_version_tag) == 4,
               "Ni: PyTypeObject.tp_version_tag: Ni reads 4 bytes");
_Static_assert(__builtin_classify_type(((PyTypeObject *)0)->tp_version_tag) ==
                   1,
               "Ni: PyTypeObject.tp_version_tag: Ni reads it as an integer");
_Static_assert(((__typeof__(((PyTypeObject *)0)->tp_version_tag))-1 < 0) == 0,
               "Ni: PyTypeObject.tp_version_tag: Ni reads it as unsigned");

#define NI_OFS_PyFloatObject__ob_fval offsetof(PyFloatObject, ob_fval)
#define NI_SIZE2_PyFloatObject__ob_fval 3
#define NI_UNSIGNED_PyFloatObject__ob_fval 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFloatObject *)0)->ob_fval), double),
               "Ni: PyFloatObject.ob_fval is not double");
_Static_assert(sizeof(((PyFloatObject *)0)->ob_fval) == 8,
               "Ni: PyFloatObject.ob_fval: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyFloatObject *)0)->ob_fval) == 8,
               "Ni: PyFloatObject.ob_fval: Ni reads it as floating");

#define NI_OFS_PyLongObject__ob_digit offsetof(PyLongObject, ob_digit)
#define NI_SIZE2_PyLongObject__ob_digit 2
#define NI_UNSIGNED_PyLongObject__ob_digit 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyLongObject *)0)->ob_digit), digit[1]),
               "Ni: PyLongObject.ob_digit is not digit[1]");
_Static_assert(sizeof(((PyLongObject *)0)->ob_digit[0]) == 4,
               "Ni: PyLongObject.ob_digit: Ni reads 4 bytes");
_Static_assert(__builtin_classify_type(((PyLongObject *)0)->ob_digit[0]) == 1,
               "Ni: PyLongObject.ob_digit: Ni reads it as an integer");
_Static_assert(((__typeof__(((PyLongObject *)0)->ob_digit[0]))-1 < 0) == 0,
               "Ni: PyLongObject.ob_digit: Ni reads it as unsigned");

#define NI_OFS_PyListObject__ob_item offsetof(PyListObject, ob_item)
#define NI_SIZE2_PyListObject__ob_item 3
#define NI_UNSIGNED_PyListObject__ob_item 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyListObject *)0)->ob_item), PyObject **),
               "Ni: PyListObject.ob_item is not PyObject **");
_Static_assert(sizeof(((PyListObject *)0)->ob_item) == 8,
               "Ni: PyListObject.ob_item: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyListObject *)0)->ob_item) == 5,
               "Ni: PyListObject.ob_item: Ni reads it as a pointer");

#define NI_OFS_PyTupleObject__ob_item offsetof(PyTupleObject, ob_item)
#define NI_SIZE2_PyTupleObject__ob_item 3
#define NI_UNSIGNED_PyTupleObject__ob_item 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTupleObject *)0)->ob_item), PyObject *[1]),
               "Ni: PyTupleObject.ob_item is not PyObject *[1]");
_Static_assert(sizeof(((PyTupleObject *)0)->ob_item[0]) == 8,
               "Ni: PyTupleObject.ob_item: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyTupleObject *)0)->ob_item[0]) == 5,
               "Ni: PyTupleObject.ob_item: Ni reads it as a pointer");

#define NI_OFS_PyBytesObject__ob_sval offsetof(PyBytesObject, ob_sval)
#define NI_SIZE2_PyBytesObject__ob_sval 0
#define NI_UNSIGNED_PyBytesObject__ob_sval 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyBytesObject *)0)->ob_sval), char[1]),
               "Ni: PyBytesObject.ob_sval is not char[1]");
_Static_assert(sizeof(((PyBytesObject *)0)->ob_sval[0]) == 1,
               "Ni: PyBytesObject.ob_sval: Ni reads 1 byte");

#define NI_OFS_PyDictObject__ma_used offsetof(PyDictObject, ma_used)
#define NI_SIZE2_PyDictObject__ma_used 2
#define NI_UNSIGNED_PyDictObject__ma_used 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyDictObject *)0)->ma_used), Py_ssize_t),
               "Ni: PyDictObject.ma_used is not Py_ssize_t");
_Static_assert(sizeof(((PyDictObject *)0)->ma_used) >= 4,
               "Ni: PyDictObject.ma_used: Ni reads 4 bytes");
#define NI_OFS_PyDictObject__ma_version_tag \
    offsetof(PyDictObject, ma_version_tag)
#define NI_SIZE2_PyDictObject__ma_version_tag 3
#define NI_UNSIGNED_PyDictObject__ma_version_tag 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyDictObject *)0)->ma_version_tag), uint64_t),
               "Ni: PyDictObject.ma_version_tag is not uint64_t");
_Static_assert(sizeof(((PyDictObject *)0)->ma_version_tag) == 8,
               "Ni: PyDictObject.ma_version_tag: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyDictObject *)0)->ma_version_tag) ==
                   1,
               "Ni: PyDictObject.ma_version_tag: Ni reads it as an integer");
_Static_assert(((__typeof__(((PyDictObject *)0)->ma_version_tag))-1 < 0) == 0,
               "Ni: PyDictObject.ma_version_tag: Ni reads it as unsigned");
#define NI_OFS_PyDictObject__ma_keys offsetof(PyDictObject, ma_keys)
#define NI_SIZE2_PyDictObject__ma_keys 3
#define NI_UNSIGNED_PyDictObject__ma_keys 0
_Static_assert(
    __builtin_types_compatible_p(__typeof__(((PyDictObject *)0)->ma_keys),
                                 PyDictKeysObject *),
    "Ni: PyDictObject.ma_keys is not PyDictKeysObject *");
_Static_assert(sizeof(((PyDictObject *)0)->ma_keys) == 8,
               "Ni: PyDictObject.ma_keys: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyDictObject *)0)->ma_keys) == 5,
               "Ni: PyDictObject.ma_keys: Ni reads it as a pointer");
#define NI_OFS_PyDictObject__ma_values offsetof(PyDictObject, ma_values)
#define NI_SIZE2_PyDictObject__ma_values 3
#define NI_UNSIGNED_PyDictObject__ma_values 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyDictObject *)0)->ma_values), PyObject **),
               "Ni: PyDictObject.ma_values is not PyObject **");
_Static_assert(sizeof(((PyDictObject *)0)->ma_values) == 8,
               "Ni: PyDictObject.ma_values: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyDictObject *)0)->ma_values) == 5,
               "Ni: PyDictObject.ma_values: Ni reads it as a pointer");

#define NI_OFS_PyASCIIObject__length offsetof(PyASCIIObject, length)
#define NI_SIZE2_PyASCIIObject__length 3
#define NI_UNSIGNED_PyASCIIObject__length 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyASCIIObject *)0)->length), Py_ssize_t),
               "Ni: PyASCIIObject.length is not Py_ssize_t");
_Static_assert(sizeof(((PyASCIIObject *)0)->length) == 8,
               "Ni: PyASCIIObject.length: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyASCIIObject *)0)->length) == 1,
               "Ni: PyASCIIObject.length: Ni reads it as an integer");
_Static_assert(((__typeof__(((PyASCIIObject *)0)->length))-1 < 0) == 1,
               "Ni: PyASCIIObject.length: Ni reads it as signed");
#define NI_OFS_PyASCIIObject__hash offsetof(PyASCIIObject, hash)
#define NI_SIZE2_PyASCIIObject__hash 3
#define NI_UNSIGNED_PyASCIIObject__hash 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyASCIIObject *)0)->hash), Py_hash_t),
               "Ni: PyASCIIObject.hash is not Py_hash_t");
_Static_assert(sizeof(((PyASCIIObject *)0)->hash) == 8,
               "Ni: PyASCIIObject.hash: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyASCIIObject *)0)->hash) == 1,
               "Ni: PyASCIIObject.hash: Ni reads it as an integer");
_Static_assert(((__typeof__(((PyASCIIObject *)0)->hash))-1 < 0) == 1,
               "Ni: PyASCIIObject.hash: Ni reads it as signed");
#define NI_OFS_PyASCIIObject__state offsetof(PyASCIIObject, state)
#define NI_SIZE2_PyASCIIObject__state 0
#define NI_UNSIGNED_PyASCIIObject__state 1
_Static_assert(sizeof(((PyASCIIObject *)0)->state) >= 1,
               "Ni: PyASCIIObject.state: Ni reads 1 byte");
#define NI_BIT_PyASCIIObject__state__interned 0x3
#define NI_BIT_PyASCIIObject__state__kind 0x1C
#define NI_BIT_PyASCIIObject__state__compact 0x20
#define NI_BIT_PyASCIIObject__state__ascii 0x40
#define NI_BIT_PyASCIIObject__state__ready 0x80

#define NI_OFS_PySliceObject__start offsetof(PySliceObject, start)
#define NI_SIZE2_PySliceObject__start 3
#define NI_UNSIGNED_PySliceObject__start 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PySliceObject *)0)->start), PyObject *),
               "Ni: PySliceObject.start is not PyObject *");
_Static_assert(sizeof(((PySliceObject *)0)->start) == 8,
               "Ni: PySliceObject.start: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PySliceObject *)0)->start) == 5,
               "Ni: PySliceObject.start: Ni reads it as a pointer");
#define NI_OFS_PySliceObject__stop offsetof(PySliceObject, stop)
#define NI_SIZE2_PySliceObject__stop 3
#define NI_UNSIGNED_PySliceObject__stop 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PySliceObject *)0)->stop), PyObject *),
               "Ni: PySliceObject.stop is not PyObject *");
_Static_assert(sizeof(((PySliceObject *)0)->stop) == 8,
               "Ni: PySliceObject.stop: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PySliceObject *)0)->stop) == 5,
               "Ni: PySliceObject.stop: Ni reads it as a pointer");
#define NI_OFS_PySliceObject__step offsetof(PySliceObject, step)
#define NI_SIZE2_PySliceObject__step 3
#define NI_UNSIGNED_PySliceObject__step 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PySliceObject *)0)->step), PyObject *),
               "Ni: PySliceObject.step is not PyObject *");
_Static_assert(sizeof(((PySliceObject *)0)->step) == 8,
               "Ni: PySliceObject.step: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PySliceObject *)0)->step) == 5,
               "Ni: PySliceObject.step: Ni reads it as a pointer");

#define NI_OFS_PyFunctionObject__func_code \
    offsetof(PyFunctionObject, func_code)
#define NI_SIZE2_PyFunctionObject__func_code 3
#define NI_UNSIGNED_PyFunctionObject__func_code 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_code), PyObject *),
               "Ni: PyFunctionObject.func_code is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_code) == 8,
               "Ni: PyFunctionObject.func_code: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyFunctionObject *)0)->func_code) ==
                   5,
               "Ni: PyFunctionObject.func_code: Ni reads it as a pointer");
#define NI_OFS_PyFunctionObject__func_globals \
    offsetof(PyFunctionObject, func_globals)
#define NI_SIZE2_PyFunctionObject__func_globals 3
#define NI_UNSIGNED_PyFunctionObject__func_globals 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_globals),
                   PyObject *),
               "Ni: PyFunctionObject.func_globals is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_globals) == 8,
               "Ni: PyFunctionObject.func_globals: Ni reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_globals) == 5,
    "Ni: PyFunctionObject.func_globals: Ni reads it as a pointer");
#define NI_OFS_PyFunctionObject__func_defaults \
    offsetof(PyFunctionObject, func_defaults)
#define NI_SIZE2_PyFunctionObject__func_defaults 3
#define NI_UNSIGNED_PyFunctionObject__func_defaults 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_defaults),
                   PyObject *),
               "Ni: PyFunctionObject.func_defaults is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_defaults) == 8,
               "Ni: PyFunctionObject.func_defaults: Ni reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_defaults) == 5,
    "Ni: PyFunctionObject.func_defaults: Ni reads it as a pointer");
#define NI_OFS_PyFunctionObject__func_kwdefaults \
    offsetof(PyFunctionObject, func_kwdefaults)
#define NI_SIZE2_PyFunctionObject__func_kwdefaults 3
#define NI_UNSIGNED_PyFunctionObject__func_kwdefaults 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_kwdefaults),
                   PyObject *),
               "Ni: PyFunctionObject.func_kwdefaults is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_kwdefaults) == 8,
               "Ni: PyFunctionObject.func_kwdefaults: Ni reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_kwdefaults) == 5,
    "Ni: PyFunctionObject.func_kwdefaults: Ni reads it as a pointer");
#define NI_OFS_PyFunctionObject__func_closure \
    offsetof(PyFunctionObject, func_closure)
#define NI_SIZE2_PyFunctionObject__func_closure 3
#define NI_UNSIGNED_PyFunctionObject__func_closure 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_closure),
                   PyObject *),
               "Ni: PyFunctionObject.func_closure is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_closure) == 8,
               "Ni: PyFunctionObject.func_closure: Ni reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_closure) == 5,
    "Ni: PyFunctionObject.func_closure: Ni reads it as a pointer");
#define NI_OFS_PyFunctionObject__func_annotations \
    offsetof(PyFunctionObject, func_annotations)
#define NI_SIZE2_PyFunctionObject__func_annotations 3
#define NI_UNSIGNED_PyFunctionObject__func_annotations 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_annotations),
                   PyObject *),
               "Ni: PyFunctionObject.func_annotations is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_annotations) == 8,
               "Ni: PyFunctionObject.func_annotations: Ni reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_annotations) == 5,
    "Ni: PyFunctionObject.func_annotations: Ni reads it as a pointer");
#define NI_OFS_PyFunctionObject__func_qualname \
    offsetof(PyFunctionObject, func_qualname)
#define NI_SIZE2_PyFunctionObject__func_qualname 3
#define NI_UNSIGNED_PyFunctionObject__func_qualname 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_qualname),
                   PyObject *),
               "Ni: PyFunctionObject.func_qualname is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_qualname) == 8,
               "Ni: PyFunctionObject.func_qualname: Ni reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_qualname) == 5,
    "Ni: PyFunctionObject.func_qualname: Ni reads it as a pointer");

#define NI_OFS_PyMethodObject__im_func offsetof(PyMethodObject, im_func)
#define NI_SIZE2_PyMethodObject__im_func 3
#define NI_UNSIGNED_PyMethodObject__im_func 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyMethodObject *)0)->im_func), PyObject *),
               "Ni: PyMethodObject.im_func is not PyObject *");
_Static_assert(sizeof(((PyMethodObject *)0)->im_func) == 8,
               "Ni: PyMethodObject.im_func: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyMethodObject *)0)->im_func) == 5,
               "Ni: PyMethodObject.im_func: Ni reads it as a pointer");
#define NI_OFS_PyMethodObject__im_self offsetof(PyMethodObject, im_self)
#define NI_SIZE2_PyMethodObject__im_self 3
#define NI_UNSIGNED_PyMethodObject__im_self 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyMethodObject *)0)->im_self), PyObject *),
               "Ni: PyMethodObject.im_self is not PyObject *");
_Static_assert(sizeof(((PyMethodObject *)0)->im_self) == 8,
               "Ni: PyMethodObject.im_self: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyMethodObject *)0)->im_self) == 5,
               "Ni: PyMethodObject.im_self: Ni reads it as a pointer");

#define NI_OFS_PyCFunctionObject__m_ml offsetof(PyCFunctionObject, m_ml)
#define NI_SIZE2_PyCFunctionObject__m_ml 3
#define NI_UNSIGNED_PyCFunctionObject__m_ml 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyCFunctionObject *)0)->m_ml), PyMethodDef *),
               "Ni: PyCFunctionObject.m_ml is not PyMethodDef *");
_Static_assert(sizeof(((PyCFunctionObject *)0)->m_ml) == 8,
               "Ni: PyCFunctionObject.m_ml: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyCFunctionObject *)0)->m_ml) == 5,
               "Ni: PyCFunctionObject.m_ml: Ni reads it as a pointer");
#define NI_OFS_PyCFunctionObject__m_self offsetof(PyCFunctionObject, m_self)
#define NI_SIZE2_PyCFunctionObject__m_self 3
#define NI_UNSIGNED_PyCFunctionObject__m_self 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyCFunctionObject *)0)->m_self), PyObject *),
               "Ni: PyCFunctionObject.m_self is not PyObject *");
_Static_assert(sizeof(((PyCFunctionObject *)0)->m_self) == 8,
               "Ni: PyCFunctionObject.m_self: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyCFunctionObject *)0)->m_self) == 5,
               "Ni: PyCFunctionObject.m_self: Ni reads it as a pointer");

#define NI_OFS_PyCellObject__ob_ref offsetof(PyCellObject, ob_ref)
#define NI_SIZE2_PyCellObject__ob_ref 3
#define NI_UNSIGNED_PyCellObject__ob_ref 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyCellObject *)0)->ob_ref), PyObject *),
               "Ni: PyCellObject.ob_ref is not PyObject *");
_Static_assert(sizeof(((PyCellObject *)0)->ob_ref) == 8,
               "Ni: PyCellObject.ob_ref: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyCellObject *)0)->ob_ref) == 5,
               "Ni: PyCellObject.ob_ref: Ni reads it as a pointer");

typedef struct {
    PyObject_HEAD Py_ssize_t it_index;
    PyObject *it_seq;
} seqiterobject;
#define NI_OFS_seqiterobject__it_index offsetof(seqiterobject, it_index)
#define NI_SIZE2_seqiterobject__it_index 3
#define NI_UNSIGNED_seqiterobject__it_index 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((seqiterobject *)0)->it_index), Py_ssize_t),
               "Ni: seqiterobject.it_index is not Py_ssize_t");
_Static_assert(sizeof(((seqiterobject *)0)->it_index) == 8,
               "Ni: seqiterobject.it_index: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((seqiterobject *)0)->it_index) == 1,
               "Ni: seqiterobject.it_index: Ni reads it as an integer");
_Static_assert(((__typeof__(((seqiterobject *)0)->it_index))-1 < 0) == 1,
               "Ni: seqiterobject.it_index: Ni reads it as signed");
#define NI_OFS_seqiterobject__it_seq offsetof(seqiterobject, it_seq)
#define NI_SIZE2_seqiterobject__it_seq 3
#define NI_UNSIGNED_seqiterobject__it_seq 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((seqiterobject *)0)->it_seq), PyObject *),
               "Ni: seqiterobject.it_seq is not PyObject *");
_Static_assert(sizeof(((seqiterobject *)0)->it_seq) == 8,
               "Ni: seqiterobject.it_seq: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((seqiterobject *)0)->it_seq) == 5,
               "Ni: seqiterobject.it_seq: Ni reads it as a pointer");

typedef struct {
    PyObject_HEAD PyObject *start;
    PyObject *stop;
    PyObject *step;
    PyObject *length;
} rangeobject;
#define NI_OFS_rangeobject__start offsetof(rangeobject, start)
#define NI_SIZE2_rangeobject__start 3
#define NI_UNSIGNED_rangeobject__start 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeobject *)0)->start), PyObject *),
               "Ni: rangeobject.start is not PyObject *");
_Static_assert(sizeof(((rangeobject *)0)->start) == 8,
               "Ni: rangeobject.start: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeobject *)0)->start) == 5,
               "Ni: rangeobject.start: Ni reads it as a pointer");
#define NI_OFS_rangeobject__stop offsetof(rangeobject, stop)
#define NI_SIZE2_rangeobject__stop 3
#define NI_UNSIGNED_rangeobject__stop 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeobject *)0)->stop), PyObject *),
               "Ni: rangeobject.stop is not PyObject *");
_Static_assert(sizeof(((rangeobject *)0)->stop) == 8,
               "Ni: rangeobject.stop: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeobject *)0)->stop) == 5,
               "Ni: rangeobject.stop: Ni reads it as a pointer");
#define NI_OFS_rangeobject__step offsetof(rangeobject, step)
#define NI_SIZE2_rangeobject__step 3
#define NI_UNSIGNED_rangeobject__step 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeobject *)0)->step), PyObject *),
               "Ni: rangeobject.step is not PyObject *");
_Static_assert(sizeof(((rangeobject *)0)->step) == 8,
               "Ni: rangeobject.step: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeobject *)0)->step) == 5,
               "Ni: rangeobject.step: Ni reads it as a pointer");
#define NI_OFS_rangeobject__length offsetof(rangeobject, length)
#define NI_SIZE2_rangeobject__length 3
#define NI_UNSIGNED_rangeobject__length 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeobject *)0)->length), PyObject *),
               "Ni: rangeobject.length is not PyObject *");
_Static_assert(sizeof(((rangeobject *)0)->length) == 8,
               "Ni: rangeobject.length: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeobject *)0)->length) == 5,
               "Ni: rangeobject.length: Ni reads it as a pointer");

typedef struct {
    PyObject_HEAD long index;
    long start;
    long step;
    long len;
} rangeiterobject;
#define NI_OFS_rangeiterobject__index offsetof(rangeiterobject, index)
#define NI_SIZE2_rangeiterobject__index 3
#define NI_UNSIGNED_rangeiterobject__index 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeiterobject *)0)->index), long),
               "Ni: rangeiterobject.index is not long");
_Static_assert(sizeof(((rangeiterobject *)0)->index) == 8,
               "Ni: rangeiterobject.index: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeiterobject *)0)->index) == 1,
               "Ni: rangeiterobject.index: Ni reads it as an integer");
_Static_assert(((__typeof__(((rangeiterobject *)0)->index))-1 < 0) == 1,
               "Ni: rangeiterobject.index: Ni reads it as signed");
#define NI_OFS_rangeiterobject__start offsetof(rangeiterobject, start)
#define NI_SIZE2_rangeiterobject__start 3
#define NI_UNSIGNED_rangeiterobject__start 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeiterobject *)0)->start), long),
               "Ni: rangeiterobject.start is not long");
_Static_assert(sizeof(((rangeiterobject *)0)->start) == 8,
               "Ni: rangeiterobject.start: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeiterobject *)0)->start) == 1,
               "Ni: rangeiterobject.start: Ni reads it as an integer");
_Static_assert(((__typeof__(((rangeiterobject *)0)->start))-1 < 0) == 1,
               "Ni: rangeiterobject.start: Ni reads it as signed");
#define NI_OFS_rangeiterobject__step offsetof(rangeiterobject, step)
#define NI_SIZE2_rangeiterobject__step 3
#define NI_UNSIGNED_rangeiterobject__step 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeiterobject *)0)->step), long),
               "Ni: rangeiterobject.step is not long");
_Static_assert(sizeof(((rangeiterobject *)0)->step) == 8,
               "Ni: rangeiterobject.step: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeiterobject *)0)->step) == 1,
               "Ni: rangeiterobject.step: Ni reads it as an integer");
_Static_assert(((__typeof__(((rangeiterobject *)0)->step))-1 < 0) == 1,
               "Ni: rangeiterobject.step: Ni reads it as signed");
#define NI_OFS_rangeiterobject__len offsetof(rangeiterobject, len)
#define NI_SIZE2_rangeiterobject__len 3
#define NI_UNSIGNED_rangeiterobject__len 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeiterobject *)0)->len), long),
               "Ni: rangeiterobject.len is not long");
_Static_assert(sizeof(((rangeiterobject *)0)->len) == 8,
               "Ni: rangeiterobject.len: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeiterobject *)0)->len) == 1,
               "Ni: rangeiterobject.len: Ni reads it as an integer");
_Static_assert(((__typeof__(((rangeiterobject *)0)->len))-1 < 0) == 1,
               "Ni: rangeiterobject.len: Ni reads it as signed");

typedef Py_ssize_t (*dict_lookup_func)(PyDictObject *mp, PyObject *key,
                                       Py_hash_t hash, PyObject **value_addr);
struct _dictkeysobject {
    Py_ssize_t dk_refcnt;
    Py_ssize_t dk_size;
    dict_lookup_func dk_lookup;
    Py_ssize_t dk_usable;
    Py_ssize_t dk_nentries;
    char dk_indices[];
};
typedef struct _dictkeysobject ni_dictkeys_t;
#define NI_OFS_ni_dictkeys_t__dk_refcnt offsetof(ni_dictkeys_t, dk_refcnt)
#define NI_SIZE2_ni_dictkeys_t__dk_refcnt 3
#define NI_UNSIGNED_ni_dictkeys_t__dk_refcnt 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_dictkeys_t *)0)->dk_refcnt), Py_ssize_t),
               "Ni: PyDictKeysObject.dk_refcnt is not Py_ssize_t");
_Static_assert(sizeof(((ni_dictkeys_t *)0)->dk_refcnt) == 8,
               "Ni: PyDictKeysObject.dk_refcnt: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_dictkeys_t *)0)->dk_refcnt) == 1,
               "Ni: PyDictKeysObject.dk_refcnt: Ni reads it as an integer");
_Static_assert(((__typeof__(((ni_dictkeys_t *)0)->dk_refcnt))-1 < 0) == 1,
               "Ni: PyDictKeysObject.dk_refcnt: Ni reads it as signed");
#define NI_OFS_ni_dictkeys_t__dk_size offsetof(ni_dictkeys_t, dk_size)
#define NI_SIZE2_ni_dictkeys_t__dk_size 3
#define NI_UNSIGNED_ni_dictkeys_t__dk_size 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_dictkeys_t *)0)->dk_size), Py_ssize_t),
               "Ni: PyDictKeysObject.dk_size is not Py_ssize_t");
_Static_assert(sizeof(((ni_dictkeys_t *)0)->dk_size) == 8,
               "Ni: PyDictKeysObject.dk_size: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_dictkeys_t *)0)->dk_size) == 1,
               "Ni: PyDictKeysObject.dk_size: Ni reads it as an integer");
_Static_assert(((__typeof__(((ni_dictkeys_t *)0)->dk_size))-1 < 0) == 1,
               "Ni: PyDictKeysObject.dk_size: Ni reads it as signed");
#define NI_OFS_ni_dictkeys_t__dk_usable offsetof(ni_dictkeys_t, dk_usable)
#define NI_SIZE2_ni_dictkeys_t__dk_usable 3
#define NI_UNSIGNED_ni_dictkeys_t__dk_usable 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_dictkeys_t *)0)->dk_usable), Py_ssize_t),
               "Ni: PyDictKeysObject.dk_usable is not Py_ssize_t");
_Static_assert(sizeof(((ni_dictkeys_t *)0)->dk_usable) == 8,
               "Ni: PyDictKeysObject.dk_usable: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_dictkeys_t *)0)->dk_usable) == 1,
               "Ni: PyDictKeysObject.dk_usable: Ni reads it as an integer");
_Static_assert(((__typeof__(((ni_dictkeys_t *)0)->dk_usable))-1 < 0) == 1,
               "Ni: PyDictKeysObject.dk_usable: Ni reads it as signed");
#define NI_OFS_ni_dictkeys_t__dk_nentries offsetof(ni_dictkeys_t, dk_nentries)
#define NI_SIZE2_ni_dictkeys_t__dk_nentries 3
#define NI_UNSIGNED_ni_dictkeys_t__dk_nentries 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_dictkeys_t *)0)->dk_nentries), Py_ssize_t),
               "Ni: PyDictKeysObject.dk_nentries is not Py_ssize_t");
_Static_assert(sizeof(((ni_dictkeys_t *)0)->dk_nentries) == 8,
               "Ni: PyDictKeysObject.dk_nentries: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_dictkeys_t *)0)->dk_nentries) == 1,
               "Ni: PyDictKeysObject.dk_nentries: Ni reads it as an integer");
_Static_assert(((__typeof__(((ni_dictkeys_t *)0)->dk_nentries))-1 < 0) == 1,
               "Ni: PyDictKeysObject.dk_nentries: Ni reads it as signed");
#define NI_OFS_ni_dictkeys_t__dk_indices offsetof(ni_dictkeys_t, dk_indices)
#define NI_SIZE2_ni_dictkeys_t__dk_indices 0
#define NI_UNSIGNED_ni_dictkeys_t__dk_indices 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_dictkeys_t *)0)->dk_indices), char[]),
               "Ni: PyDictKeysObject.dk_indices is not char[]");
_Static_assert(sizeof(((ni_dictkeys_t *)0)->dk_indices[0]) == 1,
               "Ni: PyDictKeysObject.dk_indices: Ni reads 1 byte");

struct unicode_formatter_t {
    PyObject *args;
    int args_owned;
    Py_ssize_t arglen, argidx;
    PyObject *dict;
    enum PyUnicode_Kind fmtkind;
    Py_ssize_t fmtcnt, fmtpos;
#if NI_HAS_UNICODE_CONST_DATA
    const void *fmtdata;
#else
    void *fmtdata;
#endif
    PyObject *fmtstr;
    _PyUnicodeWriter writer;
};
typedef struct unicode_formatter_t ni_formatter_t;
#define NI_OFS_ni_formatter_t__args offsetof(ni_formatter_t, args)
#define NI_SIZE2_ni_formatter_t__args 3
#define NI_UNSIGNED_ni_formatter_t__args 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_formatter_t *)0)->args), PyObject *),
               "Ni: unicode_formatter_t.args is not PyObject *");
_Static_assert(sizeof(((ni_formatter_t *)0)->args) == 8,
               "Ni: unicode_formatter_t.args: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_formatter_t *)0)->args) == 5,
               "Ni: unicode_formatter_t.args: Ni reads it as a pointer");
#define NI_OFS_ni_formatter_t__arglen offsetof(ni_formatter_t, arglen)
#define NI_SIZE2_ni_formatter_t__arglen 3
#define NI_UNSIGNED_ni_formatter_t__arglen 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_formatter_t *)0)->arglen), Py_ssize_t),
               "Ni: unicode_formatter_t.arglen is not Py_ssize_t");
_Static_assert(sizeof(((ni_formatter_t *)0)->arglen) == 8,
               "Ni: unicode_formatter_t.arglen: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_formatter_t *)0)->arglen) == 1,
               "Ni: unicode_formatter_t.arglen: Ni reads it as an integer");
_Static_assert(((__typeof__(((ni_formatter_t *)0)->arglen))-1 < 0) == 1,
               "Ni: unicode_formatter_t.arglen: Ni reads it as signed");
#define NI_OFS_ni_formatter_t__argidx offsetof(ni_formatter_t, argidx)
#define NI_SIZE2_ni_formatter_t__argidx 3
#define NI_UNSIGNED_ni_formatter_t__argidx 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_formatter_t *)0)->argidx), Py_ssize_t),
               "Ni: unicode_formatter_t.argidx is not Py_ssize_t");
_Static_assert(sizeof(((ni_formatter_t *)0)->argidx) == 8,
               "Ni: unicode_formatter_t.argidx: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_formatter_t *)0)->argidx) == 1,
               "Ni: unicode_formatter_t.argidx: Ni reads it as an integer");
_Static_assert(((__typeof__(((ni_formatter_t *)0)->argidx))-1 < 0) == 1,
               "Ni: unicode_formatter_t.argidx: Ni reads it as signed");

struct unicode_format_arg_t {
    Py_UCS4 ch;
    int flags;
    Py_ssize_t width;
    int prec;
    int sign;
};
typedef struct unicode_format_arg_t ni_format_arg_t;
#define NI_OFS_ni_format_arg_t__ch offsetof(ni_format_arg_t, ch)
#define NI_SIZE2_ni_format_arg_t__ch 2
#define NI_UNSIGNED_ni_format_arg_t__ch 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_format_arg_t *)0)->ch), Py_UCS4),
               "Ni: unicode_format_arg_t.ch is not Py_UCS4");
_Static_assert(sizeof(((ni_format_arg_t *)0)->ch) == 4,
               "Ni: unicode_format_arg_t.ch: Ni reads 4 bytes");
_Static_assert(__builtin_classify_type(((ni_format_arg_t *)0)->ch) == 1,
               "Ni: unicode_format_arg_t.ch: Ni reads it as an integer");
_Static_assert(((__typeof__(((ni_format_arg_t *)0)->ch))-1 < 0) == 0,
               "Ni: unicode_format_arg_t.ch: Ni reads it as unsigned");
#define NI_OFS_ni_format_arg_t__width offsetof(ni_format_arg_t, width)
#define NI_SIZE2_ni_format_arg_t__width 3
#define NI_UNSIGNED_ni_format_arg_t__width 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_format_arg_t *)0)->width), Py_ssize_t),
               "Ni: unicode_format_arg_t.width is not Py_ssize_t");
_Static_assert(sizeof(((ni_format_arg_t *)0)->width) == 8,
               "Ni: unicode_format_arg_t.width: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_format_arg_t *)0)->width) == 1,
               "Ni: unicode_format_arg_t.width: Ni reads it as an integer");
_Static_assert(((__typeof__(((ni_format_arg_t *)0)->width))-1 < 0) == 1,
               "Ni: unicode_format_arg_t.width: Ni reads it as signed");

typedef struct {
    Py_hash_t me_hash;
    PyObject *me_key;
    PyObject *me_value;
} ni_dictentry_t;
typedef ni_dictentry_t PyDictKeyEntry;
#define NI_OFS_ni_dictentry_t__me_hash offsetof(ni_dictentry_t, me_hash)
#define NI_SIZE2_ni_dictentry_t__me_hash 3
#define NI_UNSIGNED_ni_dictentry_t__me_hash 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_dictentry_t *)0)->me_hash), Py_hash_t),
               "Ni: PyDictKeyEntry.me_hash is not Py_hash_t");
_Static_assert(sizeof(((ni_dictentry_t *)0)->me_hash) == 8,
               "Ni: PyDictKeyEntry.me_hash: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_dictentry_t *)0)->me_hash) == 1,
               "Ni: PyDictKeyEntry.me_hash: Ni reads it as an integer");
_Static_assert(((__typeof__(((ni_dictentry_t *)0)->me_hash))-1 < 0) == 1,
               "Ni: PyDictKeyEntry.me_hash: Ni reads it as signed");
#define NI_OFS_ni_dictentry_t__me_key offsetof(ni_dictentry_t, me_key)
#define NI_SIZE2_ni_dictentry_t__me_key 3
#define NI_UNSIGNED_ni_dictentry_t__me_key 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_dictentry_t *)0)->me_key), PyObject *),
               "Ni: PyDictKeyEntry.me_key is not PyObject *");
_Static_assert(sizeof(((ni_dictentry_t *)0)->me_key) == 8,
               "Ni: PyDictKeyEntry.me_key: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_dictentry_t *)0)->me_key) == 5,
               "Ni: PyDictKeyEntry.me_key: Ni reads it as a pointer");
#define NI_OFS_ni_dictentry_t__me_value offsetof(ni_dictentry_t, me_value)
#define NI_SIZE2_ni_dictentry_t__me_value 3
#define NI_UNSIGNED_ni_dictentry_t__me_value 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((ni_dictentry_t *)0)->me_value), PyObject *),
               "Ni: PyDictKeyEntry.me_value is not PyObject *");
_Static_assert(sizeof(((ni_dictentry_t *)0)->me_value) == 8,
               "Ni: PyDictKeyEntry.me_value: Ni reads 8 bytes");
_Static_assert(__builtin_classify_type(((ni_dictentry_t *)0)->me_value) == 5,
               "Ni: PyDictKeyEntry.me_value: Ni reads it as a pointer");

typedef int8_t byte_t;
typedef int16_t word_t;
typedef int32_t dword_t;
typedef int64_t qword_t;

#if INTPTR_MAX == INT32_MAX
    typedef dword_t stackitem_t;
#elif INTPTR_MAX == INT64_MAX
    typedef int64_t qword_t;
    typedef qword_t stackitem_t;
#else
    #error "Environment not 32 or 64-bit."
#endif

/*****************************************************************/
/***   Various customizable parameters (use your compilers'    ***/
/***   option to override them, e.g. -DXXX=value in gcc)       ***/
/* set to 0 to disable all debugging checks and output */
#ifndef NI_TRACE
#define NI_TRACE 0
#endif

/* define to 1 to count code size per instruction */
#ifndef NI_CODESTATS
#define NI_CODESTATS 0
#endif

/* define to 1 for extra assert()'s */
#ifndef ALL_CHECKS
#define ALL_CHECKS 0
#endif

/*  define to inline the most common functions in the produced code
    (should be enabled unless you want to trade code size for speed) */
#ifndef INLINE_COMMON_FUNCTIONS
# define INLINE_COMMON_FUNCTIONS 1
#endif

/*****************************************************************/
/* Room in the code being written (codemanager.c). At each instruction a
   compile has at least BUFFER_MARGIN bytes before its limit, or it pauses
   there instead (pycompiler.c, vcompiler.c:psyco_compile): more than 128,
   because of how conditional jumps are emitted (pycompiler.c). Past the
   limit there are GUARANTEED_MINIMUM bytes more, for what is written
   before the next END_CODE, which then goes on in a new extent after a
   jump (codemanager.c:psyco_code_continue). */
#ifndef BUFFER_MARGIN
# define BUFFER_MARGIN 1024
#endif
#ifndef GUARANTEED_MINIMUM
# define GUARANTEED_MINIMUM 64
#endif


#define EXTERNVAR   extern
#define EXTERNFN
#define DEFINEVAR
#define DEFINEFN
#define INITIALIZATIONFN  DEFINEFN

#define psyco_assert(x) ((void)((x) || psyco_fatal_msg("assertion failed")))
#define psyco_fatal_msg(msg)  psyco_fatal_error(msg, __FILE__, __LINE__)
EXTERNFN int psyco_fatal_error(char* msg, char* filename, int lineno);

#if ALL_CHECKS
# define MALLOC_CHECK_    2  /* GCC malloc() checks */
# define extra_assert(x)  psyco_assert(x)
#else
# define extra_assert(x)  (void)0  /* nothing */
#endif

/* Under AddressSanitizer (setup.py NI_SANITIZE), Ni's own allocators mark
   what they hold free as unaddressable, so that an access through a stale
   pointer is reported as malloc'd memory would be; and LeakSanitizer is
   told to scan executable memory, where stub and pause records point to
   what they hold (NI_LSAN_ROOT) */
#if defined(__SANITIZE_ADDRESS__)
# include <sanitizer/asan_interface.h>
# include <sanitizer/lsan_interface.h>
# define NI_POISON(p, n)    ASAN_POISON_MEMORY_REGION(p, n)
# define NI_UNPOISON(p, n)  ASAN_UNPOISON_MEMORY_REGION(p, n)
# define NI_LSAN_ROOT(p, n) __lsan_register_root_region(p, n)
#else
# define NI_POISON(p, n)    ((void)0)
# define NI_UNPOISON(p, n)  ((void)0)
# define NI_LSAN_ROOT(p, n) ((void)0)
#endif

#if INLINE_COMMON_FUNCTIONS
# define PSY_INLINE	__inline static
#else
# define PSY_INLINE	static
#endif

#ifndef bool
typedef int bool;
#endif
#ifndef false
# define false   0
#endif
#ifndef true
# define true    1
#endif

#ifndef PyObject_TypeCheck
# define PyObject_TypeCheck(o,t)   ((o)->ob_type == (t))
#endif


typedef unsigned char code_t;

typedef struct vinfo_s vinfo_t;             /* defined in compiler.h */
typedef struct vinfo_array_s vinfo_array_t; /* defined in compiler.h */
typedef struct PsycoObject_s PsycoObject;   /* defined in compiler.h */
typedef struct FrozenPsycoObject_s FrozenPsycoObject; /* def in dispatcher.h */
typedef struct CodeBufferObject_s CodeBufferObject;  /* def in codemanager.h */
typedef struct global_entries_s global_entries_t;  /* def in dispatcher.h */
typedef struct mergepoint_s mergepoint_t;   /* defined in mergepoint.h */
typedef struct stack_frame_info_s stack_frame_info_t; /* def in pycompiler.h */

EXTERNVAR PyObject* PyExc_PsycoError;
EXTERNVAR PyObject* CPsycoModule;

/* Everything the compiled code of one code object holds (codemanager.c),
   released with that code object. */
typedef struct ni_owner_s {
  struct { code_t *start, *end; } *extents; /* its machine code */
  int nextents, extentscap;
  CodeBufferObject **bufs;          /* every code buffer made for it */
  int nbufs, bufscap;
  PyObject **pins;                  /* a hash set of owned references */
  size_t npins, pinmask;
  struct { void (*fn)(void *); void *closure; } *releases;
  int nreleases, releasescap;       /* its records holding something */
  void *blocks;                     /* records freed with the owner */
  code_t *stub_pool, *stub_pool_end;
  size_t stub_chunk;                /* the size of stub_pool's chunk */
  int *notes;                       /* its call sites' psyco_locals_maps */
  int nnotes, notescap;             /* indices, reused once it dies */
} ni_owner_t;

/* What Ni keeps per code object, on its weak references (ni.c): found by
   identity, not by value as a dict keyed by code objects would be (equal
   code objects compare co_consts recursively). */
typedef struct ni_code_extra_s {
  PyObject* merge_points;  /* capsule, Py_None if unsupported, NULL if not
                              built yet; owned */
  PyObject* entry_point;   /* codebuf, Py_None if compiling failed, NULL if
                              not tried; owned */
  PyObject* entry_globals; /* the globals it was compiled for: borrowed, so
                              maybe dead and reused (vcompiler.c says why
                              that is safe) */
  PyObject* entry_builtins; /* and its f_builtins: borrowed,
                               the compiled code owns it */
  long serial;             /* never reused, unlike the code object's
                              address (psyfunc.c:psyco_save_inline_po) */
  ni_owner_t owner;        /* what its compiled code holds */
  char counted;            /* in the owner scan's count (codemanager.c) */
} ni_code_extra_t;
/* the record for 'co', made on first use; NULL with an exception set */
EXTERNFN ni_code_extra_t* ni_code_extra(PyCodeObject* co);
/* the record for 'co' if it has one, never raising */
EXTERNFN ni_code_extra_t* ni_code_extra_get(PyCodeObject* co);
/* releases the record and its compiled code; its code object is dead */
EXTERNFN void ni_code_extra_free(ni_code_extra_t* x);
/* takes the record off 'co', unfreed, and returns it */
EXTERNFN ni_code_extra_t* ni_code_extra_detach(PyCodeObject* co);

/* moved here from vcompiler.h because needed by numerous header files.
   See vcompiler.h for comments */
typedef bool (*compute_fn_t)(PsycoObject* po, vinfo_t* vi);
typedef PyObject* (*direct_compute_fn_t)(vinfo_t* vi, char* data);
typedef struct {
  compute_fn_t compute_fn;
  direct_compute_fn_t direct_compute_fn;
  long pyobject_mask;
  signed char nested_weight[2];
} source_virtual_t;


#define psyco_inc_stackdepth(po) do {\
    po->stack_depth += sizeof(stackitem_t);\
} while(0)

#define psyco_dec_stackdepth(po) do {\
    po->stack_depth -= sizeof(stackitem_t);\
} while(0)


/* to display code object names */
#define PyCodeObject_NAME(co)   (co->co_name ? PyUnicode_AsUTF8(co->co_name)  \
                                 : "<anonymous code object>")


/* defined in pycompiler.c */
#define GLOBAL_ENTRY_POINT	psyco_pycompiler_mainloop
EXTERNFN code_t* psyco_pycompiler_mainloop(PsycoObject* po);


/* Mid-compile this abandons the compile (codemanager.h, compile guards);
   anywhere else it aborts. It never returns. */
EXTERNFN void psyco_out_of_memory(char *filename, int lineno);
#define OUT_OF_MEMORY()      psyco_out_of_memory(__FILE__, __LINE__)

/* For tests: NI_FAIL(site) is true at the n-th time it is reached after
   ni._fail_after(site, n), as if that allocation had failed. */
enum { NI_FAIL_PSYCOOBJECT, NI_FAIL_ARRAY, NI_FAIL_CODEBUF, NI_FAIL_LOOKUP,
       NI_FAIL_OWNER, NI_FAIL_SITES };
#if ALL_CHECKS
EXTERNVAR long psyco_fail_after[NI_FAIL_SITES];
#define NI_FAIL(site)                                                          \
  (psyco_fail_after[site] > 0 && --psyco_fail_after[site] == 0)
#else
#define NI_FAIL(site) 0
#endif

/* Getting data from the _psyco module */
EXTERNFN PyObject* need_cpsyco_obj(char* name);

/* defined in dispatcher.c */
EXTERNFN void PsycoObject_EmergencyCodeRoom(PsycoObject* po);

#if NI_TRACE
/* Calls hooked by nianalyser to collect compiler behaviour */
EXTERNFN void ni_trace_begin_simple_code(code_t *code);
EXTERNFN void ni_trace_end_simple_code(code_t *code);
EXTERNFN void ni_trace_begin_code(PsycoObject* po);
EXTERNFN void ni_trace_end_code(PsycoObject* po);
EXTERNFN void ni_trace_jump(code_t *location, code_t *target);
EXTERNFN void ni_trace_jump_update(code_t *location, code_t *target);
EXTERNFN void ni_trace_jump_reg(code_t *location, int reg_target);
EXTERNFN void ni_trace_jump_cond(code_t *location, code_t *not_taken, code_t *taken);
EXTERNFN void ni_trace_jump_cond_update(code_t *location, code_t *not_taken, code_t *taken);
EXTERNFN void ni_trace_jump_cond_reg(code_t *location, code_t *not_taken, int reg_taken);
EXTERNFN void ni_trace_call(code_t *location, code_t *call_target);
EXTERNFN void ni_trace_call_reg(code_t *location, int reg_call_target);
EXTERNFN void ni_trace_return(code_t *location, int stack_adjust);
EXTERNFN void ni_trace_unsupported_opcode(PyCodeObject *co, int bytecode_index);
EXTERNFN void ni_trace_run_fail(PyCodeObject *co, char *reason);
EXTERNFN void ni_trace_unify(PsycoObject* po, CodeBufferObject *match);
#else
#define ni_trace_begin_simple_code(code) do {} while(0)
#define ni_trace_end_simple_code(code) do {} while(0)
#define ni_trace_begin_code(po) do {} while(0)
#define ni_trace_end_code(po) do {} while(0)
#define ni_trace_jump(location, target) do {} while(0)
#define ni_trace_jump_update(location, target) do {} while(0)
#define ni_trace_jump_reg(location, target) do {} while(0)
#define ni_trace_jump_cond(location, not_taken, taken) do {} while(0)
#define ni_trace_jump_cond_update(location, not_taken, taken) do {} while(0)
#define ni_trace_jump_cond_reg(location, not_taken, taken) do {} while(0)
#define ni_trace_call(location, call_target) do {} while(0)
#define ni_trace_call_reg(location, reg_call_target) do {} while(0)
#define ni_trace_return(location, stack_adjust) do {} while(0)
#define ni_trace_unsupported_opcode(co, bytecode_index) do {} while(0)
#define ni_trace_run_fail(co, reason) do {} while(0)
#define ni_trace_unify(po, match) do {} while (0)
#endif

#define BEGIN_CODE do {\
    code_t* code = po->code;\
    ni_trace_begin_code(po);
#define END_CODE\
    po->code = code;\
    ni_trace_end_code(po);\
    if (code >= po->codelimit) {\
        PsycoObject_EmergencyCodeRoom(po);\
    }\
} while(0);

#endif /* _PSYCO_H */
