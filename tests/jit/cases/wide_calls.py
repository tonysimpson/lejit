# Calls of many values that go to the callee as ceval passes them, in one
# array (pabstract.c:psyco_vectorcall), whatever their number: 6, 8 and 20
# values, keywords among them, constants mixed with values known only at
# run time, values made in the caller. Each must give what CPython gives:
# results, errors and what the callee sees.
import functools, re


def seen(*args, **kwargs):
    return args, sorted(kwargs.items())


def eight(a, b, c, d, e, f, g, h):
    return a + b + c + d + e + f + g + h


def kw_eight(a, b, c, d, *, e, f, g, h):
    return [a, b, c, d, e, f, g, h]


class Box:
    def __init__(self, *args, **kwargs):
        self.args, self.kwargs = args, sorted(kwargs.items())

    def __repr__(self):
        return 'Box(%r, %r)' % (self.args, self.kwargs)

    def m(self, *args, **kwargs):
        return len(args), sorted(kwargs)


# many callables at one site, so the site is megamorphic and each call is
# residual
def callees():
    return [seen, functools.partial(seen, 'p'), Box, Box(1).m,
            functools.partial(eight, 1), max, min, seen, seen, seen]


def widths(n):
    fs = callees()
    out = []
    for i in range(n):
        f = fs[i % len(fs)]
        if f is fs[4]:
            out.append(f(i, 2, 3, 4, 5, 6, 7))
            continue
        out.append(repr(f(i, 1, 2, 3, 4, 5)))
        out.append(repr(f(i, 1, 2, 3, 4, 5, 6, i + 7)))
        out.append(repr(f(i, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
                          15, 16, 17, 18, i * 2)))
    return out


def keywords(n):
    fs = [seen, functools.partial(seen, 0), Box, Box(2).m, seen, seen]
    out = []
    for i in range(n):
        f = fs[i % len(fs)]
        out.append(repr(f(i, 1, 2, a=3, b=i, c=5)))
        out.append(repr(f(a=i, b=1, c=2, d=3, e=4, f=5, g=6, h=7)))
        out.append(repr(f(i, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
                          k1=i, k2=i + 1, k3=None, k4='x', k5=(), k6=1.5)))
    return out


# values known only at run time, in registers and on the stack, constants
# between them, and values made in the caller (floats, a tuple, a list)
def mixes(n):
    fs = [seen, functools.partial(seen), Box, seen, seen, seen]
    out = []
    for i in range(n):
        f = fs[i % len(fs)]
        a, b, c, d = i, i * 3, str(i), i / 2
        e, g, h, j = [i], (i, a), b - a, -i
        out.append(repr(f(a, 'k', b, None, c, 2.5, d, True, e, g, h, j,
                          a + b, (c, d), d * 2.0, i + 0.5, -1, 1 << 40,
                          1 << 70, x=a, y=d, z=(a, b))))
    return out


# a Python function's call at a site that sees many functions, and one
# whose keyword-only parameters must be bound by CPython
def functions(n):
    fs = [eight, kw_eight, seen, eight, kw_eight, seen, eight]
    out = []
    for i in range(n):
        f = fs[i % len(fs)]
        if f is kw_eight:
            out.append(f(i, 1, 2, 3, e=4, f=5, g=6, h=i))
        else:
            out.append(f(i, 1, 2, 3, 4, 5, 6, i))
    return out


# the errors CPython gives for such calls
def errors(n):
    fs = [eight, kw_eight, Box(0).m, functools.partial(eight), seen, max]
    out = []
    for i in range(n):
        f = fs[i % len(fs)]
        for call in (lambda: f(1, 2, 3, 4, 5, 6, 7),
                     lambda: f(1, 2, 3, 4, 5, 6, 7, 8, 9),
                     lambda: f(1, 2, 3, 4, e=5, f=6, g=7, h=8, z=9),
                     lambda: f(1, 2, 3, 4, 5, 6, 7, a=8)):
            try:
                out.append(repr(call()))
            except Exception as e:
                out.append((type(e).__name__, str(e)))
    return out


# a C method that takes its defining class (re's), called with self in
# front and keywords
def c_methods(n):
    p = re.compile('a')
    out = []
    for i in range(n):
        out.append(p.sub(repl='b', string='a' * (i % 4 + 1), count=i % 3))
        out.append(p.subn('c', 'aaa', i % 2))
    return out


CASES = [
    ('widths', [(1,), (12,)], {}),
    ('keywords', [(1,), (8,)], {}),
    ('mixes', [(1,), (8,)], {}),
    ('functions', [(1,), (9,)], {}),
    ('errors', [(1,), (7,)], {}),
    ('c_methods', [(1,), (6,)], {}),
]
