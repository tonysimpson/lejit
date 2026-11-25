/***************************************************************/
/***                 Global Psyco definitions                ***/
/***************************************************************/

#ifndef _PSYCO_H
#define _PSYCO_H

#include <Python.h>
#include <structmember.h> /* for offsetof() */
#include <stdint.h>
/* C's own bool, which CPython's internal headers give too: a function
   returning one must read the same in every file */
#include <stdbool.h>
/* the LE_HAS_ features of the CPython source built for */
#include "le_features_gen.h"
/* the messages (LE_MSG_) of the interpreter built for, from
   tools/features/strings.py */
#include "le_strings_gen.h"

/* sys.modules, borrowed, or NULL (with no exception) once finalisation has
   cleared it: PyImport_GetModuleDict is then a fatal error, and LeJIT
   compiles and collects while finalisers run */
static inline PyObject *
le_modules_dict(void)
{
    PyObject *t, *v, *tb, *m;
    PyErr_Fetch(&t, &v, &tb);
    m = PySys_GetObject("modules");
    PyErr_Restore(t, v, tb);
    return m != NULL && PyDict_Check(m) ? m : NULL;
}
/* offsets, sizes and private structs of the CPython fields LeJIT relies on
   (vcompiler.h:CPY_FIELD), each checked at compile time against the
   field's type */
#include <stddef.h>

#if LE_HAS_REFCOUNT_32
#define LE_OFS_PyObject__ob_refcnt offsetof(PyObject, ob_refcnt)
#define LE_SIZE2_PyObject__ob_refcnt 2
#define LE_UNSIGNED_PyObject__ob_refcnt 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyObject *)0)->ob_refcnt), uint32_t),
               "LeJIT: PyObject.ob_refcnt is not uint32_t");
_Static_assert(sizeof(((PyObject *)0)->ob_refcnt) == 4,
               "LeJIT: PyObject.ob_refcnt: LeJIT reads 4 bytes");
_Static_assert(__builtin_classify_type(((PyObject *)0)->ob_refcnt) == 1,
               "LeJIT: PyObject.ob_refcnt: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((PyObject *)0)->ob_refcnt))-1 < 0) == 0,
               "LeJIT: PyObject.ob_refcnt: LeJIT reads it as unsigned");
#else
#define LE_OFS_PyObject__ob_refcnt offsetof(PyObject, ob_refcnt)
#define LE_SIZE2_PyObject__ob_refcnt 3
#define LE_UNSIGNED_PyObject__ob_refcnt 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyObject *)0)->ob_refcnt), Py_ssize_t),
               "LeJIT: PyObject.ob_refcnt is not Py_ssize_t");
_Static_assert(sizeof(((PyObject *)0)->ob_refcnt) == 8,
               "LeJIT: PyObject.ob_refcnt: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyObject *)0)->ob_refcnt) == 1,
               "LeJIT: PyObject.ob_refcnt: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((PyObject *)0)->ob_refcnt))-1 < 0) == 1,
               "LeJIT: PyObject.ob_refcnt: LeJIT reads it as signed");
#endif
#define LE_OFS_PyObject__ob_type offsetof(PyObject, ob_type)
#define LE_SIZE2_PyObject__ob_type 3
#define LE_UNSIGNED_PyObject__ob_type 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyObject *)0)->ob_type), struct _typeobject *),
               "LeJIT: PyObject.ob_type is not struct _typeobject *");
_Static_assert(sizeof(((PyObject *)0)->ob_type) == 8,
               "LeJIT: PyObject.ob_type: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyObject *)0)->ob_type) == 5,
               "LeJIT: PyObject.ob_type: LeJIT reads it as a pointer");

#define LE_OFS_PyVarObject__ob_size offsetof(PyVarObject, ob_size)
#define LE_SIZE2_PyVarObject__ob_size 3
#define LE_UNSIGNED_PyVarObject__ob_size 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyVarObject *)0)->ob_size), Py_ssize_t),
               "LeJIT: PyVarObject.ob_size is not Py_ssize_t");
_Static_assert(sizeof(((PyVarObject *)0)->ob_size) == 8,
               "LeJIT: PyVarObject.ob_size: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyVarObject *)0)->ob_size) == 1,
               "LeJIT: PyVarObject.ob_size: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((PyVarObject *)0)->ob_size))-1 < 0) == 1,
               "LeJIT: PyVarObject.ob_size: LeJIT reads it as signed");

#define LE_OFS_PyTypeObject__tp_name offsetof(PyTypeObject, tp_name)
#define LE_SIZE2_PyTypeObject__tp_name 3
#define LE_UNSIGNED_PyTypeObject__tp_name 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTypeObject *)0)->tp_name), const char *),
               "LeJIT: PyTypeObject.tp_name is not const char *");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_name) == 8,
               "LeJIT: PyTypeObject.tp_name: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyTypeObject *)0)->tp_name) == 5,
               "LeJIT: PyTypeObject.tp_name: LeJIT reads it as a pointer");
#define LE_OFS_PyTypeObject__tp_dealloc offsetof(PyTypeObject, tp_dealloc)
#define LE_SIZE2_PyTypeObject__tp_dealloc 3
#define LE_UNSIGNED_PyTypeObject__tp_dealloc 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTypeObject *)0)->tp_dealloc), destructor),
               "LeJIT: PyTypeObject.tp_dealloc is not destructor");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_dealloc) == 8,
               "LeJIT: PyTypeObject.tp_dealloc: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyTypeObject *)0)->tp_dealloc) == 5,
               "LeJIT: PyTypeObject.tp_dealloc: LeJIT reads it as a pointer");
#define LE_OFS_PyTypeObject__tp_flags offsetof(PyTypeObject, tp_flags)
#define LE_SIZE2_PyTypeObject__tp_flags 3
#define LE_UNSIGNED_PyTypeObject__tp_flags 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTypeObject *)0)->tp_flags), unsigned long),
               "LeJIT: PyTypeObject.tp_flags is not unsigned long");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_flags) == 8,
               "LeJIT: PyTypeObject.tp_flags: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyTypeObject *)0)->tp_flags) == 1,
               "LeJIT: PyTypeObject.tp_flags: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((PyTypeObject *)0)->tp_flags))-1 < 0) == 0,
               "LeJIT: PyTypeObject.tp_flags: LeJIT reads it as unsigned");
#define LE_OFS_PyTypeObject__tp_descr_get offsetof(PyTypeObject, tp_descr_get)
#define LE_SIZE2_PyTypeObject__tp_descr_get 3
#define LE_UNSIGNED_PyTypeObject__tp_descr_get 0
_Static_assert(
    __builtin_types_compatible_p(__typeof__(((PyTypeObject *)0)->tp_descr_get),
                                 descrgetfunc),
    "LeJIT: PyTypeObject.tp_descr_get is not descrgetfunc");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_descr_get) == 8,
               "LeJIT: PyTypeObject.tp_descr_get: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyTypeObject *)0)->tp_descr_get) == 5,
    "LeJIT: PyTypeObject.tp_descr_get: LeJIT reads it as a pointer");
#define LE_OFS_PyTypeObject__tp_version_tag \
    offsetof(PyTypeObject, tp_version_tag)
#define LE_SIZE2_PyTypeObject__tp_version_tag 2
#define LE_UNSIGNED_PyTypeObject__tp_version_tag 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTypeObject *)0)->tp_version_tag),
                   unsigned int),
               "LeJIT: PyTypeObject.tp_version_tag is not unsigned int");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_version_tag) == 4,
               "LeJIT: PyTypeObject.tp_version_tag: LeJIT reads 4 bytes");
_Static_assert(
    __builtin_classify_type(((PyTypeObject *)0)->tp_version_tag) == 1,
    "LeJIT: PyTypeObject.tp_version_tag: LeJIT reads it as an integer");
_Static_assert(
    ((__typeof__(((PyTypeObject *)0)->tp_version_tag))-1 < 0) == 0,
    "LeJIT: PyTypeObject.tp_version_tag: LeJIT reads it as unsigned");

#define LE_OFS_PyFloatObject__ob_fval offsetof(PyFloatObject, ob_fval)
#define LE_SIZE2_PyFloatObject__ob_fval 3
#define LE_UNSIGNED_PyFloatObject__ob_fval 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFloatObject *)0)->ob_fval), double),
               "LeJIT: PyFloatObject.ob_fval is not double");
_Static_assert(sizeof(((PyFloatObject *)0)->ob_fval) == 8,
               "LeJIT: PyFloatObject.ob_fval: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyFloatObject *)0)->ob_fval) == 8,
               "LeJIT: PyFloatObject.ob_fval: LeJIT reads it as floating");

#if LE_HAS_LONG_VALUE
/* the digits are long_value's; its lv_tag, where ob_size was, holds the
   digit count and the sign as flags (plongobject.h:LONG_lv_tag) */
#define LE_PyLongObject_digits long_value.ob_digit
/* an int's signed digit count, as ob_size was */
static inline Py_ssize_t
le_long_size(PyObject *o)
{
    uintptr_t tag = ((PyLongObject *)o)->long_value.lv_tag;
    return (Py_ssize_t)(1 - (tag & 3)) * (Py_ssize_t)(tag >> 3);
}
#else
#define LE_PyLongObject_digits ob_digit
#define le_long_size(o) Py_SIZE(o)
#endif
#define LE_OFS_PyLongObject__ob_digit \
    offsetof(PyLongObject, LE_PyLongObject_digits)
#define LE_SIZE2_PyLongObject__ob_digit 2
#define LE_UNSIGNED_PyLongObject__ob_digit 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyLongObject *)0)->LE_PyLongObject_digits),
                   digit[1]),
               "LeJIT: PyLongObject.ob_digit is not digit[1]");
_Static_assert(sizeof(((PyLongObject *)0)->LE_PyLongObject_digits[0]) == 4,
               "LeJIT: PyLongObject.ob_digit: LeJIT reads 4 bytes");
_Static_assert(__builtin_classify_type(
                   ((PyLongObject *)0)->LE_PyLongObject_digits[0]) == 1,
               "LeJIT: PyLongObject.ob_digit: LeJIT reads it as an integer");
_Static_assert(
    ((__typeof__(((PyLongObject *)0)->LE_PyLongObject_digits[0]))-1 < 0) == 0,
    "LeJIT: PyLongObject.ob_digit: LeJIT reads it as unsigned");

#define LE_OFS_PyListObject__ob_item offsetof(PyListObject, ob_item)
#define LE_SIZE2_PyListObject__ob_item 3
#define LE_UNSIGNED_PyListObject__ob_item 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyListObject *)0)->ob_item), PyObject **),
               "LeJIT: PyListObject.ob_item is not PyObject **");
_Static_assert(sizeof(((PyListObject *)0)->ob_item) == 8,
               "LeJIT: PyListObject.ob_item: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyListObject *)0)->ob_item) == 5,
               "LeJIT: PyListObject.ob_item: LeJIT reads it as a pointer");

#define LE_OFS_PyTupleObject__ob_item offsetof(PyTupleObject, ob_item)
#define LE_SIZE2_PyTupleObject__ob_item 3
#define LE_UNSIGNED_PyTupleObject__ob_item 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTupleObject *)0)->ob_item), PyObject *[1]),
               "LeJIT: PyTupleObject.ob_item is not PyObject *[1]");
_Static_assert(sizeof(((PyTupleObject *)0)->ob_item[0]) == 8,
               "LeJIT: PyTupleObject.ob_item: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyTupleObject *)0)->ob_item[0]) == 5,
               "LeJIT: PyTupleObject.ob_item: LeJIT reads it as a pointer");

#define LE_OFS_PyBytesObject__ob_sval offsetof(PyBytesObject, ob_sval)
#define LE_SIZE2_PyBytesObject__ob_sval 0
#define LE_UNSIGNED_PyBytesObject__ob_sval 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyBytesObject *)0)->ob_sval), char[1]),
               "LeJIT: PyBytesObject.ob_sval is not char[1]");
_Static_assert(sizeof(((PyBytesObject *)0)->ob_sval[0]) == 1,
               "LeJIT: PyBytesObject.ob_sval: LeJIT reads 1 byte");

#define LE_OFS_PyDictObject__ma_used offsetof(PyDictObject, ma_used)
#define LE_SIZE2_PyDictObject__ma_used 2
#define LE_UNSIGNED_PyDictObject__ma_used 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyDictObject *)0)->ma_used), Py_ssize_t),
               "LeJIT: PyDictObject.ma_used is not Py_ssize_t");
_Static_assert(sizeof(((PyDictObject *)0)->ma_used) >= 4,
               "LeJIT: PyDictObject.ma_used: LeJIT reads 4 bytes");
/* a dict's version tag, whose low 8 bits say which watchers watch it where
   LE_HAS_DICT_WATCHER_BITS. A dict with no version tag has the watcher
   bits alone, in _ma_watcher_tag: nothing tells its changes, so no code
   is compiled (mergepoints.c:_build_merge_points). */
#if LE_HAS_DICT_VERSION_TAG
#define LE_DICT_TAG_FIELD ma_version_tag
#else
#define LE_DICT_TAG_FIELD _ma_watcher_tag
#endif
#define LE_OFS_PyDictObject__ma_version_tag \
    offsetof(PyDictObject, LE_DICT_TAG_FIELD)
#define LE_SIZE2_PyDictObject__ma_version_tag 3
#define LE_UNSIGNED_PyDictObject__ma_version_tag 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyDictObject *)0)->LE_DICT_TAG_FIELD),
                   uint64_t),
               "LeJIT: PyDictObject.ma_version_tag is not uint64_t");
_Static_assert(sizeof(((PyDictObject *)0)->LE_DICT_TAG_FIELD) == 8,
               "LeJIT: PyDictObject.ma_version_tag: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyDictObject *)0)->LE_DICT_TAG_FIELD) == 1,
    "LeJIT: PyDictObject.ma_version_tag: LeJIT reads it as an integer");
_Static_assert(
    ((__typeof__(((PyDictObject *)0)->LE_DICT_TAG_FIELD))-1 < 0) == 0,
    "LeJIT: PyDictObject.ma_version_tag: LeJIT reads it as unsigned");
#define LE_OFS_PyDictObject__ma_keys offsetof(PyDictObject, ma_keys)
#define LE_SIZE2_PyDictObject__ma_keys 3
#define LE_UNSIGNED_PyDictObject__ma_keys 0
_Static_assert(
    __builtin_types_compatible_p(__typeof__(((PyDictObject *)0)->ma_keys),
                                 PyDictKeysObject *),
    "LeJIT: PyDictObject.ma_keys is not PyDictKeysObject *");
_Static_assert(sizeof(((PyDictObject *)0)->ma_keys) == 8,
               "LeJIT: PyDictObject.ma_keys: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyDictObject *)0)->ma_keys) == 5,
               "LeJIT: PyDictObject.ma_keys: LeJIT reads it as a pointer");
#define LE_OFS_PyDictObject__ma_values offsetof(PyDictObject, ma_values)
#define LE_SIZE2_PyDictObject__ma_values 3
#define LE_UNSIGNED_PyDictObject__ma_values 0
#if LE_HAS_DICT_VALUES_STRUCT
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyDictObject *)0)->ma_values), PyDictValues *),
               "LeJIT: PyDictObject.ma_values is not PyDictValues *");
/* a split dict's values: the PyDictValues' only field, its array */
#define le_dict_values(d) ((PyObject **)(d)->ma_values)
#else
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyDictObject *)0)->ma_values), PyObject **),
               "LeJIT: PyDictObject.ma_values is not PyObject **");
/* a split dict's values */
#define le_dict_values(d) ((d)->ma_values)
#endif
_Static_assert(sizeof(((PyDictObject *)0)->ma_values) == 8,
               "LeJIT: PyDictObject.ma_values: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyDictObject *)0)->ma_values) == 5,
               "LeJIT: PyDictObject.ma_values: LeJIT reads it as a pointer");

#define LE_OFS_PyASCIIObject__length offsetof(PyASCIIObject, length)
#define LE_SIZE2_PyASCIIObject__length 3
#define LE_UNSIGNED_PyASCIIObject__length 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyASCIIObject *)0)->length), Py_ssize_t),
               "LeJIT: PyASCIIObject.length is not Py_ssize_t");
_Static_assert(sizeof(((PyASCIIObject *)0)->length) == 8,
               "LeJIT: PyASCIIObject.length: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyASCIIObject *)0)->length) == 1,
               "LeJIT: PyASCIIObject.length: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((PyASCIIObject *)0)->length))-1 < 0) == 1,
               "LeJIT: PyASCIIObject.length: LeJIT reads it as signed");
#define LE_OFS_PyASCIIObject__hash offsetof(PyASCIIObject, hash)
#define LE_SIZE2_PyASCIIObject__hash 3
#define LE_UNSIGNED_PyASCIIObject__hash 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyASCIIObject *)0)->hash), Py_hash_t),
               "LeJIT: PyASCIIObject.hash is not Py_hash_t");
_Static_assert(sizeof(((PyASCIIObject *)0)->hash) == 8,
               "LeJIT: PyASCIIObject.hash: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyASCIIObject *)0)->hash) == 1,
               "LeJIT: PyASCIIObject.hash: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((PyASCIIObject *)0)->hash))-1 < 0) == 1,
               "LeJIT: PyASCIIObject.hash: LeJIT reads it as signed");
#define LE_OFS_PyASCIIObject__state offsetof(PyASCIIObject, state)
#define LE_SIZE2_PyASCIIObject__state 0
#define LE_UNSIGNED_PyASCIIObject__state 1
_Static_assert(sizeof(((PyASCIIObject *)0)->state) >= 1,
               "LeJIT: PyASCIIObject.state: LeJIT reads 1 byte");
#define LE_BIT_PyASCIIObject__state__interned 0x3
#define LE_BIT_PyASCIIObject__state__kind 0x1C
#define LE_BIT_PyASCIIObject__state__compact 0x20
#define LE_BIT_PyASCIIObject__state__ascii 0x40
#define LE_BIT_PyASCIIObject__state__ready 0x80

#define LE_OFS_PySliceObject__start offsetof(PySliceObject, start)
#define LE_SIZE2_PySliceObject__start 3
#define LE_UNSIGNED_PySliceObject__start 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PySliceObject *)0)->start), PyObject *),
               "LeJIT: PySliceObject.start is not PyObject *");
_Static_assert(sizeof(((PySliceObject *)0)->start) == 8,
               "LeJIT: PySliceObject.start: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PySliceObject *)0)->start) == 5,
               "LeJIT: PySliceObject.start: LeJIT reads it as a pointer");
#define LE_OFS_PySliceObject__stop offsetof(PySliceObject, stop)
#define LE_SIZE2_PySliceObject__stop 3
#define LE_UNSIGNED_PySliceObject__stop 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PySliceObject *)0)->stop), PyObject *),
               "LeJIT: PySliceObject.stop is not PyObject *");
_Static_assert(sizeof(((PySliceObject *)0)->stop) == 8,
               "LeJIT: PySliceObject.stop: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PySliceObject *)0)->stop) == 5,
               "LeJIT: PySliceObject.stop: LeJIT reads it as a pointer");
#define LE_OFS_PySliceObject__step offsetof(PySliceObject, step)
#define LE_SIZE2_PySliceObject__step 3
#define LE_UNSIGNED_PySliceObject__step 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PySliceObject *)0)->step), PyObject *),
               "LeJIT: PySliceObject.step is not PyObject *");
_Static_assert(sizeof(((PySliceObject *)0)->step) == 8,
               "LeJIT: PySliceObject.step: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PySliceObject *)0)->step) == 5,
               "LeJIT: PySliceObject.step: LeJIT reads it as a pointer");

#define LE_OFS_PyFunctionObject__func_code \
    offsetof(PyFunctionObject, func_code)
#define LE_SIZE2_PyFunctionObject__func_code 3
#define LE_UNSIGNED_PyFunctionObject__func_code 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_code), PyObject *),
               "LeJIT: PyFunctionObject.func_code is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_code) == 8,
               "LeJIT: PyFunctionObject.func_code: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_code) == 5,
    "LeJIT: PyFunctionObject.func_code: LeJIT reads it as a pointer");
#define LE_OFS_PyFunctionObject__func_globals \
    offsetof(PyFunctionObject, func_globals)
#define LE_SIZE2_PyFunctionObject__func_globals 3
#define LE_UNSIGNED_PyFunctionObject__func_globals 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_globals),
                   PyObject *),
               "LeJIT: PyFunctionObject.func_globals is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_globals) == 8,
               "LeJIT: PyFunctionObject.func_globals: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_globals) == 5,
    "LeJIT: PyFunctionObject.func_globals: LeJIT reads it as a pointer");
#define LE_OFS_PyFunctionObject__func_defaults \
    offsetof(PyFunctionObject, func_defaults)
#define LE_SIZE2_PyFunctionObject__func_defaults 3
#define LE_UNSIGNED_PyFunctionObject__func_defaults 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_defaults),
                   PyObject *),
               "LeJIT: PyFunctionObject.func_defaults is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_defaults) == 8,
               "LeJIT: PyFunctionObject.func_defaults: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_defaults) == 5,
    "LeJIT: PyFunctionObject.func_defaults: LeJIT reads it as a pointer");
#define LE_OFS_PyFunctionObject__func_kwdefaults \
    offsetof(PyFunctionObject, func_kwdefaults)
#define LE_SIZE2_PyFunctionObject__func_kwdefaults 3
#define LE_UNSIGNED_PyFunctionObject__func_kwdefaults 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_kwdefaults),
                   PyObject *),
               "LeJIT: PyFunctionObject.func_kwdefaults is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_kwdefaults) == 8,
               "LeJIT: PyFunctionObject.func_kwdefaults: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_kwdefaults) == 5,
    "LeJIT: PyFunctionObject.func_kwdefaults: LeJIT reads it as a pointer");
#define LE_OFS_PyFunctionObject__func_closure \
    offsetof(PyFunctionObject, func_closure)
#define LE_SIZE2_PyFunctionObject__func_closure 3
#define LE_UNSIGNED_PyFunctionObject__func_closure 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_closure),
                   PyObject *),
               "LeJIT: PyFunctionObject.func_closure is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_closure) == 8,
               "LeJIT: PyFunctionObject.func_closure: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_closure) == 5,
    "LeJIT: PyFunctionObject.func_closure: LeJIT reads it as a pointer");
#define LE_OFS_PyFunctionObject__func_annotations \
    offsetof(PyFunctionObject, func_annotations)
#define LE_SIZE2_PyFunctionObject__func_annotations 3
#define LE_UNSIGNED_PyFunctionObject__func_annotations 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_annotations),
                   PyObject *),
               "LeJIT: PyFunctionObject.func_annotations is not PyObject *");
_Static_assert(
    sizeof(((PyFunctionObject *)0)->func_annotations) == 8,
    "LeJIT: PyFunctionObject.func_annotations: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_annotations) == 5,
    "LeJIT: PyFunctionObject.func_annotations: LeJIT reads it as a pointer");
#define LE_OFS_PyFunctionObject__func_qualname \
    offsetof(PyFunctionObject, func_qualname)
#define LE_SIZE2_PyFunctionObject__func_qualname 3
#define LE_UNSIGNED_PyFunctionObject__func_qualname 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_qualname),
                   PyObject *),
               "LeJIT: PyFunctionObject.func_qualname is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_qualname) == 8,
               "LeJIT: PyFunctionObject.func_qualname: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_qualname) == 5,
    "LeJIT: PyFunctionObject.func_qualname: LeJIT reads it as a pointer");

#define LE_OFS_PyMethodObject__im_func offsetof(PyMethodObject, im_func)
#define LE_SIZE2_PyMethodObject__im_func 3
#define LE_UNSIGNED_PyMethodObject__im_func 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyMethodObject *)0)->im_func), PyObject *),
               "LeJIT: PyMethodObject.im_func is not PyObject *");
_Static_assert(sizeof(((PyMethodObject *)0)->im_func) == 8,
               "LeJIT: PyMethodObject.im_func: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyMethodObject *)0)->im_func) == 5,
               "LeJIT: PyMethodObject.im_func: LeJIT reads it as a pointer");
#define LE_OFS_PyMethodObject__im_self offsetof(PyMethodObject, im_self)
#define LE_SIZE2_PyMethodObject__im_self 3
#define LE_UNSIGNED_PyMethodObject__im_self 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyMethodObject *)0)->im_self), PyObject *),
               "LeJIT: PyMethodObject.im_self is not PyObject *");
_Static_assert(sizeof(((PyMethodObject *)0)->im_self) == 8,
               "LeJIT: PyMethodObject.im_self: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyMethodObject *)0)->im_self) == 5,
               "LeJIT: PyMethodObject.im_self: LeJIT reads it as a pointer");

#define LE_OFS_PyCFunctionObject__m_ml offsetof(PyCFunctionObject, m_ml)
#define LE_SIZE2_PyCFunctionObject__m_ml 3
#define LE_UNSIGNED_PyCFunctionObject__m_ml 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyCFunctionObject *)0)->m_ml), PyMethodDef *),
               "LeJIT: PyCFunctionObject.m_ml is not PyMethodDef *");
_Static_assert(sizeof(((PyCFunctionObject *)0)->m_ml) == 8,
               "LeJIT: PyCFunctionObject.m_ml: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyCFunctionObject *)0)->m_ml) == 5,
               "LeJIT: PyCFunctionObject.m_ml: LeJIT reads it as a pointer");
#define LE_OFS_PyCFunctionObject__m_self offsetof(PyCFunctionObject, m_self)
#define LE_SIZE2_PyCFunctionObject__m_self 3
#define LE_UNSIGNED_PyCFunctionObject__m_self 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyCFunctionObject *)0)->m_self), PyObject *),
               "LeJIT: PyCFunctionObject.m_self is not PyObject *");
_Static_assert(sizeof(((PyCFunctionObject *)0)->m_self) == 8,
               "LeJIT: PyCFunctionObject.m_self: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyCFunctionObject *)0)->m_self) == 5,
               "LeJIT: PyCFunctionObject.m_self: LeJIT reads it as a pointer");

#define LE_OFS_PyCellObject__ob_ref offsetof(PyCellObject, ob_ref)
#define LE_SIZE2_PyCellObject__ob_ref 3
#define LE_UNSIGNED_PyCellObject__ob_ref 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyCellObject *)0)->ob_ref), PyObject *),
               "LeJIT: PyCellObject.ob_ref is not PyObject *");
_Static_assert(sizeof(((PyCellObject *)0)->ob_ref) == 8,
               "LeJIT: PyCellObject.ob_ref: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyCellObject *)0)->ob_ref) == 5,
               "LeJIT: PyCellObject.ob_ref: LeJIT reads it as a pointer");

typedef struct {
    PyObject_HEAD Py_ssize_t it_index;
    PyObject *it_seq;
} seqiterobject;
#define LE_OFS_seqiterobject__it_index offsetof(seqiterobject, it_index)
#define LE_SIZE2_seqiterobject__it_index 3
#define LE_UNSIGNED_seqiterobject__it_index 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((seqiterobject *)0)->it_index), Py_ssize_t),
               "LeJIT: seqiterobject.it_index is not Py_ssize_t");
_Static_assert(sizeof(((seqiterobject *)0)->it_index) == 8,
               "LeJIT: seqiterobject.it_index: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((seqiterobject *)0)->it_index) == 1,
               "LeJIT: seqiterobject.it_index: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((seqiterobject *)0)->it_index))-1 < 0) == 1,
               "LeJIT: seqiterobject.it_index: LeJIT reads it as signed");
#define LE_OFS_seqiterobject__it_seq offsetof(seqiterobject, it_seq)
#define LE_SIZE2_seqiterobject__it_seq 3
#define LE_UNSIGNED_seqiterobject__it_seq 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((seqiterobject *)0)->it_seq), PyObject *),
               "LeJIT: seqiterobject.it_seq is not PyObject *");
_Static_assert(sizeof(((seqiterobject *)0)->it_seq) == 8,
               "LeJIT: seqiterobject.it_seq: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((seqiterobject *)0)->it_seq) == 5,
               "LeJIT: seqiterobject.it_seq: LeJIT reads it as a pointer");

typedef struct {
    PyObject_HEAD PyObject *start;
    PyObject *stop;
    PyObject *step;
    PyObject *length;
} rangeobject;
#define LE_OFS_rangeobject__start offsetof(rangeobject, start)
#define LE_SIZE2_rangeobject__start 3
#define LE_UNSIGNED_rangeobject__start 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeobject *)0)->start), PyObject *),
               "LeJIT: rangeobject.start is not PyObject *");
_Static_assert(sizeof(((rangeobject *)0)->start) == 8,
               "LeJIT: rangeobject.start: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeobject *)0)->start) == 5,
               "LeJIT: rangeobject.start: LeJIT reads it as a pointer");
#define LE_OFS_rangeobject__stop offsetof(rangeobject, stop)
#define LE_SIZE2_rangeobject__stop 3
#define LE_UNSIGNED_rangeobject__stop 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeobject *)0)->stop), PyObject *),
               "LeJIT: rangeobject.stop is not PyObject *");
_Static_assert(sizeof(((rangeobject *)0)->stop) == 8,
               "LeJIT: rangeobject.stop: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeobject *)0)->stop) == 5,
               "LeJIT: rangeobject.stop: LeJIT reads it as a pointer");
#define LE_OFS_rangeobject__step offsetof(rangeobject, step)
#define LE_SIZE2_rangeobject__step 3
#define LE_UNSIGNED_rangeobject__step 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeobject *)0)->step), PyObject *),
               "LeJIT: rangeobject.step is not PyObject *");
_Static_assert(sizeof(((rangeobject *)0)->step) == 8,
               "LeJIT: rangeobject.step: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeobject *)0)->step) == 5,
               "LeJIT: rangeobject.step: LeJIT reads it as a pointer");
#define LE_OFS_rangeobject__length offsetof(rangeobject, length)
#define LE_SIZE2_rangeobject__length 3
#define LE_UNSIGNED_rangeobject__length 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeobject *)0)->length), PyObject *),
               "LeJIT: rangeobject.length is not PyObject *");
_Static_assert(sizeof(((rangeobject *)0)->length) == 8,
               "LeJIT: rangeobject.length: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeobject *)0)->length) == 5,
               "LeJIT: rangeobject.length: LeJIT reads it as a pointer");

#if LE_HAS_RANGE_ITER_INDEX
typedef struct {
    PyObject_HEAD long index;
    long start;
    long step;
    long len;
} rangeiterobject;
#else
/* rangeiter_next moves start on and counts len down */
typedef struct {
    PyObject_HEAD long start;
    long step;
    long len;
} rangeiterobject;
#endif
#if LE_HAS_RANGE_ITER_INDEX
#define LE_OFS_rangeiterobject__index offsetof(rangeiterobject, index)
#define LE_SIZE2_rangeiterobject__index 3
#define LE_UNSIGNED_rangeiterobject__index 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeiterobject *)0)->index), long),
               "LeJIT: rangeiterobject.index is not long");
_Static_assert(sizeof(((rangeiterobject *)0)->index) == 8,
               "LeJIT: rangeiterobject.index: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeiterobject *)0)->index) == 1,
               "LeJIT: rangeiterobject.index: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((rangeiterobject *)0)->index))-1 < 0) == 1,
               "LeJIT: rangeiterobject.index: LeJIT reads it as signed");
#endif
#define LE_OFS_rangeiterobject__start offsetof(rangeiterobject, start)
#define LE_SIZE2_rangeiterobject__start 3
#define LE_UNSIGNED_rangeiterobject__start 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeiterobject *)0)->start), long),
               "LeJIT: rangeiterobject.start is not long");
_Static_assert(sizeof(((rangeiterobject *)0)->start) == 8,
               "LeJIT: rangeiterobject.start: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeiterobject *)0)->start) == 1,
               "LeJIT: rangeiterobject.start: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((rangeiterobject *)0)->start))-1 < 0) == 1,
               "LeJIT: rangeiterobject.start: LeJIT reads it as signed");
#define LE_OFS_rangeiterobject__step offsetof(rangeiterobject, step)
#define LE_SIZE2_rangeiterobject__step 3
#define LE_UNSIGNED_rangeiterobject__step 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeiterobject *)0)->step), long),
               "LeJIT: rangeiterobject.step is not long");
_Static_assert(sizeof(((rangeiterobject *)0)->step) == 8,
               "LeJIT: rangeiterobject.step: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeiterobject *)0)->step) == 1,
               "LeJIT: rangeiterobject.step: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((rangeiterobject *)0)->step))-1 < 0) == 1,
               "LeJIT: rangeiterobject.step: LeJIT reads it as signed");
#define LE_OFS_rangeiterobject__len offsetof(rangeiterobject, len)
#define LE_SIZE2_rangeiterobject__len 3
#define LE_UNSIGNED_rangeiterobject__len 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((rangeiterobject *)0)->len), long),
               "LeJIT: rangeiterobject.len is not long");
_Static_assert(sizeof(((rangeiterobject *)0)->len) == 8,
               "LeJIT: rangeiterobject.len: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((rangeiterobject *)0)->len) == 1,
               "LeJIT: rangeiterobject.len: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((rangeiterobject *)0)->len))-1 < 0) == 1,
               "LeJIT: rangeiterobject.len: LeJIT reads it as signed");

#if LE_HAS_DICT_KEYS_KIND
typedef enum {
    DICT_KEYS_GENERAL = 0,
    DICT_KEYS_UNICODE = 1,
    DICT_KEYS_SPLIT = 2
} DictKeysKind;
struct _dictkeysobject {
    Py_ssize_t dk_refcnt;
    uint8_t dk_log2_size;
    uint8_t dk_log2_index_bytes;
    uint8_t dk_kind;
    uint32_t dk_version;
    Py_ssize_t dk_usable;
    Py_ssize_t dk_nentries;
    char dk_indices[];
};
typedef struct _dictkeysobject le_dictkeys_t;
/* the entries of keys that aren't DICT_KEYS_GENERAL */
typedef struct {
    PyObject *me_key;
    PyObject *me_value;
} le_dictunicodeentry_t;
/* emitted code compares the two as one word (pobject.h:le_dict_shape) */
_Static_assert(offsetof(le_dictkeys_t, dk_kind) ==
                   offsetof(le_dictkeys_t, dk_log2_index_bytes) + 1,
               "LeJIT: PyDictKeysObject.dk_kind follows dk_log2_index_bytes");
#else
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
typedef struct _dictkeysobject le_dictkeys_t;
#define LE_OFS_le_dictkeys_t__dk_refcnt offsetof(le_dictkeys_t, dk_refcnt)
#define LE_SIZE2_le_dictkeys_t__dk_refcnt 3
#define LE_UNSIGNED_le_dictkeys_t__dk_refcnt 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((le_dictkeys_t *)0)->dk_refcnt), Py_ssize_t),
               "LeJIT: PyDictKeysObject.dk_refcnt is not Py_ssize_t");
_Static_assert(sizeof(((le_dictkeys_t *)0)->dk_refcnt) == 8,
               "LeJIT: PyDictKeysObject.dk_refcnt: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((le_dictkeys_t *)0)->dk_refcnt) == 1,
    "LeJIT: PyDictKeysObject.dk_refcnt: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((le_dictkeys_t *)0)->dk_refcnt))-1 < 0) == 1,
               "LeJIT: PyDictKeysObject.dk_refcnt: LeJIT reads it as signed");
#define LE_OFS_le_dictkeys_t__dk_size offsetof(le_dictkeys_t, dk_size)
#define LE_SIZE2_le_dictkeys_t__dk_size 3
#define LE_UNSIGNED_le_dictkeys_t__dk_size 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((le_dictkeys_t *)0)->dk_size), Py_ssize_t),
               "LeJIT: PyDictKeysObject.dk_size is not Py_ssize_t");
_Static_assert(sizeof(((le_dictkeys_t *)0)->dk_size) == 8,
               "LeJIT: PyDictKeysObject.dk_size: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((le_dictkeys_t *)0)->dk_size) == 1,
    "LeJIT: PyDictKeysObject.dk_size: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((le_dictkeys_t *)0)->dk_size))-1 < 0) == 1,
               "LeJIT: PyDictKeysObject.dk_size: LeJIT reads it as signed");
#define LE_OFS_le_dictkeys_t__dk_usable offsetof(le_dictkeys_t, dk_usable)
#define LE_SIZE2_le_dictkeys_t__dk_usable 3
#define LE_UNSIGNED_le_dictkeys_t__dk_usable 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((le_dictkeys_t *)0)->dk_usable), Py_ssize_t),
               "LeJIT: PyDictKeysObject.dk_usable is not Py_ssize_t");
_Static_assert(sizeof(((le_dictkeys_t *)0)->dk_usable) == 8,
               "LeJIT: PyDictKeysObject.dk_usable: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((le_dictkeys_t *)0)->dk_usable) == 1,
    "LeJIT: PyDictKeysObject.dk_usable: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((le_dictkeys_t *)0)->dk_usable))-1 < 0) == 1,
               "LeJIT: PyDictKeysObject.dk_usable: LeJIT reads it as signed");
#define LE_OFS_le_dictkeys_t__dk_nentries offsetof(le_dictkeys_t, dk_nentries)
#define LE_SIZE2_le_dictkeys_t__dk_nentries 3
#define LE_UNSIGNED_le_dictkeys_t__dk_nentries 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((le_dictkeys_t *)0)->dk_nentries), Py_ssize_t),
               "LeJIT: PyDictKeysObject.dk_nentries is not Py_ssize_t");
_Static_assert(sizeof(((le_dictkeys_t *)0)->dk_nentries) == 8,
               "LeJIT: PyDictKeysObject.dk_nentries: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((le_dictkeys_t *)0)->dk_nentries) == 1,
    "LeJIT: PyDictKeysObject.dk_nentries: LeJIT reads it as an integer");
_Static_assert(
    ((__typeof__(((le_dictkeys_t *)0)->dk_nentries))-1 < 0) == 1,
    "LeJIT: PyDictKeysObject.dk_nentries: LeJIT reads it as signed");
#define LE_OFS_le_dictkeys_t__dk_indices offsetof(le_dictkeys_t, dk_indices)
#define LE_SIZE2_le_dictkeys_t__dk_indices 0
#define LE_UNSIGNED_le_dictkeys_t__dk_indices 1
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((le_dictkeys_t *)0)->dk_indices), char[]),
               "LeJIT: PyDictKeysObject.dk_indices is not char[]");
_Static_assert(sizeof(((le_dictkeys_t *)0)->dk_indices[0]) == 1,
               "LeJIT: PyDictKeysObject.dk_indices: LeJIT reads 1 byte");
#endif

typedef struct {
    Py_hash_t me_hash;
    PyObject *me_key;
    PyObject *me_value;
} le_dictentry_t;
typedef le_dictentry_t PyDictKeyEntry;
#define LE_OFS_le_dictentry_t__me_hash offsetof(le_dictentry_t, me_hash)
#define LE_SIZE2_le_dictentry_t__me_hash 3
#define LE_UNSIGNED_le_dictentry_t__me_hash 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((le_dictentry_t *)0)->me_hash), Py_hash_t),
               "LeJIT: PyDictKeyEntry.me_hash is not Py_hash_t");
_Static_assert(sizeof(((le_dictentry_t *)0)->me_hash) == 8,
               "LeJIT: PyDictKeyEntry.me_hash: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((le_dictentry_t *)0)->me_hash) == 1,
               "LeJIT: PyDictKeyEntry.me_hash: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((le_dictentry_t *)0)->me_hash))-1 < 0) == 1,
               "LeJIT: PyDictKeyEntry.me_hash: LeJIT reads it as signed");
#define LE_OFS_le_dictentry_t__me_key offsetof(le_dictentry_t, me_key)
#define LE_SIZE2_le_dictentry_t__me_key 3
#define LE_UNSIGNED_le_dictentry_t__me_key 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((le_dictentry_t *)0)->me_key), PyObject *),
               "LeJIT: PyDictKeyEntry.me_key is not PyObject *");
_Static_assert(sizeof(((le_dictentry_t *)0)->me_key) == 8,
               "LeJIT: PyDictKeyEntry.me_key: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((le_dictentry_t *)0)->me_key) == 5,
               "LeJIT: PyDictKeyEntry.me_key: LeJIT reads it as a pointer");
#define LE_OFS_le_dictentry_t__me_value offsetof(le_dictentry_t, me_value)
#define LE_SIZE2_le_dictentry_t__me_value 3
#define LE_UNSIGNED_le_dictentry_t__me_value 0
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((le_dictentry_t *)0)->me_value), PyObject *),
               "LeJIT: PyDictKeyEntry.me_value is not PyObject *");
_Static_assert(sizeof(((le_dictentry_t *)0)->me_value) == 8,
               "LeJIT: PyDictKeyEntry.me_value: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((le_dictentry_t *)0)->me_value) == 5,
               "LeJIT: PyDictKeyEntry.me_value: LeJIT reads it as a pointer");

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
#ifndef LE_TRACE
#define LE_TRACE 0
#endif

/* define to 1 to count code size per instruction */
#ifndef LE_CODESTATS
#define LE_CODESTATS 0
#endif

/* define to 1 for extra assert()'s */
#ifndef ALL_CHECKS
#define ALL_CHECKS 0
#endif

/* a counter only the tests read (lejit._core._stats): checked builds only */
#if ALL_CHECKS
#define LE_STAT(x) (x)
#else
#define LE_STAT(x) ((void)0)
#endif

/*  define to inline the most common functions in the produced code
    (should be enabled unless you want to trade code size for speed) */
#ifndef INLINE_COMMON_FUNCTIONS
#define INLINE_COMMON_FUNCTIONS 1
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
#define BUFFER_MARGIN 1024
#endif
#ifndef GUARANTEED_MINIMUM
#define GUARANTEED_MINIMUM 64
#endif

#define EXTERNVAR extern
#define EXTERNFN
#define DEFINEVAR
#define DEFINEFN
#define INITIALIZATIONFN DEFINEFN

#define psyco_assert(x) ((void)((x) || psyco_fatal_msg("assertion failed")))
#define psyco_fatal_msg(msg) psyco_fatal_error(msg, __FILE__, __LINE__)
EXTERNFN int
psyco_fatal_error(char *msg, char *filename, int lineno);

#if ALL_CHECKS
#define MALLOC_CHECK_ 2 /* GCC malloc() checks */
#define extra_assert(x) psyco_assert(x)
#else
#define extra_assert(x) (void)0 /* nothing */
#endif

/* Under AddressSanitizer, LeJIT's own allocators
   mark what they hold free as unaddressable, so that an access through a stale
   pointer is reported as malloc'd memory would be; and LeakSanitizer is
   told to scan executable memory, where stub and pause records point to
   what they hold (LE_LSAN_ROOT) */
#if defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#include <sanitizer/lsan_interface.h>
#define LE_POISON(p, n) ASAN_POISON_MEMORY_REGION(p, n)
#define LE_UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION(p, n)
#define LE_LSAN_ROOT(p, n) __lsan_register_root_region(p, n)
#else
#define LE_POISON(p, n) ((void)0)
#define LE_UNPOISON(p, n) ((void)0)
#define LE_LSAN_ROOT(p, n) ((void)0)
#endif

#if INLINE_COMMON_FUNCTIONS
#define PSY_INLINE __inline static
#else
#define PSY_INLINE static
#endif

#ifndef PyObject_TypeCheck
#define PyObject_TypeCheck(o, t) ((o)->ob_type == (t))
#endif

/* C API calls that some CPython versions lack: the function where
   libpython has it, else its public replacement */
#if LE_HAS_LOOKUP_ATTR_LINKABLE
#define le_lookup_attr _PyObject_LookupAttr
#else
#define le_lookup_attr PyObject_GetOptionalAttr
#endif
#if LE_HAS_PRIVATE_IS_FINALIZING_LINKABLE
#define le_is_finalizing _Py_IsFinalizing
#else
#define le_is_finalizing Py_IsFinalizing
#endif
#if LE_HAS_HASH_BYTES_LINKABLE
/* some versions declare it only in an internal header */
PyAPI_FUNC(Py_hash_t) _Py_HashBytes(const void *, Py_ssize_t);
#define le_hash_bytes _Py_HashBytes
#else
#define le_hash_bytes Py_HashBuffer
#endif
/* functions that every CPython LeJIT builds for exports, which some
   declare only in an internal header */
PyAPI_FUNC(int)
    _PyDict_SetItem_KnownHash(PyObject *, PyObject *, PyObject *, Py_hash_t);
PyAPI_FUNC(int) _PySet_Update(PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) PyCFunction_Call(PyObject *, PyObject *, PyObject *);
PyAPI_FUNC(PyObject *) _PyList_Extend(PyListObject *, PyObject *);
PyAPI_FUNC(void) _PyErr_SetKeyError(PyObject *);
PyAPI_FUNC(PyObject *)
    _PyUnicode_JoinArray(PyObject *, PyObject *const *, Py_ssize_t);
#if LE_HAS_PRIVATE_NUMBER_INDEX
PyAPI_FUNC(PyObject *) _PyNumber_Index(PyObject *);
#endif
#if LE_HAS_CHECK_RESULT_TSTATE
PyAPI_FUNC(PyObject *) _Py_CheckFunctionResult(PyThreadState *, PyObject *,
                                               PyObject *, const char *);
#endif
/* a type's dict, borrowed (NULL if the type isn't ready): its tp_dict, or
   PyType_GetDict's, as a static builtin type keeps its dict in the
   interpreter, which holds it as long as the type */
#if LE_HAS_TYPE_GET_DICT
static inline PyObject *
le_type_dict(PyTypeObject *tp)
{
    PyObject *d = PyType_GetDict(tp);
    Py_XDECREF(d);
    return d;
}
#else
#define le_type_dict(tp) ((tp)->tp_dict)
#endif

typedef unsigned char code_t;

typedef struct vinfo_s vinfo_t;             /* defined in compiler.h */
typedef struct vinfo_array_s vinfo_array_t; /* defined in compiler.h */
typedef struct PsycoObject_s PsycoObject;   /* defined in compiler.h */
typedef struct FrozenPsycoObject_s FrozenPsycoObject; /* def in dispatcher.h */
typedef struct CodeBufferObject_s CodeBufferObject; /* def in codemanager.h */
typedef struct global_entries_s global_entries_t;   /* def in dispatcher.h */
typedef struct mergepoint_s mergepoint_t; /* defined in mergepoint.h */
typedef struct stack_frame_info_s stack_frame_info_t; /* def in pycompiler.h */

/* Everything the compiled code of one code object holds (codemanager.c),
   released with that code object, or with a group of it (codemanager.h,
   le_group_t). */
typedef struct le_owner_s {
    struct {
        code_t *start, *end;
    } *extents; /* its machine code */
    int nextents, extentscap;
    CodeBufferObject **bufs; /* every code buffer made for it */
    int nbufs, bufscap;
    PyObject **pins; /* a hash set of owned references */
    size_t npins, pinmask;
    struct {
        void (*fn)(void *);
        void *closure;
    } *releases;
    int nreleases, releasescap; /* its records holding something */
    void *blocks;               /* records freed with the owner */
    code_t *stub_pool, *stub_pool_end;
    size_t stub_chunk;         /* the size of stub_pool's chunk */
    int *notes;                /* its call sites' psyco_locals_maps */
    int nnotes, notescap;      /* indices, reused once it dies */
    struct le_owner_s *parent; /* a group's: the owner of its site;
                                  NULL for a code object's own */
    struct le_group_s *groups; /* a code object's: its groups, newest
                                  first */
} le_owner_t;

/* What LeJIT keeps per code object, on its weak references (lejit.c): found by
   identity, not by value as a dict keyed by code objects would be (equal
   code objects compare co_consts recursively). */
/* Generator bodies run compiled, resumed through the eval hook
   (eval_hook.c:LeGen_Run), and coroutine bodies too where their awaits
   pause the frame object as ceval's YIELD_FROM leaves it (pycompiler.c) */
#define LE_RESUMES_COROUTINES (!LE_HAS_INTERPRETER_FRAME)
/* and compiled code resumes a generator itself, without gen_send_ex2 and
   the eval hook (eval_hook.c:le_gen_iternext), where its frame is inside
   it */
#define LE_RESUMES_GENERATORS_DIRECTLY LE_HAS_INTERPRETER_FRAME
#if LE_RESUMES_GENERATORS_DIRECTLY
EXTERNFN PyObject *
le_gen_iternext(PyObject *gen);
#endif
/* and a resume's compiled code is given the frame itself, whose words it
   reads where it uses them (vcompiler.c:PsycoObject_FromGenFrame), so one
   entry serves every resume at a point */
#define LE_RESUMES_ON_FRAME LE_HAS_INTERPRETER_FRAME
/* coroutine bodies run compiled: on the frame object, or on the frame
   inside the coroutine with each await a SEND loop */
#define LE_RESUMES_COROUTINES_COMPILED \
    (LE_RESUMES_COROUTINES || LE_RESUMES_ON_FRAME)
/* a frame with more words (locals plus stack) resumes interpreted */
#define LE_FRAME_WORDS_MAX 1024

/* A resumable generator's compiled entries at one resume point, compiled
   when first needed, at most LE_RESUME_ENTRIES: where LE_RESUMES_ON_FRAME,
   one; otherwise one per set of NULL words in its frame (a bit each, in
   the frame's order) */
#if LE_RESUMES_ON_FRAME
#define LE_RESUME_ENTRIES 1
#else
#define LE_RESUME_ENTRIES 4
#endif
typedef struct {
#if !LE_RESUMES_ON_FRAME
    uint64_t nulls;
#endif
    PyObject *code;    /* codebuf, Py_None if compiling failed; owned */
    PyObject *globals; /* as entry_globals and entry_builtins */
    PyObject *builtins;
} le_resume_entry_t;
typedef struct {
    int lasti; /* f_lasti there, -1 for the start */
    int count;
    int in_handler; /* a coroutine's place inside an exception handler,
                       which resumes interpreted (pycompiler.c: a handler
                       that can await runs in ceval) */
    le_resume_entry_t entries[LE_RESUME_ENTRIES];
} le_resume_point_t;

/* A loop head where a function's frame that ceval runs is taken over
   (eval_hook.c:le_osr_switch), with its entry there, compiled when first
   needed */
typedef struct {
    int head;    /* in bytes */
    int refused; /* never taken over here (eval_hook.c:osr_point) */
#if LE_HAS_EXCEPTION_TABLE
    /* where a handler covers the head, the RERAISE 1 no handler covers,
       which a raise leaves the frame by; else -1 */
    int reraise;
#endif
    le_resume_point_t point;
} le_osr_point_t;

typedef struct le_code_extra_s {
    PyObject *merge_points;   /* capsule, Py_None if unsupported, NULL if not
                                 built yet; owned */
    PyObject *entry_point;    /* codebuf, Py_None if compiling failed, NULL if
                                 not tried; owned */
    PyObject *entry_globals;  /* the globals it was compiled for: borrowed, so
                                 maybe dead and reused (vcompiler.c says why
                                 that is safe) */
    PyObject *entry_builtins; /* and its f_builtins: borrowed,
                                 the compiled code owns it */
    long serial;              /* never reused, unlike the code object's
                                 address (psyfunc.c:psyco_save_inline_po) */
    unsigned long inline_end; /* 0, or 1 + the first block serial whose
                                 code inlines no calls (psyfunc.c) */
    le_owner_t owner;         /* what its compiled code holds */
    char counted;             /* in the owner scan's count (codemanager.c) */
    int nresumes; /* a resumable generator's points, or -1: not resumable */
    le_resume_point_t *resumes;     /* one per yield and the start, or NULL */
    le_resume_point_t *last_resume; /* the one the last resume used */
#if LE_RESUMES_GENERATORS_DIRECTLY
    struct le_gen_direct_s *direct; /* le_gen_direct_t, or NULL */
#endif
    int nosr;                  /* loop heads its frames were taken over at */
    le_osr_point_t *osr;       /* one per such head, or NULL */
    long osr_left;             /* backward jumps its frames make before one is
                                  taken over (osr.c), 0 while not counted */
    unsigned char *loop_heads; /* a bit per code unit, set at each
                                  backward jump's target, or NULL
                                  (osr.c:le_osr_loop_head) */
} le_code_extra_t;
#if LE_RESUMES_GENERATORS_DIRECTLY
/* A resumable generator's compiled entries as compiled code that steps it
   calls them (psyfunc.c:psyco_gen_step): indexed by its frame's place
   (le_gen_direct_index), each the start of the entry's code, or NULL
   where none is compiled yet; one entry serves all the code's frames
   with these globals and builtins, set by the first entry put in. The
   entries' buffers are the resume points' (le_resume_entry_t). */
typedef struct le_gen_direct_s {
    PyObject *globals, *builtins; /* borrowed, as entry_globals */
    code_t *entries[1];
} le_gen_direct_t;
/* the table of 'co' (record 'x'), made if the code is a resumable
   generator's that compiled code may step; NULL if not, or out of
   memory (eval_hook.c) */
EXTERNFN le_gen_direct_t *
le_gen_direct_table(PyCodeObject *co, le_code_extra_t *x);
/* what compiled code calls after a direct step of the generator 'gen'
   whose run gave 'result' and didn't simply pause at a yield
   (eval_hook.c), as gen_iternext returns */
EXTERNFN PyObject *
le_gen_direct_end(PyObject *gen, PyObject *result);
/* the same after a step made by send(), as gen_send returns */
EXTERNFN PyObject *
le_gen_direct_send_end(PyObject *gen, PyObject *result);
/* the same after a step made by yield from's SEND, as PyIter_Send:
   1 with the value yielded in out[0], 0 with the value returned, -1 with
   the exception */
EXTERNFN int
le_gen_direct_send_status(PyObject *gen, PyObject *result, PyObject *out[]);
#endif
/* the record lives in a weak reference to its code object (lejit.c) */
typedef struct {
    PyWeakReference wr;
    char attached;
    le_code_extra_t x;
} code_record_t;
EXTERNVAR PyTypeObject CodeRecord_Type;
/* the record for 'co', made on first use; NULL with an exception set */
EXTERNFN le_code_extra_t *
le_code_extra(PyCodeObject *co);
/* the record for 'co' if it has one, never raising */
static inline le_code_extra_t *
le_code_extra_get(PyCodeObject *co)
{
    PyWeakReference *r = (PyWeakReference *)co->co_weakreflist;
    for (; r != NULL; r = r->wr_next)
        if (Py_TYPE(r) == &CodeRecord_Type)
            return &((code_record_t *)r)->x;
    return NULL;
}
/* the code last found not resumable, and its record, which outlives it
   (eval_hook.c:gen_may_resume) */
EXTERNVAR PyCodeObject *le_gen_interpreted;
EXTERNVAR le_code_extra_t *le_gen_interpreted_x;
/* releases the record and its compiled code; its code object is dead */
EXTERNFN void
le_code_extra_free(le_code_extra_t *x);
/* takes the record off 'co', unfreed, and returns it */
EXTERNFN le_code_extra_t *
le_code_extra_detach(PyCodeObject *co);

/* moved here from vcompiler.h because needed by numerous header files.
   See vcompiler.h for comments */
typedef bool (*compute_fn_t)(PsycoObject *po, vinfo_t *vi);
typedef PyObject *(*direct_compute_fn_t)(vinfo_t *vi, char *data);
typedef struct {
    compute_fn_t compute_fn;
    direct_compute_fn_t direct_compute_fn;
    long pyobject_mask;
    signed char nested_weight[2];
} source_virtual_t;

#define psyco_inc_stackdepth(po)                \
    do {                                        \
        po->stack_depth += sizeof(stackitem_t); \
    } while (0)

#define psyco_dec_stackdepth(po)                \
    do {                                        \
        po->stack_depth -= sizeof(stackitem_t); \
    } while (0)

/* to display code object names */
#define PyCodeObject_NAME(co) \
    (co->co_name ? PyUnicode_AsUTF8(co->co_name) : "<anonymous code object>")

/* defined in pycompiler.c */
#define GLOBAL_ENTRY_POINT psyco_pycompiler_mainloop
EXTERNFN code_t *
psyco_pycompiler_mainloop(PsycoObject *po);

/* Mid-compile this abandons the compile (codemanager.h, compile guards);
   anywhere else it aborts. It never returns. */
EXTERNFN void
psyco_out_of_memory(char *filename, int lineno);
#define OUT_OF_MEMORY() psyco_out_of_memory(__FILE__, __LINE__)

/* For tests: LE_FAIL(site) is true at the n-th time it is reached after
   lejit._core._fail_after(site, n), as if that allocation had failed. */
enum {
    LE_FAIL_PSYCOOBJECT,
    LE_FAIL_ARRAY,
    LE_FAIL_CODEBUF,
    LE_FAIL_LOOKUP,
    LE_FAIL_OWNER,
    LE_FAIL_TRACEBACK,
    LE_FAIL_SITES
};
#if ALL_CHECKS
EXTERNVAR long psyco_fail_after[LE_FAIL_SITES];
#define LE_FAIL(site) \
    (psyco_fail_after[site] > 0 && --psyco_fail_after[site] == 0)
#else
#define LE_FAIL(site) 0
#endif

/* Getting data from the _psyco module */
EXTERNFN PyObject *
need_cpsyco_obj(char *name);

/* defined in dispatcher.c */
EXTERNFN void
PsycoObject_EmergencyCodeRoom(PsycoObject *po);

#if LE_TRACE
/* Calls hooked by analyser to collect compiler behaviour */
EXTERNFN void
le_trace_begin_simple_code(code_t *code);
EXTERNFN void
le_trace_end_simple_code(code_t *code);
EXTERNFN void
le_trace_begin_code(PsycoObject *po);
EXTERNFN void
le_trace_end_code(PsycoObject *po);
EXTERNFN void
le_trace_jump(code_t *location, code_t *target);
EXTERNFN void
le_trace_jump_update(code_t *location, code_t *target);
EXTERNFN void
le_trace_jump_reg(code_t *location, int reg_target);
EXTERNFN void
le_trace_jump_cond(code_t *location, code_t *not_taken, code_t *taken);
EXTERNFN void
le_trace_jump_cond_update(code_t *location, code_t *not_taken, code_t *taken);
EXTERNFN void
le_trace_jump_cond_reg(code_t *location, code_t *not_taken, int reg_taken);
EXTERNFN void
le_trace_call(code_t *location, code_t *call_target);
EXTERNFN void
le_trace_call_reg(code_t *location, int reg_call_target);
EXTERNFN void
le_trace_return(code_t *location, int stack_adjust);
EXTERNFN void
le_trace_unsupported_opcode(PyCodeObject *co, int bytecode_index);
EXTERNFN void
le_trace_run_fail(PyCodeObject *co, char *reason);
EXTERNFN void
le_trace_unify(PsycoObject *po, CodeBufferObject *match);
#else
#define le_trace_begin_simple_code(code) \
    do {                                 \
    } while (0)
#define le_trace_end_simple_code(code) \
    do {                               \
    } while (0)
#define le_trace_begin_code(po) \
    do {                        \
    } while (0)
#define le_trace_end_code(po) \
    do {                      \
    } while (0)
#define le_trace_jump(location, target) \
    do {                                \
    } while (0)
#define le_trace_jump_update(location, target) \
    do {                                       \
    } while (0)
#define le_trace_jump_reg(location, target) \
    do {                                    \
    } while (0)
#define le_trace_jump_cond(location, not_taken, taken) \
    do {                                               \
    } while (0)
#define le_trace_jump_cond_update(location, not_taken, taken) \
    do {                                                      \
    } while (0)
#define le_trace_jump_cond_reg(location, not_taken, taken) \
    do {                                                   \
    } while (0)
#define le_trace_call(location, call_target) \
    do {                                     \
    } while (0)
#define le_trace_call_reg(location, reg_call_target) \
    do {                                             \
    } while (0)
#define le_trace_return(location, stack_adjust) \
    do {                                        \
    } while (0)
#define le_trace_unsupported_opcode(co, bytecode_index) \
    do {                                                \
    } while (0)
#define le_trace_run_fail(co, reason) \
    do {                              \
    } while (0)
#define le_trace_unify(po, match) \
    do {                          \
    } while (0)
#endif

#define BEGIN_CODE               \
    do {                         \
        code_t *code = po->code; \
        le_trace_begin_code(po);
#define END_CODE                           \
    po->code = code;                       \
    le_trace_end_code(po);                 \
    if (code >= po->codelimit) {           \
        PsycoObject_EmergencyCodeRoom(po); \
    }                                      \
    }                                      \
    while (0);

#endif /* _PSYCO_H */
