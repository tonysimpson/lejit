# Locals that hold a constant function, builtin or class on some paths and
# another on others, with counters, so the loop head passes the cap per
# merge point (D40). Results and identity must not change.

def twice(x):
    return 2 * x


def plus1(x):
    return x + 1


def neg(x):
    return -x


class A:
    def __init__(self, v):
        self.v = v


class B(A):
    pass


class C:
    def __init__(self, v):
        self.v = -v


def funcs(items):
    a = b = c = d = 0
    op = twice
    out = []
    for x in items:
        if x == 1:
            a += 1
            op = plus1
        elif x == 2:
            b += 1
            op = neg
        elif x == 3:
            c += 1
            op = abs
        elif x == 4:
            d += 1
            op = twice
        elif x == 5:
            op = str
        out.append(op(x))
    return a, b, c, d, out, op is twice, op is abs, op.__name__


def classes(items):
    a = b = c = 0
    cls = A
    objs = []
    for x in items:
        if x == 1:
            a += 1
            cls = B
        elif x == 2:
            b += 1
            cls = C
        elif x == 3:
            c += 1
            cls = A
        elif x == 4:
            cls = int
        o = cls(x)
        objs.append((type(o) is cls, isinstance(o, A), getattr(o, 'v', o)))
    return a, b, c, objs, cls is A, cls.__name__


def default_const(items, key=len, cut=3):
    # a constant default, as meteor_contest's bisect=bisect, and counters
    a = b = c = d = 0
    s = None
    for x in items:
        if x == 1:
            a += key([x])
        elif x == 2:
            b += key((x, x))
        elif x == 3:
            c += 1
            s = 'x' * x
        elif x == 4:
            d += cut
            s = None
        else:
            a, d = d, a
    return a, b, c, d, s, key is len


def mixed(items):
    # one local that is a function, a class, a large int or None by turns
    a = b = c = 0
    v = None
    out = []
    for x in items:
        if x == 1:
            a += 1
            v = twice
        elif x == 2:
            b += 1
            v = A
        elif x == 3:
            c += 1
            v = 1000 + x
        elif x == 4:
            v = None
        if callable(v):
            out.append(type(v(x)).__name__)
        else:
            out.append(v)
    return a, b, c, out, v is None


def orders(f, n):
    # every pair of paths, in both orders, then all of them
    out = []
    for i in range(1, 7):
        for j in range(1, 7):
            out.append(f([i, j] * n))
    out.append(f(list(range(7)) * n))
    return out


CASES = [
    ('funcs', [([],), ([1, 2, 3, 4, 5],), ([5, 5, 1, 3, 3, 2, 4],)], {}),
    ('classes', [([],), ([1, 2, 3, 4],), ([4, 4, 2, 1, 3, 3],)], {}),
    ('default_const', [([],), ([1, 2, 3, 4, 5],), ([5, 4, 3, 2, 1, 1],)], {}),
    ('mixed', [([],), ([1, 2, 3, 4],), ([3, 2, 1, 4, 4, 1],)], {}),
    ('orders', '[(funcs, 2)]', {}),
    ('orders', '[(classes, 2)]', {}),
    ('orders', '[(default_const, 2)]', {}),
    ('orders', '[(mixed, 3)]', {}),
]
