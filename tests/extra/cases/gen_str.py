# str slots generated from unicodeobject.c (punicodeobject_gen.h):
# len (the ready bit, then the length field), 'in' (constants fold, else
# PyUnicode_Contains), + (PyUnicode_Concat); for each PEP 393 kind (ASCII,
# latin-1, UCS-2, UCS-4), subclasses, empty strings. With them the builtins
# that stay C calls (iter, next, chr, ord) and the generated delattr.


class S(str):
    pass


class SLen(str):
    def __len__(self):
        return 42


KINDS = ['abc', 'caf\xe9', '€ uro', '\U0001f600 x', '', S('sub'), S(''),
         S('€')]


def lengths(xs):
    return [len(x) for x in xs]


def lengths_loop(xs, n):
    t = 0
    for i in range(n):
        for x in xs:
            t += len(x)
    return t


def sub_len(x):
    return len(x)


def contains_const():
    return ('b' in 'abc', 'z' in 'abc', '' in 'abc', '' in '', '\xe9' in 'caf\xe9',
            '€' in '€ uro', '\U0001f600' in 'x\U0001f600')


def contains(xs, ys):
    return [(y in x) for x in xs for y in ys]


def contains_error(x, y):
    try:
        return y in x
    except TypeError as e:
        return 'TypeError', str(e)


def concat(xs):
    out = []
    for a in xs:
        for b in xs:
            r = a + b
            out.append((r, type(r).__name__, len(r)))
    return out


def concat_error(a, b):
    try:
        return a + b
    except TypeError as e:
        return 'TypeError', str(e)


def compare(xs):
    return [(a == b, a != b, a < b) for a in xs for b in xs]


def iter_next(xs):
    out = []
    for x in xs:
        it = iter(x)
        out.append(next(it, 'end'))
        out.append(next(it, 'end'))
        try:
            while True:
                out.append(next(it))
        except StopIteration:
            out.append('stop')
    return out


def iter_sentinel(n):
    c = [0]

    def f():
        c[0] += 1
        return c[0]
    return list(iter(f, n))


def iter_error(x):
    try:
        return iter(x)
    except TypeError as e:
        return 'TypeError', str(e)


def next_error(x):
    try:
        return next(x)
    except TypeError as e:
        return 'TypeError', str(e)


def chr_ord(ns):
    return [(chr(n), ord(chr(n))) for n in ns]


def chr_error(n):
    try:
        return chr(n)
    except (ValueError, TypeError, OverflowError) as e:
        return type(e).__name__, str(e)


def ord_error(x):
    try:
        return ord(x)
    except TypeError as e:
        return 'TypeError', str(e)


class O:
    pass


def delattr_(names):
    o = O()
    out = []
    for n in names:
        setattr(o, n, 1)
        delattr(o, n)
        out.append(hasattr(o, n))
        try:
            delattr(o, n)
        except AttributeError as e:
            out.append(('AttributeError', str(e)))
    return out


def delattr_error(o, n):
    try:
        delattr(o, n)
        return 'deleted'
    except (AttributeError, TypeError) as e:
        return type(e).__name__, str(e)


CASES = [
    ('lengths', '[(KINDS,)]', {}),
    ('lengths_loop', '[(KINDS, 5)]', {}),
    ('sub_len', '[(SLen("ab"),), ("ab",), (S("xyz"),)]', {}),
    ('contains_const', [()], {}),
    ('contains', '[(KINDS, ["", "a", "\\xe9", "\\u20ac", "\\U0001f600", S("b")])]', {}),
    ('contains_error', [('abc', 1), ('abc', None)], {}),
    ('concat', '[(KINDS,)]', {}),
    ('concat_error', [('a', 1), ('a', b'b')], {}),
    ('compare', '[(KINDS[:5],)]', {}),
    ('iter_next', '[(["ab", [1, 2, 3], (), "\\u20ac", range(2)],)]', {}),
    ('iter_sentinel', [(4,), (1,)], {}),
    ('iter_error', [(1,), (None,)], {}),
    ('next_error', [(1,), ('abc',)], {}),
    ('chr_ord', [([0, 65, 233, 0x20ac, 0x1f600],)], {}),
    ('chr_error', [(-1,), (0x110000,), ('a',), (1 << 70,)], {}),
    ('ord_error', [('',), ('ab',), (1,), (b'xy',)], {}),
    ('delattr_', [(['a', 'b'],)], {}),
    ('delattr_error', '[(O(), "x"), (O(), 1), (1, "real")]', {}),
]
