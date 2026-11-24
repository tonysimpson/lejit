# Errors that name a type the way CPython names it: from 3.15 a type outside
# builtins and __main__ by its module too.
import fractions
import math


class Plain:
    pass


class FloatGivesStr:
    def __float__(self):
        return 'no'


class FloatGivesSubclass:
    def __float__(self):
        return Subfloat(1.5)


class Subfloat(float):
    pass


def dict_star(x):
    try:
        return {**x}
    except TypeError as e:
        return str(e)


def sine(x):
    try:
        return math.sin(x)
    except TypeError as e:
        return str(e)


CASES = [
    ('dict_star', '[(1,), (Plain(),), (fractions.Fraction(1),), ({"a": 1},)]', {}),
    ('sine', '[(FloatGivesStr(),), (FloatGivesSubclass(),), (0.5,)]', {}),
]
