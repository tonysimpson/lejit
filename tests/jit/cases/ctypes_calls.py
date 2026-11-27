# Calls of ctypes functions (Modules/pctypes.c), of a library or through a
# prototype: with argtypes set or not, exact ints and floats, bytes, None, pointer and simple ctypes instances
# and byref() go to the C function directly, as do c_char_p, c_void_p,
# c_float and c_bool arguments and results and pointer results; anything
# else, and a function whose argtypes, restype or errcheck changed, takes
# ctypes' own call, so results, truncation and every error are ctypes'.
import ctypes
import sys
from ctypes import (POINTER, byref, c_bool, c_byte, c_char_p, c_double,
                    c_float, c_int, c_long, c_short, c_size_t, c_ubyte, c_uint,
                    c_ulong, c_ushort, c_void_p)

libc = ctypes.CDLL(None)


def fn(name, argtypes=None, restype=c_int):
    f = getattr(ctypes.CDLL(None), name)
    if argtypes is not None:
        f.argtypes = argtypes
    f.restype = restype
    return f


labs = fn('labs', (c_long,), c_long)
labs_int = fn('labs', (c_long,), c_int)
labs_ushort = fn('labs', (c_long,), c_ushort)
labs_uint = fn('labs', (c_ulong,), c_uint)
labs_ulong = fn('labs', (c_long,), c_ulong)
abs_ubyte = fn('abs', (c_ubyte,))
abs_byte = fn('abs', (c_byte,))
abs_short = fn('abs', (c_short,), c_short)
abs_untyped = libc.abs
fabs = fn('fabs', (c_double,), c_double)
ldexp = fn('ldexp', (c_double, c_int), c_double)
fma = fn('fma', (c_double, c_double, c_double), c_double)
IntP = POINTER(c_int)
memcpy = fn('memcpy', (IntP, IntP, c_size_t), None)
memset = fn('memset', (IntP, c_int, c_size_t), None)
free = fn('free', (IntP,), None)
free_untyped = fn('free', None, None)
memset_untyped = fn('memset', None, None)


def error(f, *args, **kw):
    try:
        return f(*args, **kw)
    except Exception as e:
        return type(e).__name__, str(e)


def typed_ints(n):
    out = []
    for i in range(n):
        out.append((labs(-i), labs(i - 3), labs(True), labs(-2 ** 40 - i),
                    labs_int(2 ** 33 + i), labs_ushort(-70000 - i),
                    labs_uint(2 ** 64 - 5 - i), labs_ulong(-i),
                    abs_ubyte(300 + i), abs_ubyte(-1), abs_byte(200 + i),
                    abs_short(-40000 + i), labs(2 ** 70 + i),
                    abs_ubyte(2 ** 65 + 7)))
    return out


def untyped_ints(n):
    out = []
    for i in range(n):
        out.append((abs_untyped(-i), abs_untyped(2 ** 32 - 5 + i),
                    abs_untyped(2 ** 31 + i), abs_untyped(False),
                    abs_untyped(-2 ** 62), abs_untyped(2 ** 63 + i),
                    abs_untyped(c_int(-i)), abs_untyped(c_short(-7)),
                    abs_untyped(c_long(-2 ** 33 - 3))))
    return out


def doubles(n):
    out = []
    for i in range(n):
        x = i * 0.5 - 3.0
        out.append((fabs(x), fabs(-1e308 * 10), fabs(c_double(-x)),
                    ldexp(1.5, i), ldexp(x, -i), fma(x, 2.0, 0.25)))
    return out


def pointers(n):
    out = []
    a = (c_int * 4)(1, 2, 3, 4)
    pa = ctypes.cast(a, IntP)
    for i in range(n):
        b = c_int(i)
        c = c_int(-1)
        memcpy(byref(c), byref(b), 4)
        d = c_int(7)
        pd = ctypes.pointer(d)
        memcpy(pd, byref(b), 4)
        memset(pa, i & 0xFF, 4)
        free(None)
        free_untyped(None)
        memset_untyped(pa, 1, 1)
        memset_untyped(byref(b), 0, 2)
        out.append((c.value, d.value, a[0], a[1], b.value))
    return out


class Other(ctypes.Structure):
    _fields_ = [('x', c_int)]


def errors(n):
    out = []
    for i in range(n):
        out.append((error(labs, 1.5), error(labs, None), error(labs, 'x'),
                    error(labs), error(labs, 1, 2), error(abs_untyped, 1.5),
                    error(abs_untyped, 2 ** 64 + i), error(abs_untyped, -2 ** 64),
                    error(abs_untyped, object()), error(fabs, 1),
                    error(fabs, 'x'), error(memset, c_int(1), 0, 0),
                    error(memset, byref(Other()), 0, 0),
                    error(memset, byref(c_short()), 0, 0),
                    error(memcpy, ctypes.pointer(c_long()), None, 0),
                    error(labs, -i, x=1)))
    return out


class MyInt(c_int):
    @classmethod
    def from_param(cls, value):
        return c_int(value * 2)


twice = fn('abs', (MyInt,))
errno_abs = getattr(ctypes.CDLL(None, use_errno=True), 'abs')
python_abs = getattr(ctypes.PyDLL(None), 'abs')


def other_converters(n):
    out = []
    for i in range(n):
        out.append((twice(-i), errno_abs(-i), python_abs(-i),
                    ctypes.get_errno()))
    return out


changing = fn('labs', (c_long,), c_long)


def changes(n):
    out = []
    f = changing
    f.argtypes = (c_long,)
    f.restype = c_long
    for i in range(n):
        out.append(f(-i - 2 ** 40))
        if i == 2:
            f.restype = c_int
        elif i == 4:
            f.errcheck = lambda result, func, args: (result, args)
        elif i == 6:
            del f.errcheck
            f.argtypes = (c_ubyte,)
        elif i == 8:
            f.argtypes = None
            f.restype = c_short
    return out


# what a call was compiled for is what a replay of its code compiles for,
# though the function's argtypes and restype changed since: the replay
# for a branch first taken after the change goes the way the first
# compile went
retyped_later = fn('labs', (c_long,), c_long)
retyped_argument = fn('labs', (c_long,), c_long)


def retyped_in_loop(n):
    out = []
    f = retyped_later
    for i in range(n):
        out.append(f(-i))
        if i == 3:
            f.argtypes = (c_double,)
            f.restype = c_double
        elif i == 5:
            out.append('five')
        elif i == 7:
            out.append('seven')
    return out


def retyped_as_argument(n, g):
    out = []
    for i in range(n):
        a = g(-i - 0.5) if i > 3 else g(-i)
        b = i * 2 if i != 6 else -1
        out.append((a, b))
        if i == 2:
            g.argtypes = (c_double,)
            g.restype = c_double
    return out


def call_through_list(n):
    # a site that calls several functions
    fs = [labs, abs_untyped, labs_int, abs_short, labs_ushort, abs_ubyte,
          abs_byte, labs_ulong]
    return [fs[i % len(fs)](-i - 300) for i in range(n)]


seen = []


def hooked(n):
    # a ctypes call is audited where ctypes audits it
    if not seen and hasattr(sys, 'addaudithook'):
        seen.append(None)
        sys.addaudithook(lambda event, args: event == 'ctypes.call_function'
                         and seen.append(args[1]))
    out = []
    for i in range(n):
        out.append((labs(-i), abs_untyped(-i), fabs(-0.5 * i)))
    return out, seen[1:20]


strlen = fn('strlen', (c_char_p,), c_size_t)
strlen_voidp = fn('strlen', (c_void_p,), c_size_t)
strlen_untyped = fn('strlen', None, c_size_t)
strchr = fn('strchr', (c_char_p, c_int), c_char_p)
getenv = fn('getenv', (c_char_p,), c_char_p)
strtoul = fn('strtoul', (c_char_p, c_void_p, c_int), c_void_p)
labs_voidp = fn('labs', (c_void_p,), c_void_p)
labs_bool = fn('labs', (c_long,), c_bool)
abs_bool = fn('abs', (c_bool,))
fabsf = fn('fabsf', (c_float,), c_float)
fabsf_untyped = fn('fabsf', None, c_float)
ldexpf = fn('ldexpf', (c_float, c_int), c_float)
as_int_pointer = fn('labs', (c_void_p,), IntP)


def strings(n):
    out = []
    s = c_char_p(b'four')
    for i in range(n):
        b = b'x' * i
        out.append((strlen(b), strlen(b'abc'), strlen(s), strlen_voidp(b),
                    strlen_untyped(b), strlen_untyped(s), strchr(b'hello', 108),
                    strchr(b'hello', 122 - i), getenv(b'LEJIT_NO_SUCH_NAME'),
                    error(strlen, 5), error(strlen, 'str'), error(strlen, b)))
    return out


def void_pointers(n):
    out = []
    v = c_void_p(9)
    for i in range(n):
        out.append((labs_voidp(-i), labs_voidp(None), labs_voidp(v),
                    labs_voidp(True), labs_voidp(c_void_p()),
                    strtoul(b'18446744073709551615', None, 10),
                    strtoul(b'12', None, 10 - i % 2),
                    error(labs_voidp, 1.5), error(labs_voidp, 2 ** 70)))
    return out


def floats(n):
    out = []
    for i in range(n):
        x = i * 0.1 - 0.35
        out.append((fabsf(x), fabsf(-1e40), fabsf(c_float(-2.25)),
                    fabsf_untyped(c_float(x)), ldexpf(x, i), ldexpf(1.5, -i),
                    error(fabsf, 1), error(fabsf, 'x'),
                    error(fabsf_untyped, x)))
    return out


def bools(n):
    out = []
    for i in range(n):
        out.append((abs_bool(i), abs_bool(True), abs_bool(False),
                    abs_bool(2 ** 70), abs_bool(c_bool(i > 2)),
                    labs_bool(i), labs_bool(256), labs_bool(257),
                    abs_bool(None), abs_bool(0.0)))
    return out


def pointer_results(n):
    out = []
    x = c_int(41)
    vx = c_void_p(ctypes.addressof(x))
    for i in range(n):
        x.value = 41 + i
        p = as_int_pointer(vx)
        q = as_int_pointer(None)
        out.append((type(p).__name__, p.contents.value, p[0], bool(q),
                    p._b_needsfree_, p._objects, q._objects,
                    ctypes.addressof(p.contents) == vx.value))
kept = []


def byref_objects(n):
    # byref() passed straight to a C function makes no object; one that
    # escapes, is kept, or isn't a ctypes instance is byref's own
    out = []
    for i in range(n):
        b = c_int(i)
        c = c_int(-1)
        memcpy(byref(c), byref(b), 4)
        r = byref(c)
        memset(r, 0, 4)
        kept.append(byref(b))
        s = c_short(5)
        out.append((c.value, type(r).__name__, r._obj is c,
                    type(kept[-1]).__name__, kept[-1]._obj is b,
                    error(memset, byref(s), 0, 0), error(byref, 5),
                    error(byref, b, 'x'), memset_untyped(byref(s, 1), 0, 1),
                    s.value, error(labs, byref(b))))
    del kept[:]
    return out


def big_ints(n):
    # ints of more than one digit, masked as ctypes masks them, or out of
    # range for ctypes' own call to raise; each at its own call site
    out = []
    for i in range(n):
        k = 2 ** (30 + 9 * i)
        for x in (k + 5, -k - 3, 2 ** 64 - 1 - i, -2 ** 63 + i, 2 ** 64 + i,
                  -2 ** 63 - 1 - i, k * k + 255, 1 << 1000 | i):
            out.append((labs(x), labs_int(x), labs_ushort(x), labs_uint(x),
                        labs_ulong(x), abs_ubyte(x), abs_byte(x), abs_short(x),
                        error(lambda: abs_untyped(x)),
                        error(lambda: labs_voidp(x)), abs_bool(x)))
    return out


def big_computed_ints(n):
    # ints the compiled code computed, held as words
    out = []
    t = 3
    for i in range(n):
        t = t * 100003 + i
        out.append((labs(t), labs(-t), labs_int(t), abs_ubyte(t),
                    error(abs_untyped, t), error(labs_voidp, t), abs_bool(t - t),
                    abs_bool(t)))
    return out


LabsType = ctypes.CFUNCTYPE(c_long, c_long)
UntypedType = ctypes.CFUNCTYPE(c_int)
VoidpType = ctypes.CFUNCTYPE(c_void_p, c_void_p)
_labs_address = ctypes.cast(libc.labs, c_void_p).value
labs_proto = LabsType(_labs_address)
labs_voidp_proto = VoidpType(_labs_address)
memset_proto = ctypes.CFUNCTYPE(None, IntP, c_int, c_size_t)(
    ctypes.cast(libc.memset, c_void_p).value)
retyped_proto = LabsType(_labs_address)
retyped_proto.argtypes = (c_ubyte,)
retyped_proto.restype = c_short


def prototypes(n):
    out = []
    x = c_int(9)
    for i in range(n):
        memset_proto(byref(x), i, 1)
        out.append((labs_proto(-i), labs_proto(2 ** 40 + i),
                    labs_voidp_proto(-i), labs_voidp_proto(None),
                    retyped_proto(-i - 300), x.value,
                    error(labs_proto, 1.5), error(labs_proto),
                    error(labs_proto, 1, 2), error(memset_proto, 1, 0, 0),
                    UntypedType(_labs_address)(i - 5)))
    return out


def first_call_keeps_no_reference(counted):
    # the first ctypes call compiled checks ctypes' layouts with objects of
    # its own (pctypes.c:ctypes_probe), which mustn't outlive it. A
    # function made here is first known as the call runs; the class
    # counted is an argument, so compiled code holds none of its own.
    f = ctypes.CFUNCTYPE(c_int, c_int, c_int)(lambda a, b: a * b)
    before = sys.getrefcount(counted)
    f(1, 2)
    return sys.getrefcount(counted) - before


CASES = [
    ('typed_ints', [(12,)], {}),
    ('untyped_ints', [(12,)], {}),
    ('doubles', [(12,)], {}),
    ('pointers', [(12,)], {}),
    ('errors', [(4,)], {}),
    ('other_converters', [(6,)], {}),
    ('changes', [(12,)], {}),
    ('retyped_in_loop', [(12,)], {}),
    ('retyped_as_argument', '[(12, retyped_argument)]', {}),
    ('call_through_list', [(40,)], {}),
    ('hooked', [(5,)], {}),
    ('strings', [(6,)], {}),
    ('void_pointers', [(6,)], {}),
    ('floats', [(8,)], {}),
    ('bools', [(6,)], {}),
    ('pointer_results', [(6,)], {}),
    ('big_ints', [(4,)], {}),
    ('big_computed_ints', [(8,)], {}),
    ('prototypes', [(6,)], {}),
    ('byref_objects', [(6,)], {}),
    ('first_call_keeps_no_reference', '[(c_int,), (c_double,)]', {}),
]
