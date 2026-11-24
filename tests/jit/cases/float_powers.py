# float ** as CPython's float_pow gives it: its special cases (0, 1, inf, nan,
# signed zeros), negative bases to fractional powers (complex), and its
# errors (zero to a negative power, overflow), with ints on either side.
import math

VALUES = [0.0, -0.0, 1.0, -1.0, 2.0, -2.0, 0.5, -0.5, 3, -3, 0, 10.0, 1e308,
          -1e308, 1e-308, math.inf, -math.inf, math.nan, 2 ** 60]


def shown(f, a, b):
    try:
        r = f(a, b)
    except (ArithmeticError, TypeError, ValueError) as e:
        return type(e).__name__
    if isinstance(r, float):
        return repr(r) + ('-' if math.copysign(1.0, r) < 0 else '+')
    return repr(r)


def op(a, b):
    return a ** b


def builtin(a, b):
    return pow(a, b)


def inplace(a, b):
    a **= b
    return a


def three(a, b):
    return pow(a, b, 5)


def table(fn):
    return [shown(fn, a, b) for a in VALUES for b in VALUES
            if not (isinstance(a, int) and isinstance(b, int))]


def powers(n):
    return [table(op), table(builtin), table(inplace), shown(three, 2.0, n),
            [shown(op, x * 0.25, 0.5) for x in range(-4, 5)]]


CASES = [
    ('powers', [(3,), (3.0,)], {}),
]
