# Doubles from the general translator: float slots, PyFloat_AsDouble,
# int's nb_float and the math functions generated from mathmodule.c. Signs
# of zeros, NaN, infinities, division by zero, overflow, domain errors,
# ints and types with __float__ must be CPython's.
import math

INF = float('inf')
NAN = float('nan')


class I(int):
    def __float__(self):
        return 2.0


class F(float):
    pass


class HasFloat:
    def __float__(self):
        return 0.25


class BadFloat:
    def __float__(self):
        return 'x'


class SubFloat:
    def __float__(self):
        return F(0.5)


def show(x):
    """repr, keeping the sign of a zero and the type"""
    return (type(x).__name__, repr(x), math.copysign(1.0, x) if isinstance(x, float) else None)


def arith(a, b):
    out = []
    for f in (lambda: a + b, lambda: a - b, lambda: a * b, lambda: a / b,
              lambda: a % b, lambda: -a, lambda: +a, lambda: abs(a), lambda: bool(a)):
        try:
            out.append(show(f()))
        except (ZeroDivisionError, OverflowError) as e:
            out.append(type(e).__name__ + ': ' + str(e))
    return out


def mixed(a, n):
    return show(a + n), show(n * a), show(a / n), show(n - a)


def math1(x):
    out = []
    for f in (math.sqrt, math.exp, math.sin, math.cos, math.tan, math.atan,
              math.asin, math.acos, math.fabs, math.tanh, math.sinh, math.cosh):
        try:
            out.append(show(f(x)))
        except (ValueError, OverflowError, TypeError) as e:
            out.append(type(e).__name__ + ': ' + str(e))
    return out


def as_float(x):
    try:
        return show(float(x)), show(math.sqrt(x))
    except (TypeError, ValueError) as e:
        return type(e).__name__ + ': ' + str(e)


def int_float(n):
    return show(float(n)), show(n.__float__())


def loop(n):
    t = 0.0
    x = 0.5
    for i in range(n):
        t = t + x * 1.5 - x / 3.0 + math.sqrt(t + 1.0)
        x = -x % 7.0
    return t


EDGES = "[(a, b) for a in (0.0, -0.0, 1.5, -2.25, 1e308, INF, -INF, NAN) for b in (0.0, -0.0, 3.0, -1e-308, INF, NAN)]"

def hashes(x):
    # float_hash generated (tp_hash): _Py_HashDouble of the double, called
    # directly; a dict lookup by a float key hashes it too
    return hash(x), hash(-x), {x: 1}.get(x), hash(x * 2.0)


CASES = [
    ('arith', EDGES, {}),
    ('arith', "[(F(2.5), 2.0), (2.0, F(0.5)), (True, 0.5)]", {}),
    ('mixed', "[(1.5, 3), (-0.0, 0), (2.5, 2 ** 70), (1e300, 10 ** 400), (0.5, True), (2.0, I(3))]", {}),
    ('math1', "[(0.5,), (-0.0,), (2,), (-1,), (710.0,), (INF,), (-INF,), (NAN,), (True,), (2 ** 2000,)]", {}),
    ('as_float', "[(I(1),), (HasFloat(),), (BadFloat(),), (SubFloat(),), ('s',), (F(4.0),), (7,)]", {}),
    ('int_float', "[(3,), (-0,), (2 ** 60 + 1,), (10 ** 400,), (True,), (I(5),)]", {}),
    ('loop', "[(200,)]", {}),
    ('hashes', "[(0.0,), (-0.0,), (1.5,), (2.0 ** 70,), (INF,), (NAN,), (F(3.25),)]", {}),
]
