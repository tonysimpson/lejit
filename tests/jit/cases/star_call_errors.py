# f(*x) of an x that is not iterable and f(**x) of an x that is not a
# mapping raise CPython's TypeError, with its text: from 3.15 the text names
# no function, and a type outside builtins and __main__ by its module too.
import fractions


class Plain:
    pass


def target(*args, **kwargs):
    return args, kwargs


def star(x):
    try:
        return target(*x)
    except TypeError as e:
        return str(e)


def star_star(x):
    try:
        return target(**x)
    except TypeError as e:
        return str(e)


def both(x, y):
    try:
        return target(*x, **y)
    except TypeError as e:
        return str(e)


CASES = [
    ('star', '[(1,), (Plain(),), (None,), ([1],)]', {}),
    ('star_star', '[(1,), (Plain(),), (fractions.Fraction(1),), ({"a": 1},)]', {}),
    ('both', '[(1, {}), ((), 1), ((), fractions.Fraction(1)), ((1,), {"b": 2})]', {}),
]
