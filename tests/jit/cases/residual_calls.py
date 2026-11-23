# Calls LeJIT doesn't compile into the caller: a callable known only at
# run time (a site that sees many), a builtin, a bound method, a class.
# Their positional arguments go to the callee as ceval passes them
# (pabstract.c:psyco_vectorcall), with no tuple, and must give what
# CPython gives: results, errors and what the callee sees.
import functools, operator, sys


class Point:
    def __init__(self, x, y):
        self.x, self.y = x, y

    def moved(self, dx, dy):
        return Point(self.x + dx, self.y + dy)

    def __repr__(self):
        return 'Point(%r, %r)' % (self.x, self.y)


class Slotted:
    __slots__ = ('a',)

    def __init__(self, a=0):
        self.a = a

    def get(self, *more):
        return (self.a,) + more


def six(a, b, c, d, e, f):
    return a + b + c + d + e + f


def seen(*args):
    return args


def raises(x):
    raise ValueError('raised for %r' % (x,))


# many callables at one site, so the site is megamorphic and each call
# is residual; each takes the arguments given with it
CALLEES = [
    (len, ('abc',)),
    (abs, (-3,)),
    (max, (1, 7, 3)),
    (min, (4, 2, 9, 8, 6)),
    (isinstance, (3, (int, str))),
    (divmod, (17, 5)),
    (pow, (2, 10, 7)),
    (repr, (1.5,)),
    (hash, ('',)),
    (sorted, ([3, 1, 2],)),
    (seen, ()),
    (seen, (1, 2, 3, 4, 5)),
    (six, (1, 2, 3, 4, 5, 6)),
    ('-'.join, (['a', 'b'],)),
    ([1, 2, 3].index, (2,)),
    ({}.get, ('k', 'default')),
    (functools.partial(six, 1, 2), (3, 4, 5, 6)),
    (operator.itemgetter(1), ((7, 8, 9),)),
    (Point(1, 2).moved, (3, 4)),
    (Slotted(5).get, (6, 7)),
    (Point, (5, 6)),
    (tuple, ([1, 2],)),
    (range, (2, 9, 3)),
    (int, ('ff', 16)),
    (str, (b'x', 'ascii')),
    (dict, ()),
]


def megamorphic(n):
    out = []
    for _ in range(n):
        for f, args in CALLEES:
            if len(args) == 0:
                r = f()
            elif len(args) == 1:
                r = f(args[0])
            elif len(args) == 2:
                r = f(args[0], args[1])
            elif len(args) == 3:
                r = f(args[0], args[1], args[2])
            elif len(args) == 4:
                r = f(args[0], args[1], args[2], args[3])
            elif len(args) == 5:
                r = f(args[0], args[1], args[2], args[3], args[4])
            else:
                r = f(args[0], args[1], args[2], args[3], args[4], args[5])
            out.append(repr(r))
    return out


# the errors CPython gives at the call: wrong argument counts, a value
# that isn't callable, an error from inside the callee
BAD = [
    (len, (1, 2)),
    (abs, ()),
    (divmod, (1, 0)),
    (isinstance, (1,)),
    ({}.get, ()),
    (Point(0, 0).moved, (1,)),
    (Point, (1, 2, 3)),
    (tuple, (1, 2)),
    (raises, (4,)),
    (3, (1,)),
    (None, ()),
]


def errors(n):
    out = []
    for _ in range(n):
        for f, args in BAD:
            try:
                if len(args) == 0:
                    f()
                elif len(args) == 1:
                    f(args[0])
                elif len(args) == 2:
                    f(args[0], args[1])
                else:
                    f(args[0], args[1], args[2])
            except Exception as e:
                out.append((type(e).__name__, str(e)))
    return out


# arguments made in the caller: computed values, a fresh tuple, a fresh
# object; the callee keeps some of them
def computed_args(n):
    kept = []
    keep = kept.append
    fs = [keep, seen, max, operator.add]
    out = []
    for i in range(n):
        f = fs[i % len(fs)]
        r = f(i * 2, (i, i + 1)) if f is not keep else f((i * 2, [i]))
        out.append(r)
    return out, kept


# a callee seeing its arguments: through sys._getframe, the frame's locals
# are the arguments passed
def frame_args(a, b):
    return sorted(sys._getframe().f_locals.items())


def callee_frames(n):
    fs = [frame_args, seen, max]
    out = []
    for i in range(n):
        out.append(fs[i % 3](i, i + 1))
    return out


# a bound method's self, the argument in front: the callee's own args[-1]
def bound_methods(n):
    ps = [Point(i, -i) for i in range(4)]
    ms = [p.moved for p in ps] + [Slotted(i).get for i in range(4)]
    out = []
    for i in range(n):
        m = ms[i % len(ms)]
        out.append(repr(m(i, 1)))
    return out


# recursion through a residual call: RecursionError, as CPython raises it
def recurse_residual(f, k):
    return f(f, k + 1)


def residual_recursion():
    fs = [recurse_residual, recurse_residual]
    try:
        fs[0](fs[1], 0)
    except RecursionError as e:
        return type(e).__name__
    return 'no error'


CASES = [
    ('megamorphic', [(1,), (3,)], {}),
    ('errors', [(2,)], {}),
    ('computed_args', [(12,)], {}),
    ('callee_frames', [(9,)], {}),
    ('bound_methods', [(16,)], {}),
    ('residual_recursion', [()], {}),
]
