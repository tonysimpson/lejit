# Iterators and the builtins over them, with generated metas:
# PyObject_SelfIter (tp_iter of sequence and range iterators, object.c),
# callable and hasattr (bltinmodule.c). iter, next, chr and ord stay C
# calls (METACOMPILER failures log); their cases check the paths anyway.


def msg(f, *a, **k):
    try:
        return ('ok', f(*a, **k))
    except Exception as e:
        return (type(e).__name__, str(e))


def selfiter(n):
    it = iter([1, 2, 3][:n])
    r = iter(range(n))
    out = [iter(it) is it, iter(r) is r]
    for x in iter(it):
        out.append(x)
    for x in iter(r):
        out.append(x)
    return out


class Calls:
    def __call__(self):
        return 1


class NoCalls:
    pass


def callables(x):
    out = [callable(x), callable(len), callable(Calls), callable(Calls()),
           callable(NoCalls()), callable(3), callable(None)]
    if callable(x):
        out.append('yes')
    out.append(msg(callable))
    out.append(msg(callable, 1, 2))
    return out


class Attrs:
    a = 1

    @property
    def gone(self):
        raise AttributeError('gone')

    @property
    def broken(self):
        raise ValueError('broken')

    def __getattr__(self, name):
        if name == 'dyn':
            return 5
        raise AttributeError(name)


def hasattrs(o, name):
    return [hasattr(o, name), hasattr(Attrs(), 'a'), hasattr(Attrs(), 'gone'),
            hasattr(Attrs(), 'dyn'), hasattr(Attrs(), 'nope'),
            msg(hasattr, Attrs(), 'broken'), msg(hasattr, o, 3),
            msg(hasattr, o), msg(hasattr, o, name, 1), hasattr(3, 'real'),
            hasattr([], 'append')]


def sentinel(n):
    it = iter([1, 2, 3, 0, 4][:n]).__next__
    out = list(iter(it, 0))
    out.append(msg(iter, 3, 0))
    out.append(msg(iter, 3))
    out.append(msg(iter))
    out.append(type(iter([])).__name__)
    return out


def nexts(n):
    it = iter(range(n))
    out = [next(it, 'd'), next(it, 'd'), next(it, None)]
    out.append(msg(next, it))
    out.append(msg(next, [1]))
    out.append(msg(next, iter([]), 'dflt'))
    out.append(msg(next))

    def gen():
        yield 1
        raise ValueError('inside')
    g = gen()
    out.append(next(g, 'x'))
    out.append(msg(next, g, 'x'))
    return out


def chrs(i):
    return [chr(i), msg(chr, -1), msg(chr, 0x110000), msg(chr, 'a'),
            msg(chr, 2 ** 70), chr(0x10ffff), chr(True)]


def ords(s):
    return [ord(s), ord('€'), ord(b'a'), ord(bytearray(b'z')),
            msg(ord, 'ab'), msg(ord, ''), msg(ord, 3), msg(ord, b''),
            ord('\U0001f600')]


CASES = [
    ('selfiter', [(0,), (2,), (3,)], {}),
    ('callables', "[(len,), (3,), (None,), (Calls(),)]", {}),
    ('hasattrs', "[([], 'append'), ([], 'nope'), (Attrs(), 'gone'),"
                 " (Attrs(), 'broken')]", {}),
    ('sentinel', [(0,), (3,), (5,)], {}),
    ('nexts', [(0,), (1,), (3,)], {}),
    ('chrs', [(65,), (0,), (0x20ac,)], {}),
    ('ords', [('a',), ('\xe9',), ('\U0001f600',)], {}),
]
