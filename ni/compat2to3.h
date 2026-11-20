#ifndef _COMPAT2TO3_H
#define _COMPAT2TO3_H

#include <Python.h>

#define IS_PY3K
#define NiCompatStr_Type PyUnicode_Type
#define NiCompatStr_AsString PyUnicode_AsUTF8
#define NiCompatStr_AS_STRING PyUnicode_AsUTF8
#define NiCompatStr_Check PyUnicode_Check
#define NiCompatStr_CheckExact PyUnicode_CheckExact
#define NiCompatStr_CHECK_INTERNED PyUnicode_CHECK_INTERNED
#define NiCompatStr_Concat PyUnicode_Concat
#define NiCompatStr_Format PyUnicode_Format
#define NiCompatStr_FromFormat PyUnicode_FromFormat
#define NiCompatStr_FromFormatV PyUnicode_FromFormatV
#define NiCompatStr_FromString PyUnicode_FromString
#define NiCompatStr_FromStringAndSize PyUnicode_FromStringAndSize
#define NiCompatStr_GET_SIZE PyUnicode_GET_SIZE
#define NiCompatStr_InternFromString PyUnicode_InternFromString
#define NiCompatStr_InternInPlace PyUnicode_InternInPlace
#define NiCompatStr_Size PyUnicode_Size
#define NiCompatStr_Type PyUnicode_Type
#define NiCompatStr_HASH_FIELD(o) ((PyUnicodeObject*)o)->hash
#define NiCompatInt_FromLong PyLong_FromLong
#define NiCompatInt_AS_LONG PyLong_AS_LONG
#define NiCompatInt_AsLong PyLong_AsLong

#endif /* _COMPAT2TO3_H */
