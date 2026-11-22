# x[i:j] through the virtual slice (Objects/psliceobject.c): str, bytes and
# list call a C helper with the bounds as words; anything else (a step,
# __index__, floats, other types) gets the real slice object. Results,
# types and identities must match CPython: an empty slice is the shared
# empty str or bytes, a full slice of an exact str or bytes is the object
# itself, of a subclass a copy of type str or bytes.
import ctypes

WORD = 'abcdef'
BWORD = b'abcdef'
KINDS = ('hello', 'héllo', 'h€llo', 'h😀llo', '', 'x')
BKINDS = (b'hello', b'', b'x', bytes(range(250, 256)))
BIG = 2 ** 70


class S(str):
    pass


class B(bytes):
    pass


class L(list):
    pass


class LGet(list):
    def __getitem__(self, k):
        return ('LGet', k)


class Ix:
    def __init__(self, n):
        self.n = n

    def __index__(self):
        return self.n


class Key:
    def __getitem__(self, k):
        return k


_api = ctypes.pythonapi
_api.PyUnicode_FromUnicode.restype = ctypes.py_object
_api.PyUnicode_FromUnicode.argtypes = (ctypes.c_void_p, ctypes.c_ssize_t)
_api.PyUnicode_AsUnicode.restype = ctypes.c_void_p
_api.PyUnicode_AsUnicode.argtypes = (ctypes.py_object,)


def unready(text):
    """a str built by the legacy API: not ready, its length field is 0"""
    s = _api.PyUnicode_FromUnicode(None, len(text))
    buf = ctypes.create_unicode_buffer(text)
    ctypes.memmove(_api.PyUnicode_AsUnicode(s), buf,
                   len(text) * ctypes.sizeof(ctypes.c_wchar))
    return s


def describe(x, r):
    return (r, type(r).__name__, r is x, len(r) == 0 and r is x[0:0])


def all_bounds(xs):
    out = []
    for x in xs:
        n = len(x)
        for i in range(-n - 2, n + 3):
            for j in range(-n - 2, n + 3):
                out.append(describe(x, x[i:j]))
            out.append(describe(x, x[i:]))
            out.append(describe(x, x[:i]))
        out.append(describe(x, x[:]))
    return out


def empties(xs):
    # every empty str slice is '' itself, every empty bytes slice b''
    out = []
    for x in xs:
        e = x[len(x):]
        out.append((e, e is x[0:0], e is ('' if isinstance(x, str) else b'')))
    return out


def full(xs):
    return [(x[:] is x, x[0:len(x)] is x, x[-len(x) - 5:99] is x)
            for x in xs]


def big_bounds(x, big):
    return (x[-big:big], x[big:], x[:-big], x[2:BIG], x[-BIG:3],
            x[True:], x[:False], x[True:-True])


def none_bounds(x, a, b):
    return x[a:b], x[a:], x[:b]


def odd_bounds(x):
    out = []
    for a, b in ((Ix(1), Ix(3)), (1, Ix(-1)), (1.0, 2), (1, 'a'),
                 (None, 2.5), ([], None)):
        try:
            out.append(x[a:b])
        except TypeError as e:
            out.append(('TypeError', str(e)))
    return out


def steps(x, k):
    out = [x[::2], x[1:5:1], x[::-1], x[::k], x[1::k], x[None:None:None]]
    try:
        out.append(x[::0])
    except ValueError as e:
        out.append(('ValueError', str(e)))
    return out


def subclasses(text):
    s = S(text)
    b = B(text.encode('utf-8'))
    rs, rb = s[:], b[:]
    return (rs, type(rs).__name__, rs is s, type(s[1:3]).__name__,
            s[0:0] is '', rb, type(rb).__name__, rb is b,
            type(b[1:3]).__name__, b[0:0] is b'', s[1:], b[1:])


def not_ready(text):
    a = unready(text)
    b = unready(text)
    c = unready(text)
    return a[1:3], b[:], b[:] == text, c[:0] is ''


def constants():
    return (WORD[1:3], WORD[:], WORD[:] is WORD, WORD[3:3] is '',
            BWORD[1:3], BWORD[:] is BWORD, WORD[-2:], 'h€llo'[1:3],
            'h😀llo'[:2])


def constant_runtime_bounds(i, j):
    return WORD[i:j], BWORD[i:j], 'h€llo'[i:j], WORD[i:], BWORD[:j]


def lists(n):
    l = list(range(n))
    out = []
    for i in range(-n - 1, n + 2):
        for j in range(-n - 1, n + 2):
            r = l[i:j]
            out.append((r, r is l))
    t = tuple(l)
    out.append((t[1:3], t[:] is t, t[5:1]))
    return out


def list_stores(n):
    out = []
    for i in range(-n - 1, n + 2):
        for j in range(-n - 1, n + 2):
            l = list(range(n))
            l[i:j] = ['x', 'y']
            out.append(l)
    l = list(range(n))
    l[:] = l
    out.append(l)
    l[1:3] = l
    out.append(l)
    l[:] = (c for c in 'ab')
    out.append(l)
    try:
        l[0:1] = 5
    except TypeError as e:
        out.append(str(e))
    return out


def list_deletes(n):
    out = []
    for i in range(-n - 1, n + 2):
        l = list(range(n))
        del l[i:i + 2]
        out.append(l)
    return out


def move_to_front(l, c):
    # pyflate's move_to_front
    l[:] = l[c:c + 1] + l[0:c] + l[c + 1:]
    return l


def augmented(x, i, j):
    l = list(x)
    l[i:j] += ['z']
    s = x
    s += x[i:j]
    return l, s


def list_subclasses(n):
    a = L(range(n))
    b = LGet(range(n))
    r = a[1:3]
    a[0:1] = 'q'
    return r, type(r).__name__, b[1:3], list(a)


def escapes(i):
    k = Key()
    s = k[1:i]
    d = {}
    try:
        d[1:i]
    except TypeError as e:
        e = str(e)
    return s, type(s).__name__, s.start, s.stop, s.step, k[i:], e


def slice_errors(x):
    try:
        return x[1:2]
    except TypeError as e:
        return str(e)


def bytes_items(nt):
    # pyflate's run-length decoding
    out = []
    i = 0
    while i < len(nt) - 4:
        out.append(nt[i:i + 1] * 2)
        out.append(nt[i + 4:i + 5])
        i += 1
    return b''.join(out)


CASES = [
    ('all_bounds', '[(KINDS,), (BKINDS,), ([S("abc"), B(b"abc")],)]', {}),
    ('empties', '[(KINDS + BKINDS,)]', {}),
    ('full', '[(KINDS + BKINDS,), ([S("ab"), B(b"ab")],)]', {}),
    ('big_bounds', '[("hello", BIG), (b"hello", BIG), ([1, 2, 3, 4], BIG)]',
     {}),
    ('none_bounds', [('hello', None, 3), (b'hello', 1, None),
                     ([1, 2, 3], None, None)], {}),
    ('odd_bounds', [('hello',), (b'hello',), ([1, 2, 3, 4],), ((1, 2, 3),)],
     {}),
    ('steps', [('hello', 2), (b'hello', -1), ([1, 2, 3, 4, 5], 3)], {}),
    ('subclasses', [('hello',), ('héllo',)], {}),
    ('not_ready', [('hello',), ('h€llo',)], {}),
    ('constants', [()], {}),
    ('constant_runtime_bounds', [(0, 2), (2, -1), (-9, 99), (4, 1)], {}),
    ('lists', [(0,), (3,)], {}),
    ('list_stores', [(0,), (3,)], {}),
    ('list_deletes', [(0,), (4,)], {}),
    ('move_to_front', [([1, 2, 3, 4], 2), ([1, 2, 3, 4], 0), ([5], 0)], {}),
    ('augmented', [('hello', 1, 3), ('abc', -1, 9)], {}),
    ('list_subclasses', [(4,)], {}),
    ('escapes', [(3,), (None,)], {}),
    ('slice_errors', [(5,), (None,), ({},)], {}),
    ('bytes_items', [(b'abcdefgh',), (bytes(range(256)),)], {}),
]
