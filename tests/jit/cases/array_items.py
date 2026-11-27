# array.array items (Modules/parray.c): a[i] and a[i] = v of an exact
# array with an int or float typecode and an exact int index are read and
# written in compiled code; anything else, and an index or value out of
# range, takes array's own subscript, so results and every error are
# array's.
from array import array

INT_CODES = tuple('bBhHiIlLqQ')
CODES = INT_CODES + ('f', 'd')


def error(f, *args):
    try:
        return f(*args)
    except Exception as e:
        return type(e).__name__, str(e)


def get(a, i):
    return a[i]


def put(a, i, v):
    a[i] = v
    return a[i]


def every_code(n):
    out = []
    for code in CODES:
        a = array(code, range(n)) if code in INT_CODES else \
            array(code, [i * 0.25 for i in range(n)])
        s = 0
        for i in range(n):
            a[i] = a[i] + a[n - 1 - i]
            s += a[i]
        out.append((code, s, a.tolist(), type(a[0]).__name__))
    return out


def floats(n):
    a = array('d', [0.5]) * n
    f = array('f', [0.0]) * n
    for i in range(n):
        x = a[i] * 3.0 + i
        a[i] = x
        f[i] = x / 7.0
        f[i] = f[i] + 0.1
    return a.tolist(), f.tolist()


def negative_index(n):
    a = array('i', range(n))
    out = []
    for i in range(1, n + 1):
        a[-i] = a[-i] * 10 + i
        out.append(a[-i])
    return out, a.tolist()


def bounds(n):
    a = array('d', [1.0] * n)
    b = array('b', range(n))
    out = []
    for i in (n, -n - 1, n + 5, 2 ** 70, -2 ** 70, -1, 0):
        out.append((error(get, a, i), error(get, b, i),
                    error(put, a, i, 2.0), error(put, b, i, 3)))
    return out, error(get, array('i'), 0), error(put, array('i'), 0, 1)


def stores_in_range(n):
    out = []
    for code, values in (('b', (-128, 127, 0, -1)), ('B', (0, 255, 7)),
                         ('h', (-32768, 32767)), ('H', (0, 65535)),
                         ('i', (-2 ** 31, 2 ** 31 - 1, -2 ** 29)),
                         ('I', (0, 2 ** 32 - 1, 2 ** 30, 2 ** 29)),
                         ('l', (-2 ** 63, 2 ** 63 - 1)),
                         ('L', (0, 2 ** 64 - 1, 2 ** 63, 2 ** 62)),
                         ('q', (-2 ** 63, 2 ** 63 - 1, -5)),
                         ('Q', (0, 2 ** 64 - 1, 2 ** 63))):
        a = array(code, [0] * n)
        for i in range(n):
            a[i] = values[i % len(values)]
        out.append((code, a.tolist(), [a[i] for i in range(n)]))
    return out


def overflow(n):
    out = []
    for code, values in (('b', (128, -129, 2 ** 40)), ('B', (-1, 256)),
                         ('h', (2 ** 15, -2 ** 15 - 1)),
                         ('H', (-1, 2 ** 16)),
                         ('i', (2 ** 31, -2 ** 31 - 1)),
                         ('I', (-1, 2 ** 32, -2 ** 40)),
                         ('l', (2 ** 63, -2 ** 63 - 1)),
                         ('L', (-1, 2 ** 64)),
                         ('q', (2 ** 63,)), ('Q', (-1, 2 ** 64))):
        a = array(code, [0] * n)
        for i in range(n):
            out.append((code, error(put, a, i % n, values[i % len(values)])))
        out.append(a.tolist())
    return out


def computed_ints(n):
    # virtual ints, made by arithmetic, beyond one digit
    a = array('q', [0] * n)
    b = array('b', [0] * n)
    out = []
    for i in range(n):
        a[i] = (i + 1) * 2 ** 40 - i
        out.append(error(put, b, i, i * 60 - 100))
    return a.tolist(), b.tolist(), out


def float_conversions(n):
    # 'f' stores round to a float; a finite value too large for one is
    # stored as infinity or refused, as the version's array does
    f = array('f', [0.0] * n)
    d = array('d', [0.0] * n)
    out = []
    for i in range(n):
        f[i] = 0.1 * i
        out.append(error(put, f, i, 1e300 if i == 1 else f[i]))
        out.append(error(put, f, i, (float('nan'), float('inf'), -1e39,
                                     -float('inf'), 3.4e38, 1e-46)[i % 6]))
        d[i] = 1.0 / 3.0 + i
        out.append((f[i], d[i]))
    return out, f.tolist(), d.tolist()


def other_values(n):
    # values array's own setitem converts: an int into a float array, a
    # float into an int one, a bool, other types
    class Index:
        def __index__(self):
            return 3

    class Float:
        def __float__(self):
            return 2.5
    d = array('d', [0.0] * n)
    i = array('i', [0] * n)
    out = []
    for k in range(n):
        out.append((error(put, d, k, k), error(put, d, k, True),
                    error(put, d, k, 'x'), error(put, d, k, Float()),
                    error(put, i, k, 1.5), error(put, i, k, True),
                    error(put, i, k, Index()), error(put, i, k, None),
                    error(put, d, k, 2 ** 2000)))
    return out


def other_indexes(n):
    class Index:
        def __index__(self):
            return 1

    a = array('d', [float(k) for k in range(n)])
    out = []
    for k in range(n):
        out.append((error(get, a, True), error(get, a, Index()),
                    error(get, a, 1.0), error(get, a, 'x'),
                    error(get, a, slice(1, 3)), a[k:k + 2].tolist(),
                    a[::-1][k], error(put, a, Index(), 7.5),
                    error(put, a, slice(0, 1), array('d', [9.0]))))
    return out, a.tolist()


def other_codes(n):
    codes = ['u'] + (['w'] if 'w' in __import__('array').typecodes else [])
    out = []
    for code in codes:
        a = array(code, 'abcdef'[:n])
        for k in range(n):
            a[k] = 'z' if k % 2 else a[n - 1 - k]
        out.append((code, a.tolist(), error(put, a, 0, 5)))
    return out


def deletion(n):
    a = array('i', range(n))
    out = []
    while len(a) > 1:
        del a[0]
        del a[-1:]
        out.append(a.tolist())
    return out


def mixed_codes(n):
    # one site meeting arrays of every typecode
    arrays = [array(c, [1, 2, 3]) if c in INT_CODES else
              array(c, [0.5, 1.5, 2.5]) for c in CODES]
    out = []
    for k in range(n):
        for a in arrays:
            a[k % 3] = a[(k + 1) % 3]
            out.append(a[k % 3])
    return out


def bump(a):
    a.append(a[-1] + 1)


def resized(n):
    # the array grows and shrinks under the loop: its items and size are
    # read again after each call, and items move when it grows
    a = array('d', [1.0])
    out = []
    for i in range(n):
        bump(a)
        a.extend(a)
        out.append((len(a), a[i], a[-1]))
        a[i] = a[i] + 0.5
        while len(a) > i + 2:
            a.pop()
        out.append(error(get, a, i + 2))
    return out, a.tolist()


def changed_between(n):
    a = array('l', range(n))
    b = a
    out = []
    for i in range(n):
        x = a[i]
        b[i] = x * 3
        out.append((x, a[i]))
        if i == n // 2:
            a = array('l', [-1] * n)
    return out


class Sub(array):
    pass


class Doubled(array):
    def __getitem__(self, i):
        return 2 * array.__getitem__(self, i)

    def __setitem__(self, i, v):
        array.__setitem__(self, i, v + 1)


def subclasses(n):
    s = Sub('d', [1.0] * n)
    d = Doubled('i', range(n))
    out = []
    for i in range(n):
        s[i] = s[i] + i
        d[i] = d[i]
        out.append((s[i], d[i], error(get, s, n), error(put, d, n, 1)))
    return out, s.tolist(), d.tolist()


def buffer_held(n):
    # a store while a memoryview holds the buffer is allowed; a resize isn't
    a = array('i', range(n))
    m = memoryview(a)
    for i in range(n):
        a[i] = a[i] + 1
    out = (m.tolist(), error(a.append, 1))
    m.release()
    return out


CASES = [
    ('every_code', [(6,)], {}),
    ('floats', [(8,)], {}),
    ('negative_index', [(6,)], {}),
    ('bounds', [(4,)], {}),
    ('stores_in_range', [(8,)], {}),
    ('overflow', [(4,)], {}),
    ('computed_ints', [(5,)], {}),
    ('float_conversions', [(6,)], {}),
    ('other_values', [(3,)], {}),
    ('other_indexes', [(3,)], {}),
    ('other_codes', [(4,)], {}),
    ('deletion', [(6,)], {}),
    ('mixed_codes', [(5,)], {}),
    ('resized', [(5,)], {}),
    ('changed_between', [(6,)], {}),
    ('subclasses', [(4,)], {}),
    ('buffer_held', [(4,)], {}),
]
