# Calls of functions with keyword-only parameters, bound by the binder:
# given by name, or taken from __kwdefaults__ as the call was compiled
# with them (pfuncobject.c:compiled_kwdefaults), which can change in place
# or be replaced under the call site; and the calls left to the real one,
# whose TypeError is CPython's own.


def ko(x, *, k=1, j=2):
    return x, k, j


def ko_required(x, *, k):
    return x, k


def ko_star(*args, k=0):
    return args, k


class Obj:
    def m(self, x, *, scale=3):
        return x * scale


def given(n):
    o = Obj()
    return [(ko(i), ko(i, k=5), ko(i, j=6, k=7), ko(x=i, j=0),
             ko_required(i, k=i), ko_star(i, i, k=2), ko_star(), o.m(i),
             o.m(i, scale=1)) for i in range(n)]


def changed(n):
    out = []
    saved = ko.__kwdefaults__
    try:
        for i in range(n):
            out.append(ko(i))
            ko.__kwdefaults__['k'] = i * 10          # changed in place
            out.append(ko(i))
            ko.__kwdefaults__ = {'k': -1, 'j': -2}  # replaced
            out.append(ko(i))
            del ko.__kwdefaults__['j']              # one missing now
            try:
                out.append(ko(i))
            except TypeError as e:
                out.append(str(e))
            ko.__kwdefaults__ = None
            try:
                out.append(ko(i))
            except TypeError as e:
                out.append(str(e))
            out.append(ko(i, k=1, j=1))
            ko.__kwdefaults__ = dict(saved)
    finally:
        ko.__kwdefaults__ = saved
    return out


def errors(n):
    out = []
    for i in range(n):
        for f in (lambda: ko_required(i), lambda: ko(i, i),
                  lambda: ko(i, q=1), lambda: Obj().m(i, 2)):
            try:
                out.append(f())
            except TypeError as e:
                out.append(str(e))
    return out


class Key(str):
    pass


def odd_keys(n):
    # a key that isn't an exact str: the defaults aren't kept known
    def f(*, a=1):
        return a
    f.__kwdefaults__ = {Key('a'): 5}
    return [f() for i in range(n)]


def kept_identity(n):
    sentinel = object()

    def f(*, d=sentinel):
        return d
    return [f() is sentinel for i in range(n)]


def nested(n):
    out = []
    for i in range(n):
        def inner(a, *, b=i):
            return a + b
        out.append((inner(1), inner(1, b=0)))
    return out


def folded(n):
    # a default read at compile time folds the branch on it
    def f(x, *, flag=False):
        if flag:
            return -x
        return x
    return [f(i) for i in range(n)] + [f(i, flag=True) for i in range(n)]


CASES = [
    ('given', [(0,), (3,)], {}),
    ('changed', [(3,)], {}),
    ('errors', [(2,)], {}),
    ('odd_keys', [(3,)], {}),
    ('kept_identity', [(3,)], {}),
    ('nested', [(3,)], {}),
    ('folded', [(3,)], {}),
]
