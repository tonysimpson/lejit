# Keyword calls LeJIT doesn't compile into the caller: a callable known
# only at run time (a site that sees many). The values go to the callee
# as ceval passes them, in one array with the call's names tuple
# (pabstract.c:psyco_vectorcall), and must give what CPython gives:
# results, what the callee sees, and the TypeError for a keyword it
# doesn't take or one that repeats a positional.
import functools, operator


class Point:
    def __init__(self, x, y=0):
        self.x, self.y = x, y

    def moved(self, dx=0, dy=0):
        return Point(self.x + dx, self.y + dy)

    def __repr__(self):
        return 'Point(%r, %r)' % (self.x, self.y)


class Slotted:
    __slots__ = ('a', 'b')

    def __init__(self, a=0, b=1):
        self.a, self.b = a, b

    def __repr__(self):
        return 'Slotted(%r, %r)' % (self.a, self.b)


class Called:
    def __call__(self, *args, **kw):
        return 'called', args, sorted(kw.items())


def pair(a, b=2):
    return a, b


def only_kw(a, *, b, c=3):
    return a, b, c


def star_kw(*args, **kw):
    # the dict is the callee's own: changing it changes nothing outside
    kw['added'] = len(kw)
    return args, sorted(kw.items())


def wide(a, b, c, d, e, f, g, h, i, j):
    return a + b + c + d + e + f + g + h + i + j


# many callables at one site, so the site is megamorphic and each call
# is residual; each has a call of its own shape, chosen by number
CALLEES = [
    pair, only_kw, star_kw, Point, Slotted, Point(1, 2).moved,
    functools.partial(pair, 1), Called(), sorted, int, dict, str.split,
    'a-b-c'.split, wide, max, '{0}{end}{sep}'.format,
]


def call(k, f, i):
    if k == 0:
        return f(i, b=i + 1)
    if k == 1:
        return f(i, b=1)
    if k == 2:
        return f(i, x=1, y=i)
    if k == 3:
        return f(x=i, y=2)
    if k == 4:
        return f(b=i)
    if k == 5:
        return f(dy=i)
    if k == 6:
        return f(b=i)
    if k == 7:
        return f(i, z=i)
    if k == 8:
        return f([3, i, 1], key=operator.neg, reverse=True)
    if k == 9:
        return f('101', base=2 + i % 8)
    if k == 10:
        return f(a=i, b=2)
    if k == 11:
        return f('a b c', maxsplit=1)
    if k == 12:
        return f(sep='-', maxsplit=i % 3)
    if k == 13:
        return f(j=i, i=1, h=2, g=3, f=4, e=5, d=6, c=7, b=8, a=9)
    if k == 14:
        return f([i, 5, 2], key=operator.neg)
    return f(i, end='', sep=',', file=None, flush=False)


def megamorphic(n):
    out = []
    for i in range(n):
        for k, f in enumerate(CALLEES):
            out.append(repr(call(k, f, i)))
    return out


# CPython's TypeError: a keyword the callee doesn't take, one that repeats
# a positional, a C function that takes no keywords, a missing
# keyword-only argument, a class or a bound method given a wrong one, a
# value that isn't callable. Each call shape is one site that sees many.
BAD = [
    pair, only_kw, wide, len, abs, isinstance, Point, Slotted,
    Point(0).moved, sorted, int, dict.fromkeys, object,
    functools.partial(pair, 1), star_kw, 3, None,
]


def errors(n):
    out = []
    for _ in range(n):
        for f in BAD:
            for shape in range(4):
                try:
                    if shape == 0:
                        r = f(1, c=2)
                    elif shape == 1:
                        r = f(1, a=2)
                    elif shape == 2:
                        r = f(1, 2, b=3)
                    else:
                        r = f(x=1, dx=2, keyy=None)
                    out.append(repr(r))
                except TypeError as e:
                    out.append(str(e))
    return out


# keyword values made in the caller: computed, a fresh tuple, a fresh
# object, which the callee keeps
def computed_values(n):
    kept = []
    fs = [star_kw, Called(), dict, Point]
    out = []
    for i in range(n):
        f = fs[i % len(fs)]
        r = f(x=(i, [i]), y=i * 2)
        kept.append(r)
        out.append(repr(r))
    return out, len(kept)


# a keyword call of a callable known only at run time, which raises
def raises(x=None):
    raise ValueError('raised for %r' % (x,))


def raising(n):
    fs = [raises, pair]
    out = []
    for i in range(n):
        try:
            out.append(fs[i % 2](i, b=i) if i % 2 else fs[0](x=i))
        except ValueError as e:
            out.append(str(e))
    return out


CASES = [
    ('megamorphic', [(1,), (3,)], {}),
    ('errors', [(2,)], {}),
    ('computed_values', [(12,)], {}),
    ('raising', [(8,)], {}),
]
