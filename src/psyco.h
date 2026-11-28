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
/* the offsets of the CPython fields that hand-written emitted code reads
   (vcompiler.h:CPY_OFFSET), each checked at compile time for the width
   and kind that code reads, and private structs of CPython's */
#include <stddef.h>

#if LE_HAS_REFCOUNT_32
#define LE_OFS_PyObject__ob_refcnt offsetof(PyObject, ob_refcnt)
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
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyObject *)0)->ob_type), struct _typeobject *),
               "LeJIT: PyObject.ob_type is not struct _typeobject *");
_Static_assert(sizeof(((PyObject *)0)->ob_type) == 8,
               "LeJIT: PyObject.ob_type: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyObject *)0)->ob_type) == 5,
               "LeJIT: PyObject.ob_type: LeJIT reads it as a pointer");

#define LE_OFS_PyTypeObject__tp_dealloc offsetof(PyTypeObject, tp_dealloc)
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTypeObject *)0)->tp_dealloc), destructor),
               "LeJIT: PyTypeObject.tp_dealloc is not destructor");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_dealloc) == 8,
               "LeJIT: PyTypeObject.tp_dealloc: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyTypeObject *)0)->tp_dealloc) == 5,
               "LeJIT: PyTypeObject.tp_dealloc: LeJIT reads it as a pointer");
#define LE_OFS_PyTypeObject__tp_flags offsetof(PyTypeObject, tp_flags)
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyTypeObject *)0)->tp_flags), unsigned long),
               "LeJIT: PyTypeObject.tp_flags is not unsigned long");
_Static_assert(sizeof(((PyTypeObject *)0)->tp_flags) == 8,
               "LeJIT: PyTypeObject.tp_flags: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyTypeObject *)0)->tp_flags) == 1,
               "LeJIT: PyTypeObject.tp_flags: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((PyTypeObject *)0)->tp_flags))-1 < 0) == 0,
               "LeJIT: PyTypeObject.tp_flags: LeJIT reads it as unsigned");
#define LE_OFS_PyTypeObject__tp_version_tag \
    offsetof(PyTypeObject, tp_version_tag)
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

#define LE_OFS_PyDictObject__ma_used offsetof(PyDictObject, ma_used)
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyDictObject *)0)->ma_used), Py_ssize_t),
               "LeJIT: PyDictObject.ma_used is not Py_ssize_t");
_Static_assert(sizeof(((PyDictObject *)0)->ma_used) == 8,
               "LeJIT: PyDictObject.ma_used: LeJIT reads 8 bytes");
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
_Static_assert(
    __builtin_types_compatible_p(__typeof__(((PyDictObject *)0)->ma_keys),
                                 PyDictKeysObject *),
    "LeJIT: PyDictObject.ma_keys is not PyDictKeysObject *");
_Static_assert(sizeof(((PyDictObject *)0)->ma_keys) == 8,
               "LeJIT: PyDictObject.ma_keys: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyDictObject *)0)->ma_keys) == 5,
               "LeJIT: PyDictObject.ma_keys: LeJIT reads it as a pointer");
#define LE_OFS_PyDictObject__ma_values offsetof(PyDictObject, ma_values)
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

#define LE_OFS_PyASCIIObject__hash offsetof(PyASCIIObject, hash)
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyASCIIObject *)0)->hash), Py_hash_t),
               "LeJIT: PyASCIIObject.hash is not Py_hash_t");
_Static_assert(sizeof(((PyASCIIObject *)0)->hash) == 8,
               "LeJIT: PyASCIIObject.hash: LeJIT reads 8 bytes");
_Static_assert(__builtin_classify_type(((PyASCIIObject *)0)->hash) == 1,
               "LeJIT: PyASCIIObject.hash: LeJIT reads it as an integer");
_Static_assert(((__typeof__(((PyASCIIObject *)0)->hash))-1 < 0) == 1,
               "LeJIT: PyASCIIObject.hash: LeJIT reads it as signed");
#define LE_BIT_PyASCIIObject__state__interned 0x3
#define LE_BIT_PyASCIIObject__state__kind 0x1C
#define LE_BIT_PyASCIIObject__state__compact 0x20
#define LE_BIT_PyASCIIObject__state__ascii 0x40
#define LE_BIT_PyASCIIObject__state__ready 0x80

#define LE_OFS_PyFunctionObject__func_code \
    offsetof(PyFunctionObject, func_code)
_Static_assert(__builtin_types_compatible_p(
                   __typeof__(((PyFunctionObject *)0)->func_code), PyObject *),
               "LeJIT: PyFunctionObject.func_code is not PyObject *");
_Static_assert(sizeof(((PyFunctionObject *)0)->func_code) == 8,
               "LeJIT: PyFunctionObject.func_code: LeJIT reads 8 bytes");
_Static_assert(
    __builtin_classify_type(((PyFunctionObject *)0)->func_code) == 5,
    "LeJIT: PyFunctionObject.func_code: LeJIT reads it as a pointer");

typedef struct {
    PyObject_HEAD Py_ssize_t it_index;
    PyObject *it_seq;
} seqiterobject;

typedef struct {
    PyObject_HEAD PyObject *start;
    PyObject *stop;
    PyObject *step;
    PyObject *length;
} rangeobject;

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
#endif

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

#endif

typedef struct {
    Py_hash_t me_hash;
    PyObject *me_key;
    PyObject *me_value;
} le_dictentry_t;
typedef le_dictentry_t PyDictKeyEntry;

typedef int8_t byte_t;
typedef int16_t word_t;
typedef int32_t dword_t;
typedef int64_t qword_t;
typedef qword_t stackitem_t;

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

/*****************************************************************/
/* Room in the code being written (codemanager.c). At each instruction a
   compile has at least BUFFER_MARGIN bytes before its limit, or it pauses
   there instead (pycompiler.c, vcompiler.c:psyco_compile): more than 128,
   because of how conditional jumps are emitted (pycompiler.c). Past the
   limit there are GUARANTEED_MINIMUM bytes more, for what is written
   before the next END_CODE, which then goes on in a new extent after a
   jump (codemanager.c:psyco_code_continue). So no block between
   BEGIN_CODE and END_CODE may write more than that, less the jump, from
   wherever it starts; checked builds assert it at every END_CODE, and a
   block that writes in a loop as long as its input makes room as it goes
   (CODE_ROOM_IN_BLOCK). */
#ifndef BUFFER_MARGIN
#define BUFFER_MARGIN 1024
#endif
#ifndef GUARANTEED_MINIMUM
#define GUARANTEED_MINIMUM 1024
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

#define PSY_INLINE __inline static

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

/* Relocations: every place that holds where compiled code is, noted as
   it is written while a compile runs. JUMP: the rel32 at 'at' of a jump
   or call, counted from at + end; DATA: a 32-bit distance counted from
   at + end that must stay in reach (RIP-relative data, a record's offset
   to its jump); WORD: the address of code, in code or in a record, or with
   'end' 1 the end of a piece of code. A checked build moves the code each
   outermost compile wrote as it ends, patching from the notes
   (codemanager.c:psyco_code_move_made), so that a place not noted traps
   in tests; the owner then keeps the relocations in its code. */
enum { LE_RELOC_JUMP, LE_RELOC_DATA, LE_RELOC_WORD };
#if ALL_CHECKS
/* notes 'at' once written */
EXTERNFN void
le_reloc_note(void *at, int kind, int end);
/* notes the rel32 'value' about to be written for a jump ending at 'end',
   and gives it back */
EXTERNFN long
le_reloc_jump(code_t *end, long value);
/* code written from 'start' up to 'end' is written again from 'start':
   the notes made in it since le_reloc_mark() gave 'mark' are dropped, as
   what is written there next may hold the noted bytes by chance */
EXTERNFN long
le_reloc_mark(void);
EXTERNFN void
le_reloc_forget(long mark, code_t *start, code_t *end);
/* where code that the compile just ended wrote at 'p' is now */
EXTERNFN code_t *
psyco_code_moved(code_t *p);
/* stops unless the 'size' bytes at 'at', about to be patched, are code
   and not an old copy of moved code (x64/iencoding.h:le_patch_rel32) */
EXTERNFN void
le_patch_check(void *at, size_t size);
#else
#define le_reloc_note(at, kind, end) ((void)0)
#define le_reloc_jump(end, value) (value)
#define le_reloc_mark() 0L
#define le_reloc_forget(mark, start, end) ((void)(mark))
#define psyco_code_moved(p) (p)
#define le_patch_check(at, size) ((void)0)
#endif
#define LE_RELOC_SLOT(p) le_reloc_note((void *)(p), LE_RELOC_WORD, 0)
#define LE_RELOC_SLOT_END(p) le_reloc_note((void *)(p), LE_RELOC_WORD, 1)

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
    size_t stub_chunk;           /* the size of stub_pool's chunk */
    code_t *consts, *consts_end; /* its constant slots (dispatcher.c) */
    size_t const_chunk;          /* the size of consts' chunk */
    int *notes;                  /* its call sites' psyco_locals_maps */
    int nnotes, notescap;        /* indices, reused once it dies */
    struct le_owner_s *parent;   /* a group's: the owner of its site;
                                    NULL for a code object's own */
    struct le_group_s *groups;   /* a code object's: its groups, newest
                                    first */
#if ALL_CHECKS
    struct le_reloc_s *relocs; /* the relocations in its code */
    int nrelocs, relocscap;
#endif
} le_owner_t;

/* What LeJIT keeps per code object, on its weak references (lejit.c): found by
   identity, not by value as a dict keyed by code objects would be (equal
   code objects compare co_consts recursively). */
/* Generator, coroutine and async generator bodies run compiled, resumed
   on their frame: compiled code resumes a generator itself, without
   gen_send_ex and the eval hook (eval_hook.c:le_gen_iternext), and each
   resume's compiled code is given the frame, whose words it reads where it
   uses them (vcompiler.c:PsycoObject_FromGenFrame), so one entry serves
   every resume at a point */
EXTERNFN PyObject *
le_gen_iternext(PyObject *gen);
/* CPython runs a Python-to-Python call in the caller's C frame, where its
   frames are interpreter frames: compiled calls switch to a stack segment
   when the stack is low (segments.c) */
#define LE_STACK_SEGMENTS LE_HAS_INTERPRETER_FRAME
/* a frame with more words (locals plus stack) resumes interpreted */
#define LE_FRAME_WORDS_MAX 1024

/* A resumable generator's compiled entry at one resume point, compiled
   when first needed */
typedef struct {
    int lasti;         /* f_lasti there, -1 for the start */
    PyObject *code;    /* codebuf, Py_None if compiling failed, NULL if not
                          compiled yet; owned */
    PyObject *globals; /* as entry_globals and entry_builtins */
    PyObject *builtins;
} le_resume_point_t;

/* A call of a Python function from C (_PyFunction_Vectorcall) may run
   its compiled code with no frame (eval_hook.c:le_frameless_vectorcall) */
#define LE_ENTERS_FUNCTIONS_FRAMELESS LE_HAS_FUNCTION_VECTORCALL

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
    unsigned char cold;       /* sweeps since its code was last entered
                                 (through the hook, or frameless from C)
                                 or found running (codemanager.c) */
    unsigned char releases;   /* times the sweep released its code */
    char swept;               /* being released by the sweep */
    char frameless;           /* the entry point may be run with no frame,
                                 by a call from C (eval_hook.c) */
    struct le_code_extra_s *prev, *next; /* in le_code_extras */
    int nresumes; /* a resumable generator's points, or -1: not resumable */
    le_resume_point_t *resumes;     /* one per yield and the start, or NULL */
    le_resume_point_t *last_resume; /* the one the last resume used */
    struct le_gen_direct_s *direct; /* le_gen_direct_t, or NULL */
} le_code_extra_t;
/* A resumable generator's compiled entries as compiled code that steps it
   calls them (psyfunc.c:psyco_gen_step): indexed by its frame's place
   (le_gen_direct_index), each the start of the entry's code, or NULL
   where none is compiled yet; one entry serves all the code's frames
   with these globals and builtins, set by the first entry put in. The
   entries' buffers are the resume points' (le_resume_point_t). */
typedef struct le_gen_direct_s {
    PyObject *globals, *builtins; /* borrowed, as entry_globals */
    code_t *entries[1];
} le_gen_direct_t;
/* the table of 'co' (record 'x'), made if the code is a resumable
   generator's that compiled code may step; NULL if not, or out of
   memory (eval_hook.c) */
EXTERNFN le_gen_direct_t *
le_gen_direct_table(PyCodeObject *co, le_code_extra_t *x);
/* its entries emptied, as if none were compiled yet */
EXTERNFN void
le_gen_direct_clear(PyCodeObject *co, le_code_extra_t *x);
/* what compiled code calls on thread 'ts' after a direct step of the
   generator 'gen' whose run gave 'result' and didn't simply pause at a
   yield (eval_hook.c), as gen_iternext returns */
EXTERNFN PyObject *
le_gen_direct_end(PyThreadState *ts, PyObject *gen, PyObject *result);
/* the same after a step made by send(), as gen_send returns */
EXTERNFN PyObject *
le_gen_direct_send_end(PyThreadState *ts, PyObject *gen, PyObject *result);
/* the same after a step made by yield from's SEND, as PyIter_Send:
   1 with the value yielded in out[0], 0 with the value returned, -1 with
   the exception */
EXTERNFN int
le_gen_direct_send_status(PyThreadState *ts, PyObject *gen, PyObject *result,
                          PyObject *out[]);
/* every record, newest first (lejit.c) */
EXTERNVAR le_code_extra_t *le_code_extras;
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
/* the code object of an attached record */
static inline PyCodeObject *
le_code_extra_code(le_code_extra_t *x)
{
    code_record_t *r =
        (code_record_t *)((char *)x - offsetof(code_record_t, x));
    return (PyCodeObject *)r->wr.wr_object;
}
/* 'x' as a new record, but for its owner, which the caller releases with
   the report and entry point given back, and its table of direct entries,
   which compiled code may still hold: emptied. Its code object lives. */
EXTERNFN void
le_code_extra_reset(le_code_extra_t *x, PyObject **mergepoints,
                    PyObject **entry_point);
/* the code last found not resumable, and its record, which outlives it
   (eval_hook.c:gen_may_resume) */
EXTERNVAR PyCodeObject *le_gen_interpreted;
EXTERNVAR le_code_extra_t *le_gen_interpreted_x;
/* the compiler is set up: lejit._core's first _start did it (lejit.c) */
EXTERNVAR int le_set_up;
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
    LE_FAIL_FREELIST,
    LE_FAIL_PINS,
    LE_FAIL_SITES
};
#if ALL_CHECKS
EXTERNVAR long psyco_fail_after[LE_FAIL_SITES];
#define LE_FAIL(site) \
    (psyco_fail_after[site] > 0 && --psyco_fail_after[site] == 0)
#else
#define LE_FAIL(site) 0
#endif

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
le_trace_jump_reg(code_t *location, int reg_target);
EXTERNFN void
le_trace_jump_cond(code_t *location, code_t *not_taken, code_t *taken);
EXTERNFN void
le_trace_patch_rel32(code_t *end, code_t *target);
EXTERNFN void
le_trace_patch_word(void *at, long value);
EXTERNFN void
le_trace_call(code_t *location, code_t *call_target);
EXTERNFN void
le_trace_call_reg(code_t *location, int reg_call_target);
EXTERNFN void
le_trace_return(code_t *location, int stack_adjust);
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
#define le_trace_jump_reg(location, target) \
    do {                                    \
    } while (0)
#define le_trace_jump_cond(location, not_taken, taken) \
    do {                                               \
    } while (0)
#define le_trace_patch_rel32(end, target) \
    do {                                  \
    } while (0)
#define le_trace_patch_word(at, value) \
    do {                               \
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
#define le_trace_unify(po, match) \
    do {                          \
    } while (0)
#endif

#if ALL_CHECKS
/* where the block, or its part since CODE_ROOM_IN_BLOCK, began */
#define CODE_BLOCK_BEGIN code_t *code_block_start = code;
#define CODE_BLOCK_RESTART code_block_start = code;
#define CODE_BLOCK_CHECK                    \
    psyco_assert(code < code_block_start || \
                 code - code_block_start <= \
                     GUARANTEED_MINIMUM - MAXIMUM_SIZE_OF_FAR_JUMP);
#else
#define CODE_BLOCK_BEGIN
#define CODE_BLOCK_RESTART
#define CODE_BLOCK_CHECK
#endif

#define BEGIN_CODE               \
    do {                         \
        code_t *code = po->code; \
        CODE_BLOCK_BEGIN         \
        le_trace_begin_code(po);
#define END_CODE                           \
    po->code = code;                       \
    CODE_BLOCK_CHECK                       \
    le_trace_end_code(po);                 \
    if (code >= po->codelimit) {           \
        PsycoObject_EmergencyCodeRoom(po); \
    }                                      \
    }                                      \
    while (0);

/* inside a block, at a point where nothing written so far depends on where
   the code after it goes: go on in a new extent if past the limit, as
   END_CODE does */
#define CODE_ROOM_IN_BLOCK()                   \
    do {                                       \
        po->code = code;                       \
        CODE_BLOCK_CHECK                       \
        if (code >= po->codelimit) {           \
            PsycoObject_EmergencyCodeRoom(po); \
            code = po->code;                   \
        }                                      \
        CODE_BLOCK_RESTART                     \
    } while (0)

#endif /* _PSYCO_H */
