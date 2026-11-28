/*
 * Direct calls of ctypes functions. A call of a CDLL function whose
 * arguments are all of kinds this file converts exactly as ctypes would
 * (exact ints, floats, bytes, None, pointer and simple ctypes instances)
 * calls the C function from compiled code: the GIL is released around it
 * as ctypes releases it, but no tuple, converter call or libffi call is
 * made, and an int or float result stays unboxed. Anything else, and any
 * change to the function's argtypes, restype or errcheck, takes ctypes' own
 * call, so every error and message is ctypes'.
 *
 * It relies on ctypes' object layouts (Modules/_ctypes/ctypes.h): a ctypes
 * instance's b_ptr after the object header, and the fields of a function
 * pointer. They are checked against the running ctypes once
 * (ctypes_probe); if any differs, nothing is called directly.
 */
#include "pctypes.h"
#include "../Objects/pboolobject.h"
#include "../Objects/pfloatobject.h"
#include "../Objects/plongobject.h"
#include "../Objects/ptupleobject.h"
#include "../Python/pycompiler.h"
#include "../codemanager.h"
#include "../mechanisms/tstate.h"
#include "../mechanisms/gil.h"
#include "../Python/pyruntime.h"

/* ctypes' CDataObject and PyCFuncPtrObject on Linux */
typedef struct {
    PyObject_HEAD char *b_ptr;
    int b_needsfree;
    PyObject *b_base;
    Py_ssize_t b_size;
    Py_ssize_t b_length;
    Py_ssize_t b_index;
    PyObject *b_objects;
    union {
        char c[16];
        long double D;
    } b_value;
    PyObject *thunk;
    PyObject *callable;
    PyObject *converters;
    PyObject *argtypes;
    PyObject *restype;
    PyObject *checker;
    PyObject *errcheck;
    PyObject *paramflags;
} le_cfuncptr_t;

/* the start of ctypes' PyCArgObject, what byref() returns; its 'obj'
   follows a union whose size differs between versions, so its offset
   is found (ctypes_probe). Its tag is a char, or a string; byref()'s is
   then always the same string. */
typedef struct {
    PyObject_HEAD void *pffi_type;
    union {
        char c;
        const char *s;
    } tag;
    union {
        void *p;
        long double D;
    } value;
} le_carg_t;
static long carg_obj_offset;
/* byref()'s tag: its size (1 or 8) and its value */
static int carg_tag_size;
static long carg_tag_word;

/* finds how byref()'s object 'ref' holds its tag, told from those of two
   other arguments ctypes makes: 'i' for c_int and 'd' for c_double */
static bool
carg_tag_found(PyObject *ref, PyObject *c_int, PyObject *c_dbl)
{
    PyObject *pi = PyObject_CallMethod(c_int, "from_param", "i", 5);
    PyObject *pd = PyObject_CallMethod(c_dbl, "from_param", "d", 1.5);
    bool ok = false;
    if (pi != NULL && pd != NULL && Py_TYPE(pi) == Py_TYPE(ref) &&
        Py_TYPE(pd) == Py_TYPE(ref)) {
        if (((le_carg_t *)ref)->tag.c == 'P' &&
            ((le_carg_t *)pi)->tag.c == 'i' &&
            ((le_carg_t *)pd)->tag.c == 'd') {
            carg_tag_size = 1;
            carg_tag_word = 'P';
            ok = true;
        }
        else if (strcmp(((le_carg_t *)ref)->tag.s, "P") == 0 &&
                 strcmp(((le_carg_t *)pi)->tag.s, "i") == 0 &&
                 strcmp(((le_carg_t *)pd)->tag.s, "d") == 0) {
            carg_tag_size = sizeof(char *);
            carg_tag_word = (long)((le_carg_t *)ref)->tag.s;
            ok = true;
        }
    }
    Py_XDECREF(pi);
    Py_XDECREF(pd);
    PyErr_Clear();
    return ok;
}

/* _ctypes' FUNCFLAG_CDECL: a CDLL function, the GIL released, no errno */
#define FUNCFLAG_CDECL 0x1

/* 1 usable, -1 not, 0 not yet probed */
static int ctypes_state = 0;
static ternaryfunc cfuncptr_call;
static PyTypeObject *simple_meta, *pointer_meta, *simple_base, *carg_type;
static PyTypeObject *pointer_base;
static PyMethodDef *simple_from_param, *pointer_from_param;
/* c_char_p's and c_void_p's own from_param */
static PyMethodDef *char_p_from_param, *void_p_from_param;
/* byref(), kept by the module, and the type of what it takes */
static PyObject *byref_function;
static PyTypeObject *cdata_base;

/* A virtual byref(obj): [iOB_TYPE] the PyCArgObject type, [iBYREF_OBJ]
   obj, a ctypes instance. Made, when it must be, by byref itself. */
#define iBYREF_OBJ 1
#define BYREF_TOTAL 2
static source_virtual_t computed_byref;

static PyObject *
cimpl_byref(PyObject *obj)
{
    return PyObject_CallFunctionObjArgs(byref_function, obj, NULL);
}

static bool
compute_byref(PsycoObject *po, vinfo_t *v)
{
    vinfo_t *obj = vinfo_getitem(v, iBYREF_OBJ), *newobj;
    if (obj == NULL)
        return false;
    newobj = psyco_generic_call(
        po, cimpl_byref, CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v",
        obj);
    if (newobj == NULL)
        return false;
    vinfo_move(po, v, newobj);
    return true;
}

static PyObject *
direct_compute_byref(vinfo_t *v, char *data)
{
    PyObject *r = NULL;
    PyObject *obj = direct_xobj_vinfo(vinfo_getitem(v, iBYREF_OBJ), data);
    if (obj != NULL)
        r = cimpl_byref(obj);
    Py_XDECREF(obj);
    return r;
}

/* where a prototype's argtypes, converters, restype and checker are
   (StgDictObject or StgInfo): at proto_offset in its tp_dict if
   proto_in_dict, else in the type itself; proto_offset 0 if not found */
static bool proto_in_dict;
static long proto_offset;

static PyObject **
proto_fields(PyTypeObject *tp)
{
    char *base = proto_in_dict ? (char *)tp->tp_dict : (char *)tp;
    if (proto_offset == 0 || base == NULL)
        return NULL;
    return (PyObject **)(base + proto_offset);
}

/* a word in [start, end) of o followed by a, converters for a, r and a
   NULL checker: its offset, or 0 */
static long
find_proto_fields(PyObject *o, long start, long end, PyObject *a, PyObject *r)
{
    long k;
    Py_ssize_t i;
    for (k = start; k + 4 * (long)sizeof(PyObject *) <= end;
         k += sizeof(PyObject *)) {
        PyObject **w = (PyObject **)((char *)o + k), *c = w[1];
        if (w[0] != a || w[2] != r || w[3] != NULL || !PyTuple_Check(c) ||
            PyTuple_GET_SIZE(c) != PyTuple_GET_SIZE(a))
            continue;
        for (i = 0; i < PyTuple_GET_SIZE(a); i++)
            if (!PyCFunction_Check(PyTuple_GET_ITEM(c, i)) ||
                PyCFunction_GET_SELF(PyTuple_GET_ITEM(c, i)) !=
                    PyTuple_GET_ITEM(a, i))
                break;
        if (i == PyTuple_GET_SIZE(a))
            return k;
    }
    return 0;
}

/* finds proto_fields from a prototype made as CFUNCTYPE(c_dbl, c_int,
   c_dbl) makes it, but by the metaclass itself: CFUNCTYPE is Python code,
   which could let another thread run while this one compiles */
static void
probe_prototypes(PyObject *ctypes, PyObject *c_int, PyObject *c_dbl)
{
    PyObject *base = PyObject_GetAttrString(ctypes, "_CFuncPtr");
    PyObject *d =
        base == NULL
            ? NULL
            : Py_BuildValue("{s(OO)sOsi}", "_argtypes_", c_int, c_dbl,
                            "_restype_", c_dbl, "_flags_", FUNCFLAG_CDECL);
    PyObject *p =
        d == NULL ? NULL
                  : PyObject_CallFunction((PyObject *)Py_TYPE(base), "s(O)O",
                                          "CFunctionType", base, d);
    PyObject *a = p == NULL ? NULL : PyObject_GetAttrString(p, "_argtypes_");
    if (a != NULL && PyTuple_Check(a) && PyType_Check(p)) {
        PyTypeObject *tp = (PyTypeObject *)p;
        if (tp->tp_dict != NULL && Py_TYPE(tp->tp_dict) != &PyDict_Type)
            proto_offset = find_proto_fields(
                tp->tp_dict, sizeof(PyDictObject),
                Py_TYPE(tp->tp_dict)->tp_basicsize, a, c_dbl);
        proto_in_dict = proto_offset != 0;
        if (proto_offset == 0)
            proto_offset =
                find_proto_fields(p, sizeof(PyHeapTypeObject),
                                  Py_TYPE(p)->tp_basicsize, a, c_dbl);
    }
    Py_XDECREF(a);
    /* a class is in cycles of its own: cleared as the collector would
       clear it, it dies here, not at the next collection, so c_int and
       c_double are held no longer than the probe runs */
    if (p != NULL && Py_TYPE(p)->tp_clear != NULL)
        Py_TYPE(p)->tp_clear(p);
    Py_XDECREF(p);
    Py_XDECREF(d);
    Py_XDECREF(base);
    PyErr_Clear();
}

static PyMethodDef *
meta_method(PyTypeObject *meta, const char *name)
{
    PyObject *d = PyDict_GetItemString(meta->tp_dict, name);
    if (d == NULL || !PyObject_TypeCheck(d, &PyMethodDescr_Type))
        return NULL;
    return ((PyMethodDescrObject *)d)->d_method;
}

/* the from_param of ctypes' class 'name' */
static PyMethodDef *
class_from_param(PyObject *ctypes, const char *name)
{
    PyObject *cls = PyObject_GetAttrString(ctypes, name), *m;
    PyMethodDef *ml = NULL;
    if (cls == NULL)
        return NULL;
    m = PyObject_GetAttrString(cls, "from_param");
    if (m != NULL && PyCFunction_Check(m) && PyCFunction_GET_SELF(m) == cls)
        ml = ((PyCFunctionObject *)m)->m_ml;
    Py_XDECREF(m);
    Py_DECREF(cls);
    return ml;
}

/* the function's type tp, of the module ctypes: checks the layout with
   an instance of tp (whose new and init are CFuncPtr's) and one of
   c_int */
static int
ctypes_probe(PyTypeObject *tp)
{
    PyObject *ctypes = PyImport_GetModuleDict(), *mod_ctypes, *c_int, *c_dbl;
    PyObject *cfuncptr, *probe = NULL, *argtypes = NULL, *i7 = NULL;
    PyObject *byref = NULL, *ref = NULL;
    PyObject *exc_type, *exc_value, *exc_tb;
    le_cfuncptr_t *f;
    int ok = 0;
    mod_ctypes = PyDict_GetItemString(ctypes, "_ctypes");
    ctypes = PyDict_GetItemString(ctypes, "ctypes");
    if (mod_ctypes == NULL || ctypes == NULL)
        return 0;
    PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
    cfuncptr = PyObject_GetAttrString(mod_ctypes, "CFuncPtr");
    c_int = PyObject_GetAttrString(ctypes, "c_int");
    c_dbl = PyObject_GetAttrString(ctypes, "c_double");
    simple_base =
        (PyTypeObject *)PyObject_GetAttrString(mod_ctypes, "_SimpleCData");
    pointer_meta = NULL;
    {
        PyObject *ptr = PyObject_GetAttrString(mod_ctypes, "_Pointer");
        if (ptr != NULL) {
            pointer_meta = Py_TYPE(ptr);
            pointer_base = (PyTypeObject *)ptr;
            Py_DECREF(ptr); /* the module keeps it */
        }
    }
    if (cfuncptr == NULL || c_int == NULL || c_dbl == NULL ||
        simple_base == NULL || pointer_meta == NULL ||
        !PyType_Check(cfuncptr) || !PyType_Check(simple_base) ||
        !PyType_IsSubtype(tp, (PyTypeObject *)cfuncptr) ||
        tp->tp_new != ((PyTypeObject *)cfuncptr)->tp_new ||
        tp->tp_init != ((PyTypeObject *)cfuncptr)->tp_init)
        goto done;
    simple_meta = Py_TYPE(simple_base);
    cfuncptr_call = ((PyTypeObject *)cfuncptr)->tp_call;
    simple_from_param = meta_method(simple_meta, "from_param");
    pointer_from_param = meta_method(pointer_meta, "from_param");
    char_p_from_param = class_from_param(ctypes, "c_char_p");
    void_p_from_param = class_from_param(ctypes, "c_void_p");
    if (simple_from_param == NULL || pointer_from_param == NULL ||
        char_p_from_param == NULL || void_p_from_param == NULL)
        goto done;
    /* a function pointer at address 0x1234: its fields */
    probe = PyObject_CallFunction((PyObject *)tp, "l", 0x1234L);
    argtypes = PyTuple_Pack(1, c_int);
    if (probe == NULL || argtypes == NULL ||
        PyObject_SetAttrString(probe, "argtypes", argtypes) < 0 ||
        PyObject_SetAttrString(probe, "restype", c_dbl) < 0 ||
        PyObject_SetAttrString(probe, "errcheck", c_int) < 0)
        goto done;
    f = (le_cfuncptr_t *)probe;
    if (*(void **)f->b_ptr != (void *)0x1234L || f->argtypes != argtypes ||
        f->restype != c_dbl || f->errcheck != c_int || f->converters == NULL ||
        !PyTuple_Check(f->converters) ||
        PyTuple_GET_SIZE(f->converters) != 1 || f->paramflags != NULL)
        goto done;
    /* an int's b_ptr */
    i7 = PyObject_CallFunction(c_int, "i", 7);
    if (i7 == NULL || *(int *)((le_cfuncptr_t *)i7)->b_ptr != 7)
        goto done;
    /* byref()'s object */
    byref = PyObject_GetAttrString(mod_ctypes, "byref");
    ref = byref == NULL ? NULL : PyObject_CallFunction(byref, "Oi", i7, 4);
    if (ref == NULL || !carg_tag_found(ref, c_int, c_dbl) ||
        ((le_carg_t *)ref)->value.p != ((le_cfuncptr_t *)i7)->b_ptr + 4)
        goto done;
    for (carg_obj_offset = sizeof(le_carg_t);
         carg_obj_offset + (long)sizeof(PyObject *) <=
         Py_TYPE(ref)->tp_basicsize;
         carg_obj_offset += sizeof(PyObject *))
        if (*(PyObject **)((char *)ref + carg_obj_offset) == i7)
            break;
    if (carg_obj_offset + (long)sizeof(PyObject *) >
        Py_TYPE(ref)->tp_basicsize)
        goto done;
    carg_type = Py_TYPE(ref); /* static, or kept by the module */
    /* what byref() takes: PyCData_Type, the base of c_int's base */
    cdata_base = simple_base->tp_base;
    if (!PyCFunction_Check(byref) || cdata_base == NULL ||
        !PyObject_TypeCheck(i7, cdata_base) ||
        !PyType_IsSubtype(pointer_base, cdata_base) ||
        cdata_base == &PyBaseObject_Type)
        goto done;
    Py_INCREF(byref);
    byref_function = byref; /* kept for ever */
    INIT_SVIRTUAL(computed_byref, compute_byref, direct_compute_byref,
                  1 << iBYREF_OBJ, 0, 0);
    probe_prototypes(ctypes, c_int, c_dbl);
    ok = 1;
done:
    Py_XDECREF(ref);
    Py_XDECREF(byref);
    Py_XDECREF(i7);
    Py_XDECREF(argtypes);
    Py_XDECREF(probe);
    Py_XDECREF(cfuncptr);
    Py_XDECREF(c_int);
    Py_XDECREF(c_dbl);
    Py_XDECREF(simple_base); /* the module keeps it */
    PyErr_Restore(exc_type, exc_value, exc_tb);
    return ok ? 1 : -1;
}

DEFINEFN
bool
PsycoCtypes_IsFunction(PyTypeObject *tp, ternaryfunc call)
{
    if (ctypes_state == 0) {
        /* probed with the first function of ctypes' own call, new and
           init that is met */
        /* sys.modules, not PyImport_GetModuleDict(): that one is fatal
           once shutdown has cleared it, and compiled code still runs then */
        PyObject *modules = PySys_GetObject("modules");
        PyObject *mod, *base;
        bool candidate;
        if (modules == NULL || !PyDict_Check(modules))
            return false;
        mod = PyDict_GetItemString(modules, "_ctypes");
        if (mod == NULL || !PyModule_Check(mod))
            return false;
        /* from the module's dict: a getattr would leave the name, made
           afresh on every call, in CPython's type cache */
        base = PyDict_GetItemString(PyModule_GetDict(mod), "CFuncPtr");
        candidate = base != NULL && PyType_Check(base) &&
                    call == ((PyTypeObject *)base)->tp_call &&
                    tp->tp_new == ((PyTypeObject *)base)->tp_new &&
                    tp->tp_init == ((PyTypeObject *)base)->tp_init;
        if (!candidate)
            return false;
        ctypes_state = ctypes_probe(tp);
    }
    return ctypes_state == 1 && call == cfuncptr_call;
}

/* What a C value is, by ctypes' type code (_type_) */
typedef enum {
    K_NONE,
    K_INT,     /* an integer of 'size' bytes */
    K_DOUBLE,  /* a double */
    K_FLOAT,   /* a float */
    K_BOOL,    /* a bool, one byte */
    K_CHARP,   /* c_char_p: a char *, a bytes object or None in Python */
    K_VOIDP,   /* c_void_p: a void *, an int or None in Python */
    K_POINTER, /* a pointer */
} kind_t;

typedef struct {
    kind_t kind;
    int size;
    bool is_signed;
} ctype_t;

static ctype_t
code_ctype(char code)
{
    ctype_t t = {K_NONE, 0, false};
    switch (code) {
        case 'b':
            t.kind = K_INT, t.size = 1, t.is_signed = true;
            break;
        case 'B':
            t.kind = K_INT, t.size = 1;
            break;
        case 'h':
            t.kind = K_INT, t.size = 2, t.is_signed = true;
            break;
        case 'H':
            t.kind = K_INT, t.size = 2;
            break;
        case 'i':
            t.kind = K_INT, t.size = 4, t.is_signed = true;
            break;
        case 'I':
            t.kind = K_INT, t.size = 4;
            break;
        case 'l':
        case 'q':
            t.kind = K_INT, t.size = 8, t.is_signed = true;
            break;
        case 'L':
        case 'Q':
            t.kind = K_INT, t.size = 8;
            break;
        case 'd':
            t.kind = K_DOUBLE, t.size = 8;
            break;
        case 'f':
            t.kind = K_FLOAT, t.size = 4;
            break;
        case '?':
            t.kind = K_BOOL, t.size = 1;
            break;
        case 'z':
            t.kind = K_CHARP, t.size = 8;
            break;
        case 'P':
            t.kind = K_VOIDP, t.size = 8;
            break;
    }
    return t;
}

/* a fundamental simple type's (c_int, not a subclass) C type */
static ctype_t
simple_ctype(PyTypeObject *tp)
{
    ctype_t none = {K_NONE, 0, false};
    PyObject *code;
    const char *s;
    ctype_t t;
    if (Py_TYPE(tp) != simple_meta || tp->tp_base != simple_base)
        return none;
    code = PyObject_GetAttrString((PyObject *)tp, "_type_");
    if (code == NULL) {
        PyErr_Clear();
        return none;
    }
    s = PyUnicode_Check(code) ? PyUnicode_AsUTF8(code) : NULL;
    if (s == NULL || s[0] == 0 || s[1] != 0) {
        PyErr_Clear();
        t = none;
    }
    else
        t = code_ctype(s[0]);
    Py_DECREF(code);
    return t;
}

/* the word at 'offset' in run-time 'v', of 'size' bytes */
static vinfo_t *
load_word(PsycoObject *po, vinfo_t *v, long offset, int size, bool is_signed)
{
    switch (size * 2 + (is_signed ? 1 : 0)) {
        case 2:
            return psyco_get_field_offset(
                po, v, FMUT(DEF_ARRAY(unsigned char, 0)), offset);
        case 3:
            return psyco_get_field_offset(
                po, v, FMUT(DEF_ARRAY(signed char, 0)), offset);
        case 4:
            return psyco_get_field_offset(
                po, v, FMUT(DEF_ARRAY(unsigned short, 0)), offset);
        case 5:
            return psyco_get_field_offset(po, v, FMUT(DEF_ARRAY(short, 0)),
                                          offset);
        case 8:
            return psyco_get_field_offset(
                po, v, FMUT(DEF_ARRAY(unsigned int, 0)), offset);
        case 9:
            return psyco_get_field_offset(po, v, FMUT(DEF_ARRAY(int, 0)),
                                          offset);
        default:
            return psyco_get_field_offset(po, v, FMUT(DEF_ARRAY(long, 0)),
                                          offset);
    }
}

/* a ctypes instance's b_ptr */
static vinfo_t *
data_pointer(PsycoObject *po, vinfo_t *v)
{
    return load_word(po, v, offsetof(le_cfuncptr_t, b_ptr), 8, true);
}

static unsigned long long
cimpl_int_mask(PyObject *o)
{
    return PyLong_AsUnsignedLongLongMask(o);
}

/* whether ConvParam and PyLong_AsVoidPtr take the int o: -2**63 <= o <
   2**64 */
static long
cimpl_int_fits(PyObject *o)
{
    int overflow;
    PyLong_AsLongLongAndOverflow(o, &overflow);
    if (overflow < 0)
        return 0;
    if (overflow > 0) {
        PyLong_AsUnsignedLongLong(o);
        if (PyErr_Occurred()) {
            PyErr_Clear();
            return 0;
        }
    }
    return 1;
}

/* the low 64 bits of an exact int, as PyLong_AsUnsignedLongLongMask and
   ctypes' setters take them; if 'fits', only of an int cimpl_int_fits
   takes, 0 for any other (ctypes' own call then converts it) */
static int
int_word(PsycoObject *po, vinfo_t *v, bool fits, vinfo_t **pw)
{
    vinfo_t *f;
    condition_code_t cc;
    int r = PsycoLong_Word(po, v, pw);
    if (r != 0)
        return r;
    /* more than one digit */
    if (fits) {
        f = psyco_generic_call(
            po, cimpl_int_fits,
            CfReturnTypeLong | CfPure | CfNoPython | CfNoBlock, "v", v);
        if (f == NULL)
            return -1;
        cc = integer_non_null(po, f);
        vinfo_decref(f, po);
        if (cc == CC_ERROR)
            return -1;
        if (!runtime_condition_t(po, cc))
            return 0;
    }
    *pw = psyco_generic_call(
        po, cimpl_int_mask, CfReturnTypeLong | CfPure | CfNoPython | CfNoBlock,
        "v", v);
    return *pw == NULL ? -1 : 1;
}

/* whether the run-time word at 'offset' in 'v' is 'value': true if it is,
   false (and *error on error) on the path where it isn't */
static bool
word_is(PsycoObject *po, vinfo_t *v, long offset, int size, long value,
        bool *error)
{
    condition_code_t cc;
    vinfo_t *w = load_word(po, v, offset, size, false);
    if (w == NULL) {
        *error = true;
        return false;
    }
    cc = integer_cmp_i(po, w, value, Py_EQ);
    vinfo_decref(w, po);
    if (cc == CC_ERROR) {
        *error = true;
        return false;
    }
    return runtime_condition_t(po, cc);
}

/* a byref() result: a pointer's tag and, for argument type POINTER(T),
   an object of exactly type T (CDataType_from_param) */
static int
carg_arg(PsycoObject *po, vinfo_t *v, PyTypeObject *argtype, vinfo_t **pw)
{
    bool error = false;
    /* a virtual byref(obj): obj's buffer, its type known */
    vinfo_t *vobj = v->source == VirtualTime_New(&computed_byref)
                        ? v->array->items[iBYREF_OBJ]
                        : NULL;
    if (argtype != NULL) {
        PyObject *target =
            PyObject_GetAttrString((PyObject *)argtype, "_type_");
        bool same;
        if (target == NULL) {
            PyErr_Clear();
            return 0;
        }
        Py_DECREF(target); /* the pointer type keeps it */
        if (!PyType_Check(target))
            return 0;
        if (vobj != NULL) {
            if (Psyco_KnownType(vobj) != (PyTypeObject *)target)
                return 0;
            goto virtual;
        }
        vobj = load_word(po, v, carg_obj_offset, 8, true);
        if (vobj == NULL)
            return -1;
        same = word_is(po, vobj, offsetof(PyObject, ob_type), 8, (long)target,
                       &error);
        vinfo_decref(vobj, po);
        if (!same)
            return error ? -1 : 0;
    }
    else if (vobj != NULL)
        goto virtual;
    if (!word_is(po, v, offsetof(le_carg_t, tag), carg_tag_size, carg_tag_word,
                 &error))
        return error ? -1 : 0;
    *pw = load_word(po, v, offsetof(le_carg_t, value), 8, true);
    return *pw == NULL ? -1 : 1;
    virtual : *pw = data_pointer(po, vobj);
    return *pw == NULL ? -1 : 1;
}

/* a double as the float a C function takes: its bits in the low word */
static double
cimpl_float_bits(double x)
{
    float f = (float)x;
    double d = 0;
    memcpy(&d, &f, sizeof(f));
    return d;
}

/* a float result, its bits in the low word, as a double */
static double
cimpl_float_of_bits(double d)
{
    float f;
    memcpy(&f, &d, sizeof(f));
    return f;
}

/* a pointer to a bytes object's characters, as ctypes passes it */
static vinfo_t *
bytes_data(PsycoObject *po, vinfo_t *v)
{
    return integer_add_i(po, v, offsetof(PyBytesObject, ob_sval), false);
}

/* argument 'v' of type 'tp' as ctypes passes it for C type 't' (typed)
   or by ConvParam (untyped: t.kind == K_NONE): 1 with a new reference in
   *pw and *is_double set, 0 to take ctypes' call, -1 to stop */
static int
convert_arg(PsycoObject *po, vinfo_t *v, PyTypeObject *tp, ctype_t t,
            PyTypeObject *argtype, vinfo_t **pw, bool *is_double)
{
    vinfo_t *w, *b;
    ctype_t own;
    int r;
    *is_double = false;
    if (argtype != NULL && Py_TYPE(argtype) == pointer_meta) {
        /* POINTER(T).from_param: None, or an instance of exactly that
           type that isn't an instance of T */
        if (tp == Py_TYPE(Py_None)) {
            *pw = vinfo_new(CompileTime_New(0));
            return 1;
        }
        if (tp == carg_type)
            return carg_arg(po, v, argtype, pw);
        if (tp != argtype)
            return 0;
        b = data_pointer(po, v);
        if (b == NULL)
            return -1;
        *pw = load_word(po, b, 0, 8, true);
        vinfo_decref(b, po);
        return *pw == NULL ? -1 : 1;
    }
    if (argtype != NULL) {
        /* a simple type's from_param: an exact int or float, or an
           instance of the type itself */
        if (tp == argtype) {
            own = t;
            goto instance;
        }
        if (t.kind == K_INT && (tp == &PyLong_Type || tp == &PyBool_Type)) {
            r = int_word(po, v, false, &w);
            if (r != 1)
                return r;
            *pw = integer_extend(po, w, t.size, t.is_signed);
            vinfo_decref(w, po);
            return *pw == NULL ? -1 : 1;
        }
        if (t.kind == K_DOUBLE && tp == &PyFloat_Type) {
            *pw = PsycoFloat_AS_DOUBLE(po, v);
            if (*pw == NULL)
                return -1;
            vinfo_incref(*pw);
            *is_double = true;
            return 1;
        }
        if (t.kind == K_FLOAT && tp == &PyFloat_Type) {
            b = PsycoFloat_AS_DOUBLE(po, v);
            if (b == NULL)
                return -1;
            *pw = psyco_generic_call(
                po, cimpl_float_bits,
                CfReturnTypeDouble | CfPure | CfNoPython | CfNoBlock, "d", b);
            *is_double = true;
            return *pw == NULL ? -1 : 1;
        }
        if (t.kind == K_BOOL && (tp == &PyLong_Type || tp == &PyBool_Type)) {
            /* bool_set: the int's truth; one of more than one digit is
               never 0 */
            condition_code_t cc;
            r = PsycoLong_Word(po, v, &w);
            if (r < 0)
                return r;
            if (r == 0) {
                *pw = vinfo_new(CompileTime_New(1));
                return 1;
            }
            cc = integer_non_null(po, w);
            vinfo_decref(w, po);
            if (cc == CC_ERROR)
                return -1;
            *pw = psyco_vinfo_condition(po, cc);
            return *pw == NULL ? -1 : 1;
        }
        if (t.kind == K_CHARP || t.kind == K_VOIDP) {
            /* c_char_p's and c_void_p's from_param: None, bytes, and for
               c_void_p an int, its address */
            if (tp == Py_TYPE(Py_None)) {
                *pw = vinfo_new(CompileTime_New(0));
                return 1;
            }
            if (tp == &PyBytes_Type) {
                *pw = bytes_data(po, v);
                return *pw == NULL ? -1 : 1;
            }
            if (t.kind == K_VOIDP &&
                (tp == &PyLong_Type || tp == &PyBool_Type))
                return int_word(po, v, true, pw);
        }
        return 0;
    }
    /* untyped (ConvParam) */
    if (tp == Py_TYPE(Py_None)) {
        *pw = vinfo_new(CompileTime_New(0));
        return 1;
    }
    if (tp == &PyLong_Type || tp == &PyBool_Type) {
        /* a C int */
        r = int_word(po, v, true, &w);
        if (r != 1)
            return r;
        *pw = integer_extend(po, w, 4, true);
        vinfo_decref(w, po);
        return *pw == NULL ? -1 : 1;
    }
    if (tp == carg_type)
        return carg_arg(po, v, NULL, pw);
    if (tp == &PyBytes_Type) {
        *pw = bytes_data(po, v);
        return *pw == NULL ? -1 : 1;
    }
    if (Py_TYPE(tp) == pointer_meta) {
        b = data_pointer(po, v);
        if (b == NULL)
            return -1;
        *pw = load_word(po, b, 0, 8, true);
        vinfo_decref(b, po);
        return *pw == NULL ? -1 : 1;
    }
    own = simple_ctype(tp);
    if (own.kind == K_NONE)
        return 0;
instance:
    b = data_pointer(po, v);
    if (b == NULL)
        return -1;
    *pw = load_word(po, b, 0, own.size, own.is_signed);
    vinfo_decref(b, po);
    /* a float's bits, zero-extended, as for a float argument */
    *is_double = own.kind == K_DOUBLE || own.kind == K_FLOAT;
    return *pw == NULL ? -1 : 1;
}

/* a guard that the word at 'offset' in run-time 'vf' is still 'value':
   true if it holds, false on the path where it doesn't (or on error) */
static bool
field_unchanged(PsycoObject *po, vinfo_t *vf, long offset, PyObject *value,
                bool *error)
{
    condition_code_t cc;
    vinfo_t *w = load_word(po, vf, offset, 8, true);
    if (w == NULL) {
        *error = true;
        return false;
    }
    cc = integer_cmp_i(po, w, (long)value, Py_EQ);
    vinfo_decref(w, po);
    if (cc == CC_ERROR) {
        *error = true;
        return false;
    }
    return runtime_condition_t(po, cc);
}

#if LE_HAS_CTYPES_CALL_AUDITED
/* PySys_Audit makes an event's arguments only when it has a hook to call
   them with: this converter then stops it, and ctypes' own call raises the
   event instead */
static PyObject *
cimpl_hooked(void *hooked)
{
    *(int *)hooked = 1;
    PyErr_SetNone(PyExc_RuntimeError);
    return NULL;
}

static int
cimpl_audit_hooked(void)
{
    int hooked = 0;
    if (PySys_Audit("ctypes.call_function", "O&", cimpl_hooked, &hooked) < 0)
        PyErr_Clear();
    return hooked;
}
#endif

/* true where no audit hook would see the call, as ctypes audits it (on
   the path where one would, false, and ctypes' call is made) */
static bool
not_audited(PsycoObject *po, bool *error)
{
#if LE_HAS_CTYPES_CALL_AUDITED
    condition_code_t cc;
    void *runtime_head, *interp_hooks;
    vinfo_t *v;
    if (le_audit_hook_words(&runtime_head, &interp_hooks)) {
        /* hooks are never removed while the program runs */
        vinfo_t *a = vinfo_new(CompileTime_New((long)runtime_head));
        vinfo_t *b = vinfo_new(CompileTime_New((long)interp_hooks));
        vinfo_t *wa = load_word(po, a, 0, 8, true);
        vinfo_t *wb = wa == NULL ? NULL : load_word(po, b, 0, 8, true);
        vinfo_decref(a, po);
        vinfo_decref(b, po);
        v = wb == NULL ? NULL : integer_or(po, wa, wb);
        vinfo_xdecref(wa, po);
        vinfo_xdecref(wb, po);
    }
    else
        v = psyco_generic_call(po, cimpl_audit_hooked,
                               CfReturnTypeInt | CfNoPython | CfNoBlock, "");
    if (v == NULL) {
        *error = true;
        return false;
    }
    cc = integer_non_null(po, v);
    vinfo_decref(v, po);
    if (cc == CC_ERROR) {
        *error = true;
        return false;
    }
    return !runtime_condition_f(po, cc);
#else
    return true;
#endif
}

/* P_get: None for NULL, else PyLong_FromVoidPtr (consumes r) */
static vinfo_t *
void_p_result(PsycoObject *po, vinfo_t *r)
{
    vinfo_t *o;
    condition_code_t cc = integer_cmp_i(po, r, 0, Py_EQ);
    if (cc == CC_ERROR)
        goto error;
    if (runtime_condition_f(po, cc)) {
        vinfo_decref(r, po);
        return psyco_vi_None();
    }
    cc = integer_cmp_i(po, r, 0, Py_LT);
    if (cc == CC_ERROR)
        goto error;
    if (runtime_condition_f(po, cc)) {
        o = PsycoLong_FromUnsignedLong(po, r);
        vinfo_decref(r, po);
        return o;
    }
    return PsycoLong_FROM_LONG(po, r);
error:
    vinfo_decref(r, po);
    return NULL;
}

/* z_get */
static PyObject *
cimpl_charp_result(const char *p)
{
    if (p == NULL)
        Py_RETURN_NONE;
    return PyBytes_FromStringAndSize(p, strlen(p));
}

/* PyCData_FromBaseObj for a pointer type tp, with no base: a new
   instance holding a copy of the pointer in its own buffer */
static PyObject *
cimpl_pointer_result(PyTypeObject *tp, Py_ssize_t length, void *p)
{
    le_cfuncptr_t *r = (le_cfuncptr_t *)tp->tp_alloc(tp, 0);
    if (r == NULL)
        return NULL;
    r->b_length = length;
    r->b_size = sizeof(void *);
    r->b_ptr = r->b_value.c;
    r->b_needsfree = 1;
    *(void **)r->b_ptr = p;
    return (PyObject *)r;
}

/* whether instances of the pointer type tp are made as
   cimpl_pointer_result makes them: checked against ctypes' own copy
   (from_buffer_copy, which also makes the type final as GetResult does),
   whose length is put in *length */
static bool
pointer_result_length(PyTypeObject *tp, Py_ssize_t *length)
{
    PyObject *zeros;
    le_cfuncptr_t *c;
    bool ok;
    if (tp->tp_alloc != pointer_base->tp_alloc ||
        PyDict_GetItemString(tp->tp_dict, "from_buffer_copy") != NULL)
        return false;
    zeros = PyBytes_FromStringAndSize(NULL, sizeof(void *));
    if (zeros == NULL) {
        PyErr_Clear();
        return false;
    }
    memset(PyBytes_AS_STRING(zeros), 0, sizeof(void *));
    c = (le_cfuncptr_t *)PyObject_CallMethod((PyObject *)tp,
                                             "from_buffer_copy", "O", zeros);
    Py_DECREF(zeros);
    if (c == NULL) {
        PyErr_Clear();
        return false;
    }
    ok = Py_TYPE(c) == tp && c->b_ptr == c->b_value.c && c->b_needsfree == 1 &&
         c->b_base == NULL && c->b_size == sizeof(void *) && c->b_index == 0 &&
         c->b_objects == NULL;
    *length = c->b_length;
    Py_DECREF(c);
    return ok;
}

DEFINEFN
vinfo_t *
PsycoCtypes_Byref(PsycoObject *po, PyMethodDef *ml, vinfo_t *vargs)
{
    vinfo_t *vobj, *v;
    PyTypeObject *tp;
    if (strcmp(ml->ml_name, "byref") != 0)
        return NULL;
    if (ctypes_state == 0) {
        /* probed with a function ctypes makes itself, memmove */
        PyObject *modules = PySys_GetObject("modules");
        PyObject *mod, *f;
        if (modules == NULL || !PyDict_Check(modules))
            return NULL;
        mod = PyDict_GetItemString(modules, "ctypes");
        if (mod == NULL || !PyModule_Check(mod))
            return NULL;
        /* from the module's dict, as for CFuncPtr */
        f = PyDict_GetItemString(PyModule_GetDict(mod), "memmove");
        if (f == NULL)
            return NULL;
        Py_INCREF(f); /* the probe runs Python */
        PsycoCtypes_IsFunction(Py_TYPE(f), Py_TYPE(f)->tp_call);
        Py_DECREF(f);
    }
    if (ctypes_state != 1 || ml != ((PyCFunctionObject *)byref_function)->m_ml)
        return NULL;
    if (!is_virtualtime(vargs->source) || PsycoTuple_Load(vargs) != 1)
        return NULL;
    vobj = PsycoTuple_GET_ITEM(vargs, 0);
    tp = Psyco_NeedType(po, vobj);
    if (tp == NULL || !PyType_IsSubtype(tp, cdata_base))
        return NULL;
    v = vinfo_new(VirtualTime_New(&computed_byref));
    v->array = array_new(BYREF_TOTAL);
    v->array->items[iOB_TYPE] = vinfo_new(CompileTime_New((long)carg_type));
    vinfo_incref(vobj);
    v->array->items[iBYREF_OBJ] = vobj;
    return v;
}

#define MAX_INT_ARGS 6
#define MAX_ARGS 8

/* The fields of a ctypes function a compile decides from (references; NULL
   as they were), and what reading its restype and argtypes gave then: the
   fields, or its type's, which the function keeps (borrowed) */
#define N_FIELDS 6
typedef struct {
    PyObject *fields[N_FIELDS]; /* converters, argtypes, restype, checker,
                                   errcheck, paramflags */
    PyObject *restype;
    PyObject *argtypes;
    bool changed;
    unsigned long changed_at; /* the first block serial after LeJIT saw the
                                 fields change */
} seen_fields_t;

static void
function_fields(le_cfuncptr_t *fp, PyObject *out[N_FIELDS])
{
    out[0] = fp->converters;
    out[1] = fp->argtypes;
    out[2] = fp->restype;
    out[3] = fp->checker;
    out[4] = fp->errcheck;
    out[5] = fp->paramflags;
}

static void
seen_fields_free(PyObject *capsule)
{
    seen_fields_t *s = PyCapsule_GetPointer(capsule, NULL);
    int i;
    for (i = 0; i < N_FIELDS; i++) Py_XDECREF(s->fields[i]);
    PyMem_Free(s);
}

/* What a compile of a call of 'f' decides from, in a way that a replay
   repeats (see psyco_block_serial): the fields LeJIT first saw, in blocks
   older than the moment it first saw them changed; NULL in later blocks,
   which take ctypes' own call. A replay can't read them afresh: the
   change that sent the code down a new path may have come after its
   block was compiled. What LeJIT saw is a record (psyco_record), held by
   the code that relied on it. */
static seen_fields_t *
seen_fields(PsycoObject *po, PyObject *f)
{
    static le_addrmap_t seen;
    PyObject **slot = psyco_record(&seen, f);
    PyObject *now[N_FIELDS];
    seen_fields_t *s;
    int i;
    function_fields((le_cfuncptr_t *)f, now);
    if (*slot == NULL) {
        s = PyMem_Calloc(1, sizeof(seen_fields_t));
        if (s == NULL)
            OUT_OF_MEMORY();
        for (i = 0; i < N_FIELDS; i++) {
            Py_XINCREF(now[i]);
            s->fields[i] = now[i];
        }
        s->restype = PyObject_GetAttrString(f, "restype");
        Py_XDECREF(s->restype);
        s->argtypes = PyObject_GetAttrString(f, "argtypes");
        Py_XDECREF(s->argtypes);
        PyErr_Clear();
        *slot = PyCapsule_New(s, NULL, seen_fields_free);
        if (*slot == NULL) {
            for (i = 0; i < N_FIELDS; i++) Py_XDECREF(s->fields[i]);
            PyMem_Free(s);
            OUT_OF_MEMORY();
        }
    }
    s = PyCapsule_GetPointer(*slot, NULL);
    for (i = 0; i < N_FIELDS && !s->changed; i++)
        if (now[i] != s->fields[i]) {
            s->changed = true;
            s->changed_at = psyco_next_serial;
        }
    if (s->changed && psyco_block_serial(po) >= s->changed_at)
        return NULL;
    return s;
}

DEFINEFN
vinfo_t *
PsycoCtypes_Call(PsycoObject *po, PyTypeObject *tp, vinfo_t *vf,
                 vinfo_t *vargs, vinfo_t *kw)
{
    PyObject *f, *flags, *restype, *converters;
    seen_fields_t *fs;
    vinfo_t *w[MAX_ARGS], *vptr, *vfn, *vts, *r;
    char tags[MAX_ARGS + 1];
    int n, i, nints = 0, ok;
    long lflags;
    Py_ssize_t rlength = 0;
    ctype_t rt = {K_NONE, 0, false}, at[MAX_ARGS];
    PyTypeObject *argtypes[MAX_ARGS];
    bool error = false, is_double;

    if (!psyco_knowntobe(kw, 0) || !is_virtualtime(vargs->source))
        return NULL;
    n = PsycoTuple_Load(vargs);
    if (n < 0 || n > MAX_ARGS)
        return NULL;
    if (is_megamorphic(vf->source))
        return NULL;
    switch (psyco_pyobj_atcompiletime_mega(po, vf, &f)) {
        case 1:
            break;
        case 0:
            return NULL;
        default:
            return NULL;
    }
    for (i = 0; i < n; i++)
        if (Psyco_NeedType(po, PsycoTuple_GET_ITEM(vargs, i)) == NULL)
            return NULL;

    /* what ctypes' call reads */
    fs = seen_fields(po, f);
    if (fs == NULL || fs->fields[4] != NULL || fs->fields[3] != NULL ||
        fs->fields[5] != NULL)
        return NULL;
    flags = PyObject_GetAttrString((PyObject *)tp, "_flags_");
    if (flags == NULL) {
        PyErr_Clear();
        return NULL;
    }
    lflags = PyLong_Check(flags) ? PyLong_AsLong(flags) : -1;
    Py_DECREF(flags);
    if (lflags != FUNCFLAG_CDECL) {
        PyErr_Clear();
        return NULL;
    }
    restype = fs->restype;
    if (restype == NULL)
        return NULL;
    if (restype != Py_None) {
        if (!PyType_Check(restype))
            return NULL;
        if (Py_TYPE(restype) == pointer_meta &&
            ((PyTypeObject *)restype)->tp_base == pointer_base) {
            if (!pointer_result_length((PyTypeObject *)restype, &rlength))
                return NULL;
            rt.kind = K_POINTER;
            rt.size = 8;
        }
        else
            rt = simple_ctype((PyTypeObject *)restype);
        if (rt.kind == K_NONE)
            return NULL;
        if (PyObject_HasAttrString(restype, "_check_retval_"))
            return NULL;
    }
    converters = fs->fields[0];
    if (converters == NULL) {
        /* untyped, unless its type (a prototype) has argtypes */
        PyObject *a = fs->argtypes;
        if (a == NULL)
            return NULL;
        if (a != Py_None) {
            /* the type's, set when it was made */
            PyObject **pf = proto_fields(tp);
            if (pf == NULL || pf[0] != a || pf[3] != NULL)
                return NULL;
            converters = pf[1];
        }
        else
            for (i = 0; i < n; i++) argtypes[i] = NULL, at[i].kind = K_NONE;
    }
    if (converters != NULL) {
        if (PyTuple_GET_SIZE(converters) != n)
            return NULL;
        for (i = 0; i < n; i++) {
            PyObject *c = PyTuple_GET_ITEM(converters, i);
            PyTypeObject *a;
            if (!PyCFunction_Check(c) || PyCFunction_GET_SELF(c) == NULL ||
                !PyType_Check(PyCFunction_GET_SELF(c)))
                return NULL;
            a = (PyTypeObject *)PyCFunction_GET_SELF(c);
            argtypes[i] = a;
            if (((PyCFunctionObject *)c)->m_ml == pointer_from_param &&
                Py_TYPE(a) == pointer_meta)
                at[i].kind = K_POINTER;
            else if (((PyCFunctionObject *)c)->m_ml == simple_from_param) {
                at[i] = simple_ctype(a);
                if (at[i].kind == K_NONE || at[i].kind == K_CHARP ||
                    at[i].kind == K_VOIDP)
                    return NULL;
            }
            else if (((PyCFunctionObject *)c)->m_ml == char_p_from_param ||
                     ((PyCFunctionObject *)c)->m_ml == void_p_from_param) {
                at[i] = simple_ctype(a);
                if (at[i].kind !=
                    (((PyCFunctionObject *)c)->m_ml == char_p_from_param
                         ? K_CHARP
                         : K_VOIDP))
                    return NULL;
            }
            else
                return NULL;
        }
    }

    /* the arguments; each kind's guard comes before anything run */
    for (i = 0; i < n; i++) {
        vinfo_t *v = PsycoTuple_GET_ITEM(vargs, i);
        ok = convert_arg(po, v, Psyco_KnownType(v), at[i], argtypes[i], &w[i],
                         &is_double);
        if (ok != 1) {
            while (i--) vinfo_decref(w[i], po);
            return NULL;
        }
        tags[i] = is_double ? 'd' : 'v';
        if (!is_double && ++nints > MAX_INT_ARGS) {
            for (; i >= 0; i--) vinfo_decref(w[i], po);
            return NULL;
        }
    }
    tags[n] = 0;

    /* the fields compiled for are still the function's (the record holds
       them) */
    if (!field_unchanged(po, vf, offsetof(le_cfuncptr_t, converters),
                         fs->fields[0], &error) ||
        !field_unchanged(po, vf, offsetof(le_cfuncptr_t, restype),
                         fs->fields[2], &error) ||
        !field_unchanged(po, vf, offsetof(le_cfuncptr_t, checker), NULL,
                         &error) ||
        !field_unchanged(po, vf, offsetof(le_cfuncptr_t, errcheck), NULL,
                         &error) ||
        !not_audited(po, &error)) {
        for (i = 0; i < n; i++) vinfo_decref(w[i], po);
        if (error)
            return NULL;
        return NULL; /* ctypes' call, on this path */
    }

    vptr = data_pointer(po, vf);
    if (vptr == NULL)
        goto fail;
    vfn = load_word(po, vptr, 0, 8, true);
    vinfo_decref(vptr, po);
    if (vfn == NULL)
        goto fail;

    /* the GIL released around the call, as ctypes releases it */
    vts = le_gil_release_call(po);
    if (vts == NULL) {
        vinfo_decref(vfn, po);
        goto fail;
    }
    {
        int cflags = rt.kind == K_NONE ? CfNoReturnValue
                     : rt.kind == K_DOUBLE || rt.kind == K_FLOAT
                         ? CfReturnTypeDouble
                         : CfReturnTypeLong;
        cflags |= CfNoBlock;
        switch (n) {
#define W(k) w[k]
            case 0:
                r = psyco_call_address(po, vfn, cflags, tags);
                break;
            case 1:
                r = psyco_call_address(po, vfn, cflags, tags, W(0));
                break;
            case 2:
                r = psyco_call_address(po, vfn, cflags, tags, W(0), W(1));
                break;
            case 3:
                r = psyco_call_address(po, vfn, cflags, tags, W(0), W(1),
                                       W(2));
                break;
            case 4:
                r = psyco_call_address(po, vfn, cflags, tags, W(0), W(1), W(2),
                                       W(3));
                break;
            case 5:
                r = psyco_call_address(po, vfn, cflags, tags, W(0), W(1), W(2),
                                       W(3), W(4));
                break;
            case 6:
                r = psyco_call_address(po, vfn, cflags, tags, W(0), W(1), W(2),
                                       W(3), W(4), W(5));
                break;
            case 7:
                r = psyco_call_address(po, vfn, cflags, tags, W(0), W(1), W(2),
                                       W(3), W(4), W(5), W(6));
                break;
            default:
                r = psyco_call_address(po, vfn, cflags, tags, W(0), W(1), W(2),
                                       W(3), W(4), W(5), W(6), W(7));
                break;
#undef W
        }
    }
    vinfo_decref(vfn, po);
    for (i = 0; i < n; i++) vinfo_decref(w[i], po);
    n = 0;
    if (r == NULL || !le_gil_take_call(po, vts)) {
        if (r != NULL && r != VINFO_OK)
            vinfo_decref(r, po);
        vinfo_decref(vts, po);
        return NULL;
    }
    vinfo_decref(vts, po);

    /* the result, as GetResult makes it */
    switch (rt.kind) {
        case K_NONE:
            return psyco_vi_None();
        case K_DOUBLE:
            return PsycoFloat_FROM_DOUBLE(r);
        case K_FLOAT: {
            vinfo_t *x = psyco_generic_call(
                po, cimpl_float_of_bits,
                CfReturnTypeDouble | CfPure | CfNoPython | CfNoBlock, "d", r);
            vinfo_decref(r, po);
            return x == NULL ? NULL : PsycoFloat_FROM_DOUBLE(x);
        }
        case K_BOOL: {
            /* bool_get: the byte's truth */
            vinfo_t *x = integer_extend(po, r, 1, false), *o;
            vinfo_decref(r, po);
            if (x == NULL)
                return NULL;
            o = PsycoBool_FromLong(po, x);
            vinfo_decref(x, po);
            return o;
        }
        case K_CHARP: {
            vinfo_t *o = psyco_generic_call(
                po, cimpl_charp_result,
                CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "v", r);
            vinfo_decref(r, po);
            return o;
        }
        case K_VOIDP:
            return void_p_result(po, r);
        case K_POINTER: {
            /* a collection the allocation starts may run finalisers */
            vinfo_t *o = psyco_generic_call(
                po, cimpl_pointer_result, CfCommonNewRefPyObject | CfNoBlock,
                "llv", (long)restype, (long)rlength, r);
            vinfo_decref(r, po);
            return o;
        }
        default: {
            vinfo_t *x = integer_extend(po, r, rt.size, rt.is_signed);
            vinfo_decref(r, po);
            if (x == NULL)
                return NULL;
            if (rt.size == 8 && !rt.is_signed) {
                vinfo_t *o = PsycoLong_FromUnsignedLong(po, x);
                vinfo_decref(x, po);
                return o;
            }
            return PsycoLong_FROM_LONG(po, x);
        }
    }
fail:
    for (i = 0; i < n; i++) vinfo_decref(w[i], po);
    return NULL;
}
