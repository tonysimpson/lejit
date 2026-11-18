# Virtual ranges and range iterators (ni/Objects/prangeobject.c):
# every argument shape, empty and negative ranges, steps, the one-digit
# boundary where the real call takes over, escapes of the range and of the
# iterator, and mutation of the loop variable.

M = 2 ** 30 - 1


def loops(a, b, c):
    out = []
    for i in range(a):
        out.append(i)
    for i in range(a, b):
        out.append(i)
    for i in range(a, b, c):
        out.append(i)
    for i in range(b, a, -c):
        out.append(i)
    return out


def nested(n):
    t = 0
    for i in range(n):
        for j in range(i, n, 2):
            t = t + i * j
    return t


def edges():
    out = []
    for args in ((0,), (-3,), (5, 5), (5, 2), (2, 5, 7), (M - 2, M + 2),
                 (-M - 2, -M + 1), (2 ** 40, 2 ** 40 + 3), (0, 10, M),
                 (10, 0, -M)):
        out.append(collect(*args))
    return out


def collect(*args):
    return [i for i in range(*args)]


def collect3(a, b, c):
    t = []
    for i in range(a, b, c):
        t.append(i)
    return t


def zero_step():
    try:
        return collect3(1, 5, 0)
    except ValueError as e:
        return str(e)


def not_ints():
    out = []
    for args in ((3.0,), ('3',), (True, 4)):
        try:
            out.append(collect3(*(args + (5, 1))[:3]) if len(args) == 2
                       else [i for i in range(*args)])
        except TypeError as e:
            out.append(str(e))
    return out


def escapes(n):
    r = range(1, n, 2)          # the range escapes
    it = iter(range(n))         # an iterator escapes, half used
    first = []
    for x in it:
        first.append(x)
        if x == n // 2:
            break
    rest = list(it)
    return list(r), len(r), r[1], first, rest, type(r).__name__


def iterator_after_loop(n):
    it = iter(range(n))
    t = 0
    for x in it:
        t = t + x
    return t, list(it), next(it, 'done')


def loop_var_rebound(n):
    t = 0
    for i in range(n):
        i = i * 10              # does not affect the iteration
        t = t + i
    return t


def big_counts(n):
    t = 0
    for i in range(n, n + 50):
        t = t + i
    return t


CASES = [
    ('loops', [(5, 9, 2), (0, 3, 1), (-4, 4, 3)], {}),
    ('nested', [(30,)], {}),
    ('edges', [()], {}),
    ('zero_step', [()], {}),
    ('not_ints', [()], {}),
    ('escapes', [(9,)], {}),
    ('iterator_after_loop', [(6,)], {}),
    ('loop_var_rebound', [(7,)], {}),
    ('big_counts', [(M - 20,), (2 ** 62,), (-2 ** 63,)], {}),
]
