/*
 * str (PEP 393). Compiled code reads a str only through the fields every
 * kind shares (PyASCIIObject), and its characters inline only for a
 * compact ASCII string, the common case, or for a constant whose kind is
 * known now. Anything else (other kinds at run time; subclasses, which are
 * never compact; strings not yet "ready", whose length field is 0) goes to
 * a C call on a respawned branch. Strings are immutable, so constants fold.
 * A one-character result below 256 is CPython's own cached string (latin1[]
 * holds them, taken from CPython's cache at start-up), as unicode_char
 * returns, so identities match CPython's.
 *
 * Length, + and 'in' are translated from unicodeobject.c with the
 * descriptors below; the rest is the representation the C doesn't state: the inline character read, the
 * slices and compare's constant fold and one-call equality.
 */
#include "punicodeobject.h"
#include "pboolobject.h"
#include "piterobject.h"
#include "plistobject.h"
#include "plongobject.h"
#include "psliceobject.h"
#include "pmethodobject.h"
#include "ptupleobject.h"
#include "../pycodegen.h"
#include "../psyfunc.h"
#include "../Python/pycheader.h"

#if NI_HAS_PYCORE_FORMAT_H
/* the format flags, no longer in bytesobject.h but in the internal
   pycore_format.h */
#define F_LJUST (1<<0)
#define F_SIGN  (1<<1)
#define F_BLANK (1<<2)
#define F_ALT   (1<<3)
#define F_ZERO  (1<<4)
#endif

/* 'state' is read as its low byte, which holds the bit-fields ready, ascii,
   compact, kind and interned (their masks are NI_BIT_, psyco.h's, from
   the bit order its layouts check, which puts ready, ascii and
   compact at the top, as state_at_least's unsigned >= needs). Only
   'interned' changes once a string is ready; the rest of the word is
   padding that CPython never initialises. */
#define UNICODE_length CPY_NONNEG_FIELD(PyASCIIObject, length, OB_type)
#define UNICODE_hash CPY_FIELD(PyASCIIObject, hash, UNICODE_length)
#define UNICODE_state CPY_FIELD(PyASCIIObject, state, UNICODE_hash)
#define STATE_COMPACT_ASCII                                                    \
  (NI_BIT_PyASCIIObject__state__ready | NI_BIT_PyASCIIObject__state__ascii |  \
   NI_BIT_PyASCIIObject__state__compact)

static PyObject *latin1[256];
static PyObject *unicode_empty; /* CPython's shared '' */
static binaryfunc unicode_subscript;

/* unicode_length, PyUnicode_Concat, PyUnicode_Contains, translated from
   unicodeobject.c */
/* the C functions, for the fallbacks: set by the registration */
static PyCFunction cfunc_unicode_lstrip;
static PyCFunction cfunc_unicode_rstrip;
static PyCFunction cfunc_unicode_strip;


/* CPython's code, copied for the metas to call at run time */
static PyObject *
cimpl_do_strip(PyObject *self, int striptype)
{
    Py_ssize_t len, i, j;
    if ((assert(PyUnicode_Check(self)),
         (PyUnicode_IS_READY(self) ? 0 : _PyUnicode_Ready(self))) == -1)
        return NULL;
    len = PyUnicode_GET_LENGTH(self);
    if (PyUnicode_IS_ASCII(self)) {
        const Py_UCS1 *data = PyUnicode_1BYTE_DATA(self);
        i = 0;
        if (striptype != 1) {
            while (i < len) {
                Py_UCS1 ch = data[i];
                if (!_Py_ascii_whitespace[ch])
                    break;
                i++;
            }
        }
        j = len;
        if (striptype != 0) {
            j--;
            while (j >= i) {
                Py_UCS1 ch = data[j];
                if (!_Py_ascii_whitespace[ch])
                    break;
                j--;
            }
            j++;
        }
    }
    else {
        int kind = PyUnicode_KIND(self);
        const void *data = PyUnicode_DATA(self);
        i = 0;
        if (striptype != 1) {
            while (i < len) {
                Py_UCS4 ch = PyUnicode_READ(kind, data, i);
                if (!Py_UNICODE_ISSPACE(ch))
                    break;
                i++;
            }
        }
        j = len;
        if (striptype != 0) {
            j--;
            while (j >= i) {
                Py_UCS4 ch = PyUnicode_READ(kind, data, j);
                if (!Py_UNICODE_ISSPACE(ch))
                    break;
                j--;
            }
            j++;
        }
    }
    return PyUnicode_Substring(self, i, j);
}

static const char *cstatic_stripfuncnames[] = {"lstrip", "rstrip", "strip"};

static PyObject *
cimpl_do_argstrip(PyObject *self, int striptype, PyObject *sep)
{
#if NI_HAS_UNICODE_STRIP_NONE_DEFAULT
    if (sep != Py_None) {
#else
    if (sep != NULL && sep != Py_None) {
#endif
        if (PyUnicode_Check(sep))
            return _PyUnicode_XStrip(self, striptype, sep);
        else {
            PyErr_Format(PyExc_TypeError, "%s arg must be None or str",
                         (cstatic_stripfuncnames[striptype]));
            return NULL;
        }
    }
    return cimpl_do_strip(self, striptype);
}

static PyObject *
cimpl_unicode_format_getnextarg(struct unicode_formatter_t *ctx)
{
    Py_ssize_t argidx = ctx->argidx;
    if (argidx < ctx->arglen) {
        ctx->argidx++;
        if (ctx->arglen < 0)
            return ctx->args;
        else
            return PyTuple_GetItem(ctx->args, argidx);
    }
    PyErr_SetString(PyExc_TypeError, "not enough arguments for format string");
    return NULL;
}

static int
cimpl_unicode_format_arg_parse(struct unicode_formatter_t *ctx,
                               struct unicode_format_arg_t *arg)
{
    PyObject *v;
    if (arg->ch == '(') {
        Py_ssize_t keystart;
        Py_ssize_t keylen;
        PyObject *key;
        int pcount = 1;
        if (ctx->dict == NULL) {
            PyErr_SetString(PyExc_TypeError, "format requires a mapping");
            return -1;
        }
        ++ctx->fmtpos;
        --ctx->fmtcnt;
        keystart = ctx->fmtpos;
        while (pcount > 0 && --ctx->fmtcnt >= 0) {
            arg->ch =
                PyUnicode_READ((ctx)->fmtkind, (ctx)->fmtdata, (ctx)->fmtpos);
            if (arg->ch == ')')
                --pcount;
            else if (arg->ch == '(')
                ++pcount;
            ctx->fmtpos++;
        }
        keylen = ctx->fmtpos - keystart - 1;
        if (ctx->fmtcnt < 0 || pcount > 0) {
            PyErr_SetString(PyExc_ValueError, "incomplete format key");
            return -1;
        }
        key = PyUnicode_Substring(ctx->fmtstr, keystart, keystart + keylen);
        if (key == NULL)
            return -1;
        if (ctx->args_owned) {
            ctx->args_owned = 0;
            Py_DECREF(ctx->args);
        }
        ctx->args = PyObject_GetItem(ctx->dict, key);
        Py_DECREF(key);
        if (ctx->args == NULL)
            return -1;
        ctx->args_owned = 1;
        ctx->arglen = -1;
        ctx->argidx = -2;
    }
    while (--ctx->fmtcnt >= 0) {
        arg->ch =
            PyUnicode_READ((ctx)->fmtkind, (ctx)->fmtdata, (ctx)->fmtpos);
        ctx->fmtpos++;
        switch (arg->ch) {
            case '-':
                arg->flags |= F_LJUST;
                continue;
            case '+':
                arg->flags |= F_SIGN;
                continue;
            case ' ':
                arg->flags |= F_BLANK;
                continue;
            case '#':
                arg->flags |= F_ALT;
                continue;
            case '0':
                arg->flags |= F_ZERO;
                continue;
        }
        break;
    }
    if (arg->ch == '*') {
        v = cimpl_unicode_format_getnextarg(ctx);
        if (v == NULL)
            return -1;
        if (!PyLong_Check(v)) {
            PyErr_SetString(PyExc_TypeError, "* wants int");
            return -1;
        }
        arg->width = PyLong_AsSsize_t(v);
        if (arg->width == -1 && PyErr_Occurred())
            return -1;
        if (arg->width < 0) {
            arg->flags |= F_LJUST;
            arg->width = -arg->width;
        }
        if (--ctx->fmtcnt >= 0) {
            arg->ch =
                PyUnicode_READ((ctx)->fmtkind, (ctx)->fmtdata, (ctx)->fmtpos);
            ctx->fmtpos++;
        }
    }
    else if (arg->ch >= '0' && arg->ch <= '9') {
        arg->width = arg->ch - '0';
        while (--ctx->fmtcnt >= 0) {
            arg->ch =
                PyUnicode_READ((ctx)->fmtkind, (ctx)->fmtdata, (ctx)->fmtpos);
            ctx->fmtpos++;
            if (arg->ch < '0' || arg->ch > '9')
                break;
            if (arg->width > (PY_SSIZE_T_MAX - ((int)arg->ch - '0')) / 10) {
                PyErr_SetString(PyExc_ValueError, "width too big");
                return -1;
            }
            arg->width = arg->width * 10 + (arg->ch - '0');
        }
    }
    if (arg->ch == '.') {
        arg->prec = 0;
        if (--ctx->fmtcnt >= 0) {
            arg->ch =
                PyUnicode_READ((ctx)->fmtkind, (ctx)->fmtdata, (ctx)->fmtpos);
            ctx->fmtpos++;
        }
        if (arg->ch == '*') {
            v = cimpl_unicode_format_getnextarg(ctx);
            if (v == NULL)
                return -1;
            if (!PyLong_Check(v)) {
                PyErr_SetString(PyExc_TypeError, "* wants int");
                return -1;
            }
            arg->prec = _PyLong_AsInt(v);
            if (arg->prec == -1 && PyErr_Occurred())
                return -1;
            if (arg->prec < 0)
                arg->prec = 0;
            if (--ctx->fmtcnt >= 0) {
                arg->ch = PyUnicode_READ((ctx)->fmtkind, (ctx)->fmtdata,
                                         (ctx)->fmtpos);
                ctx->fmtpos++;
            }
        }
        else if (arg->ch >= '0' && arg->ch <= '9') {
            arg->prec = arg->ch - '0';
            while (--ctx->fmtcnt >= 0) {
                arg->ch = PyUnicode_READ((ctx)->fmtkind, (ctx)->fmtdata,
                                         (ctx)->fmtpos);
                ctx->fmtpos++;
                if (arg->ch < '0' || arg->ch > '9')
                    break;
                if (arg->prec > (INT_MAX - ((int)arg->ch - '0')) / 10) {
                    PyErr_SetString(PyExc_ValueError, "precision too big");
                    return -1;
                }
                arg->prec = arg->prec * 10 + (arg->ch - '0');
            }
        }
    }
    if (ctx->fmtcnt >= 0) {
        if (arg->ch == 'h' || arg->ch == 'l' || arg->ch == 'L') {
            if (--ctx->fmtcnt >= 0) {
                arg->ch = PyUnicode_READ((ctx)->fmtkind, (ctx)->fmtdata,
                                         (ctx)->fmtpos);
                ctx->fmtpos++;
            }
        }
    }
    if (ctx->fmtcnt < 0) {
        PyErr_SetString(PyExc_ValueError, "incomplete format");
        return -1;
    }
    return 0;
}

static int
cimpl_mainformatlong(PyObject *v, struct unicode_format_arg_t *arg,
                     PyObject **p_output, _PyUnicodeWriter *writer)
#if NI_HAS_PRIVATE_NUMBER_INDEX
{
    PyObject *iobj, *res;
    char type = (char)arg->ch;
    if (!PyNumber_Check(v))
        goto wrongtype;
    if (!PyLong_Check(v)) {
        if (type == 'o' || type == 'x' || type == 'X') {
            iobj = _PyNumber_Index(v);
        }
        else {
            iobj = PyNumber_Long(v);
        }
        if (iobj == NULL) {
            if (PyErr_ExceptionMatches(PyExc_TypeError))
                goto wrongtype;
            return -1;
        }
        assert(PyLong_Check(iobj));
    }
    else {
        iobj = v;
        Py_INCREF(iobj);
    }
    if (PyLong_CheckExact(v) && arg->width == -1 && arg->prec == -1 &&
        !(arg->flags & ((1 << 1) | (1 << 2))) && type != 'X') {
        int alternate = arg->flags & (1 << 3);
        int base;
        switch (type) {
            default:
                Py_UNREACHABLE();
            case 'd':
            case 'i':
            case 'u':
                base = 10;
                break;
            case 'o':
                base = 8;
                break;
            case 'x':
            case 'X':
                base = 16;
                break;
        }
        if (_PyLong_FormatWriter(writer, v, base, alternate) == -1) {
            Py_DECREF(iobj);
            return -1;
        }
        Py_DECREF(iobj);
        return 1;
    }
    res = _PyUnicode_FormatLong(iobj, arg->flags & (1 << 3), arg->prec, type);
    Py_DECREF(iobj);
    if (res == NULL)
        return -1;
    *p_output = res;
    return 0;
wrongtype:
    switch (type) {
        case 'o':
        case 'x':
        case 'X':
            PyErr_Format(PyExc_TypeError,
                         "%%%c format: an integer is required, "
                         "not %.200s",
                         type, Py_TYPE(v)->tp_name);
            break;
        default:
            PyErr_Format(PyExc_TypeError,
                         "%%%c format: a real number is required, "
                         "not %.200s",
                         type, Py_TYPE(v)->tp_name);
            break;
    }
    return -1;
}
#else
{
    PyObject *iobj, *res;
    char type = (char)arg->ch;
    if (!PyNumber_Check(v))
        goto wrongtype;
    if (!PyLong_Check(v)) {
        if (type == 'o' || type == 'x' || type == 'X') {
            iobj = PyNumber_Index(v);
            if (iobj == NULL) {
                if (PyErr_ExceptionMatches(PyExc_TypeError))
                    goto wrongtype;
                return -1;
            }
        }
        else {
            iobj = PyNumber_Long(v);
            if (iobj == NULL) {
                if (PyErr_ExceptionMatches(PyExc_TypeError))
                    goto wrongtype;
                return -1;
            }
        }
        assert(PyLong_Check(iobj));
    }
    else {
        iobj = v;
        Py_INCREF(iobj);
    }
    if (PyLong_CheckExact(v) && arg->width == -1 && arg->prec == -1 &&
        !(arg->flags & (F_SIGN | F_BLANK)) && type != 'X') {
        int alternate = arg->flags & F_ALT;
        int base;
        switch (type) {
            default:
                Py_UNREACHABLE();
            case 'd':
            case 'i':
            case 'u':
                base = 10;
                break;
            case 'o':
                base = 8;
                break;
            case 'x':
            case 'X':
                base = 16;
                break;
        }
        if (_PyLong_FormatWriter(writer, v, base, alternate) == -1) {
            Py_DECREF(iobj);
            return -1;
        }
        Py_DECREF(iobj);
        return 1;
    }
    res = _PyUnicode_FormatLong(iobj, arg->flags & F_ALT, arg->prec, type);
    Py_DECREF(iobj);
    if (res == NULL)
        return -1;
    *p_output = res;
    return 0;
wrongtype:
    switch (type) {
        case 'o':
        case 'x':
        case 'X':
            PyErr_Format(PyExc_TypeError,
                         "%%%c format: an integer is required, "
                         "not %.200s",
                         type, Py_TYPE(v)->tp_name);
            break;
        default:
            PyErr_Format(PyExc_TypeError,
                         "%%%c format: a number is required, "
                         "not %.200s",
                         type, Py_TYPE(v)->tp_name);
            break;
    }
    return -1;
}
#endif

static int
cimpl_formatfloat(PyObject *v, struct unicode_format_arg_t *arg,
                  PyObject **p_output, _PyUnicodeWriter *writer)
{
    char *p;
    double x;
    Py_ssize_t len;
    int prec;
    int dtoa_flags;
    x = PyFloat_AsDouble(v);
    if (x == -1.0 && PyErr_Occurred())
        return -1;
    prec = arg->prec;
    if (prec < 0)
        prec = 6;
    if (arg->flags & F_ALT)
        dtoa_flags = Py_DTSF_ALT;
    else
        dtoa_flags = 0;
    p = PyOS_double_to_string(x, arg->ch, prec, dtoa_flags, NULL);
    if (p == NULL)
        return -1;
    len = strlen(p);
    if (writer) {
        if (_PyUnicodeWriter_WriteASCIIString(writer, p, len) < 0) {
            PyMem_Free(p);
            return -1;
        }
    }
    else
        *p_output = _PyUnicode_FromASCII(p, len);
    PyMem_Free(p);
    return 0;
}

static Py_UCS4
cimpl_formatchar(PyObject *v)
{
    if (PyUnicode_Check(v)) {
        if (PyUnicode_GET_LENGTH(v) == 1) {
            return PyUnicode_READ_CHAR(v, 0);
        }
        goto onError;
    }
    else {
#if NI_HAS_FORMATCHAR_INDEX_ONLY
        int overflow;
        long x = PyLong_AsLongAndOverflow(v, &overflow);
        if (x == -1 && PyErr_Occurred()) {
            if (PyErr_ExceptionMatches(PyExc_TypeError)) {
                goto onError;
            }
            return (Py_UCS4)-1;
        }
#else
        PyObject *iobj;
        long x;
        if (!PyLong_Check(v)) {
            iobj = PyNumber_Index(v);
            if (iobj == NULL) {
                goto onError;
            }
            x = PyLong_AsLong(iobj);
            Py_DECREF(iobj);
        }
        else {
            x = PyLong_AsLong(v);
        }
        if (x == -1 && PyErr_Occurred())
            goto onError;
#endif
        if (x < 0 || x > 0x10ffff) {
            PyErr_SetString(PyExc_OverflowError,
                            "%c arg not in range(0x110000)");
            return (Py_UCS4)-1;
        }
        return (Py_UCS4)x;
    }
onError:
    PyErr_SetString(PyExc_TypeError, "%c requires int or char");
    return (Py_UCS4)-1;
}

static int
cimpl__PyUnicodeWriter_WriteCharInline(_PyUnicodeWriter *writer, Py_UCS4 ch)
{
    assert(ch <= 0x10ffff);
    if (_PyUnicodeWriter_Prepare(writer, 1, ch) < 0)
        return -1;
    PyUnicode_WRITE(writer->kind, writer->data, writer->pos, ch);
    writer->pos++;
    return 0;
}

static int
cimpl_unicode_format_arg_format(struct unicode_formatter_t *ctx,
                                struct unicode_format_arg_t *arg,
                                PyObject **p_str)
{
    PyObject *v;
    _PyUnicodeWriter *writer = &ctx->writer;
    if (ctx->fmtcnt == 0)
        ctx->writer.overallocate = 0;
    v = cimpl_unicode_format_getnextarg(ctx);
    if (v == NULL)
        return -1;
    switch (arg->ch) {
        case 's':
        case 'r':
        case 'a':
            if (PyLong_CheckExact(v) && arg->width == -1 && arg->prec == -1) {
                if (_PyLong_FormatWriter(writer, v, 10, arg->flags & F_ALT) ==
                    -1)
                    return -1;
                return 1;
            }
            if (PyUnicode_CheckExact(v) && arg->ch == 's') {
                *p_str = v;
                Py_INCREF(*p_str);
            }
            else {
                if (arg->ch == 's')
                    *p_str = PyObject_Str(v);
                else if (arg->ch == 'r')
                    *p_str = PyObject_Repr(v);
                else
                    *p_str = PyObject_ASCII(v);
            }
            break;
        case 'i':
        case 'd':
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            int ret = cimpl_mainformatlong(v, arg, p_str, writer);
            if (ret != 0)
                return ret;
            arg->sign = 1;
            break;
        }
        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
            if (arg->width == -1 && arg->prec == -1 &&
                !(arg->flags & (F_SIGN | F_BLANK))) {
                if (cimpl_formatfloat(v, arg, NULL, writer) == -1)
                    return -1;
                return 1;
            }
            arg->sign = 1;
            if (cimpl_formatfloat(v, arg, p_str, NULL) == -1)
                return -1;
            break;
        case 'c': {
            Py_UCS4 ch = cimpl_formatchar(v);
            if (ch == (Py_UCS4)-1)
                return -1;
            if (arg->width == -1 && arg->prec == -1) {
                if (cimpl__PyUnicodeWriter_WriteCharInline(writer, ch) < 0)
                    return -1;
                return 1;
            }
            *p_str = PyUnicode_FromOrdinal(ch);
            break;
        }
        default:
            PyErr_Format(
                PyExc_ValueError,
                "unsupported format character '%c' (0x%x) "
                "at index %zd",
                (31 <= arg->ch && arg->ch <= 126) ? (char)arg->ch : '?',
                (int)arg->ch, ctx->fmtpos - 1);
            return -1;
    }
    if (*p_str == NULL)
        return -1;
    assert(PyUnicode_Check(*p_str));
    return 0;
}

#if NI_HAS_UNICODE_FILL
static void
cimpl_unicode_fill(enum PyUnicode_Kind kind, void *data, Py_UCS4 value,
                   Py_ssize_t start, Py_ssize_t length)
{
    assert(0 <= start);
    assert(kind != PyUnicode_WCHAR_KIND);
    switch (kind) {
        case PyUnicode_1BYTE_KIND: {
            assert(value <= 0xff);
            Py_UCS1 ch = (unsigned char)value;
            Py_UCS1 *to = (Py_UCS1 *)data + start;
            memset(to, ch, length);
            break;
        }
        case PyUnicode_2BYTE_KIND: {
            assert(value <= 0xffff);
            Py_UCS2 ch = (Py_UCS2)value;
            Py_UCS2 *to = (Py_UCS2 *)data + start;
            const Py_UCS2 *end = to + length;
            for (; to < end; ++to) *to = ch;
            break;
        }
        case PyUnicode_4BYTE_KIND: {
            assert(value <= 0x10ffff);
            Py_UCS4 ch = value;
            Py_UCS4 *to = (Py_UCS4 *)data + start;
            const Py_UCS4 *end = to + length;
            for (; to < end; ++to) *to = ch;
            break;
        }
        default:
            Py_UNREACHABLE();
    }
}
#endif

static int
cimpl_unicode_format_arg_output(struct unicode_formatter_t *ctx,
                                struct unicode_format_arg_t *arg,
                                PyObject *str)
#if NI_HAS_UNICODE_FILL
{
    Py_ssize_t len;
    enum PyUnicode_Kind kind;
    const void *pbuf;
    Py_ssize_t pindex;
    Py_UCS4 signchar;
    Py_ssize_t buflen;
    Py_UCS4 maxchar;
    Py_ssize_t sublen;
    _PyUnicodeWriter *writer = &ctx->writer;
    Py_UCS4 fill;
    fill = ' ';
    if (arg->sign && arg->flags & F_ZERO)
        fill = '0';
    if ((assert(PyUnicode_Check(str)),
         (PyUnicode_IS_READY(str) ? 0 : _PyUnicode_Ready(str))) == -1)
        return -1;
    len = PyUnicode_GET_LENGTH(str);
    if ((arg->width == -1 || arg->width <= len) &&
        (arg->prec == -1 || arg->prec >= len) &&
        !(arg->flags & (F_SIGN | F_BLANK))) {
        if (_PyUnicodeWriter_WriteStr(writer, str) == -1)
            return -1;
        return 0;
    }
    if (arg->ch == 's' || arg->ch == 'r' || arg->ch == 'a') {
        if (arg->prec >= 0 && len > arg->prec)
            len = arg->prec;
    }
    kind = PyUnicode_KIND(str);
    pbuf = PyUnicode_DATA(str);
    pindex = 0;
    signchar = '\0';
    if (arg->sign) {
        Py_UCS4 ch = PyUnicode_READ(kind, pbuf, pindex);
        if (ch == '-' || ch == '+') {
            signchar = ch;
            len--;
            pindex++;
        }
        else if (arg->flags & F_SIGN)
            signchar = '+';
        else if (arg->flags & F_BLANK)
            signchar = ' ';
        else
            arg->sign = 0;
    }
    if (arg->width < len)
        arg->width = len;
    maxchar = writer->maxchar;
    if (!(arg->flags & F_LJUST)) {
        if (arg->sign) {
            if ((arg->width - 1) > len)
                maxchar = Py_MAX(maxchar, fill);
        }
        else {
            if (arg->width > len)
                maxchar = Py_MAX(maxchar, fill);
        }
    }
    if (PyUnicode_MAX_CHAR_VALUE(str) > maxchar) {
        Py_UCS4 strmaxchar = _PyUnicode_FindMaxChar(str, 0, pindex + len);
        maxchar = Py_MAX(maxchar, strmaxchar);
    }
    buflen = arg->width;
    if (arg->sign && len == arg->width)
        buflen++;
    if (_PyUnicodeWriter_Prepare(writer, buflen, maxchar) == -1)
        return -1;
    if (arg->sign) {
        if (fill != ' ') {
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos, signchar);
            writer->pos += 1;
        }
        if (arg->width > len)
            arg->width--;
    }
    if ((arg->flags & F_ALT) &&
        (arg->ch == 'x' || arg->ch == 'X' || arg->ch == 'o')) {
        assert(PyUnicode_READ(kind, pbuf, pindex) == '0');
        assert(PyUnicode_READ(kind, pbuf, pindex + 1) == arg->ch);
        if (fill != ' ') {
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos, '0');
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos + 1,
                            arg->ch);
            writer->pos += 2;
            pindex += 2;
        }
        arg->width -= 2;
        if (arg->width < 0)
            arg->width = 0;
        len -= 2;
    }
    if (arg->width > len && !(arg->flags & F_LJUST)) {
        sublen = arg->width - len;
        cimpl_unicode_fill(writer->kind, writer->data, fill, writer->pos,
                           sublen);
        writer->pos += sublen;
        arg->width = len;
    }
    if (fill == ' ') {
        if (arg->sign) {
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos, signchar);
            writer->pos += 1;
        }
        if ((arg->flags & F_ALT) &&
            (arg->ch == 'x' || arg->ch == 'X' || arg->ch == 'o')) {
            assert(PyUnicode_READ(kind, pbuf, pindex) == '0');
            assert(PyUnicode_READ(kind, pbuf, pindex + 1) == arg->ch);
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos, '0');
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos + 1,
                            arg->ch);
            writer->pos += 2;
            pindex += 2;
        }
    }
    if (len) {
        _PyUnicode_FastCopyCharacters(writer->buffer, writer->pos, str, pindex,
                                      len);
        writer->pos += len;
    }
    if (arg->width > len) {
        sublen = arg->width - len;
        cimpl_unicode_fill(writer->kind, writer->data, ' ', writer->pos,
                           sublen);
        writer->pos += sublen;
    }
    return 0;
}
#else
{
    Py_ssize_t len;
    enum PyUnicode_Kind kind;
    void *pbuf;
    Py_ssize_t pindex;
    Py_UCS4 signchar;
    Py_ssize_t buflen;
    Py_UCS4 maxchar;
    Py_ssize_t sublen;
    _PyUnicodeWriter *writer = &ctx->writer;
    Py_UCS4 fill;
    fill = ' ';
    if (arg->sign && arg->flags & F_ZERO)
        fill = '0';
    if ((assert(PyUnicode_Check(str)),
         (PyUnicode_IS_READY(str) ? 0 : _PyUnicode_Ready(str))) == -1)
        return -1;
    len = PyUnicode_GET_LENGTH(str);
    if ((arg->width == -1 || arg->width <= len) &&
        (arg->prec == -1 || arg->prec >= len) &&
        !(arg->flags & (F_SIGN | F_BLANK))) {
        if (_PyUnicodeWriter_WriteStr(writer, str) == -1)
            return -1;
        return 0;
    }
    if (arg->ch == 's' || arg->ch == 'r' || arg->ch == 'a') {
        if (arg->prec >= 0 && len > arg->prec)
            len = arg->prec;
    }
    kind = PyUnicode_KIND(str);
    pbuf = PyUnicode_DATA(str);
    pindex = 0;
    signchar = '\0';
    if (arg->sign) {
        Py_UCS4 ch = PyUnicode_READ(kind, pbuf, pindex);
        if (ch == '-' || ch == '+') {
            signchar = ch;
            len--;
            pindex++;
        }
        else if (arg->flags & F_SIGN)
            signchar = '+';
        else if (arg->flags & F_BLANK)
            signchar = ' ';
        else
            arg->sign = 0;
    }
    if (arg->width < len)
        arg->width = len;
    maxchar = writer->maxchar;
    if (!(arg->flags & F_LJUST)) {
        if (arg->sign) {
            if ((arg->width - 1) > len)
                maxchar = Py_MAX(maxchar, fill);
        }
        else {
            if (arg->width > len)
                maxchar = Py_MAX(maxchar, fill);
        }
    }
    if (PyUnicode_MAX_CHAR_VALUE(str) > maxchar) {
        Py_UCS4 strmaxchar = _PyUnicode_FindMaxChar(str, 0, pindex + len);
        maxchar = Py_MAX(maxchar, strmaxchar);
    }
    buflen = arg->width;
    if (arg->sign && len == arg->width)
        buflen++;
    if (_PyUnicodeWriter_Prepare(writer, buflen, maxchar) == -1)
        return -1;
    if (arg->sign) {
        if (fill != ' ') {
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos, signchar);
            writer->pos += 1;
        }
        if (arg->width > len)
            arg->width--;
    }
    if ((arg->flags & F_ALT) &&
        (arg->ch == 'x' || arg->ch == 'X' || arg->ch == 'o')) {
        assert(PyUnicode_READ(kind, pbuf, pindex) == '0');
        assert(PyUnicode_READ(kind, pbuf, pindex + 1) == arg->ch);
        if (fill != ' ') {
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos, '0');
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos + 1,
                            arg->ch);
            writer->pos += 2;
            pindex += 2;
        }
        arg->width -= 2;
        if (arg->width < 0)
            arg->width = 0;
        len -= 2;
    }
    if (arg->width > len && !(arg->flags & F_LJUST)) {
        sublen = arg->width - len;
        do {
            assert(0 <= writer->pos);
            assert(writer->kind != PyUnicode_WCHAR_KIND);
            switch (writer->kind) {
                case PyUnicode_1BYTE_KIND: {
                    assert(fill <= 0xff);
                    Py_UCS1 ch = (unsigned char)fill;
                    Py_UCS1 *to = (Py_UCS1 *)writer->data + writer->pos;
                    memset(to, ch, sublen);
                    break;
                }
                case PyUnicode_2BYTE_KIND: {
                    assert(fill <= 0xffff);
                    Py_UCS2 ch = (Py_UCS2)fill;
                    Py_UCS2 *to = (Py_UCS2 *)writer->data + writer->pos;
                    const Py_UCS2 *end = to + sublen;
                    for (; to < end; ++to) *to = ch;
                    break;
                }
                case PyUnicode_4BYTE_KIND: {
                    assert(fill <= 0x10ffff);
                    Py_UCS4 ch = fill;
                    Py_UCS4 *to = (Py_UCS4 *)writer->data + writer->pos;
                    const Py_UCS4 *end = to + sublen;
                    for (; to < end; ++to) *to = ch;
                    break;
                }
                default:
                    Py_UNREACHABLE();
            }
        } while (0);
        writer->pos += sublen;
        arg->width = len;
    }
    if (fill == ' ') {
        if (arg->sign) {
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos, signchar);
            writer->pos += 1;
        }
        if ((arg->flags & F_ALT) &&
            (arg->ch == 'x' || arg->ch == 'X' || arg->ch == 'o')) {
            assert(PyUnicode_READ(kind, pbuf, pindex) == '0');
            assert(PyUnicode_READ(kind, pbuf, pindex + 1) == arg->ch);
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos, '0');
            PyUnicode_WRITE(writer->kind, writer->data, writer->pos + 1,
                            arg->ch);
            writer->pos += 2;
            pindex += 2;
        }
    }
    if (len) {
        _PyUnicode_FastCopyCharacters(writer->buffer, writer->pos, str, pindex,
                                      len);
        writer->pos += len;
    }
    if (arg->width > len) {
        sublen = arg->width - len;
        do {
            assert(0 <= writer->pos);
            assert(writer->kind != PyUnicode_WCHAR_KIND);
            switch (writer->kind) {
                case PyUnicode_1BYTE_KIND: {
                    assert(' ' <= 0xff);
                    Py_UCS1 ch = (unsigned char)' ';
                    Py_UCS1 *to = (Py_UCS1 *)writer->data + writer->pos;
                    memset(to, ch, sublen);
                    break;
                }
                case PyUnicode_2BYTE_KIND: {
                    assert(' ' <= 0xffff);
                    Py_UCS2 ch = (Py_UCS2)' ';
                    Py_UCS2 *to = (Py_UCS2 *)writer->data + writer->pos;
                    const Py_UCS2 *end = to + sublen;
                    for (; to < end; ++to) *to = ch;
                    break;
                }
                case PyUnicode_4BYTE_KIND: {
                    assert(' ' <= 0x10ffff);
                    Py_UCS4 ch = ' ';
                    Py_UCS4 *to = (Py_UCS4 *)writer->data + writer->pos;
                    const Py_UCS4 *end = to + sublen;
                    for (; to < end; ++to) *to = ch;
                    break;
                }
                default:
                    Py_UNREACHABLE();
            }
        } while (0);
        writer->pos += sublen;
    }
    return 0;
}
#endif

static vinfo_t *
PsycoUnicode_Concat(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
PsycoUnicode_Contains(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
unicode_length(PsycoObject *po, vinfo_t *);
static vinfo_t *
do_argstrip(PsycoObject *po, vinfo_t *, int, vinfo_t *);
static vinfo_t *
unicode_lstrip_impl(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
unicode_lstrip(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
unicode_rstrip_impl(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
unicode_rstrip(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
unicode_strip_impl(PsycoObject *po, vinfo_t *, vinfo_t *);
static vinfo_t *
unicode_strip(PsycoObject *po, vinfo_t *, vinfo_t *);

static vinfo_t *
PsycoUnicode_Concat(PsycoObject *po, vinfo_t *left, vinfo_t *right)
{
    return psyco_generic_call(po, PyUnicode_Concat,
                              CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                              "vv", left, right);
}

static vinfo_t *
PsycoUnicode_Contains(PsycoObject *po, vinfo_t *str, vinfo_t *substr)
{
    if (is_compiletime(str->source) && is_compiletime(substr->source)) {
        int r = PyUnicode_Contains(
            (PyObject *)CompileTime_Get(str->source)->value,
            (PyObject *)CompileTime_Get(substr->source)->value);
        if (r >= 0)
            return vinfo_new(CompileTime_New(r));
        PyErr_Clear(); /* raised again at run time */
    }
    return psyco_generic_call(po, PyUnicode_Contains,
                              CfCommonIntBoolOrError | CfNoPython | CfNoBlock,
                              "vv", str, substr);
}

static vinfo_t *
unicode_length(PsycoObject *po, vinfo_t *self)
{
    vinfo_t *_t1;
    condition_code_t _t2;
    int _t3;
    vinfo_t *_t4;
    _t1 = psyco_get_field(po, self, UNICODE_state);
    if (_t1 == NULL) {
        return NULL;
    }
    _t2 = integer_cmp_i(po, _t1, NI_BIT_PyASCIIObject__state__ready,
                        Py_GE | COMPARE_UNSIGNED);
    vinfo_decref(_t1, po);
    if (_t2 == CC_ERROR) {
        return NULL;
    }
    _t3 = runtime_condition_t(po, _t2);
    if (!_t3) {
        return psyco_generic_call(
            po, PyUnicode_Type.tp_as_sequence->sq_length,
            CfCommonPySSizeTResult | CfNoPython | CfNoBlock, "v", self);
    }
    if (0 == (-1)) {
        return NULL;
    }
    _t4 = psyco_get_field(po, self, UNICODE_length);
    if (_t4 == NULL) {
        return NULL;
    }
    return _t4;
}

static vinfo_t *
do_argstrip(PsycoObject *po, vinfo_t *self, int striptype, vinfo_t *sep)
{
    return psyco_generic_call(po, cimpl_do_argstrip,
                              CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                              "vlv", self, (long)(striptype), sep);
}

static vinfo_t *
unicode_lstrip_impl(PsycoObject *po, vinfo_t *self, vinfo_t *chars)
{
    vinfo_t *_t1;
    _t1 = do_argstrip(po, self, 0, chars);
    return _t1;
}

static vinfo_t *
unicode_lstrip(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    int _t1;
    vinfo_t *chars;
    vinfo_t *return_value;
    _t1 = PsycoTuple_Load(vargs);
    if (_t1 < 0 || _t1 > 1) {
        return PsycoCFunction_CallFast(po, cfunc_unicode_lstrip, METH_FASTCALL,
                                       self, vargs);
    }
    if (_t1 == 0) {
#if NI_HAS_UNICODE_STRIP_NONE_DEFAULT
        chars = psyco_vi_None();
#else
        chars = psyco_vi_Zero();
#endif
        return_value = unicode_lstrip_impl(po, self, chars);
        vinfo_decref(chars, po);
        return return_value;
    }
    else {
        chars = PsycoTuple_GET_ITEM(vargs, 0);
        return_value = unicode_lstrip_impl(po, self, chars);
        return return_value;
    }
}

static vinfo_t *
unicode_rstrip_impl(PsycoObject *po, vinfo_t *self, vinfo_t *chars)
{
    vinfo_t *_t1;
    _t1 = do_argstrip(po, self, 1, chars);
    return _t1;
}

static vinfo_t *
unicode_rstrip(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    int _t1;
    vinfo_t *chars;
    vinfo_t *return_value;
    _t1 = PsycoTuple_Load(vargs);
    if (_t1 < 0 || _t1 > 1) {
        return PsycoCFunction_CallFast(po, cfunc_unicode_rstrip, METH_FASTCALL,
                                       self, vargs);
    }
    if (_t1 == 0) {
#if NI_HAS_UNICODE_STRIP_NONE_DEFAULT
        chars = psyco_vi_None();
#else
        chars = psyco_vi_Zero();
#endif
        return_value = unicode_rstrip_impl(po, self, chars);
        vinfo_decref(chars, po);
        return return_value;
    }
    else {
        chars = PsycoTuple_GET_ITEM(vargs, 0);
        return_value = unicode_rstrip_impl(po, self, chars);
        return return_value;
    }
}

static vinfo_t *
unicode_strip_impl(PsycoObject *po, vinfo_t *self, vinfo_t *chars)
{
    vinfo_t *_t1;
    _t1 = do_argstrip(po, self, 2, chars);
    return _t1;
}

static vinfo_t *
unicode_strip(PsycoObject *po, vinfo_t *self, vinfo_t *vargs)
{
    int _t1;
    vinfo_t *chars;
    vinfo_t *return_value;
    _t1 = PsycoTuple_Load(vargs);
    if (_t1 < 0 || _t1 > 1) {
        return PsycoCFunction_CallFast(po, cfunc_unicode_strip, METH_FASTCALL,
                                       self, vargs);
    }
    if (_t1 == 0) {
        chars = psyco_vi_None();
        return_value = unicode_strip_impl(po, self, chars);
        vinfo_decref(chars, po);
        return return_value;
    }
    else {
        chars = PsycoTuple_GET_ITEM(vargs, 0);
        return_value = unicode_strip_impl(po, self, chars);
        return return_value;
    }
}

static void
gen_unicodeobject_init(void)
{
    Psyco_DefineMeta(PyUnicode_Type.tp_as_sequence->sq_concat,
                     PsycoUnicode_Concat);
    Psyco_DefineMeta(PyUnicode_Type.tp_as_sequence->sq_contains,
                     PsycoUnicode_Contains);
    Psyco_DefineMeta(PyUnicode_Type.tp_as_sequence->sq_length, unicode_length);
}
static void
gen_unicodeobject_methods_init(void)
{
    cfunc_unicode_lstrip = Psyco_DefineMethodFn(
        &PyUnicode_Type, "lstrip", METH_FASTCALL, &unicode_lstrip);
    cfunc_unicode_rstrip = Psyco_DefineMethodFn(
        &PyUnicode_Type, "rstrip", METH_FASTCALL, &unicode_rstrip);
    cfunc_unicode_strip = Psyco_DefineMethodFn(&PyUnicode_Type, "strip",
                                               METH_FASTCALL, &unicode_strip);
}

/* A view is a str not built yet: 'len' characters of 'root' (a real str,
   ready) from 'start'. It is exact str and its length is the real
   string's field, so len() and type tests read it as they would the
   object's. A view made from another view keeps that view as 'parent'
   and its own start in it as 'rel'; the root and start are the parent's
   followed down, so helpers read the characters without forcing.

   Forced, it is PyUnicode_Substring, the C function CPython's slices,
   strip and split items end in, so the result's identity is CPython's:
   the shared '' or a cached character when short, the string itself when
   the bounds take all of an exact str. A view of a view is
   Substring(parent, rel, ...) as CPython's would be, which keeps
   'w.strip() is w' when nothing was stripped. So that the parent need not
   be built for that:
     - a parent no one else holds is skipped (Substring of the root), as no
       one can compare with it;
     - the first child forced of a parent that is itself a view of a real
       str, while others hold the parent, is Substring of the root too,
       and the parent records it ('alias'): forced later, the parent is
       that child if their bounds are equal, as Substring(parent, ...)
       would have given the parent itself, else its own Substring;
     - otherwise the parent is forced first.
   Every other read of a field (state, hash, the characters through any
   other meta) forces the view. */
static condition_code_t state_at_least(PsycoObject *po, vinfo_t *v, int bits);

#define iUNICODE_LENGTH FIELD_INDEX(UNICODE_length)
#define iVIEW_ROOT FIELDS_TOTAL(UNICODE_state)
#define iVIEW_START (iVIEW_ROOT + 1)
#define iVIEW_PARENT (iVIEW_ROOT + 2)
#define iVIEW_REL (iVIEW_ROOT + 3)
#define iVIEW_ALIAS (iVIEW_ROOT + 4)
#define iVIEW_ALIAS_START (iVIEW_ROOT + 5)
#define iVIEW_ALIAS_LEN (iVIEW_ROOT + 6)
#define VIEW_TOTAL (iVIEW_ROOT + 7)

static source_virtual_t psyco_computed_view;

static bool is_view(vinfo_t *v) {
  return v->source == VirtualTime_New(&psyco_computed_view);
}

/* lower() or upper() not yet run, on a compact ASCII root (below) */
#define iCASE_ROOT FIELDS_TOTAL(UNICODE_state)
#define iCASE_START (iCASE_ROOT + 1)
#define iCASE_UPPER (iCASE_ROOT + 2)
#define CASE_TOTAL (iCASE_ROOT + 3)

static source_virtual_t psyco_computed_casemap;

static bool is_casemap(vinfo_t *v) {
  return v->source == VirtualTime_New(&psyco_computed_casemap);
}

/* takes the references to start, len and rel */
static vinfo_t *new_view(PsycoObject *po, vinfo_t *root, vinfo_t *start,
                         vinfo_t *len, vinfo_t *parent, vinfo_t *rel) {
  vinfo_t *v = vinfo_new(VirtualTime_New(&psyco_computed_view));
  vinfo_t **items;
  v->array = array_new(VIEW_TOTAL);
  items = v->array->items;
  items[iOB_TYPE] = vinfo_new(CompileTime_New((long)&PyUnicode_Type));
  assert_nonneg(len);
  items[iUNICODE_LENGTH] = len;
  need_reference(po, root);
  vinfo_incref(root);
  items[iVIEW_ROOT] = root;
  items[iVIEW_START] = start;
  if (parent != NULL) {
    vinfo_incref(parent);
    items[iVIEW_PARENT] = parent;
    items[iVIEW_REL] = rel;
  }
  return v;
}

/* a view with an alias: the alias if the bounds are equal */
static PyObject *cimpl_view_or_alias(PyObject *root, Py_ssize_t start,
                                     Py_ssize_t len, PyObject *alias,
                                     Py_ssize_t astart, Py_ssize_t alen) {
  if (astart == start && alen == len) {
    Py_INCREF(alias);
    return alias;
  }
  return PyUnicode_Substring(root, start, start + len);
}

static bool compute_view(PsycoObject *po, vinfo_t *v) {
  vinfo_t **items = v->array->items;
  vinfo_t *parent = items[iVIEW_PARENT], *alias_of = NULL;
  vinfo_t *base = items[iVIEW_ROOT], *start = items[iVIEW_START], *end, *r;
  if (items[iVIEW_ALIAS] != NULL) {
    r = psyco_generic_call(po, cimpl_view_or_alias,
                           CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                           "vvvvvv", base, start, items[iUNICODE_LENGTH],
                           items[iVIEW_ALIAS], items[iVIEW_ALIAS_START],
                           items[iVIEW_ALIAS_LEN]);
    goto done;
  }
  if (parent == NULL ||
      (is_virtualtime(parent->source) && parent->refcount == 1))
    ; /* the root */
  else if (is_virtualtime(parent->source) &&
           parent->array->items[iVIEW_PARENT] == NULL &&
           vinfo_getitem(parent, iVIEW_ALIAS) == NULL)
    alias_of = parent;
  else {
    if (!compute_vinfo(parent, po))
      return false;
    base = parent;
    start = items[iVIEW_REL];
  }
  end = integer_add(po, start, items[iUNICODE_LENGTH], false);
  if (end == NULL)
    return false;
  r = psyco_generic_call(po, PyUnicode_Substring,
                         CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                         "vvv", base, start, end);
  vinfo_decref(end, po);
done:
  if (r == NULL)
    return false;
  if (alias_of != NULL) {
    vinfo_t **p = alias_of->array->items;
    vinfo_incref(items[iVIEW_START]);
    p[iVIEW_ALIAS_START] = items[iVIEW_START];
    vinfo_incref(items[iUNICODE_LENGTH]);
    p[iVIEW_ALIAS_LEN] = items[iUNICODE_LENGTH];
  }
  /* the length stays: it is the real string's */
  vinfo_array_shrink(po, v, iVIEW_ROOT);
  vinfo_move(po, v, r);
  if (alias_of != NULL) {
    vinfo_incref(v); /* the alias */
    vinfo_setitem(po, alias_of, iVIEW_ALIAS, v);
  }
  return true;
}

static PyObject *direct_compute_view(vinfo_t *v, char *data) {
  vinfo_t *parent = vinfo_getitem(v, iVIEW_PARENT);
  vinfo_t *alias = vinfo_getitem(v, iVIEW_ALIAS);
  int real_parent = parent != NULL && !is_virtualtime(parent->source);
  PyObject *base, *r;
  Py_ssize_t start, len;
  len = direct_read_vinfo(vinfo_getitem(v, iUNICODE_LENGTH), data);
  start = direct_read_vinfo(
      vinfo_getitem(v, real_parent ? iVIEW_REL : iVIEW_START), data);
  if ((len == -1 || start == -1) && PyErr_Occurred())
    return NULL;
  if (alias != NULL &&
      direct_read_vinfo(vinfo_getitem(v, iVIEW_ALIAS_START), data) == start &&
      direct_read_vinfo(vinfo_getitem(v, iVIEW_ALIAS_LEN), data) == len)
    return direct_xobj_vinfo(alias, data);
  base = direct_xobj_vinfo(vinfo_getitem(v, real_parent ? iVIEW_PARENT
                                                        : iVIEW_ROOT),
                           data);
  if (base == NULL)
    return NULL;
  r = PyUnicode_Substring(base, start, start + len);
  Py_DECREF(base);
  return r;
}

/* a str whose characters helpers can read: a view, or an exact str that
   is ready (tested at run time). New references to its root, its start
   there and its length, and the view itself as parent or NULL. 1, 0 for
   any other value, -1 on an error. */
static int view_parts(PsycoObject *po, vinfo_t *v, vinfo_t **root,
                      vinfo_t **start, vinfo_t **len, vinfo_t **parent) {
  if (is_view(v)) {
    vinfo_t **items = v->array->items;
    *root = items[iVIEW_ROOT];
    *start = items[iVIEW_START];
    *len = items[iUNICODE_LENGTH];
    *parent = v;
    vinfo_incref(*root);
    vinfo_incref(*start);
    vinfo_incref(*len);
    vinfo_incref(v);
    return 1;
  }
  if (Psyco_KnownType(v) != &PyUnicode_Type || is_virtualtime(v->source))
    return 0;
  if (is_compiletime(v->source)) {
    if (!PyUnicode_IS_READY((PyObject *)CompileTime_Get(v->source)->value))
      return 0;
  } else {
    condition_code_t cc = state_at_least(po, v,
                                         NI_BIT_PyASCIIObject__state__ready);
    if (cc == CC_ERROR)
      return -1;
    if (!runtime_condition_t(po, cc))
      return 0;
  }
  *len = psyco_get_field(po, v, UNICODE_length);
  if (*len == NULL)
    return -1;
  *root = v;
  vinfo_incref(v);
  *start = psyco_vi_Zero();
  *parent = NULL;
  return 1;
}

static void release_parts(PsycoObject *po, vinfo_t *root, vinfo_t *start,
                          vinfo_t *len, vinfo_t *parent) {
  vinfo_decref(root, po);
  vinfo_decref(start, po);
  vinfo_decref(len, po);
  vinfo_xdecref(parent, po);
}

/* the view of root[i:j] as part of a string with the given parts */
static vinfo_t *sub_view(PsycoObject *po, vinfo_t *root, vinfo_t *start,
                         vinfo_t *parent, vinfo_t *i, vinfo_t *j) {
  vinfo_t *len = integer_sub(po, j, i, false), *rel = NULL;
  if (len == NULL)
    return NULL;
  if (parent != NULL && (rel = integer_sub(po, i, start, false)) == NULL) {
    vinfo_decref(len, po);
    return NULL;
  }
  if (rel != NULL)
    assert_nonneg(rel);
  vinfo_incref(i);
  return new_view(po, root, i, len, parent, rel);
}

/* ---- run-time helpers: CPython's loops on s[i:j] of a ready str ---- */

/* unicodeobject.c's BLOOM_MASK of a ready str, on 64-bit */
static unsigned long bloom_mask(PyObject *sep) {
  unsigned long mask = 0;
  Py_ssize_t k, n = PyUnicode_GET_LENGTH(sep);
  int kind = PyUnicode_KIND(sep);
  void *data = PyUnicode_DATA(sep);
  for (k = 0; k < n; k++)
    mask |= 1UL << (PyUnicode_READ(kind, data, k) & 63);
  return mask;
}

/* A helper giving two bounds returns the first and leaves the second
   here, which the code loads right after the call (VIEW_BOUND_end is
   mutable, so it is never folded or kept): only the call stands between
   the store and the load. */
struct view_bound {
  Py_ssize_t end;
};
static struct view_bound view_bound;
#define VIEW_BOUND_end                                                         \
  FMUT(NONNEG_FIELD(struct view_bound, Py_ssize_t, end, NO_PREV_FIELD))

/* the bound the helper just called left */
static vinfo_t *view_bound_end(PsycoObject *po) {
  vinfo_t *g = vinfo_new(CompileTime_New((long)&view_bound));
  vinfo_t *r = psyco_get_field(po, g, VIEW_BOUND_end);
  vinfo_decref(g, po);
  return r;
}

/* the body once per kind, with 'd' the characters and SPACE CPython's
   whitespace test for them; a compact ASCII string first, its test the
   table (do_strip's ASCII arm) */
#define FOR_KIND(s, BODY)                                                      \
  if (PyUnicode_IS_COMPACT_ASCII(s)) {                                         \
    const Py_UCS1 *d = (const Py_UCS1 *)((PyASCIIObject *)(s) + 1);            \
    BODY(ASCII_SPACE);                                                         \
  }                                                                            \
  switch (PyUnicode_KIND(s)) {                                                 \
  case PyUnicode_1BYTE_KIND: {                                                 \
    const Py_UCS1 *d = PyUnicode_1BYTE_DATA(s);                                \
    BODY(Py_UNICODE_ISSPACE);                                                  \
  }                                                                            \
  case PyUnicode_2BYTE_KIND: {                                                 \
    const Py_UCS2 *d = PyUnicode_2BYTE_DATA(s);                                \
    BODY(Py_UNICODE_ISSPACE);                                                  \
  }                                                                            \
  default: {                                                                   \
    const Py_UCS4 *d = PyUnicode_4BYTE_DATA(s);                                \
    BODY(Py_UNICODE_ISSPACE);                                                  \
  }                                                                            \
  }
#define ASCII_SPACE(ch) (_Py_ascii_whitespace[ch])

/* do_strip's and _PyUnicode_XStrip's loops on s[i:j] (striptype as
   there): the new i, and the new j in view_bound */
#define STRIP_LOOPS(TEST)                                                      \
  if (striptype != 1)                                                          \
    while (i < j && TEST(d[i]))                                                \
      i++;                                                                     \
  if (striptype != 0) {                                                        \
    j--;                                                                       \
    while (j >= i && TEST(d[j]))                                               \
      j--;                                                                     \
    j++;                                                                       \
  }                                                                            \
  view_bound.end = j;                                                          \
  return i

#define STRIP_WS(SPACE) STRIP_LOOPS(SPACE)
static Py_ssize_t cimpl_strip_ws(PyObject *s, Py_ssize_t i, Py_ssize_t j,
                                 long striptype) {
  FOR_KIND(s, STRIP_WS)
}
#undef STRIP_WS

/* with sep's bloom mask */
static Py_ssize_t cimpl_strip_chars(PyObject *s, Py_ssize_t i, Py_ssize_t j,
                                    long striptype, PyObject *sep,
                                    unsigned long mask) {
  Py_ssize_t seplen = PyUnicode_GET_LENGTH(sep);
#define IN_SEP(ch)                                                             \
  ((mask & (1UL << ((ch)&63))) && PyUnicode_FindChar(sep, ch, 0, seplen, 1) >= 0)
#define STRIP_SEP(SPACE) STRIP_LOOPS(IN_SEP)
  FOR_KIND(s, STRIP_SEP)
#undef STRIP_SEP
#undef IN_SEP
}
#undef STRIP_LOOPS

/* split.h:split_whitespace's two loops on s[i:n]: where the next item
   starts (-1 if none does), and where it ends in view_bound */
#define SPLIT_WS(SPACE)                                                        \
  while (i < n && SPACE(d[i]))                                                 \
    i++;                                                                       \
  if (i == n)                                                                  \
    return -1;                                                                 \
  j = i + 1;                                                                   \
  while (j < n && !SPACE(d[j]))                                                \
    j++;                                                                       \
  view_bound.end = j;                                                          \
  return i
static Py_ssize_t cimpl_split_ws(PyObject *s, Py_ssize_t i, Py_ssize_t n) {
  Py_ssize_t j;
  FOR_KIND(s, SPLIT_WS)
}
#undef SPLIT_WS
#undef ASCII_SPACE
#undef FOR_KIND

/* where the item at i ends: the next 'sep' (one character) before n, or
   n */
static Py_ssize_t cimpl_split_char_end(PyObject *s, Py_ssize_t i,
                                       Py_ssize_t n, Py_UCS4 ch) {
  Py_ssize_t r = PyUnicode_FindChar(s, ch, i, n, 1);
  return r < 0 ? n : r;
}

/* the same for a longer 'sep' (ASCII 's', so no conversion of 'sep') */
static Py_ssize_t cimpl_split_sep_end(PyObject *s, Py_ssize_t i,
                                      Py_ssize_t n, PyObject *sep) {
  Py_ssize_t r = PyUnicode_Find(s, sep, i, n, 1);
  return r == -1 ? n : r; /* -2: an error */
}

/* find or rfind of 'sep' in s[i:j]: -1 not found, -2 an error */
static Py_ssize_t cimpl_find(PyObject *s, Py_ssize_t i, Py_ssize_t j,
                             PyObject *sep, long direction) {
  return PyUnicode_Find(s, sep, i, j, (int)direction);
}

/* unicode_eq of s[i:i+n] and the str t, which may not be ready */
static int cimpl_view_eq(PyObject *s, Py_ssize_t i, Py_ssize_t n,
                         PyObject *t) {
  int ks, kt;
  void *ds, *dt;
  Py_ssize_t k;
  if (PyUnicode_READY(t) == -1)
    return -1;
  if (PyUnicode_GET_LENGTH(t) != n)
    return 0;
  ks = PyUnicode_KIND(s);
  kt = PyUnicode_KIND(t);
  ds = PyUnicode_DATA(s);
  dt = PyUnicode_DATA(t);
  if (ks == kt)
    return memcmp((char *)ds + i * ks, dt, n * ks) == 0;
  for (k = 0; k < n; k++)
    if (PyUnicode_READ(ks, ds, i + k) != PyUnicode_READ(kt, dt, k))
      return 0;
  return 1;
}

/* the same of s[i:i+n] and t[j:j+m] */
static int cimpl_views_eq(PyObject *s, Py_ssize_t i, Py_ssize_t n,
                          PyObject *t, Py_ssize_t j, Py_ssize_t m) {
  int ks = PyUnicode_KIND(s), kt = PyUnicode_KIND(t);
  void *ds = PyUnicode_DATA(s), *dt = PyUnicode_DATA(t);
  Py_ssize_t k;
  if (n != m)
    return 0;
  if (ks == kt)
    return memcmp((char *)ds + i * ks, (char *)dt + j * kt, n * ks) == 0;
  for (k = 0; k < n; k++)
    if (PyUnicode_READ(ks, ds, i + k) != PyUnicode_READ(kt, dt, j + k))
      return 0;
  return 1;
}

/* PyUnicode_Contains(s[i:i+n], sub) for a str 'sub' */
static int cimpl_view_contains(PyObject *s, Py_ssize_t i, Py_ssize_t n,
                               PyObject *sub) {
  Py_ssize_t r = PyUnicode_Find(s, sub, i, i + n, 1);
  return r == -2 ? -1 : r >= 0;
}

/* unicode_subscript's step-1 bounds of x[i:j] on a length n
   (PySlice_AdjustIndices): the start, and the length (0 if empty) */
static Py_ssize_t cimpl_slice_start(Py_ssize_t n, Py_ssize_t i,
                                    Py_ssize_t j) {
  return PySlice_AdjustIndices(n, &i, &j, 1) <= 0 ? n : i;
}

static Py_ssize_t cimpl_slice_len(Py_ssize_t n, Py_ssize_t i, Py_ssize_t j) {
  Py_ssize_t r = PySlice_AdjustIndices(n, &i, &j, 1);
  return r < 0 ? 0 : r;
}

/* a helper's call: never CfPure, as some leave a bound in view_bound */
#define VIEW_HELPER (CfReturnTypeLong | CfNoPython | CfNoBlock)

/* unicode_richcompare compares any two str instances, subclasses included
   (an override in a subclass is its own slot, tried by the caller); anything
   else is NotImplemented */
static bool both_str(vinfo_t *v, vinfo_t *w) {
  PyTypeObject *vtp = Psyco_KnownType(v);
  PyTypeObject *wtp = Psyco_KnownType(w);
  return vtp != NULL && wtp != NULL && PyType_IsSubtype(vtp, &PyUnicode_Type) &&
         PyType_IsSubtype(wtp, &PyUnicode_Type);
}

/* _PyUnicode_EQ needs both strings ready (from 3.9 it doesn't ready them
   itself); a failure to ready one, which only running out of memory can
   cause, leaves its MemoryError set and compares unequal */
static int cimpl_unicode_eq(PyObject *a, PyObject *b) {
  return a == b || (PyUnicode_READY(a) == 0 && PyUnicode_READY(b) == 0 &&
                    _PyUnicode_EQ(a, b));
}

/* == of two str, one a view at least: a known int, 1 if equal */
static vinfo_t *views_eq(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t **a, **b;
  if (!is_view(v)) {
    vinfo_t *t = v;
    v = w;
    w = t;
  }
  a = v->array->items;
  if (!is_view(w))
    return psyco_generic_call(po, cimpl_view_eq,
                              CfCommonIntBoolOrError | CfNoPython | CfNoBlock,
                              "vvvv", a[iVIEW_ROOT], a[iVIEW_START],
                              a[iUNICODE_LENGTH], w);
  b = w->array->items;
  return psyco_generic_call(po, cimpl_views_eq,
                            CfReturnTypeInt | CfNoPython | CfNoBlock |
                                CfPure,
                            "vvvvvv", a[iVIEW_ROOT], a[iVIEW_START],
                            a[iUNICODE_LENGTH], b[iVIEW_ROOT],
                            b[iVIEW_START], b[iUNICODE_LENGTH]);
}

static int cimpl_casemap_eq(PyObject *root, Py_ssize_t start, Py_ssize_t n,
                            long upper, PyObject *t);

/* unicode_richcompare. Strings are immutable, so two constants compare at
   compile time; == and != against a run-time string are a C call giving a
   known bool, not a new object to test with PyObject_IsTrue. */
static vinfo_t *punicode_richcompare(PsycoObject *po, vinfo_t *v, vinfo_t *w,
                                     int op) {
  vinfo_t *x;
  condition_code_t cc;
  if (!both_str(v, w))
    return psyco_vi_NotImplemented();
  if ((op == Py_EQ || op == Py_NE) &&
      ((is_casemap(v) && !is_virtualtime(w->source)) ||
       (is_casemap(w) && !is_virtualtime(v->source)))) {
    vinfo_t *m = is_casemap(v) ? v : w, *t = m == v ? w : v;
    vinfo_t **a = m->array->items;
    x = psyco_generic_call(po, cimpl_casemap_eq,
                           CfCommonIntBoolOrError | CfNoPython | CfNoBlock,
                           "vvvvv", a[iCASE_ROOT], a[iCASE_START],
                           a[iUNICODE_LENGTH], a[iCASE_UPPER], t);
    goto eq;
  }
  if ((op == Py_EQ || op == Py_NE) && (is_view(v) || is_view(w))) {
    x = views_eq(po, v, w);
    goto eq;
  }
  if (is_compiletime(v->source) && is_compiletime(w->source)) {
    PyObject *r = PyUnicode_Type.tp_richcompare(
        (PyObject *)CompileTime_Get(v->source)->value,
        (PyObject *)CompileTime_Get(w->source)->value, op);
    if (r == NULL) {
      psyco_virtualize_exception(po);
      return NULL;
    }
    cc = r == Py_True ? CC_ALWAYS_TRUE : CC_ALWAYS_FALSE;
    Py_DECREF(r);
    return PsycoBool_FromCondition(po, cc);
  }
  if (op != Py_EQ && op != Py_NE)
    return psyco_generic_call(po, PyUnicode_Type.tp_richcompare,
                              CfCommonNewRefPyObject, "vvl", v, w, (long)op);
  x = psyco_generic_call(po, cimpl_unicode_eq, CfPure | CfReturnTypeInt, "vv",
                         v, w);
eq:
  if (x == NULL)
    return NULL;
  cc = integer_non_null(po, x);
  vinfo_decref(x, po);
  if (cc == CC_ERROR)
    return NULL;
  if (op == Py_NE)
    cc = INVERT_CC(cc);
  return PsycoBool_FromCondition(po, cc);
}

/* the state byte is >= 'bits' at run time: a condition code */
static condition_code_t state_at_least(PsycoObject *po, vinfo_t *v, int bits) {
  vinfo_t *st = psyco_get_const(po, v, UNICODE_state);
  if (st == NULL)
    return CC_ERROR;
  return integer_cmp_i(po, st, bits, Py_GE | COMPARE_UNSIGNED);
}

/* unicode_subscript's integer case, for any str */
static PyObject *cimpl_unicode_item(PyObject *s, Py_ssize_t i) {
  if (PyUnicode_READY(s) == -1)
    return NULL;
  if (i < 0)
    i += PyUnicode_GET_LENGTH(s);
  if (i < 0 || i >= PyUnicode_GET_LENGTH(s)) {
    PyErr_SetString(PyExc_IndexError, "string index out of range");
    return NULL;
  }
  return PyUnicode_FromOrdinal(PyUnicode_READ_CHAR(s, i));
}

/* the same for i of the view s[start:start+n] of a ready str */
static PyObject *cimpl_view_item(PyObject *s, Py_ssize_t start, Py_ssize_t n,
                                 Py_ssize_t i) {
  if (i < 0)
    i += n;
  if (i < 0 || i >= n) {
    PyErr_SetString(PyExc_IndexError, "string index out of range");
    return NULL;
  }
  return PyUnicode_FromOrdinal(PyUnicode_READ_CHAR(s, start + i));
}

/* unicode_subscript's integer case then unicode_getitem, inline when the
   characters' place and size are known: 'data' bytes in, 'kind' bytes
   each. Every 1-byte character is below 256, so in latin1[]. A view reads
   its root's character, the one its Substring would hold. */
static vinfo_t *punicode_item(PsycoObject *po, vinfo_t *v, vinfo_t *i) {
  int kind = PyUnicode_1BYTE_KIND;
  long data = sizeof(PyASCIIObject);
  vinfo_t *root = v, *vlen, *ch, *r, *release_me = NULL;
  condition_code_t cc;
  defield_t df;

  if (is_view(v))
    root = v->array->items[iVIEW_ROOT];
  if (is_compiletime(root->source)) {
    PyObject *s = (PyObject *)CompileTime_Get(root->source)->value;
    if (!PyUnicode_IS_READY(s) || !PyUnicode_IS_COMPACT(s))
      goto helper;
    kind = PyUnicode_KIND(s);
    data = (char *)PyUnicode_DATA(s) - (char *)s;
  } else {
    cc = state_at_least(po, root, STATE_COMPACT_ASCII);
    if (cc == CC_ERROR)
      return NULL;
    if (!runtime_condition_t(po, cc))
      goto helper;
  }

  if (root != v)
    vlen = v->array->items[iUNICODE_LENGTH];
  else if ((vlen = psyco_get_const(po, v, UNICODE_length)) == NULL)
    return NULL;
  cc = integer_cmp_i(po, i, 0, Py_LT);
  if (cc == CC_ERROR)
    return NULL;
  if (runtime_condition_f(po, cc)) {
    release_me = i = integer_add(po, i, vlen, false);
    if (i == NULL)
      return NULL;
  } else
    assert_nonneg(i);
  cc = integer_cmp(po, i, vlen, Py_GE | COMPARE_UNSIGNED);
  if (cc == CC_ERROR || runtime_condition_f(po, cc)) {
    if (cc != CC_ERROR)
      PycException_SetString(po, PyExc_IndexError,
                             "string index out of range");
    vinfo_xdecref(release_me, po);
    return NULL;
  }
  if (root != v) {
    vinfo_t *idx = integer_add(po, i, v->array->items[iVIEW_START], false);
    vinfo_xdecref(release_me, po);
    if (idx == NULL)
      return NULL;
    assert_nonneg(idx);
    release_me = i = idx;
  }

  df = kind == PyUnicode_1BYTE_KIND   ? UNSIGNED_ARRAY(Py_UCS1, data)
       : kind == PyUnicode_2BYTE_KIND ? UNSIGNED_ARRAY(Py_UCS2, data)
                                      : UNSIGNED_ARRAY(Py_UCS4, data);
  ch = psyco_get_field_array(po, root, df, i);
  vinfo_xdecref(release_me, po);
  if (ch == NULL)
    return NULL;
  if (kind == PyUnicode_1BYTE_KIND) {
    vinfo_t *table = vinfo_new(CompileTime_New((long)latin1));
    r = psyco_get_field_array(po, table, FPYREF(DEF_ARRAY(PyObject *, 0)), ch);
    vinfo_decref(table, po);
  } else
    r = psyco_generic_call(po, PyUnicode_FromOrdinal,
                           CfPure | CfCommonNewRefPyObject, "v", ch);
  vinfo_decref(ch, po);
  if (r != NULL && !is_compiletime(r->source))
    Psyco_AssertType(po, r, &PyUnicode_Type);
  return r;

helper:
  if (root != v) {
    vinfo_t **a = v->array->items;
    return psyco_generic_call(po, cimpl_view_item,
                              CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                              "vvvv", root, a[iVIEW_START], a[iUNICODE_LENGTH],
                              i);
  }
  return psyco_generic_call(po, cimpl_unicode_item, CfCommonNewRefPyObject,
                            "vv", v, i);
}

/* unicode_subscript's slice case with step 1, after PySlice_Unpack. The
   empty string is CPython's shared one, and PyUnicode_Substring gives the
   string itself for a full slice of an exact str (unicode_result_unchanged),
   a copy for a subclass. */
static PyObject *cimpl_unicode_slice(PyObject *self, Py_ssize_t start,
                                     Py_ssize_t stop) {
  if (PyUnicode_READY(self) == -1)
    return NULL;
  if (PySlice_AdjustIndices(PyUnicode_GET_LENGTH(self), &start, &stop, 1) <=
      0)
    return PyUnicode_New(0, 0);
  return PyUnicode_Substring(self, start, stop);
}

#define VIEW_NOT_ONE ((vinfo_t *)1)

/* v[i:j], step 1, as a view of a view or of an exact str: the bounds
   clipped as unicode_subscript's are. VIEW_NOT_ONE if v is neither. */
static vinfo_t *slice_view(PsycoObject *po, vinfo_t *v, vinfo_t *i,
                           vinfo_t *j) {
  vinfo_t *root, *start, *len, *parent, *st, *n, *abs, *rel = NULL;
  vinfo_t *r = NULL;
  switch (view_parts(po, v, &root, &start, &len, &parent)) {
  case -1:
    return NULL;
  case 0:
    return VIEW_NOT_ONE;
  }
  st = psyco_generic_call(po, cimpl_slice_start, VIEW_HELPER | CfPure, "vvv",
                          len, i,
                          j);
  if (st == NULL)
    goto done;
  n = psyco_generic_call(po, cimpl_slice_len, VIEW_HELPER | CfPure, "vvv", len,
                         i, j);
  if (n == NULL) {
    vinfo_decref(st, po);
    goto done;
  }
  assert_nonneg(st);
  abs = integer_add(po, start, st, false);
  if (abs == NULL) {
    vinfo_decref(n, po);
    vinfo_decref(st, po);
    goto done;
  }
  if (parent != NULL) {
    rel = st;
    vinfo_incref(rel);
  }
  vinfo_decref(st, po);
  r = new_view(po, root, abs, n, parent, rel);
done:
  release_parts(po, root, start, len, parent);
  return r;
}

/* unicode_subscript: an int index or a virtual slice here, anything else in
   C. A slice of a constant exact str folds. */
static vinfo_t *punicode_subscript(PsycoObject *po, vinfo_t *v,
                                   vinfo_t *key) {
  vinfo_t *i, *j, *r;
  PyTypeObject *ktp = Psyco_NeedType(po, key);
  if (ktp == NULL)
    return NULL;
  if (ktp == &PySlice_Type) {
    switch (PsycoSlice_Unpack(po, key, &i, &j)) {
    case -1:
      return NULL;
    case 1:
      if (!(is_compiletime(v->source) && is_compiletime(i->source) &&
            is_compiletime(j->source))) {
        r = slice_view(po, v, i, j);
        if (r != VIEW_NOT_ONE) {
          vinfo_decref(j, po);
          vinfo_decref(i, po);
          return r;
        }
      }
      r = psyco_generic_call(
          po, cimpl_unicode_slice,
          CfCommonNewRefPyObject |
              (Psyco_KnownType(v) == &PyUnicode_Type ? CfPure : 0),
          "vvv", v, i, j);
      vinfo_decref(j, po);
      vinfo_decref(i, po);
      if (r != NULL && !is_compiletime(r->source))
        Psyco_AssertType(po, r, &PyUnicode_Type);
      return r;
    }
  }
  if (!PyType_IsSubtype(ktp, &PyLong_Type))
    return psyco_generic_call(po, unicode_subscript, CfCommonNewRefPyObject,
                              "vv", v, key);
  i = PsycoNumber_AsSsize_t(po, key, PyExc_IndexError);
  if (i == NULL)
    return NULL;
  r = punicode_item(po, v, i);
  vinfo_decref(i, po);
  return r;
}

/* ---- methods that make views ---- */

/* a constant exact str that is ready, or NULL */
static PyObject *const_str(vinfo_t *v) {
  PyObject *o;
  if (!is_compiletime(v->source))
    return NULL;
  o = (PyObject *)CompileTime_Get(v->source)->value;
  return o != NULL && PyUnicode_CheckExact(o) && PyUnicode_IS_READY(o) ? o
                                                                       : NULL;
}

/* lstrip, rstrip and strip (do_argstrip's striptype) with no argument,
   None or a constant str: the bounds of _PyUnicode_XStrip's or do_strip's
   loops, then their Substring as a view. Anything else is the translated
   meta. */
static vinfo_t *strip_view(PsycoObject *po, vinfo_t *self, vinfo_t *vargs,
                           int striptype) {
  vinfo_t *root, *start, *len, *parent, *end, *i, *j, *r = NULL;
  vinfo_t *chars = NULL;
  unsigned long mask = 0;
  int n = PsycoTuple_Load(vargs);
  if (n < 0 || n > 1)
    return VIEW_NOT_ONE;
  if (n == 1) {
    chars = PsycoTuple_GET_ITEM(vargs, 0);
    if (psyco_knowntobe(chars, (long)Py_None))
      chars = NULL;
    else if (const_str(chars) == NULL)
      return VIEW_NOT_ONE;
    else
      mask = bloom_mask(const_str(chars));
  }
  switch (view_parts(po, self, &root, &start, &len, &parent)) {
  case -1:
    return NULL;
  case 0:
    return VIEW_NOT_ONE;
  }
  end = integer_add(po, start, len, false);
  if (end == NULL)
    goto done;
  if (chars == NULL)
    i = psyco_generic_call(po, cimpl_strip_ws, VIEW_HELPER, "vvvl", root,
                           start, end, (long)striptype);
  else
    i = psyco_generic_call(po, cimpl_strip_chars, VIEW_HELPER, "vvvlvl", root,
                           start, end, (long)striptype, chars, (long)mask);
  if (i != NULL) {
    assert_nonneg(i);
    j = view_bound_end(po);
    if (j != NULL) {
      r = sub_view(po, root, start, parent, i, j);
      vinfo_decref(j, po);
    }
    vinfo_decref(i, po);
  }
  vinfo_decref(end, po);
done:
  release_parts(po, root, start, len, parent);
  return r;
}

#define STRIP_META(NAME, TYPE)                                                 \
  static vinfo_t *p##NAME(PsycoObject *po, vinfo_t *self, vinfo_t *vargs) {   \
    vinfo_t *r = strip_view(po, self, vargs, TYPE);                            \
    return r != VIEW_NOT_ONE ? r : NAME(po, self, vargs);                      \
  }
STRIP_META(unicode_lstrip, 0)
STRIP_META(unicode_rstrip, 1)
STRIP_META(unicode_strip, 2)
#undef STRIP_META

static PyCFunction cfunc_unicode_partition, cfunc_unicode_rpartition;

/* PyUnicode_Partition and PyUnicode_RPartition (the stringlib functions
   they call) with a constant non-empty separator: found, the pieces
   before and after are views (STRINGLIB_NEW gives what Substring does
   there, as neither can be the whole string) and the middle is the
   separator itself; not found, the string itself and two shared ''. */
static vinfo_t *partition_view(PsycoObject *po, vinfo_t *self, vinfo_t *sep,
                               int right) {
  vinfo_t *root, *start, *len, *parent, *end, *pos, *after, *r = NULL;
  vinfo_t *items[3] = {NULL, NULL, NULL};
  condition_code_t cc;
  PyObject *s = const_str(sep);
  int k;
  if (s == NULL || PyUnicode_GET_LENGTH(s) == 0)
    return VIEW_NOT_ONE;
  switch (view_parts(po, self, &root, &start, &len, &parent)) {
  case -1:
    return NULL;
  case 0:
    return VIEW_NOT_ONE;
  }
  end = integer_add(po, start, len, false);
  if (end == NULL)
    goto done;
  pos = psyco_generic_call(po, cimpl_find,
                           CfReturnTypeLong | CfPyErrCheckNeg | CfNoPython |
                               CfNoBlock,
                           "vvvvl", root, start, end, sep, right ? -1L : 1L);
  if (pos == NULL)
    goto done_end;
  cc = integer_cmp_i(po, pos, 0, Py_GE);
  if (cc == CC_ERROR)
    goto done_pos;
  if (runtime_condition_t(po, cc)) {
    assert_nonneg(pos);
    after = integer_add_i(po, pos, (long)PyUnicode_GET_LENGTH(s), false);
    if (after == NULL)
      goto done_pos;
    items[0] = sub_view(po, root, start, parent, start, pos);
    items[2] = sub_view(po, root, start, parent, after, end);
    vinfo_decref(after, po);
    if (items[0] == NULL || items[2] == NULL)
      goto done_items;
    items[1] = sep;
    vinfo_incref(sep);
  } else {
    for (k = 0; k < 3; k++)
      items[k] = vinfo_new(CompileTime_New((long)unicode_empty));
    vinfo_decref(items[right ? 2 : 0], po);
    items[right ? 2 : 0] = self;
    vinfo_incref(self);
  }
  r = PsycoTuple_New(3, items);
done_items:
  for (k = 0; k < 3; k++)
    vinfo_xdecref(items[k], po);
done_pos:
  vinfo_decref(pos, po);
done_end:
  vinfo_decref(end, po);
done:
  release_parts(po, root, start, len, parent);
  return r;
}

static vinfo_t *punicode_partition(PsycoObject *po, vinfo_t *self,
                                   vinfo_t *sep) {
  vinfo_t *r = partition_view(po, self, sep, 0);
  return r != VIEW_NOT_ONE ? r
                           : psyco_generic_call(po, cfunc_unicode_partition,
                                                CfCommonNewRefPyObject, "vv",
                                                self, sep);
}

static vinfo_t *punicode_rpartition(PsycoObject *po, vinfo_t *self,
                                    vinfo_t *sep) {
  vinfo_t *r = partition_view(po, self, sep, 1);
  return r != VIEW_NOT_ONE ? r
                           : psyco_generic_call(po, cfunc_unicode_rpartition,
                                                CfCommonNewRefPyObject, "vv",
                                                self, sep);
}

/* ---- lower() and upper() of an ASCII str ---- */

/* A case map is unicode_lower_impl's or unicode_upper_impl's ASCII path
   not yet run: ascii_upper_or_lower on 'len' characters of 'root' from
   'start', where root is a real str, compact and ASCII, and 'upper' a
   constant 0 or 1. It is made from an exact str, a view or another case
   map (whose map the new one replaces: on ASCII, lower(upper(x)) is
   lower(x)), so 'w.strip().lower()' reads the root's bytes once and
   builds one string. Forced, it is ascii_upper_or_lower's body:
   PyUnicode_New(len, 127), so '' is the shared one and one character a
   new object, as CPython's, then the map. A force for a dict key
   (PsycoUnicode_ForceKey) also stores the hash, computed in the same pass
   as the map where CPython's hash is the SipHash-2-4 below. len reads it
   unforced, == and != against a real str compare through the map, and
   anything else forces it. */

/* the bytes in 'A'..'Z' (or 'a'..'z') of 8 ASCII bytes, case flipped: no
   byte's sum carries, as each is below 0x80 */
static inline uint64_t case_map8(uint64_t x, long upper) {
  const uint64_t ones = 0x0101010101010101ULL;
  uint64_t lo = upper ? 'a' : 'A';
  uint64_t ge = x + ones * (0x80 - lo), gt = x + ones * (0x80 - lo - 26);
  return x ^ (((ge & ~gt) & (ones * 0x80)) >> 2);
}

/* The tail of fewer than 8 bytes as a little-endian word, and its store:
   two loads or stores of a fixed size that overlap where n is not a power
   of two (the overlapping bytes are the same), so no call to memcpy for a
   length known only at run time, which costs more than the hash rounds on
   a short word, and no byte past the n given is read or written. */
static inline uint64_t load_tail(const unsigned char *s, Py_ssize_t n) {
  uint32_t a, b;
  uint16_t c, d;
  if (n >= 4) {
    memcpy(&a, s, 4);
    memcpy(&b, s + n - 4, 4);
    return a | ((uint64_t)b << (8 * (n - 4)));
  }
  if (n >= 2) {
    memcpy(&c, s, 2);
    memcpy(&d, s + n - 2, 2);
    return c | ((uint64_t)d << (8 * (n - 2)));
  }
  return n ? s[0] : 0;
}

static inline void store_tail(char *s, uint64_t w, Py_ssize_t n) {
  uint32_t a, b;
  uint16_t c, d;
  if (n >= 4) {
    a = (uint32_t)w;
    b = (uint32_t)(w >> (8 * (n - 4)));
    memcpy(s, &a, 4);
    memcpy(s + n - 4, &b, 4);
  } else if (n >= 2) {
    c = (uint16_t)w;
    d = (uint16_t)(w >> (8 * (n - 2)));
    memcpy(s, &c, 2);
    memcpy(s + n - 2, &d, 2);
  } else if (n)
    s[0] = (char)w;
}

static void case_map(char *dst, const char *src, Py_ssize_t n, long upper) {
  uint64_t w;
  for (; n >= 8; n -= 8, src += 8, dst += 8) {
    memcpy(&w, src, 8);
    w = case_map8(w, upper);
    memcpy(dst, &w, 8);
  }
  w = case_map8(load_tail((const unsigned char *)src, n), upper);
  store_tail(dst, w, n);
}

/* pyhash.c's siphash24 on the mapped bytes as they are written, x86-64
   being little-endian; used only if it gives _Py_HashBytes's values
   (case_hash_checked) */
#define SIP_ROTATE(x, b) (uint64_t)(((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_HALF_ROUND(a, b, c, d, s, t)                                       \
  a += b;                                                                      \
  c += d;                                                                      \
  b = SIP_ROTATE(b, s) ^ a;                                                    \
  d = SIP_ROTATE(d, t) ^ c;                                                    \
  a = SIP_ROTATE(a, 32);
#define SIP_DOUBLE_ROUND(v0, v1, v2, v3)                                       \
  SIP_HALF_ROUND(v0, v1, v2, v3, 13, 16);                                      \
  SIP_HALF_ROUND(v2, v1, v0, v3, 17, 21);                                      \
  SIP_HALF_ROUND(v0, v1, v2, v3, 13, 16);                                      \
  SIP_HALF_ROUND(v2, v1, v0, v3, 17, 21);

static Py_hash_t case_map_hash(char *dst, const char *src, Py_ssize_t n,
                               long upper) {
  uint64_t k0 = (uint64_t)_Py_HashSecret.siphash.k0;
  uint64_t k1 = (uint64_t)_Py_HashSecret.siphash.k1;
  uint64_t b = (uint64_t)n << 56, w, t;
  uint64_t v0 = k0 ^ 0x736f6d6570736575ULL;
  uint64_t v1 = k1 ^ 0x646f72616e646f6dULL;
  uint64_t v2 = k0 ^ 0x6c7967656e657261ULL;
  uint64_t v3 = k1 ^ 0x7465646279746573ULL;
  Py_hash_t x;
  for (; n >= 8; n -= 8, src += 8, dst += 8) {
    memcpy(&w, src, 8);
    w = case_map8(w, upper);
    memcpy(dst, &w, 8);
    v3 ^= w;
    SIP_DOUBLE_ROUND(v0, v1, v2, v3);
    v0 ^= w;
  }
  w = case_map8(load_tail((const unsigned char *)src, n), upper);
  store_tail(dst, w, n);
  b |= w;
  v3 ^= b;
  SIP_DOUBLE_ROUND(v0, v1, v2, v3);
  v0 ^= b;
  v2 ^= 0xff;
  SIP_DOUBLE_ROUND(v0, v1, v2, v3);
  SIP_DOUBLE_ROUND(v0, v1, v2, v3);
  t = (v0 ^ v1) ^ (v2 ^ v3);
  x = (Py_hash_t)t;
  return x == -1 ? -2 : x; /* _Py_HashBytes's */
}
#undef SIP_DOUBLE_ROUND
#undef SIP_HALF_ROUND
#undef SIP_ROTATE

static int case_hash_checked; /* case_map_hash is _Py_HashBytes */

/* ascii_upper_or_lower on root[start:start+len]; 'hashed': with the hash
   set, as unicode_hash would set it */
static PyObject *cimpl_casemap(PyObject *root, Py_ssize_t start,
                               Py_ssize_t len, long upper, long hashed) {
  PyObject *r = PyUnicode_New(len, 127);
  const char *src = (const char *)((PyASCIIObject *)root + 1) + start;
  char *dst;
  if (r == NULL || len == 0)
    return r;
  dst = (char *)((PyASCIIObject *)r + 1);
  if (!hashed)
    case_map(dst, src, len, upper);
  else if (case_hash_checked)
    ((PyASCIIObject *)r)->hash = case_map_hash(dst, src, len, upper);
  else {
    case_map(dst, src, len, upper);
    ((PyASCIIObject *)r)->hash = _Py_HashBytes(dst, len);
  }
  return r;
}

static bool force_casemap(PsycoObject *po, vinfo_t *v, long hashed) {
  vinfo_t **items = v->array->items;
  vinfo_t *r = psyco_generic_call(
      po, cimpl_casemap, CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
      "vvvvl", items[iCASE_ROOT], items[iCASE_START], items[iUNICODE_LENGTH],
      items[iCASE_UPPER], hashed);
  if (r == NULL)
    return false;
  vinfo_array_shrink(po, v, iCASE_ROOT);
  vinfo_move(po, v, r);
  return true;
}

static bool compute_casemap(PsycoObject *po, vinfo_t *v) {
  return force_casemap(po, v, 0);
}

static PyObject *direct_compute_casemap(vinfo_t *v, char *data) {
  PyObject *root, *r;
  Py_ssize_t start = direct_read_vinfo(vinfo_getitem(v, iCASE_START), data);
  Py_ssize_t len = direct_read_vinfo(vinfo_getitem(v, iUNICODE_LENGTH), data);
  long upper = direct_read_vinfo(vinfo_getitem(v, iCASE_UPPER), data);
  if ((len == -1 || start == -1) && PyErr_Occurred())
    return NULL;
  root = direct_xobj_vinfo(vinfo_getitem(v, iCASE_ROOT), data);
  if (root == NULL)
    return NULL;
  r = cimpl_casemap(root, start, len, upper, 0);
  Py_DECREF(root);
  return r;
}

DEFINEFN
bool PsycoUnicode_ForceKey(PsycoObject *po, vinfo_t *v) {
  return !is_casemap(v) || force_casemap(po, v, 1);
}

DEFINEFN
bool PsycoUnicode_KeyParts(vinfo_t *v, vinfo_t **root, vinfo_t **start,
                           vinfo_t **len, long *map) {
  vinfo_t **items;
  if (is_view(v)) {
    items = v->array->items;
    *root = items[iVIEW_ROOT];
    *start = items[iVIEW_START];
    *map = -1;
  } else if (is_casemap(v)) {
    items = v->array->items;
    *root = items[iCASE_ROOT];
    *start = items[iCASE_START];
    *map = CompileTime_Get(items[iCASE_UPPER]->source)->value;
  } else
    return false;
  *len = items[iUNICODE_LENGTH];
  return true;
}

/* A view's characters are its root's, of the canonical kind where the
   root is ASCII; a case map's are mapped into 'buf' as cimpl_casemap
   writes them, hashed in the same pass. The empty str's hash is 0, as
   unicode_hash's. */
DEFINEFN
int PsycoUnicode_KeyChars(ni_strkey_t *k, Py_hash_t *hash, char *buf,
                          PyObject *root, Py_ssize_t start, Py_ssize_t len,
                          long map) {
  const char *src = (const char *)((PyASCIIObject *)root + 1) + start;
  if (!PyUnicode_IS_COMPACT_ASCII(root))
    return 0;
  k->length = len;
  if (map < 0) {
    k->data = (const Py_UCS1 *)src;
    *hash = _Py_HashBytes(src, len);
    return 1;
  }
  if (len > NI_KEY_BUF)
    return 0;
  k->data = (const Py_UCS1 *)buf;
  if (len == 0)
    *hash = 0;
  else if (case_hash_checked)
    *hash = case_map_hash(buf, src, len, map);
  else {
    case_map(buf, src, len, map);
    *hash = _Py_HashBytes(buf, len);
  }
  return 1;
}

/* takes the references to start and len */
static vinfo_t *new_casemap(PsycoObject *po, vinfo_t *root, vinfo_t *start,
                            vinfo_t *len, int upper) {
  vinfo_t *v = vinfo_new(VirtualTime_New(&psyco_computed_casemap));
  vinfo_t **items;
  v->array = array_new(CASE_TOTAL);
  items = v->array->items;
  items[iOB_TYPE] = vinfo_new(CompileTime_New((long)&PyUnicode_Type));
  assert_nonneg(len);
  items[iUNICODE_LENGTH] = len;
  need_reference(po, root);
  vinfo_incref(root);
  items[iCASE_ROOT] = root;
  items[iCASE_START] = start;
  items[iCASE_UPPER] = vinfo_new(CompileTime_New(upper));
  return v;
}

/* the case map of 'self' (an exact str, a view or a case map), or
   VIEW_NOT_ONE if its characters are not compact ASCII (tested at run
   time) */
static vinfo_t *casemap_of(PsycoObject *po, vinfo_t *self, int upper) {
  vinfo_t *root, *start, *len;
  if (is_casemap(self)) {
    vinfo_t **items = self->array->items;
    vinfo_incref(items[iCASE_START]);
    vinfo_incref(items[iUNICODE_LENGTH]);
    return new_casemap(po, items[iCASE_ROOT], items[iCASE_START],
                       items[iUNICODE_LENGTH], upper);
  }
  if (is_view(self))
    root = self->array->items[iVIEW_ROOT];
  else if (Psyco_KnownType(self) == &PyUnicode_Type &&
           !is_virtualtime(self->source))
    root = self;
  else
    return VIEW_NOT_ONE;
  if (is_compiletime(root->source)) {
    PyObject *o = (PyObject *)CompileTime_Get(root->source)->value;
    if (!PyUnicode_IS_READY(o) || !PyUnicode_IS_COMPACT_ASCII(o))
      return VIEW_NOT_ONE;
  } else {
    condition_code_t cc = state_at_least(po, root, STATE_COMPACT_ASCII);
    if (cc == CC_ERROR)
      return NULL;
    if (!runtime_condition_t(po, cc))
      return VIEW_NOT_ONE;
  }
  if (root != self) {
    start = self->array->items[iVIEW_START];
    len = self->array->items[iUNICODE_LENGTH];
    vinfo_incref(start);
    vinfo_incref(len);
  } else {
    if ((len = psyco_get_const(po, self, UNICODE_length)) == NULL)
      return NULL;
    vinfo_incref(len);
    start = psyco_vi_Zero();
  }
  return new_casemap(po, root, start, len, upper);
}

static PyCFunction cfunc_unicode_lower, cfunc_unicode_upper;

static vinfo_t *casemap_meta(PsycoObject *po, vinfo_t *self, int upper) {
  vinfo_t *r = casemap_of(po, self, upper);
  if (r != VIEW_NOT_ONE)
    return r;
  r = psyco_generic_call(po, upper ? cfunc_unicode_upper : cfunc_unicode_lower,
                         CfCommonNewRefPyObject, "vl", self, 0L);
  if (r != NULL && Psyco_KnownType(self) == &PyUnicode_Type)
    Psyco_AssertType(po, r, &PyUnicode_Type);
  return r;
}

static vinfo_t *punicode_lower(PsycoObject *po, vinfo_t *self,
                               vinfo_t *noarg) {
  return casemap_meta(po, self, 0);
}

static vinfo_t *punicode_upper(PsycoObject *po, vinfo_t *self,
                               vinfo_t *noarg) {
  return casemap_meta(po, self, 1);
}

/* unicode_eq of the case map of root[start:start+n] and the str t, which
   may not be ready: the map's result is ASCII, so of kind 1 */
static int cimpl_casemap_eq(PyObject *root, Py_ssize_t start, Py_ssize_t n,
                            long upper, PyObject *t) {
  const unsigned char *s = (const unsigned char *)((PyASCIIObject *)root + 1) +
                           start;
  const unsigned char *d;
  unsigned char lo = upper ? 'a' : 'A';
  Py_ssize_t k;
  if (PyUnicode_READY(t) == -1)
    return -1;
  if (PyUnicode_GET_LENGTH(t) != n || PyUnicode_KIND(t) != PyUnicode_1BYTE_KIND)
    return 0;
  d = PyUnicode_1BYTE_DATA(t);
  for (k = 0; k < n; k++) {
    unsigned char c = s[k];
    if ((unsigned char)(c - lo) < 26)
      c ^= 0x20;
    if (c != d[k])
      return 0;
  }
  return 1;
}

/* ---- consumers that read a view without forcing it ---- */

/* unicode_length (sq_length and mp_length) */
static vinfo_t *punicode_length(PsycoObject *po, vinfo_t *self) {
  if (is_view(self) || is_casemap(self)) {
    vinfo_t *len = self->array->items[iUNICODE_LENGTH];
    vinfo_incref(len);
    return len;
  }
  return unicode_length(po, self);
}

/* PyUnicode_Contains, a view holding any str */
static vinfo_t *punicode_contains(PsycoObject *po, vinfo_t *str,
                                  vinfo_t *sub) {
  PyTypeObject *tp;
  if (is_view(str) && (tp = Psyco_KnownType(sub)) != NULL &&
      PyType_IsSubtype(tp, &PyUnicode_Type)) {
    vinfo_t **a = str->array->items;
    return psyco_generic_call(po, cimpl_view_contains,
                              CfCommonIntBoolOrError | CfNoPython | CfNoBlock,
                              "vvvv", a[iVIEW_ROOT], a[iVIEW_START],
                              a[iUNICODE_LENGTH], sub);
  }
  return PsycoUnicode_Contains(po, str, sub);
}

/* ---- s.split([sep]) ---- */

/* The list, virtual: its source (an exact str or a view) and its
   separator (a constant str, or NULL for whitespace) past list's own
   fields, which it doesn't have. Forced, it is PyUnicode_Split, which
   str.split's impl is for a str or None separator. Fused with a for loop
   (PsycoUnicode_SplitIter), each item is a view of the source: split.h's
   STRINGLIB_NEW gives what Substring does, and the string itself as the
   one item when it holds no separator. A view source's root and bounds
   are kept as the list is made, as the scan reads the root even after
   the view itself is forced; its items are views of it, the parent. */
#define iSPLIT_SRC LIST_TOTAL
#define iSPLIT_SEP (LIST_TOTAL + 1)
#define iSPLIT_ROOT (LIST_TOTAL + 2) /* NULL: the source is the root */
#define iSPLIT_START (LIST_TOTAL + 3)
#define iSPLIT_END (LIST_TOTAL + 4)
#define SPLIT_TOTAL (LIST_TOTAL + 5)

static source_virtual_t psyco_computed_splitlist;
static PyCFunction cfunc_unicode_split;

DEFINEFN
bool PsycoUnicode_IsSplitList(vinfo_t *v) {
  return v->source == VirtualTime_New(&psyco_computed_splitlist);
}

static bool compute_splitlist(PsycoObject *po, vinfo_t *v) {
  vinfo_t *r = psyco_generic_call(
      po, PyUnicode_Split, CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
      "vvl", v->array->items[iSPLIT_SRC], v->array->items[iSPLIT_SEP], -1L);
  if (r == NULL)
    return false;
  vinfo_array_shrink(po, v, iSPLIT_SRC);
  vinfo_move(po, v, r);
  return true;
}

static PyObject *direct_compute_splitlist(vinfo_t *v, char *data) {
  PyObject *src = direct_xobj_vinfo(vinfo_getitem(v, iSPLIT_SRC), data);
  PyObject *sep, *r;
  if (src == NULL)
    return NULL;
  sep = (PyObject *)direct_read_vinfo(vinfo_getitem(v, iSPLIT_SEP), data);
  r = PyUnicode_Split(src, sep, -1);
  Py_DECREF(src);
  return r;
}

/* unicode_split with no argument, None or a constant non-empty str, and
   an exact str or a view as receiver. Anything else, maxsplit included,
   is CPython's function. */
static vinfo_t *punicode_split(PsycoObject *po, vinfo_t *self,
                               vinfo_t *vargs) {
  vinfo_t *sep = NULL, *r;
  int n = PsycoTuple_Load(vargs);
  if (Psyco_KnownType(self) != &PyUnicode_Type || n < 0 || n > 1)
    goto generic;
  if (n == 1) {
    sep = PsycoTuple_GET_ITEM(vargs, 0);
    if (psyco_knowntobe(sep, (long)Py_None))
      sep = NULL;
    else if (const_str(sep) == NULL ||
             PyUnicode_GET_LENGTH(const_str(sep)) == 0)
      goto generic;
  }
  if (!is_view(self) && !compute_vinfo(self, po))
    return NULL;
  need_reference(po, self);
  r = vinfo_new(VirtualTime_New(&psyco_computed_splitlist));
  r->array = array_new(SPLIT_TOTAL);
  r->array->items[iOB_TYPE] = vinfo_new(CompileTime_New((long)&PyList_Type));
  vinfo_incref(self);
  r->array->items[iSPLIT_SRC] = self;
  if (sep != NULL)
    vinfo_incref(sep);
  else
    sep = psyco_vi_Zero();
  r->array->items[iSPLIT_SEP] = sep;
  if (is_view(self)) {
    vinfo_t **a = self->array->items;
    vinfo_t *end = integer_add(po, a[iVIEW_START], a[iUNICODE_LENGTH], false);
    if (end == NULL) {
      vinfo_decref(r, po);
      return NULL;
    }
    vinfo_incref(a[iVIEW_ROOT]);
    r->array->items[iSPLIT_ROOT] = a[iVIEW_ROOT];
    vinfo_incref(a[iVIEW_START]);
    r->array->items[iSPLIT_START] = a[iVIEW_START];
    r->array->items[iSPLIT_END] = end;
  }
  return r;
generic:
  return PsycoCFunction_CallFast(po, cfunc_unicode_split,
                                 METH_FASTCALL | METH_KEYWORDS, self, vargs);
}

/* The iterator is Ni's virtual sequence iterator over the list (forced,
   it is that iterator over the real list at the index of the items
   given), with the scan position as one more word, in the root's
   characters when the source is a view. The root must be ready (a
   run-time test) and, for a separator longer than one character, ASCII,
   so that PyUnicode_Find never converts the separator once per item;
   otherwise it is the plain iterator, which forces the list. */
DEFINEFN
vinfo_t *PsycoUnicode_SplitIter(PsycoObject *po, vinfo_t *list) {
  vinfo_t *src = list->array->items[iSPLIT_SRC];
  vinfo_t *first = list->array->items[iSPLIT_START];
  PyObject *sep =
      (PyObject *)CompileTime_Get(list->array->items[iSPLIT_SEP]->source)
          ->value;
  int ascii = sep != NULL && PyUnicode_GET_LENGTH(sep) > 1;
  vinfo_t *it, *zero;
  if (list->refcount != 1)
    return PsycoSeqIter_New(po, list);
  if (first != NULL)
    src = list->array->items[iSPLIT_ROOT];
  if (is_compiletime(src->source)) {
    PyObject *o = (PyObject *)CompileTime_Get(src->source)->value;
    if (!PyUnicode_IS_READY(o) || (ascii && !PyUnicode_IS_COMPACT_ASCII(o)))
      return PsycoSeqIter_New(po, list);
  } else {
    condition_code_t cc = state_at_least(
        po, src,
        ascii ? STATE_COMPACT_ASCII : NI_BIT_PyASCIIObject__state__ready);
    if (cc == CC_ERROR)
      return NULL;
    if (!runtime_condition_t(po, cc))
      return PsycoSeqIter_New(po, list);
  }
  it = PsycoSeqIter_New(po, list);
  zero = first != NULL ? first : psyco_vi_Zero();
  vinfo_setitem(po, it, iSEQITER_SPLIT_POS, make_runtime_copy(po, zero));
  if (first == NULL)
    vinfo_decref(zero, po);
  return it;
}

/* split.h's loops one item at a time: split_whitespace, split_char and
   split, without maxcount. The items of a view are views of it. */
DEFINEFN
vinfo_t *PsycoUnicode_SplitNext(PsycoObject *po, vinfo_t *it, vinfo_t *list) {
  vinfo_t *src = list->array->items[iSPLIT_SRC], *root = src, *n;
  vinfo_t *vsep = list->array->items[iSPLIT_SEP];
  PyObject *sep = (PyObject *)CompileTime_Get(vsep->source)->value;
  vinfo_t *pos = vinfo_getitem(it, iSEQITER_SPLIT_POS);
  vinfo_t *index, *start, *end = NULL, *next, *len, *r, *zero;
  condition_code_t cc;
  bool ok;

  if (list->array->items[iSPLIT_ROOT] != NULL) {
    root = list->array->items[iSPLIT_ROOT];
    n = list->array->items[iSPLIT_END];
  } else if ((n = psyco_get_const(po, src, UNICODE_length)) == NULL)
    return NULL;
  vinfo_incref(n);
  index = psyco_get_field(po, it, SEQITER_it_index);
  if (index == NULL)
    goto error_n;
  if (sep == NULL) {
    start = psyco_generic_call(po, cimpl_split_ws, VIEW_HELPER, "vvv", root,
                               pos, n);
    if (start == NULL)
      goto error;
    if ((end = view_bound_end(po)) == NULL)
      goto error_start;
    cc = integer_cmp_i(po, start, 0, Py_LT);
  } else {
    /* past the end once the last item, which may be empty, is given */
    start = pos;
    vinfo_incref(start);
    cc = integer_cmp(po, pos, n, Py_GT);
  }
  if (cc == CC_ERROR)
    goto error_end;
  if (runtime_condition_f(po, cc)) {
    /* exhausted, as piter_iternext ends */
    vinfo_xdecref(end, po);
    vinfo_decref(start, po);
    vinfo_decref(index, po);
    vinfo_decref(n, po);
    zero = psyco_vi_Zero();
    ok = psyco_put_field(po, it, SEQITER_it_seq, zero);
    vinfo_decref(zero, po);
    if (ok)
      PycException_SetVInfo(po, PyExc_StopIteration, psyco_vi_None());
    return NULL;
  }
  assert_nonneg(start);
  if (sep != NULL) {
    if (PyUnicode_GET_LENGTH(sep) == 1)
      end = psyco_generic_call(po, cimpl_split_char_end, VIEW_HELPER, "vvvl",
                               root, start, n,
                               (long)PyUnicode_READ_CHAR(sep, 0));
    else
      end = psyco_generic_call(po, cimpl_split_sep_end,
                               VIEW_HELPER | CfPyErrIfNeg, "vvvv", root, start,
                               n, vsep);
  }
  if (end == NULL)
    goto error_start;
  assert_nonneg(end);
  if (sep == NULL) {
    next = end;
    vinfo_incref(next);
  } else if ((next = integer_add_i(po, end, (long)PyUnicode_GET_LENGTH(sep),
                                   false)) == NULL)
    goto error_end;
  if (root != src) {
    vinfo_setitem(po, it, iSEQITER_SPLIT_POS, next);
    r = sub_view(po, root, list->array->items[iSPLIT_START], src, start,
                 end);
    vinfo_decref(start, po);
    if (r == NULL) {
      vinfo_decref(end, po);
      goto error;
    }
  } else {
    len = integer_sub(po, end, start, false);
    if (len == NULL) {
      vinfo_decref(next, po);
      goto error_end;
    }
    vinfo_setitem(po, it, iSEQITER_SPLIT_POS, next);
    r = new_view(po, src, start, len, NULL, NULL);
  }
  vinfo_decref(n, po);
  next = integer_add_i(po, index, 1, true);
  if (next == NULL || !psyco_put_field(po, it, SEQITER_it_index, next)) {
    vinfo_decref(r, po);
    r = NULL;
  }
  vinfo_xdecref(next, po);
  vinfo_decref(end, po);
  vinfo_decref(index, po);
  return r;

error_end:
  vinfo_xdecref(end, po);
error_start:
  vinfo_decref(start, po);
error:
  vinfo_decref(index, po);
error_n:
  vinfo_decref(n, po);
  return NULL;
}

/* ---- str % args with a constant format ---- */

/* PyUnicode_Format's loop with the format read once, when the code is
   compiled, by the copy of unicode_format_arg_parse: a plan of
   pieces, each a run of the format's characters, a '%%', or one argument
   with its parsed spec and the format's position and count after it (the
   copies read them: the error message's index, the writer's last
   allocation). At run time each argument is formatted and written by the
   copies of unicode_format_arg_format and
   unicode_format_arg_output into one _PyUnicodeWriter, set up as
   PyUnicode_Format sets it, and finished by _PyUnicodeWriter_Finish, so the
   result, its identity and kind, and every error are CPython's. Only its
   first allocation differs: the plan's text and 8 characters an argument,
   where CPython takes the format's length and 100 and shrinks the result
   after; when the writer may share an argument's str (no buffer yet, no
   over-allocation) is CPython's. A plan is
   made only when the format parses whole without the arguments (no '*', no
   '(key)'), so no parse error is left to run time, and is used only when
   the number of arguments is the plan's (a non-tuple argument is one: a
   mapping changes nothing then), else it is CPython's function, which
   raises its own errors. Plans live as long as Ni, one per format. */
enum { FMT_TEXT, FMT_PERCENT, FMT_ARG };

typedef struct {
  int kind;
  Py_ssize_t start, end; /* FMT_TEXT: the format's characters */
  int last;              /* FMT_TEXT: to the end of the format */
  ni_format_arg_t arg;   /* FMT_ARG */
  Py_ssize_t fmtpos, fmtcnt;
} fmt_piece_t;

typedef struct {
  PyObject *format;
  Py_ssize_t min_length;
  int nargs, npieces;
  fmt_piece_t pieces[1];
} fmt_plan_t;

#define FMT_ARGS_MAX 5 /* passed as words; more as the tuple */
static PyObject *format_plans; /* format -> capsule of its plan, or None */

static void free_plan(PyObject *capsule) {
  PyMem_Free(PyCapsule_GetPointer(capsule, NULL));
}

/* PyUnicode_Format's loop and unicode_format_arg's, without the args */
static fmt_plan_t *make_plan(PyObject *format) {
  ni_formatter_t ctx;
  ni_format_arg_t arg;
  fmt_plan_t *p;
  fmt_piece_t *q;
  int k;
  Py_ssize_t max = PyUnicode_GET_LENGTH(format) + 1;
  p = PyMem_Malloc(sizeof(fmt_plan_t) + max * sizeof(fmt_piece_t));
  if (p == NULL)
    return NULL;
  p->format = format;
  p->nargs = p->npieces = 0;
  ctx.fmtstr = format;
  ctx.fmtdata = PyUnicode_DATA(format);
  ctx.fmtkind = PyUnicode_KIND(format);
  ctx.fmtcnt = PyUnicode_GET_LENGTH(format);
  ctx.fmtpos = 0;
  ctx.args = PyTuple_New(0);
  ctx.arglen = ctx.argidx = 0;
  ctx.args_owned = 0;
  ctx.dict = NULL;
  if (ctx.args == NULL)
    goto fail;
  while (--ctx.fmtcnt >= 0) {
    q = &p->pieces[p->npieces++];
    if (PyUnicode_READ(ctx.fmtkind, ctx.fmtdata, ctx.fmtpos) != '%') {
      q->kind = FMT_TEXT;
      q->start = ctx.fmtpos++;
      while (ctx.fmtcnt >= 0 &&
             PyUnicode_READ(ctx.fmtkind, ctx.fmtdata, ctx.fmtpos) != '%') {
        ctx.fmtpos++;
        ctx.fmtcnt--;
      }
      q->last = ctx.fmtcnt < 0;
      if (q->last)
        ctx.fmtpos--;
      q->end = ctx.fmtpos;
      continue;
    }
    ctx.fmtpos++;
    arg.ch = PyUnicode_READ(ctx.fmtkind, ctx.fmtdata, ctx.fmtpos);
    if (arg.ch == '%') {
      ctx.fmtpos++;
      ctx.fmtcnt--;
      q->kind = FMT_PERCENT;
      continue;
    }
    arg.flags = 0;
    arg.width = -1;
    arg.prec = -1;
    arg.sign = 0;
    if (cimpl_unicode_format_arg_parse(&ctx, &arg) == -1 || ctx.argidx != 0 ||
        ctx.args_owned)
      goto fail;
    q->kind = FMT_ARG;
    q->arg = arg;
    q->fmtpos = ctx.fmtpos;
    q->fmtcnt = ctx.fmtcnt;
    p->nargs++;
  }
  p->min_length = 8 * (Py_ssize_t)p->nargs;
  for (k = 0; k < p->npieces; k++)
    p->min_length += p->pieces[k].kind == FMT_PERCENT ? 1 :
                     p->pieces[k].kind == FMT_TEXT ?
                     p->pieces[k].end - p->pieces[k].start : 0;
  Py_DECREF(ctx.args);
  return p;
fail:
  PyErr_Clear();
  Py_XDECREF(ctx.args);
  PyMem_Free(p);
  return NULL;
}

/* the plan of a constant format, or NULL; made once */
static fmt_plan_t *format_plan(PyObject *format) {
  PyObject *c;
  fmt_plan_t *p;
  if (format_plans == NULL && (format_plans = PyDict_New()) == NULL)
    goto fail;
  c = PyDict_GetItem(format_plans, format);
  if (c != NULL)
    return c == Py_None ? NULL : PyCapsule_GetPointer(c, NULL);
  p = make_plan(format);
  c = p != NULL ? PyCapsule_New(p, NULL, free_plan) : Py_None;
  if (c == NULL) {
    PyMem_Free(p);
    goto fail;
  }
  if (p == NULL)
    Py_INCREF(c);
  if (PyDict_SetItem(format_plans, format, c) < 0) {
    Py_DECREF(c);
    goto fail;
  }
  Py_DECREF(c);
  return p;
fail:
  PyErr_Clear();
  return NULL;
}

/* PyUnicode_Format(p->format, args) where args are the tuple 'tuple', or
   if it is NULL the words given (one for a non-tuple argument) */
static PyObject *cimpl_format_plan(fmt_plan_t *p, PyObject *tuple,
                                   PyObject *a0, PyObject *a1, PyObject *a2,
                                   PyObject *a3, PyObject *a4) {
  PyObject *words[FMT_ARGS_MAX] = {a0, a1, a2, a3, a4}, **items = words;
  ni_formatter_t ctx;
  ni_format_arg_t arg;
  PyObject *str;
  int i, k = 0, r;
  if (tuple != NULL) {
    if (PyTuple_GET_SIZE(tuple) != p->nargs)
      return PyUnicode_Format(p->format, tuple);
    items = &PyTuple_GET_ITEM(tuple, 0);
  }
  ctx.fmtstr = p->format;
  ctx.fmtdata = PyUnicode_DATA(p->format);
  ctx.fmtkind = PyUnicode_KIND(p->format);
  _PyUnicodeWriter_Init(&ctx.writer);
  ctx.writer.min_length = p->min_length;
  ctx.writer.overallocate = 1;
  ctx.args_owned = 0;
  ctx.dict = NULL;
  for (i = 0; i < p->npieces; i++) {
    fmt_piece_t *q = &p->pieces[i];
    if (q->kind == FMT_TEXT) {
      if (q->last)
        ctx.writer.overallocate = 0;
      if (_PyUnicodeWriter_WriteSubstring(&ctx.writer, p->format, q->start,
                                          q->end) < 0)
        goto error;
      continue;
    }
    if (q->kind == FMT_PERCENT) {
      if (cimpl__PyUnicodeWriter_WriteCharInline(&ctx.writer, '%') < 0)
        goto error;
      continue;
    }
    /* unicode_format_getnextarg gives ctx.args itself */
    ctx.args = items[k++];
    ctx.arglen = -1;
    ctx.argidx = -2;
    ctx.fmtpos = q->fmtpos;
    ctx.fmtcnt = q->fmtcnt;
    arg = q->arg;
    str = NULL;
    r = cimpl_unicode_format_arg_format(&ctx, &arg, &str);
    if (r == -1)
      goto error;
    if (r != 1) {
      r = cimpl_unicode_format_arg_output(&ctx, &arg, str);
      Py_DECREF(str);
      if (r == -1)
        goto error;
    }
  }
  return _PyUnicodeWriter_Finish(&ctx.writer);
error:
  _PyUnicodeWriter_Dealloc(&ctx.writer);
  return NULL;
}

static binaryfunc unicode_mod;

/* str's nb_remainder (unicode_mod, PyUnicode_Format) for a constant format
   whose plan the arguments fit: an exact tuple of a known length or one
   at run time (tested there), or one argument of any other type */
static vinfo_t *punicode_mod(PsycoObject *po, vinfo_t *v, vinfo_t *w) {
  vinfo_t *a[FMT_ARGS_MAX], *tuple = NULL, *zero;
  PyTypeObject *tw;
  PyObject *format;
  fmt_plan_t *p;
  int n = 1, i;
  if (!is_compiletime(v->source))
    goto generic;
  format = (PyObject *)CompileTime_Get(v->source)->value;
  if (!PyUnicode_CheckExact(format) || !PyUnicode_IS_READY(format) ||
      (p = format_plan(format)) == NULL)
    goto generic;
  tw = Psyco_NeedType(po, w);
  if (tw == NULL)
    return NULL;
  if (PyType_IsSubtype(tw, &PyTuple_Type)) {
    if (tw != &PyTuple_Type)
      goto generic;
    n = PsycoTuple_Load(w);
    if (n >= 0 && n != p->nargs)
      goto generic;
    if (n < 0 || n > FMT_ARGS_MAX)
      tuple = w;
    else
      for (i = 0; i < n; i++)
        a[i] = PsycoTuple_GET_ITEM(w, i);
  } else {
    if (p->nargs != 1)
      goto generic;
    a[0] = w;
  }
  zero = psyco_vi_Zero();
  for (i = tuple != NULL ? 0 : n; i < FMT_ARGS_MAX; i++)
    a[i] = zero;
  v = psyco_generic_call(po, cimpl_format_plan, CfCommonNewRefPyObject,
                         "lvvvvvv", (long)p, tuple != NULL ? tuple : zero,
                         a[0], a[1], a[2], a[3], a[4]);
  vinfo_decref(zero, po);
  if (v != NULL)
    Psyco_AssertType(po, v, &PyUnicode_Type);
  return v;
generic:
  return psyco_generic_call(po, unicode_mod, CfCommonNewRefPyObject, "vv", v,
                            w);
}

/* ---- sep.join(x) ---- */

static PyCFunction cfunc_unicode_join;

/* unicode_join is PyUnicode_Join(self, x), which for an exact list or
   tuple is _PyUnicode_JoinArray over its items, one buffer: called
   directly. A generator expression only the call holds is first run
   fused into a list (GENEXPR_LIST, as list() runs it: psyfunc.c), which
   is the list PySequence_Fast would make of the generator, so the items,
   their order and the errors are the same, without the generator. */
static vinfo_t *punicode_join(PsycoObject *po, vinfo_t *self, vinfo_t *x) {
  vinfo_t *list, *r;
  PyTypeObject *tp;
  int fuse = psyco_genexpr_fuse(po, x);
  if (fuse < 0)
    return NULL;
  if (fuse > 0) {
    vinfo_t *acc = psyco_generic_call(po, PyList_New, CfCommonNewRefPyObject,
                                      "l", 0L);
    if (acc == NULL)
      return NULL;
    list = psyco_call_genexpr(po, x, GENEXPR_LIST, acc);
    vinfo_decref(acc, po);
    if (list == NULL)
      return NULL;
    r = psyco_generic_call(po, PyUnicode_Join, CfCommonNewRefPyObject, "vv",
                           self, list);
    vinfo_decref(list, po);
    goto done;
  }
  tp = Psyco_KnownType(x);
  if (tp == &PyList_Type || tp == &PyTuple_Type)
    r = psyco_generic_call(po, PyUnicode_Join, CfCommonNewRefPyObject, "vv",
                           self, x);
  else
    r = psyco_generic_call(po, cfunc_unicode_join, CfCommonNewRefPyObject,
                           "vv", self, x);
done:
  if (r != NULL)
    Psyco_AssertType(po, r, &PyUnicode_Type);
  return r;
}

/* ---- f-strings: an int's part not yet made ---- */

/* FORMAT_VALUE of an exact int with no conversion and no spec is
   PyObject_Format(v, NULL), which is PyObject_Str(v): a decimal holds the
   int and is forced by PyObject_Str. BUILD_STRING writes it into its one
   result without making it (cimpl_build_parts). Made only for an int that
   can't reach int_max_str_digits' error (at most DEC_DIGITS_MAX digits of
   PyLong_SHIFT bits, below the threshold CPython tests first), so writing
   it later fails only where an allocation would. */
#define iDEC_INT FIELDS_TOTAL(UNICODE_state)
#define DEC_TOTAL (iDEC_INT + 1)
#define DEC_DIGITS_MAX 70

static source_virtual_t psyco_computed_decimal;

static bool is_decimal(vinfo_t *v) {
  return v->source == VirtualTime_New(&psyco_computed_decimal);
}

static bool compute_decimal(PsycoObject *po, vinfo_t *v) {
  vinfo_t *r = psyco_generic_call(po, PyObject_Str, CfCommonNewRefPyObject,
                                  "v", v->array->items[iDEC_INT]);
  if (r == NULL)
    return false;
  vinfo_array_shrink(po, v, iDEC_INT);
  vinfo_move(po, v, r);
  return true;
}

static PyObject *direct_compute_decimal(vinfo_t *v, char *data) {
  PyObject *n = direct_xobj_vinfo(vinfo_getitem(v, iDEC_INT), data), *r;
  if (n == NULL)
    return NULL;
  r = PyObject_Str(n);
  Py_DECREF(n);
  return r;
}

DEFINEFN
int PsycoUnicode_DecimalPart(PsycoObject *po, vinfo_t *n, vinfo_t **part) {
  vinfo_t *v, *size, *t;
  condition_code_t cc;
  if (is_compiletime(n->source)) {
    Py_ssize_t s = Py_SIZE((PyObject *)CompileTime_Get(n->source)->value);
    if (s < -DEC_DIGITS_MAX || s > DEC_DIGITS_MAX)
      return 0;
  } else if (!PsycoLong_IsVirtual(n)) {
    if ((size = psyco_get_const(po, n, LONG_ob_size)) == NULL)
      return -1;
    t = integer_add_i(po, size, DEC_DIGITS_MAX, false);
    if (t == NULL)
      return -1;
    cc = integer_cmp_i(po, t, 2 * DEC_DIGITS_MAX, Py_LE | COMPARE_UNSIGNED);
    vinfo_decref(t, po);
    if (cc == CC_ERROR)
      return -1;
    if (!runtime_condition_t(po, cc))
      return 0;
  }
  v = vinfo_new(VirtualTime_New(&psyco_computed_decimal));
  v->array = array_new(DEC_TOTAL);
  v->array->items[iOB_TYPE] =
      vinfo_new(CompileTime_New((long)&PyUnicode_Type));
  need_reference(po, n);
  vinfo_incref(n);
  v->array->items[iDEC_INT] = n;
  *part = v;
  return 1;
}

/* a part of cimpl_build_parts: 2 bits each above the count's 4 */
enum { PART_STR, PART_INT, PART_WORD };
#define PARTS_MAX 6

/* the decimal digits of x into 'buf' from its end; their count */
static int word_digits(char *end, long x) {
  unsigned long u = x < 0 ? 0UL - (unsigned long)x : (unsigned long)x;
  char *p = end;
  do
    *--p = (char)('0' + u % 10);
  while ((u /= 10) != 0);
  if (x < 0)
    *--p = '-';
  return (int)(end - p);
}

/* _PyUnicode_JoinArray('', parts) where a part is a str or the str() of
   an int not made: the length and widest kind summed first, one
   PyUnicode_New, then the copies, as JoinArray does (a sum of 0 is its
   shared empty str; one part is never given). An int of more than a word
   is _PyLong_Format's str first, as str() makes it. */
static PyObject *cimpl_build_parts(long tags, long a0, long a1, long a2,
                                   long a3, long a4, long a5) {
  long a[PARTS_MAX] = {a0, a1, a2, a3, a4, a5};
  PyObject *made[PARTS_MAX] = {NULL}, *res = NULL, *s;
  char digits[PARTS_MAX][24];
  int tag[PARTS_MAX], lens[PARTS_MAX], i, j, n = tags & 15, overflow;
  Py_ssize_t sz = 0, pos = 0;
  Py_UCS4 maxchar = 0;
  for (i = 0; i < n; i++) {
    tag[i] = (tags >> (4 + 2 * i)) & 3;
    if (tag[i] == PART_INT) {
      long x = PyLong_AsLongAndOverflow((PyObject *)a[i], &overflow);
      if (!overflow) {
        a[i] = x;
        tag[i] = PART_WORD;
      } else if ((made[i] = _PyLong_Format((PyObject *)a[i], 10)) == NULL)
        goto done;
      else {
        a[i] = (long)made[i];
        tag[i] = PART_STR;
      }
    }
    if (tag[i] == PART_WORD) {
      lens[i] = word_digits(digits[i] + sizeof(digits[i]), a[i]);
      sz += lens[i];
      maxchar = Py_MAX(maxchar, 127);
      continue;
    }
    s = (PyObject *)a[i];
    if (PyUnicode_READY(s) == -1)
      goto done;
    if (PyUnicode_GET_LENGTH(s) > PY_SSIZE_T_MAX - sz) {
      PyErr_SetString(PyExc_OverflowError,
                      "join() result is too long for a Python string");
      goto done;
    }
    sz += PyUnicode_GET_LENGTH(s);
    maxchar = Py_MAX(maxchar, PyUnicode_MAX_CHAR_VALUE(s));
  }
  res = PyUnicode_New(sz, maxchar);
  if (res == NULL || sz == 0)
    goto done;
  for (i = 0; i < n; i++) {
    if (tag[i] == PART_WORD) {
      const char *d = digits[i] + sizeof(digits[i]) - lens[i];
      int kind = PyUnicode_KIND(res);
      void *data = PyUnicode_DATA(res);
      for (j = 0; j < lens[i]; j++)
        PyUnicode_WRITE(kind, data, pos + j, d[j]);
      pos += lens[i];
    } else {
      s = (PyObject *)a[i];
      _PyUnicode_FastCopyCharacters(res, pos, s, 0, PyUnicode_GET_LENGTH(s));
      pos += PyUnicode_GET_LENGTH(s);
    }
  }
done:
  for (i = 0; i < n; i++)
    Py_XDECREF(made[i]);
  return res;
}

/* BUILD_STRING of the n items from 'items' when one is a decimal: 1 with
   the str in *result, 0 if not, -1 on an error */
DEFINEFN
int PsycoUnicode_BuildParts(PsycoObject *po, vinfo_t **items, int n,
                            vinfo_t **result) {
  vinfo_t *a[PARTS_MAX], *zero, *r;
  long tags = n;
  int i, any = 0;
  if (n < 2 || n > PARTS_MAX)
    return 0;
  for (i = 0; i < n; i++) {
    vinfo_t *x = items[i];
    int tag = PART_STR;
    if (is_decimal(x)) {
      x = x->array->items[iDEC_INT];
      tag = PART_INT;
      if (PsycoLong_IsVirtual(x)) {
        x = x->array->items[iLONG_VALUE];
        tag = PART_WORD;
      }
      any = 1;
    }
    a[i] = x;
    tags |= (long)tag << (4 + 2 * i);
  }
  if (!any)
    return 0;
  zero = psyco_vi_Zero();
  for (; i < PARTS_MAX; i++)
    a[i] = zero;
  r = psyco_generic_call(po, cimpl_build_parts,
                         CfCommonNewRefPyObject | CfNoPython | CfNoBlock,
                         "lvvvvvv", tags, a[0], a[1], a[2], a[3], a[4], a[5]);
  vinfo_decref(zero, po);
  if (r == NULL)
    return -1;
  Psyco_AssertType(po, r, &PyUnicode_Type);
  *result = r;
  return 1;
}

/* case_map_hash against _Py_HashBytes on every length to 40 and each
   case of byte, so another algorithm, a small-string cut-off or another
   byte order is never taken for CPython's hash; and the map itself
   against Py_TOLOWER and Py_TOUPPER, with nothing written past the
   length */
static void check_case_hash(void) {
  char src[41], a[42], b[42];
  int n, k, i;
  for (k = 0; k < 40; k++)
    src[k] = "aZ0 Mz.@[`{~"[k % 12] + (k / 12);
  for (n = 0; n <= 40; n++)
    for (k = 0; k < 2; k++) {
      Py_hash_t h;
      a[n] = b[n] = '#';
      h = case_map_hash(a, src, n, k);
      case_map(b, src, n, k);
      for (i = 0; i < n; i++)
        if (b[i] != (char)(k ? Py_TOUPPER(src[i]) : Py_TOLOWER(src[i])))
          Py_FatalError("ni: case_map is not ascii_upper_or_lower's map");
      if (a[n] != '#' || b[n] != '#')
        Py_FatalError("ni: case_map writes past the length");
      if (memcmp(a, b, n) != 0 || (n > 0 && h != _Py_HashBytes(b, n)))
        return;
    }
  case_hash_checked = 1;
}

INITIALIZATIONFN
void psy_unicodeobject_init(void) {
  int c;
  PyObject *a = PyUnicode_FromOrdinal('a');
  if (a == NULL ||
      (*(unsigned char *)&((PyASCIIObject *)a)->state & STATE_COMPACT_ASCII) !=
          STATE_COMPACT_ASCII ||
      PyUnicode_DATA(a) != (char *)a + sizeof(PyASCIIObject))
    Py_FatalError("ni: str layout is not what punicodeobject.c expects");
  Py_DECREF(a);
  for (c = 0; c < 256; c++)
    if ((latin1[c] = PyUnicode_FromOrdinal(c)) == NULL)
      Py_FatalError("ni: no one-character strings");
  unicode_subscript = PyUnicode_Type.tp_as_mapping->mp_subscript;
  Psyco_DefineMeta(PyUnicode_Type.tp_richcompare, punicode_richcompare);
  Psyco_DefineMeta(unicode_subscript, punicode_subscript);
  if ((unicode_empty = PyUnicode_New(0, 0)) == NULL)
    Py_FatalError("ni: no empty string");
  gen_unicodeobject_init();
  gen_unicodeobject_methods_init();
  /* the views' metas in front of the translated ones they fall back on */
  Psyco_DefineMeta(PyUnicode_Type.tp_as_sequence->sq_length, punicode_length);
  Psyco_DefineMeta(PyUnicode_Type.tp_as_sequence->sq_contains,
                   punicode_contains);
  if (cfunc_unicode_lstrip != NULL)
    Psyco_DefineMeta(cfunc_unicode_lstrip, punicode_lstrip);
  if (cfunc_unicode_rstrip != NULL)
    Psyco_DefineMeta(cfunc_unicode_rstrip, punicode_rstrip);
  if (cfunc_unicode_strip != NULL)
    Psyco_DefineMeta(cfunc_unicode_strip, punicode_strip);
  cfunc_unicode_split = Psyco_DefineMethodFn(
      &PyUnicode_Type, "split", METH_FASTCALL | METH_KEYWORDS, punicode_split);
  cfunc_unicode_lower = Psyco_DefineMethodFn(&PyUnicode_Type, "lower",
                                             METH_NOARGS, punicode_lower);
  cfunc_unicode_upper = Psyco_DefineMethodFn(&PyUnicode_Type, "upper",
                                             METH_NOARGS, punicode_upper);
  check_case_hash();
  unicode_mod = PyUnicode_Type.tp_as_number->nb_remainder;
  Psyco_DefineMeta(unicode_mod, punicode_mod);
  cfunc_unicode_join = Psyco_DefineMethodFn(&PyUnicode_Type, "join", METH_O,
                                            punicode_join);
  cfunc_unicode_partition = Psyco_DefineMethodFn(
      &PyUnicode_Type, "partition", METH_O, punicode_partition);
  cfunc_unicode_rpartition = Psyco_DefineMethodFn(
      &PyUnicode_Type, "rpartition", METH_O, punicode_rpartition);
  INIT_SVIRTUAL(psyco_computed_view, compute_view, direct_compute_view,
                (1 << iVIEW_ROOT) | (1 << iVIEW_PARENT) | (1 << iVIEW_ALIAS), 4,
                NW_FORCE);
  INIT_SVIRTUAL(psyco_computed_casemap, compute_casemap,
                direct_compute_casemap, 1 << iCASE_ROOT, 4, NW_FORCE);
  INIT_SVIRTUAL(psyco_computed_decimal, compute_decimal,
                direct_compute_decimal, 1 << iDEC_INT, 4, NW_FORCE);
  INIT_SVIRTUAL(psyco_computed_splitlist, compute_splitlist,
                direct_compute_splitlist,
                (1 << iSPLIT_SRC) | (1 << iSPLIT_ROOT), 1, NW_FORCE);
}
