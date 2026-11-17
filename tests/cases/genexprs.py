# Generator expressions consumed by sum, any, all, list, tuple and set run
# fused with the consumer (psyfunc.c:psyco_call_genexpr): no generator
# object. Only a temporary is fused; a generator something can still use
# (a live local, after a partial run or an exception) must stay one object.


def consumers(xs, k):
    return (sum(x * k for x in xs), list(x + k for x in xs if x & 1),
            tuple(x for x in xs), sorted(set(x % 3 for x in xs)),
            any(x > k for x in xs), all(x < 100 for x in xs),
            sum((x * 0.5 for x in xs), 1.5))


def nested_loops(rows, k):
    return sum(x * k for r in rows for x in r), any(x > k for r in rows for x in r)


def big_ints(xs):
    return sum(x for x in xs), sum((x for x in xs), 2 ** 64)


def sum_start_str(xs):
    try:
        return sum((x for x in xs), '')
    except TypeError as e:
        return str(e)


def consumed_twice(xs):
    g = (x * 2 for x in xs)
    return sum(g), sum(g), list(g)


def partial_then_rest(xs):
    g = (x for x in xs)
    return any(g), list(g)


def nested_partial(rows):
    g = (x for r in rows for x in r)
    return any(g), list(g)          # the inner loop's position is kept


def escapes(xs):
    g = (x + 1 for x in xs)
    first = next(g)
    return first, sum(g), type(g).__name__


def passed_on(xs):
    return helper(x * 3 for x in xs)


def helper(g):
    return list(g)


def shared_iterator(xs):
    it = iter(xs)
    a = any(x == 3 for x in it)
    return a, list(it)


def raises_midway(xs):
    g = (10 // x for x in xs)
    try:
        sum(g)
    except ZeroDivisionError as e:
        r = str(e)
    return r, list(g)


def raises_in_temporary(xs):
    try:
        return sum(10 // x for x in xs)
    except ZeroDivisionError as e:
        return str(e)


def stop_iteration_inside(xs):
    # PEP 479: StopIteration leaving a generator is a RuntimeError
    try:
        return any(next(iter(())) for x in xs)
    except RuntimeError as e:
        return str(e), type(e.__cause__).__name__


def shadowed_builtin(xs):
    sum = len
    return sum(x for x in xs) if False else sum([x for x in xs])


CASES = [
    ('consumers', [(list(range(10)), 4), ((), 2), ([3, 2], 1)], {}),
    ('nested_loops', [([[1, 2], [3, 4]], 2), ([[]], 0)], {}),
    ('big_ints', [([2 ** 70, 5],), ([1, -1],)], {}),
    ('sum_start_str', [(['a'],)], {}),
    ('consumed_twice', [([1, 2, 3],)], {}),
    ('partial_then_rest', [([0, 4, 5],), ([0, 0],)], {}),
    ('nested_partial', [([[0, 2, 7], [3, 4]],)], {}),
    ('escapes', [([1, 2, 3],)], {}),
    ('passed_on', [([1, 2],)], {}),
    ('shared_iterator', [([1, 3, 4, 5],), ([1],)], {}),
    ('raises_midway', [([5, 0, 2],)], {}),
    ('raises_in_temporary', [([5, 0, 2],), ([1, 2],)], {}),
    ('stop_iteration_inside', [([1],), ([],)], {}),
    ('shadowed_builtin', [([1, 2],)], {}),
]
