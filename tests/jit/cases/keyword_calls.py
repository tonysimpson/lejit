# Keyword calls of Python functions known at compile time, bound by the
# binder from the call's pending keywords (pdictobject.c,
# psyfunc.c:_PsycoEval_EvalCodeWithName); and the calls it leaves to the
# real one, whose TypeError must be CPython's own text on every version: a
# keyword it doesn't take, one that repeats a positional, a positional-only
# name, a missing or extra argument. Calls of anything else (C functions,
# classes, a megamorphic site) get the dict CPython makes.
import sys


def leaf(x, y=1, z=2):
    return x, y, z


def two(a, b):
    return a * 10 + b


class Obj:
    def m(self, x, y=5):
        return 'm', x, y

    @staticmethod
    def st(p, q=0):
        return p - q


def bound(n):
    out = []
    o = Obj()
    for i in range(n):
        out.append((leaf(i, y=i + 1), leaf(i, z=7), leaf(x=i), leaf(z=1, x=i),
                    leaf(i, z=3, y=4), two(b=i, a=2), o.m(i, y=9), o.m(x=i),
                    Obj.st(q=i, p=5)))
    return out


def errors(n):
    out = []
    for i in range(n):
        for f in (lambda: leaf(i, w=1), lambda: leaf(i, x=2),
                  lambda: two(a=i), lambda: two(1, 2, b=3),
                  lambda: leaf(y=i), lambda: Obj().m(i, q=1),
                  lambda: Obj().m(i, self=1)):
            try:
                out.append(f())
            except TypeError as e:
                out.append(str(e))
    return out


# the syntax is 3.8's: made where the code object has the count
POSONLY = {}
if hasattr(leaf.__code__, 'co_posonlyargcount'):
    exec('def po(a, /, b=1):\n    return a, b\n'
         'def pk(a, /, **kw):\n    return a, kw\n', POSONLY)


def positional_only(n):
    if not POSONLY:
        return None
    po, pk = POSONLY['po'], POSONLY['pk']
    out = []
    for i in range(n):
        out.append((po(i, b=2), pk(i, a=3)))
        try:
            po(a=i)
        except TypeError as e:
            out.append(str(e))
    return out


def nested(n):
    out = []

    def inner(a, b=0):
        return a - b
    for i in range(n):
        out.append((inner(i, b=1), inner(b=i, a=10), (lambda p, q: p * q)(q=i, p=3)))
    return out


def others(n):
    # C functions, classes and a builtin method with keywords
    out = []
    for i in range(n):
        out.append((sorted([3, i, 1], reverse=True), dict(a=i, b=2),
                    int('11', base=i + 2), 'a,b'.split(sep=','),
                    sorted([2, 1, i], key=lambda v: -v)))
    return out


def frame_sees(a, b=0):
    return sorted(sys._getframe().f_locals.items()), sys._getframe(1).f_code.co_name


def seen_by_frames(n):
    return [frame_sees(b=i, a=1) for i in range(n)]


def changing(n):
    # the code and the defaults change under the call site
    def a(x, y=1):
        return 'a', x, y

    def b(x, y=2):
        return 'b', x, y
    code_a = a.__code__
    out = []
    for i in range(n):
        out.append(a(i, y=3))
        out.append(a(x=i))
        a.__defaults__ = (i,)
        out.append(a(x=i))
        a.__code__ = b.__code__
        out.append(a(x=i, y=0))
        a.__code__ = code_a
        a.__defaults__ = (1,)
    return out


def megamorphic(n):
    fs = [lambda x, y=0, k=k: (k, x, y) for k in range(12)]
    return [f(i, y=i) for i in range(n) for f in fs]


def value_identity(n):
    out = []
    for i in range(n):
        v = [i]
        r = leaf(1, y=v)
        out.append(r[1] is v)
    return out


def recursive(n, acc=0):
    if n == 0:
        return acc
    return recursive(acc=acc + n, n=n - 1)


CASES = [
    ('bound', [(0,), (3,)], {}),
    ('errors', [(2,)], {}),
    ('positional_only', [(3,)], {}),
    ('nested', [(3,)], {}),
    ('others', [(3,)], {}),
    ('seen_by_frames', [(3,)], {}),
    ('changing', [(3,)], {}),
    ('megamorphic', [(2,)], {}),
    ('value_identity', [(3,)], {}),
    ('recursive', [(30,)], {}),
]
