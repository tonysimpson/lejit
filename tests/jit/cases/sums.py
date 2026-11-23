# sum() of a generator expression and of a tuple, as CPython adds: plain +
# for ints, and where CPython compensates its float and complex sums,
# compensated, so 1e100 + 1.0 - 1e100 keeps the 1.0.
def of_genexpr(xs):
    return sum(x for x in xs)


def of_tuple(a, b, c):
    return sum((a, b, c))


def with_start(xs, start):
    return sum((x * 2 for x in xs), start)


def mixed(n):
    return [sum(x for x in (1, 2.5, 3, 1e100, -1e100, n)),
            sum(x for x in (n, 2, 3)), sum((n, 0.1, 0.2, 0.3))]


CASES = [
    ('of_genexpr', '[([1e100, 1.0, -1e100],), ([0.1] * 10,), ([1, 2, 3],), '
                   '([1e100j, 1j, -1e100j],), ([1, 2.5, 1j],)]', {}),
    ('of_tuple', '[(1e100, 1.0, -1e100), (0.1, 0.2, 0.3), (1, 2, 3), '
                 '(1e100j, 1j, -1e100j)]', {}),
    ('with_start', '[([1e100, 1.0, -1e100], 0.0), ([1, 2], 10), '
                   '([0.1, 0.2], 1e16)]', {}),
    ('mixed', [(1,), (7,)], {}),
]
