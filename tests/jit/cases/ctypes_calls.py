# Calls of ctypes functions (Modules/pctypes.c): with argtypes set or not,
# exact ints and floats, None, pointer and simple ctypes instances and
# byref() go to the C function directly; anything else, and a function
# whose argtypes, restype or errcheck changed, takes ctypes' own call, so
# results, truncation and every error are ctypes'.
import ctypes
import sys
from ctypes import (POINTER, byref, c_byte, c_double, c_int, c_long,
                    c_short, c_size_t, c_ubyte, c_uint, c_ulong, c_ushort)

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


CASES = [
    ('typed_ints', [(12,)], {}),
    ('untyped_ints', [(12,)], {}),
    ('doubles', [(12,)], {}),
    ('pointers', [(12,)], {}),
    ('errors', [(4,)], {}),
    ('other_converters', [(6,)], {}),
    ('changes', [(12,)], {}),
    ('call_through_list', [(40,)], {}),
    ('hooked', [(5,)], {}),
]
