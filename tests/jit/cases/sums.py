# sum() of a generator expression and of a tuple, as CPython adds: where
# CPython compensates its float (and complex) sums, the results are its
# compensated ones, bit for bit, through each of its loops: ints while they
# fit a C long, floats, complex numbers where it compensates them, and
# plain + once it has left those. Results are reprs, so -0.0 and nan count.
class IntSub(int):
    pass


class FloatSub(float):
    pass


class Radd:
    def __init__(self, v):
        self.v = v

    def __radd__(self, other):
        return other + self.v


def of_genexpr(xs):
    try:
        return repr(sum(x for x in xs))
    except (TypeError, OverflowError) as e:
        return repr(e)


def of_tuple(a, b, c):
    try:
        return repr(sum((a, b, c)))
    except (TypeError, OverflowError) as e:
        return repr(e)


def with_start(xs, start):
    try:
        return repr(sum((x for x in xs), start))
    except (TypeError, OverflowError) as e:
        return repr(e)


def tuple_with_start(a, b, c, start):
    try:
        return repr(sum((a, b, c), start))
    except (TypeError, OverflowError) as e:
        return repr(e)


def mixed(n):
    return [sum(x for x in (1, 2.5, 3, 1e100, -1e100, n)),
            sum(x for x in (n, 2, 3)), sum((n, 0.1, 0.2, 0.3))]


def squares(n):
    return sum(i * i for i in range(n)) + sum(x for x in range(n) if x & 1)


def word_totals(n):
    """totals past one digit and at a C long's limits, either way, kept as a
    C long by CPython's int loop until one overflows it"""
    big = 2 ** 62
    return [sum(i * 40000 for i in range(n)),
            sum(x for x in (big, big, -big, -big, n)),
            sum(x for x in (-big, -big, -big, -n)),
            sum(x for x in (big, big - 1, n, -n)),
            sum((x for x in range(n)), 2 ** 63 - 2),
            sum((big, big - 1 + n, 1)),
            sum((-big, -big - n, 1)),
            sum(b for b in (True, False, 2 ** 40) * n),
            sum((x for x in (n, 1.5, 2 ** 70)), 2 ** 31)]


SEQS = '''[
    [1e100, 1.0, -1e100], [0.1] * 10, [1, 2, 3], [], [True, 2, False],
    [1e100j, 1j, -1e100j], [1, 2.5, 1j], [1j, 1e100, 1.0, -1e100],
    [2 ** 62, 2 ** 62, 1e100, 1.0, -1e100], [1, 2 ** 70, -2 ** 70, 1e100, 1.0, -1e100],
    [1e100, 1, -1e100], [1.5, 2 ** 70, 1e100, 1.0, -1e100], [0.5, True, 1e100, 1, -1e100],
    [-0.0], [-0.0, -0.0], [0.0, -0.0], [1e308, 1e308, -1e308],
    [float("inf"), 1.0], [float("inf"), float("-inf")], [float("nan"), 1.0],
    [0.1, 2 ** 1100], [2 ** 1100, 0.5], [IntSub(3), 1.5, 1e100, 1.0, -1e100],
    [FloatSub(1.5), 1e100, 1.0, -1e100], [1e100, FloatSub(1.0), -1e100],
    [1.5, Radd(2.0), 1e100, 1.0, -1e100], [1, Radd(0.5), 1e100, 1.0, -1e100],
    [1, "a"], [1.5, "a"], [2 ** 63 - 1, 1], [-2 ** 63, -1, 0.5],
    [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9, 1.0],
]'''


CASES = [
    ('of_genexpr', '[(xs,) for xs in %s]' % SEQS, {}),
    ('of_tuple', '[tuple((xs + [0, 0, 0])[:3]) for xs in %s]' % SEQS, {}),
    ('with_start', '[(xs, s) for xs in %s for s in '
                   '(0.0, -0.0, 10, 2 ** 70, True, 1j, 1e100, "")]' % SEQS, {}),
    ('tuple_with_start', '[tuple((xs + [0, 0, 0])[:3]) + (s,) for xs in %s '
                         'for s in (0.0, -0.0, 10, 2 ** 70, 1j, 1e100)]' % SEQS, {}),
    ('mixed', [(1,), (7,)], {}),
    ('squares', [(30,), (0,), (100000,)], {}),
    ('word_totals', [(0,), (1,), (2,), (60000,)], {}),
]
