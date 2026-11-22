# Keyword-only arguments, compiled at the frame entry (CPython fills them
# in after the positional ones); with *args and **kwargs around them, as
# cells, defaults, and calls from compiled code (real calls).


def f(a, *, k, m=10):
    t = 0
    for i in range(a):
        t += i * k + m
    return t


def g(a, *rest, k=2, **kw):
    return a, rest, k, sorted(kw.items())


def cell(a, *, k):
    return [x * k for x in range(a)], (lambda: k)()


def caller(n):
    return f(n, k=3), f(n, m=1, k=2), g(1, 2, 3, k=5, z=6), g(4), cell(3, k=7)


def missing():
    try:
        return f(3)
    except TypeError as e:
        return str(e)


CASES = [
    ('caller', [(7,)], {}),
    ('caller', [(4,), (0,)], {}),
    ('missing', [()], {}),
]
