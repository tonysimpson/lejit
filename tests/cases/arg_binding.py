# Argument binding of compiled calls, generated from ceval.c's
# _PyEval_EvalCodeWithName (psyfunc_gen.h): positional arguments,
# defaults (some, all, none used), *args (empty, a few, more than a virtual
# tuple holds), keyword-only parameters and their defaults, and calls the
# binder leaves to the real call, whose TypeError must be CPython's own
# message (too many, missing one, two or three, with a range of counts).
# Also bound methods, closures, and builtins of each ml_flags kind
# (pmethodobject.c's dispatch), with their wrong-count errors.


def d2(a, b=2, c=3):
    return a, b, c


def star(a, *rest):
    return a, rest


def star_d(a, b=7, *rest):
    return a, b, rest


def all_d(a=1, b=2):
    return a * 10 + b


def defaults_and_star(n):
    out = []
    for i in range(n):
        out.append((d2(i), d2(i, i), d2(i, i, i), all_d(), all_d(i),
                    star(i), star(i, 1), star(i, 1, 2, 3),
                    star_d(i), star_d(i, 0), star_d(i, 0, 1, 2)))
    return out


def many_extras(n):
    # 20 extra positional arguments: more than a virtual tuple holds
    t = 0
    for i in range(n):
        a, rest = star(i, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
                       16, 17, 18, 19, 20)
        t += a + sum(rest)
    return t


def kwo(a, *, k=5, m):
    return a, k, m


def kwo_d(a, *, k=5):
    return a + k


def kwonly_defaults(n):
    out = []
    for i in range(n):
        out.append((kwo_d(i), kwo_d(i, k=1), kwo(i, m=2), kwo(i, k=0, m=1)))
    return out


def catch(f, *args):
    try:
        return f(*args)
    except TypeError as e:
        return str(e)


def one(a):
    return a


def two(a, b):
    return a + b


def three(a, b, c):
    return a + b + c


def wrong_counts(n):
    out = []
    for i in range(n):
        r = []
        try:
            r.append(one())
        except TypeError as e:
            r.append(str(e))
        try:
            r.append(one(i, i))
        except TypeError as e:
            r.append(str(e))
        try:
            r.append(two(i))
        except TypeError as e:
            r.append(str(e))
        try:
            r.append(three())
        except TypeError as e:
            r.append(str(e))
        try:
            r.append(d2())
        except TypeError as e:
            r.append(str(e))
        try:
            r.append(d2(1, 2, 3, 4))
        except TypeError as e:
            r.append(str(e))
        try:
            r.append(kwo(i))
        except TypeError as e:
            r.append(str(e))
        try:
            r.append(kwo(i, i, m=1))
        except TypeError as e:
            r.append(str(e))
        out.append(r)
    return out


class C:
    def __init__(self, v):
        self.v = v

    def m(self, a, b=1, *rest):
        return self.v + a * b + len(rest)

    def k(self, a, *, s=3):
        return self.v * s + a


def methods(n):
    c = C(n)
    t = 0
    for i in range(n):
        t += c.m(i) + c.m(i, 2) + c.m(i, 2, 3, 4) + c.k(i) + c.k(i, s=1)
    try:
        c.m()
    except TypeError as e:
        return t, str(e)
    return t


def closures(n):
    def add(a, b=n, *rest):
        return a + b + len(rest)
    sq = (lambda x, y=2: x ** y)
    t = 0
    for i in range(n):
        t += add(i) + add(i, 1) + add(i, 1, 2) + sq(i) + sq(i, 3)
    try:
        add()
    except TypeError as e:
        return t, str(e)
    return t


def builtins_by_flags(xs):
    d = {1: 2}
    out = []
    for x in xs:
        out.append((
            xs.copy(),             # METH_NOARGS
            len(xs), abs(x),       # METH_O
            iter(xs).__class__,    # METH_VARARGS
            max(x, 3), min(xs),    # METH_VARARGS | METH_KEYWORDS
            max(xs, key=abs),
            isinstance(x, int), getattr(d, 'get')(1),  # METH_FASTCALL
            sorted(xs), round(x * 1.5, 1),   # METH_FASTCALL | METH_KEYWORDS
            sorted(xs, reverse=True), d.get(x, 0), divmod(x, 3)))
    return out


def builtin_errors(x):
    out = []
    for f, args in ((len, ()), (len, (1, 2)), ([].copy, (1,)),
                    (abs, (x, x)), (isinstance, (x,)), (divmod, (x,))):
        try:
            out.append(f(*args))
        except TypeError as e:
            out.append(str(e))
    try:
        out.append(len(x, y=1))
    except TypeError as e:
        out.append(str(e))
    try:
        out.append([].copy(1))
    except TypeError as e:
        out.append(str(e))
    try:
        out.append(abs())
    except TypeError as e:
        out.append(str(e))
    return out


CASES = [
    ('defaults_and_star', [(0,), (3,)], {}),
    ('many_extras', [(2,)], {}),
    ('kwonly_defaults', [(3,)], {}),
    ('wrong_counts', [(2,)], {}),
    ('methods', [(4,)], {}),
    ('closures', [(4,)], {}),
    ('builtins_by_flags', [([3, -1, 2],)], {}),
    ('builtin_errors', [(5,)], {}),
]
