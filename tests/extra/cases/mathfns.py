# The math module metas (pmath.c): results, the int result of
# floor/ceil, and the errors of 3.7's math_1.
import math
from fractions import Fraction


def floors(x):
    return math.floor(x), math.ceil(x)


def floor_types(x):
    return type(math.floor(x)).__name__, type(math.ceil(x)).__name__


def unary(x):
    return (math.sqrt(x), math.exp(x), math.sin(x), math.cos(x), math.tan(x),
            math.atan(x), math.fabs(x), math.tanh(x))


def domain(x):
    return math.sqrt(x)


def acos_of(x):
    return math.acos(x)


def overflow(x):
    return math.exp(x)


def cosh_of(x):
    return math.cosh(x)


def two_args(x, y):
    return math.atan2(x, y), math.hypot(x, y), math.pow(x, y), math.fmod(x, y)


def pow_error(x, y):
    return math.pow(x, y)


def fmod_error(x, y):
    return math.fmod(x, y)


def float_loop(n):
    t = 0.0
    for i in range(1, n):
        t += math.sqrt(i) + math.floor(i / 3.0)
    return t


CASES = [
    ('floors', "[(2.5,), (-2.5,), (3.0,), (7,), (10**20,), (Fraction(7, 2),)]", {}),
    ('floors', "[(float('inf'),), (float('nan'),)]", {}),
    ('floor_types', "[(1.5,), (2,)]", {}),
    ('unary', "[(0.5,), (2,), (0.0,)]", {}),
    ('domain', "[(-1.0,), (-1,), (4,), ('x',)]", {}),
    ('acos_of', "[(2.0,), (1.0,)]", {}),
    ('overflow', "[(1000.0,), (1.0,), (-1000.0,)]", {}),
    ('cosh_of', "[(1000.0,)]", {}),
    ('two_args', "[(1.0, 2.0), (3.0, 4.0)]", {}),
    ('pow_error', "[(0.0, -1.0), (-1.0, 0.5), (10.0, 400.0)]", {}),
    ('fmod_error', "[(1.0, 0.0), (float('inf'), 1.0)]", {}),
    ('float_loop', "[(100,)]", {}),
]
