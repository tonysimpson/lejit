/*
 * Direct calls of cffi functions. A call of a function of a cffi API-mode
 * module (a builtin whose PyMethodDef is cffi's, its self the module's
 * Lib) or of a function pointer cdata (ABI mode, ffi.dlopen), whose
 * arguments are all of kinds cffi converts without running Python code,
 * calls the C function from compiled code as cffi's own wrapper does: the
 * GIL released, cffi's saved errno restored, the call, errno saved, the
 * GIL taken back. No tuple, libffi call or converter call is made, and an
 * int or double result stays unboxed.
 *
 * Arguments done here: an exact int or bool in the C integer type's range
 * (0 or 1 for _Bool), an exact float or int for a double, and a cdata
 * whose C type is exactly the parameter's pointer type. Results: void,
 * integers, _Bool, double and pointers. Any other argument, or a value out
 * of range, takes cffi's own call at run time, so every conversion, error
 * and message is cffi's.
 *
 * It relies on cffi's layouts (src/c/_cffi_backend.c and lib_obj.c, the
 * same in cffi 1.15 to 2.1): a cdata's c_type and c_data, a C type's
 * fields up to ct_flags and the CT_ flags, the CPyExtFunc_s holding an
 * API-mode function's PyMethodDef, and the _cffi_exports table compiled
 * modules call. They are checked against the running cffi once
 * (cffi_probe), and an API-mode function's against ffi.addressof(); if
 * any differs, nothing is called directly.
 */
#include "pcffi.h"
#include "../Objects/pboolobject.h"
#include "../Objects/pfloatobject.h"
#include "../Objects/plongobject.h"
#include "../Objects/ptupleobject.h"
#include "../Python/pycompiler.h"
#include "../codemanager.h"
#include "../mechanisms/tstate.h"
#include "../mechanisms/gil.h"

/* cffi's CTypeDescrObject up to ct_flags, and CDataObject */
typedef struct {
    PyObject_VAR_HEAD void *ct_itemdescr;
    PyObject *ct_stuff; /* a function type's (abi, result, args...) */
    void *ct_extra;     /* a function type's libffi cif: NULL if variadic */
    PyObject *ct_weakreflist;
    PyObject *ct_unique_key;
    Py_ssize_t ct_size;
    Py_ssize_t ct_length;
    int ct_flags;
} le_ctype_t;

typedef struct {
    PyObject_HEAD le_ctype_t *c_type;
    char *c_data;
} le_cdata_t;

#define CT_PRIMITIVE_SIGNED 0x001
#define CT_PRIMITIVE_UNSIGNED 0x002
#define CT_PRIMITIVE_FLOAT 0x008
#define CT_POINTER 0x010
#define CT_FUNCTIONPTR 0x100
#define CT_VOID 0x200
#define CT_PRIMITIVE_COMPLEX 0x400
#define CT_IS_LONGDOUBLE 0x00040000
#define CT_IS_BOOL 0x00080000
#define CT_KIND_FLAGS                                                   \
    (CT_PRIMITIVE_SIGNED | CT_PRIMITIVE_UNSIGNED | CT_PRIMITIVE_FLOAT | \
     CT_POINTER | CT_FUNCTIONPTR | CT_VOID | CT_PRIMITIVE_COMPLEX |     \
     CT_IS_LONGDOUBLE | CT_IS_BOOL)

/* the _cffi_exports entries (cffi/_cffi_include.h) */
#define EXPORT_FROM_C_POINTER 10
#define EXPORT_RESTORE_ERRNO 13
#define EXPORT_SAVE_ERRNO 14

/* 1 usable, -1 not, 0 not yet probed */
static int cffi_state = 0;
static PyTypeObject *cdata_type, *ctype_type;
static PyObject *cffi_ffi; /* an FFI of LeJIT's own, for addressof() */
static PyObject *default_abi;
static void **cffi_exports;

/* ffi.typeof(name), a new reference, if it is a C type of size 'size'
   whose kind flags are 'flags' */
static PyObject *
probe_type(const char *name, Py_ssize_t size, int flags)
{
    PyObject *t = PyObject_CallMethod(cffi_ffi, "typeof", "s", name);
    if (t == NULL)
        return NULL;
    if (Py_TYPE(t) != ctype_type ||
        (size >= 0 && ((le_ctype_t *)t)->ct_size != size) ||
        (((le_ctype_t *)t)->ct_flags & CT_KIND_FLAGS) != flags) {
        Py_DECREF(t);
        return NULL;
    }
    return t;
}

/* the layouts above, checked against the _cffi_backend module 'mod' */
static int
cffi_probe(PyObject *mod)
{
    PyObject *ffitype, *cap, *null;
    PyObject *t_int = NULL, *t_dbl = NULL, *t_intp = NULL, *t_fn = NULL;
    PyObject *t_varfn = NULL, *other = NULL, *seven = NULL;
    le_ctype_t *fn;
    int ok = 0;

    ffitype = PyObject_GetAttrString(mod, "FFI");
    cap = PyObject_GetAttrString(mod, "_C_API");
    if (ffitype == NULL || cap == NULL)
        goto done;
    cffi_exports = PyCapsule_GetPointer(cap, "cffi");
    cffi_ffi = PyObject_CallObject(ffitype, NULL);
    if (cffi_exports == NULL || cffi_ffi == NULL)
        goto done;
    null = PyObject_GetAttrString(cffi_ffi, "NULL");
    if (null == NULL)
        goto done;
    cdata_type = Py_TYPE(null); /* static */
    Py_DECREF(null);
    t_int = PyObject_CallMethod(cffi_ffi, "typeof", "s", "int");
    if (t_int == NULL)
        goto done;
    ctype_type = Py_TYPE(t_int); /* static */
    Py_DECREF(t_int);

    /* each kind's size and flags */
    t_int = probe_type("int", sizeof(int), CT_PRIMITIVE_SIGNED);
    t_dbl = probe_type("double", sizeof(double), CT_PRIMITIVE_FLOAT);
    t_intp = probe_type("int *", sizeof(void *), CT_POINTER);
    if (t_int == NULL || t_dbl == NULL || t_intp == NULL)
        goto done;
    if ((other = probe_type("unsigned short", 2, CT_PRIMITIVE_UNSIGNED)) ==
        NULL)
        goto done;
    Py_DECREF(other);
    if ((other = probe_type("_Bool", 1, CT_PRIMITIVE_UNSIGNED | CT_IS_BOOL)) ==
        NULL)
        goto done;
    Py_DECREF(other);
    if ((other = probe_type("long double", -1,
                            CT_PRIMITIVE_FLOAT | CT_IS_LONGDOUBLE)) == NULL)
        goto done;
    Py_DECREF(other);
    if ((other = probe_type("void", -1, CT_VOID)) == NULL)
        goto done;
    Py_DECREF(other);
    other = NULL;

    /* a function type's signature, and a variadic one's missing cif */
    t_fn = probe_type("int(*)(int, double)", sizeof(void *), CT_FUNCTIONPTR);
    t_varfn = probe_type("int(*)(int, ...)", sizeof(void *), CT_FUNCTIONPTR);
    if (t_fn == NULL || t_varfn == NULL)
        goto done;
    fn = (le_ctype_t *)t_fn;
    if (fn->ct_extra == NULL || ((le_ctype_t *)t_varfn)->ct_extra != NULL ||
        fn->ct_stuff == NULL || !PyTuple_Check(fn->ct_stuff) ||
        PyTuple_GET_SIZE(fn->ct_stuff) != 4 ||
        !PyLong_Check(PyTuple_GET_ITEM(fn->ct_stuff, 0)) ||
        PyTuple_GET_ITEM(fn->ct_stuff, 1) != t_int ||
        PyTuple_GET_ITEM(fn->ct_stuff, 2) != t_int ||
        PyTuple_GET_ITEM(fn->ct_stuff, 3) != t_dbl)
        goto done;
    default_abi = PyTuple_GET_ITEM(fn->ct_stuff, 0);
    Py_INCREF(default_abi);

    /* a cdata's type and data */
    seven = PyObject_CallMethod(cffi_ffi, "new", "si", "int *", 7);
    if (seven == NULL || !PyType_IsSubtype(Py_TYPE(seven), cdata_type) ||
        ((le_cdata_t *)seven)->c_type != (le_ctype_t *)t_intp ||
        *(int *)((le_cdata_t *)seven)->c_data != 7)
        goto done;
    ok = 1;
done:
    Py_XDECREF(seven);
    Py_XDECREF(t_varfn);
    Py_XDECREF(t_fn);
    Py_XDECREF(t_intp);
    Py_XDECREF(t_dbl);
    Py_XDECREF(t_int);
    Py_XDECREF(cap); /* the module keeps the table */
    Py_XDECREF(ffitype);
    PyErr_Clear();
    return ok ? 1 : -1;
}

/* whether cffi's layouts are the expected ones: probed the first time
   it is asked once _cffi_backend is loaded */
static bool
cffi_usable(void)
{
    if (cffi_state == 0) {
        /* sys.modules, not PyImport_GetModuleDict(): that one is fatal
           once shutdown has cleared it, and compiled code still runs then */
        PyObject *modules = PySys_GetObject("modules");
        PyObject *mod, *exc_type, *exc_value, *exc_tb;
        if (modules == NULL || !PyDict_Check(modules))
            return false;
        mod = PyDict_GetItemString(modules, "_cffi_backend");
        if (mod == NULL)
            return false;
        PyErr_Fetch(&exc_type, &exc_value, &exc_tb);
        cffi_state = cffi_probe(mod);
        PyErr_Restore(exc_type, exc_value, exc_tb);
    }
    return cffi_state == 1;
}

DEFINEFN
bool
PsycoCffi_IsCData(PyTypeObject *tp)
{
    if (cffi_state == 0 && strcmp(tp->tp_name, "_cffi_backend._CDataBase"))
        return false;
    /* cffi's cdata types are static and can't be subclassed */
    return cffi_usable() && PyType_IsSubtype(tp, cdata_type);
}

/* What a C value is */
typedef enum {
    K_VOID,
    K_INT, /* an integer of 'size' bytes */
    K_BOOL,
    K_DOUBLE,
    K_POINTER, /* a pointer of type 'ct' */
} kind_t;

typedef struct {
    kind_t kind;
    int size;
    bool is_signed;
    le_ctype_t *ct;
} ctype_t;

/* C type 'ct' as a parameter or result: false if not one done here */
static bool
describe_type(PyObject *ct, bool is_result, ctype_t *out)
{
    le_ctype_t *t = (le_ctype_t *)ct;
    int fl;
    if (Py_TYPE(ct) != ctype_type)
        return false;
    fl = t->ct_flags & CT_KIND_FLAGS;
    out->ct = t;
    out->size = (int)t->ct_size;
    out->is_signed = (fl & CT_PRIMITIVE_SIGNED) != 0;
    if (fl == CT_VOID) {
        out->kind = K_VOID;
        return is_result;
    }
    if (fl == (CT_PRIMITIVE_UNSIGNED | CT_IS_BOOL)) {
        out->kind = K_BOOL;
        return t->ct_size == 1;
    }
    if (fl == CT_PRIMITIVE_SIGNED || fl == CT_PRIMITIVE_UNSIGNED) {
        out->kind = K_INT;
        return t->ct_size == 1 || t->ct_size == 2 || t->ct_size == 4 ||
               t->ct_size == 8;
    }
    if (fl == CT_PRIMITIVE_FLOAT) {
        out->kind = K_DOUBLE;
        return t->ct_size == sizeof(double);
    }
    if (fl == CT_POINTER) {
        out->kind = K_POINTER;
        return t->ct_size == sizeof(void *);
    }
    return false;
}

#define MAX_ARGS 8
#define MAX_INT_ARGS 6
#define MAX_DOUBLE_ARGS 8

typedef struct {
    void *fn;
    le_ctype_t *fct;
    int nargs;
    ctype_t result;
    ctype_t args[MAX_ARGS];
} signature_t;

/* function type 'fct' at address 'fn': false if not one done here */
static bool
describe_function(le_ctype_t *fct, void *fn, signature_t *sig)
{
    PyObject *stuff = fct->ct_stuff;
    Py_ssize_t i, n;
    int nints = 0, ndoubles = 0;
    if (Py_TYPE(fct) != ctype_type ||
        (fct->ct_flags & CT_KIND_FLAGS) != CT_FUNCTIONPTR ||
        fct->ct_extra == NULL || fn == NULL || stuff == NULL ||
        !PyTuple_Check(stuff) || PyTuple_GET_SIZE(stuff) < 2)
        return false;
    /* cffi calls with libffi's default ABI unless told otherwise */
    if (PyObject_RichCompareBool(PyTuple_GET_ITEM(stuff, 0), default_abi,
                                 Py_EQ) != 1) {
        PyErr_Clear();
        return false;
    }
    n = PyTuple_GET_SIZE(stuff) - 2;
    if (n > MAX_ARGS)
        return false;
    sig->fn = fn;
    sig->fct = fct;
    sig->nargs = (int)n;
    if (!describe_type(PyTuple_GET_ITEM(stuff, 1), true, &sig->result))
        return false;
    for (i = 0; i < n; i++) {
        if (!describe_type(PyTuple_GET_ITEM(stuff, 2 + i), false,
                           &sig->args[i]))
            return false;
        if (sig->args[i].kind == K_DOUBLE ? ++ndoubles > MAX_DOUBLE_ARGS
                                          : ++nints > MAX_INT_ARGS)
            return false;
    }
    return true;
}

/* an API-mode function: ml is the head of cffi's CPyExtFunc_s, which
   holds the C function, the function's type, and its doc string, which
   ml_doc points at; lib is the module's Lib */
static bool
describe_api_function(PyMethodDef *ml, PyObject *lib, signature_t *sig)
{
    PyObject *func, *addr = NULL;
    void *direct_fn = *(void **)(ml + 1);
    bool ok = false;
    if (strcmp(Py_TYPE(lib)->tp_name, "_cffi_backend.Lib") != 0 ||
        !cffi_usable())
        return false;
    /* the function is lib's own, and ffi.addressof gives its C function
       and type */
    func = PyObject_GetAttrString(lib, ml->ml_name);
    if (func == NULL || !PyCFunction_Check(func) ||
        ((PyCFunctionObject *)func)->m_ml != ml ||
        PyCFunction_GET_SELF(func) != lib)
        goto done;
    addr = PyObject_CallMethod(cffi_ffi, "addressof", "Os", lib, ml->ml_name);
    if (addr == NULL || Py_TYPE(addr) != cdata_type ||
        ((le_cdata_t *)addr)->c_data != direct_fn)
        goto done;
    /* the Lib keeps the type */
    ok = describe_function(((le_cdata_t *)addr)->c_type, direct_fn, sig);
done:
    Py_XDECREF(addr);
    Py_XDECREF(func);
    PyErr_Clear();
    return ok;
}

/* the word at 'offset' in run-time 'v' */
static vinfo_t *
load_word(PsycoObject *po, vinfo_t *v, long offset)
{
    return psyco_get_field_offset(po, v, FMUT(DEF_ARRAY(long, 0)), offset);
}

/* an exact int or bool v for integer type t: 1 with a new reference to
   its word in *pw, 0 to take cffi's call (on the path where it is out of
   t's range, which cffi reports), -1 to stop */
static int
int_arg(PsycoObject *po, vinfo_t *v, const ctype_t *t, vinfo_t **pw)
{
    /* the range of the word, and of the type */
    long wlo, whi, lo, hi;
    vinfo_t *w, *shifted;
    condition_code_t cc;
    int r;
    if (PsycoLong_IsVirtual(v))
        wlo = LONG_MIN, whi = LONG_MAX;
    else
        wlo = -(long)PyLong_MASK, whi = (long)PyLong_MASK;
    if ((r = PsycoLong_Word(po, v, &w)) != 1)
        return r;
    if (t->kind == K_BOOL)
        lo = 0, hi = 1;
    else if (t->size == 8)
        lo = t->is_signed ? LONG_MIN : 0, hi = LONG_MAX;
    else if (t->is_signed)
        lo = -(1L << (t->size * 8 - 1)), hi = (1L << (t->size * 8 - 1)) - 1;
    else
        lo = 0, hi = (1L << (t->size * 8)) - 1;
    if (lo <= wlo && whi <= hi) {
        *pw = w;
        return 1;
    }
    if (lo == 0 && whi <= hi)
        cc = integer_cmp_i(po, w, 0, Py_GE);
    else {
        /* lo <= w <= hi, as one unsigned comparison */
        shifted = integer_add_i(po, w, -lo, false);
        if (shifted == NULL) {
            vinfo_decref(w, po);
            return -1;
        }
        cc = integer_cmp_i(po, shifted, hi - lo, Py_LE | COMPARE_UNSIGNED);
        vinfo_decref(shifted, po);
    }
    if (cc == CC_ERROR) {
        vinfo_decref(w, po);
        return -1;
    }
    if (!runtime_condition_t(po, cc)) {
        vinfo_decref(w, po);
        return 0;
    }
    *pw = w;
    return 1;
}

/* argument v as cffi converts it for C type t: 1 with a new reference to
   the C value in *pw, 0 to take cffi's call, -1 to stop */
static int
convert_arg(PsycoObject *po, vinfo_t *v, const ctype_t *t, vinfo_t **pw)
{
    PyTypeObject *tp = Psyco_NeedType(po, v);
    vinfo_t *vct;
    condition_code_t cc;
    if (tp == NULL)
        return -1;
    switch (t->kind) {
        case K_INT:
        case K_BOOL:
            if (tp != &PyLong_Type && tp != &PyBool_Type)
                return 0;
            return int_arg(po, v, t, pw);
        case K_DOUBLE:
            /* PyFloat_AsDouble, as cffi converts it */
            if (tp != &PyFloat_Type && tp != &PyLong_Type)
                return 0;
            *pw = PsycoFloat_AsDouble(po, v);
            return *pw == NULL ? -1 : 1;
        case K_POINTER:
            /* a cdata of exactly the parameter's type passes its c_data */
            if (!PsycoCffi_IsCData(tp))
                return 0;
            vct = load_word(po, v, offsetof(le_cdata_t, c_type));
            if (vct == NULL)
                return -1;
            cc = integer_cmp_i(po, vct, (long)t->ct, Py_EQ);
            vinfo_decref(vct, po);
            if (cc == CC_ERROR)
                return -1;
            if (!runtime_condition_t(po, cc))
                return 0;
            *pw = load_word(po, v, offsetof(le_cdata_t, c_data));
            return *pw == NULL ? -1 : 1;
        default:
            return 0;
    }
}

/* the result word r (consumed) as cffi's wrapper boxes it */
static vinfo_t *
box_result(PsycoObject *po, const ctype_t *t, vinfo_t *r)
{
    vinfo_t *x;
    switch (t->kind) {
        case K_VOID:
            return psyco_vi_None();
        case K_DOUBLE:
            return PsycoFloat_FROM_DOUBLE(r);
        case K_POINTER:
            x = psyco_generic_call(
                po, cffi_exports[EXPORT_FROM_C_POINTER],
                CfCommonNewRefPyObject | CfNoPython | CfNoBlock, "vl", r,
                (long)t->ct);
            vinfo_decref(r, po);
            return x;
        case K_BOOL:
            x = integer_extend(po, r, 1, false);
            vinfo_decref(r, po);
            if (x == NULL)
                return NULL;
            r = PsycoBool_FromZeroOrOne(po, x);
            vinfo_decref(x, po);
            return r;
        default:
            x = integer_extend(po, r, t->size, t->is_signed);
            vinfo_decref(r, po);
            if (x == NULL)
                return NULL;
            if (t->size == 8 && !t->is_signed) {
                /* past LONG_MAX, an int object; else an unboxed one */
                condition_code_t cc = integer_cmp_i(po, x, 0, Py_LT);
                if (cc == CC_ERROR) {
                    vinfo_decref(x, po);
                    return NULL;
                }
                if (runtime_condition_f(po, cc)) {
                    r = PsycoLong_FromUnsignedLong(po, x);
                    vinfo_decref(x, po);
                    return r;
                }
            }
            return PsycoLong_FROM_LONG(po, x);
    }
}

/* the call of sig->fn with the arguments in the tuple vargs, as cffi's
   wrapper makes it; NULL with no exception set for cffi's own call */
static vinfo_t *
direct_call(PsycoObject *po, signature_t *sig, vinfo_t *vargs)
{
    vinfo_t *w[MAX_ARGS] = {NULL}, *vts, *r;
    char tags[MAX_ARGS + 1];
    int i, n = sig->nargs, ok, cflags;
    if (PsycoTuple_Load(vargs) != n)
        return NULL;
    /* each argument's guard comes before anything is run */
    for (i = 0; i < n; i++) {
        ok = convert_arg(po, PsycoTuple_GET_ITEM(vargs, i), &sig->args[i],
                         &w[i]);
        if (ok != 1) {
            while (i--) vinfo_decref(w[i], po);
            return NULL;
        }
        tags[i] = sig->args[i].kind == K_DOUBLE ? 'd' : 'v';
    }
    tags[n] = 0;
    /* the types the code compares with and boxes into */
    psyco_owner_pin((PyObject *)sig->fct);

    r = NULL;
    vts = le_gil_release_call(po);
    if (vts == NULL)
        goto done;
    if (psyco_generic_call(po, cffi_exports[EXPORT_RESTORE_ERRNO],
                           CfNoReturnValue | CfNoPython | CfNoBlock,
                           "") == NULL)
        goto done;
    cflags = sig->result.kind == K_VOID     ? CfNoReturnValue
             : sig->result.kind == K_DOUBLE ? CfReturnTypeDouble
                                            : CfReturnTypeLong;
    /* a callback from the C function may run Python */
    r = psyco_generic_call(po, sig->fn, cflags | CfNoBlock, tags, w[0], w[1],
                           w[2], w[3], w[4], w[5], w[6], w[7]);
    if (r == NULL)
        goto done;
    if (psyco_generic_call(po, cffi_exports[EXPORT_SAVE_ERRNO],
                           CfNoReturnValue | CfNoPython | CfNoBlock,
                           "") == NULL ||
        !le_gil_take_call(po, vts)) {
        if (r != VINFO_OK)
            vinfo_decref(r, po);
        r = NULL;
        goto done;
    }
    r = box_result(po, &sig->result, sig->result.kind == K_VOID ? NULL : r);
done:
    vinfo_xdecref(vts, po);
    for (i = 0; i < n; i++) vinfo_decref(w[i], po);
    return r;
}

DEFINEFN
vinfo_t *
PsycoCffi_ApiCall(PsycoObject *po, PyMethodDef *ml, vinfo_t *vself,
                  vinfo_t *vargs)
{
    /* ml_doc points just after cffi's CPyExtFunc_s: the PyMethodDef, the
       C function, in later cffi a cdata of it, and the type's index */
    const char *after = (const char *)(ml + 1);
    signature_t sig;
    PyObject *lib;
    if (ml->ml_doc != after + sizeof(void *) + sizeof(int) &&
        ml->ml_doc != after + 2 * sizeof(void *) + sizeof(int))
        return NULL;
    if (!is_virtualtime(vargs->source))
        return NULL;
    /* the Lib, kept by the code */
    lib = psyco_pyobj_atcompiletime(po, vself);
    if (lib == NULL)
        return NULL;
    if (!describe_api_function(ml, lib, &sig))
        return NULL;
    return direct_call(po, &sig, vargs);
}

DEFINEFN
vinfo_t *
PsycoCffi_Call(PsycoObject *po, vinfo_t *vf, vinfo_t *vargs, vinfo_t *kw)
{
    signature_t sig;
    PyObject *f;
    if (!psyco_knowntobe(kw, 0) || !is_virtualtime(vargs->source) ||
        is_megamorphic(vf->source))
        return NULL;
    /* the function, kept by the code */
    if (psyco_pyobj_atcompiletime_mega(po, vf, &f) != 1)
        return NULL;
    /* a plain cdata's c_data never changes */
    if (Py_TYPE(f) != cdata_type ||
        !describe_function(((le_cdata_t *)f)->c_type,
                           ((le_cdata_t *)f)->c_data, &sig))
        return NULL;
    return direct_call(po, &sig, vargs);
}
