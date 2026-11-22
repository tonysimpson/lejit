# Methods of builtin types whose Argument Clinic wrappers are generated
# (dict.get, dict.setdefault, str and bytes strip, lstrip, rstrip): every
# count of arguments, wrong types, subclasses as self, the identity CPython
# gives an unchanged result, and the TypeError messages of a bad count.


class D(dict):
    pass


class Missing(dict):
    def __missing__(self, key):
        return 'missing'


class S(str):
    pass


class B(bytes):
    pass


def err(f, *a):
    try:
        return f(*a)
    except Exception as e:
        return type(e).__name__, str(e)


def gets(d, keys):
    out = []
    for k in keys:
        out.append(d.get(k))
        out.append(d.get(k, 0))
        out.append(d.get(k, k))
    out.append(err(d.get))
    out.append(err(d.get, 1, 2, 3))
    out.append(err(d.get, []))
    return out


def setdefaults(keys):
    d = {}
    out = []
    for k in keys:
        out.append(d.setdefault(k))
        out.append(d.setdefault(k, 5))
        out.append(d.setdefault(k * 2, [k]))
    out.append(err(d.setdefault))
    out.append(err(d.setdefault, 1, 2, 3))
    out.append(err(d.setdefault, {}))
    return out, sorted(d.items(), key=repr)


def get_subclass(n):
    d, m = D(a=1), Missing(a=1)
    out = []
    for i in range(n):
        out.append((d.get('a'), d.get('b', i), m.get('b', i), m.get('a')))
    return out


def strips(words):
    out = []
    for w in words:
        out.append((w.strip(), w.lstrip(), w.rstrip(), w.strip(None),
                    w.strip('.,'), w.lstrip('.x'), w.rstrip(',y'), w.strip(''),
                    w.strip() is w, w.strip('.,') is w, w.lstrip(None) is w))
    out.append(err(words[0].strip, 1))
    out.append(err(words[0].strip, '.', ','))
    out.append(err(words[0].lstrip, b'.'))
    return out


def strip_subclass(n):
    s = S('  ab  ')
    t = S('ab')
    out = []
    for i in range(n):
        u = t.strip()
        out.append((s.strip(), type(s.strip()).__name__, u is t, type(u).__name__,
                    s.rstrip('b '), s.lstrip(' a')))
    return out


def bytes_strips(words):
    out = []
    for w in words:
        out.append((w.strip(), w.lstrip(), w.rstrip(), w.strip(None), w.strip(b'.,'),
                    w.strip(bytearray(b'x')), w.strip() is w, w.strip(b'') is w))
    out.append(err(words[0].strip, '.'))
    out.append(err(words[0].strip, 1, 2))
    b = B(b' q ')
    out.append((b.strip(), type(b.strip()).__name__, B(b'q').strip() is b))
    return out


CASES = [
    ('gets', [({'a': 1, 'b': None, 1: 'one', (1, 2): 't'}, ['a', 'b', 'c', 1, 2, (1, 2)])], {}),
    ('setdefaults', [(['x', 'y', 'x'],)], {}),
    ('get_subclass', [(4,)], {}),
    ('strips', [([' a. ', '..b,,', 'c', '', ' \t ', 'x.y,', 'é ', ' ü€ '],)], {}),
    ('strip_subclass', [(3,)], {}),
    ('bytes_strips', [([b' a. ', b'..b,,', b'x', b'', b'xyx'],)], {}),
]
