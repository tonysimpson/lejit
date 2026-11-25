# Calls of cffi functions (Modules/pcffi.c), in API mode (a compiled module's
# lib) and ABI mode (ffi.dlopen): exact ints in range, floats and cdata
# pointers of the parameter's own type go to the C function directly;
# anything else takes cffi's own call, so results, range errors and every
# message are cffi's. errno is cffi's saved errno, and a callback from the
# C function runs Python.
from cffi_lib import abi, abi_ffi, api, api_ffi

BOTH = '[(api, api_ffi), (abi, abi_ffi)]'


def show(ffi, r):
    if isinstance(r, ffi.CData):
        t = ffi.typeof(r)
        return 'cdata', t.cname, r == ffi.NULL
    return type(r).__name__, r


def ints(lib, ffi):
    out = []
    for i in (0, 1, -1, 7, 2 ** 30 - 1, -2 ** 30):
        out.append((lib.add_int(i, 2), lib.id_ll(i * 3), lib.id_size(i & 0xff),
                    lib.from_i8(i & 0x7f), lib.from_u16(i & 0xffff),
                    lib.to_i8(i), lib.to_u16(i), lib.to_u8(i),
                    lib.id_u32(i & 0xffff), lib.next_color(i & 5),
                    lib.six(i, 1, 2, 3, 4, 5), lib.nothing()))
    out.append((lib.max_ull(0), lib.max_ull(5), lib.id_ull(5),
                lib.add_int(True, False), lib.not_(True), lib.not_(0)))
    return out


def unboxed(lib, ffi):
    total = 0
    t = 0.0
    for i in range(1000):
        total += lib.add_int(i, 2)
        t += lib.add_double(t, 0.5) * 0.0 + lib.add_double(i, 0.25)
        if lib.not_(i & 1):
            total += 1
        total += lib.id_ll(i * 1000003 * 1000003)
    return total, t


def _try(thunk):
    try:
        return thunk()
    except Exception as e:
        return type(e).__name__, str(e)


def ranges(lib, ffi):
    # the range check of each width, at one call site per function: the
    # run-time path out of range is cffi's call
    out = []
    for x in (2 ** 31 - 1, 2 ** 31, -2 ** 31, -2 ** 31 - 1, 2 ** 40, -1, 0,
              1, 2, 127, 128, 255, 256, -128, -129, 65535, 65536,
              2 ** 32 - 1, 2 ** 32, 2 ** 63 - 1, 2 ** 63, 2 ** 64 - 1, 2 ** 64,
              -2 ** 63, -2 ** 63 - 1, 2 ** 70, True, False):
        out.append((_try(lambda: lib.add_int(x, 0)),
                    _try(lambda: lib.from_i8(x)),
                    _try(lambda: lib.from_u16(x)),
                    _try(lambda: lib.id_u32(x)),
                    _try(lambda: lib.id_ll(x)),
                    _try(lambda: lib.id_ull(x)),
                    _try(lambda: lib.id_size(x)),
                    _try(lambda: lib.not_(x)),
                    _try(lambda: lib.next_color(x))))
    return out


def big_words(lib, ffi):
    # ints the compiled code holds as words, past one digit
    out = []
    for i in range(3):
        big = (i + 1) * 2 ** 40
        for v in (big, -big, big * 2 ** 21, -(big * 2 ** 21), big - big - 1):
            out.append((_try(lambda: lib.add_int(v, 1)),
                        _try(lambda: lib.id_ll(v)),
                        _try(lambda: lib.id_ull(v)),
                        _try(lambda: lib.id_u32(v)),
                        _try(lambda: lib.from_i8(v + 2 ** 40))))
    return out


def doubles(lib, ffi):
    out = []
    for x in (1.5, 2, -0.0, 1e308, 2 ** 80, True, float('inf'), 2 ** 2000):
        out.append(_try(lambda: lib.add_double(x, 0.25)))
    out.append(lib.mixed(1, 2.5, 3, 4.5, 5, 6.5))
    out.append(lib.scale(PT[ffi], 2.0, 3))
    out.append(lib.half(3.0))
    return out


def bad_arguments(lib, ffi):
    out = []
    for x in (1.5, 'x', None, 2 ** 2000, [1]):
        out.append(_try(lambda: lib.add_int(x, 2)))
        out.append(_try(lambda: lib.add_int(2, x)))
    for x in ('a', None, [1.0]):
        out.append(_try(lambda: lib.add_double(x, 1.0)))
    for x in (None, 2, 1.0):
        out.append(_try(lambda: lib.not_(x)))
    out.append(_try(lambda: lib.add_int(1)))
    out.append(_try(lambda: lib.add_int(1, 2, 3)))
    out.append(_try(lambda: lib.add_int(a=1, b=2)))
    out.append(_try(lambda: lib.nothing(1)))
    out.append(_try(lambda: lib.add_double(2 ** 2000, 1.0)))
    return out


def _objects(ffi):
    return {'NULL': ffi.NULL, 'point': PT[ffi], 'intp': ffi.new('int *', 7),
            'pointarr': ARR[ffi], 'voidp': ffi.cast('void *', PT[ffi]),
            'bytes': b'ab', 'None': None, 'int': 5}


def pointers(lib, ffi):
    objs = _objects(ffi)
    out = []
    for name in ('point', 'NULL', 'intp', 'pointarr', 'voidp', 'bytes',
                 'None', 'int'):
        o = objs[name]
        # point_x would read through NULL
        rs = [_try(lambda: lib.id_point(o)), _try(lambda: lib.id_ptr(o))]
        if name != 'NULL':
            rs.append(_try(lambda: lib.point_x(o)))
        for r in rs:
            out.append((name, show(ffi, r),
                        r == PT[ffi] if isinstance(r, ffi.CData) else None))
    return out


def void_results(lib, ffi):
    out = []
    for i in range(3):
        out.append((lib.nothing_void(), lib.nothing()))
    return out


def stack_arguments(lib, ffi):
    # seven ints: the seventh goes on the stack, so cffi's call
    return [lib.seven(i, 1, 2, 3, 4, 5, 6) for i in range(3)]


def errno_kept(lib, ffi):
    out = []
    for e in (42, 0, 7):
        lib.set_errno(e)
        x = [1] * 3  # code that may change errno
        out.append((ffi.errno, len(x)))
        ffi.errno = e + 1
        out.append(lib.get_errno())
    return out


SEEN = []


def callbacks(lib, ffi):
    out = []

    @ffi.callback('int(int)')
    def times_ten(x):
        SEEN.append(x)
        return x * 10

    def caught(exc_type, value, tb):
        SEEN.append((exc_type.__name__, str(value)))

    @ffi.callback('int(int)', error=-5, onerror=caught)
    def raises(x):
        raise ValueError(x)

    cb = ffi.cast('void *', times_ten)
    del SEEN[:]
    for i in range(4):
        n = len(SEEN)
        out.append((lib.call_cb(cb, i), len(SEEN) - n))
    out.append(lib.call_cb(ffi.cast('void *', raises), 3))
    out.append(SEEN[-1])
    return out


PT = {}
ARR = {}
for _ffi in (api_ffi, abi_ffi):
    if _ffi is not None:
        PT[_ffi] = _ffi.new('struct point *', (1.25, 2.5))
        ARR[_ffi] = _ffi.new('struct point[2]', [(3.0, 4.0)])

CASES = [] if api is None else [
    ('ints', BOTH, {}),
    ('unboxed', BOTH, {}),
    ('ranges', BOTH, {}),
    ('big_words', BOTH, {}),
    ('doubles', BOTH, {}),
    ('bad_arguments', BOTH, {}),
    ('pointers', BOTH, {}),
    ('void_results', BOTH, {}),
    ('stack_arguments', BOTH, {}),
    ('errno_kept', BOTH, {}),
    ('callbacks', BOTH, {}),
]
