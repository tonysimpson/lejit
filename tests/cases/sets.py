# Set displays and set comprehensions (BUILD_SET, SET_ADD), as in ceval:
# items added deepest first, so the first of equal items is kept.


def displays(a, b):
    return sorted({a, b, a + 1}), {1, True, 1.0}, type({1, 2.0}).__name__, set()


def first_kept(x):
    s = {x, 1.0, 1}
    return [type(v).__name__ for v in s]


def comprehension(xs, k):
    return sorted({x % k for x in xs}), sorted({x for x in xs if x > k})


def unhashable(x):
    try:
        return {1, x}
    except TypeError as e:
        return str(e)


def in_loop(n):
    t = 0
    for i in range(n):
        t += len({i % 3, i % 5, 7})
    return t


CASES = [
    ('displays', [(1, 5), (2.5, -1)], {}),
    ('first_kept', [(1,), (True,)], {}),
    ('comprehension', [(list(range(20)), 4), ((), 1)], {}),
    ('unhashable', [([],), (2,)], {}),
    ('in_loop', [(50,)], {}),
]
