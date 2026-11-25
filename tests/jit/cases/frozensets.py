# frozenset() of a set display or a set comprehension, which CPython main
# builds as a set and freezes in place (INTRINSIC_BUILD_FROZENSET) when the
# name is still the builtin, and calls frozenset otherwise. Results are
# sorted, and whether the collector tracks the frozenset is compared too.
import gc


class Unhashable:
    __hash__ = None


def display(a, b, c):
    try:
        s = frozenset({a, b, c})
    except TypeError as e:
        return repr(e)
    return type(s).__name__, sorted(s, key=repr), gc.is_tracked(s)


def comprehension(xs):
    s = frozenset({x * 2 for x in xs})
    return type(s).__name__, sorted(s), gc.is_tracked(s)


def of_genexpr(xs):
    s = frozenset(x + 1 for x in xs)
    return type(s).__name__, sorted(s), gc.is_tracked(s)


def holding(n):
    # frozensets of tuples, which may hold objects the collector tracks
    s = frozenset({(n, n), (n, str(n))})
    return sorted(s, key=repr), gc.is_tracked(s), hash(s) == hash(frozenset(s))


# with frozenset rebound in the function's globals, the display is passed
# to whatever the name now holds
_REBOUND = {'frozenset': lambda s: ('rebound', sorted(s))}
exec('def rebound(a, b):\n    return frozenset({a, b})\n', _REBOUND)
rebound = _REBOUND['rebound']


CASES = [
    ('display', '[(1, 2, 3), (1, 1, 1), ("a", "b", "a"), (None, (1, 2), 3.5),'
                ' (1, Unhashable(), 2), ([], 1, 2)]', {}),
    ('comprehension', [([1, 2, 3],), ([],), (range(10),), ((5, 5, 5),)], {}),
    ('of_genexpr', [([1, 2, 3],), ([],), (range(10),)], {}),
    ('holding', [(1,), (2,)], {}),
    ('rebound', [(1, 2), (3, 3)], {}),
]
