# str len(), s[i] and 'in' (Objects/punicodeobject.c). Compact ASCII
# strings are read inline; other kinds, subclasses and strings not yet
# "ready" go to C on a respawned branch, so each test mixes them after an
# ASCII warm-up. Constants fold. One-character results below 256 must be
# CPython's cached strings ('is' agrees with CPython).
import ctypes

WORD = 'abc'
KINDS = ('hello', 'héllo', 'h€llo', 'h😀llo', '', 'x')


class S(str):
    pass


class Len(str):
    def __len__(self):
        return 99


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


def lengths(xs):
    return [len(x) for x in xs]


def items(xs):
    out = []
    for x in xs:
        for i in range(-len(x) - 2, len(x) + 2):
            try:
                c = x[i]
                out.append((i, c, c is x[i], c is chr(ord(c))))
            except IndexError as e:
                out.append((i, str(e)))
    return out


def first_last(x):
    return x[0], x[-1], x[0] is 'h', x[-1] is 'o'


def loop_chars(x):
    n = 0
    for i in range(len(x)):
        if x[i] == 'l':
            n += 1
    return n


def odd_keys(x):
    out = [x[True], x[False]]
    for k in (2 ** 70, -2 ** 70, 1.0, 'a', None, slice(1, 3)):
        try:
            out.append(x[k])
        except (IndexError, TypeError) as e:
            out.append((type(e).__name__, str(e)))
    return out


def subclasses(x):
    s = S(x)
    return len(s), s[0], s[-1], s[0] is x[0], len(Len(x)), Len(x)[-1]


def not_ready(text):
    a = unready(text)
    b = unready(text)
    return len(a), b[1], b[-1], len(b)


def constants():
    return (len(WORD), WORD[0], WORD[-1], WORD[1] is 'b', len('h€llo'),
            'h€llo'[1], 'h😀llo'[-4], 'héllo'[1] is 'é', 'b' in WORD,
            'z' in WORD, 'bc' in WORD)


def constant_out_of_range():
    return WORD[3]


def constant_runtime_index(i):
    return WORD[i], 'h€llo'[i], 'h😀llo'[i], 'héllo'[i]


def contains(xs):
    return [(a in b, b in a) for a in xs for b in KINDS]


def contains_errors(x):
    try:
        return 1 in x
    except TypeError as e:
        return str(e)


CASES = [
    ('lengths', '[(KINDS,), (["".join(["a", "b"]), "é" * 3],)]', {}),
    ('items', '[(["".join(["ab", "c"])],), (KINDS,)]', {}),
    ('first_last', '[("".join(["hel", "lo"]),), ("héllo",), ("h😀llo",)]',
     {}),
    ('loop_chars', '[("hello world",), ("hé€😀llo",)]', {}),
    ('odd_keys', [('abc',), ('h€llo',)], {}),
    ('subclasses', [('abc',), ('héllo',)], {}),
    ('not_ready', [('hello',), ('h€llo',)], {}),
    ('constants', [()], {}),
    ('constant_out_of_range', [()], {}),
    ('constant_runtime_index', [(0,), (2,), (-1,), (4,), (9,)], {}),
    ('contains', '[(KINDS,)]', {}),
    ('contains_errors', [('abc',)], {}),
]
