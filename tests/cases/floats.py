# Float slots generated from Objects/floatobject.c (pfloatobject_gen.h):
# + - * / % unary - abs bool. Results, signs of zeros, NaN and infinities,
# errors and mixed int operands must be CPython's.

INF = float('inf')
NAN = float('nan')
EDGES = [0.0, -0.0, 1.0, -1.0, 2.5, -7.25, 1e300, -1e-300, INF, -INF, NAN,
         3, -2, 0, 2 ** 70]


def table(op):
    out = []
    for a in EDGES:
        for b in EDGES:
            try:
                out.append(repr(op(a, b)))
            except (ZeroDivisionError, OverflowError) as e:
                out.append(type(e).__name__ + ': ' + str(e))
    return out


def op_arith(a, b):
    return (a + b, a - b, a * b)


def op_div(a, b):
    return a / b


def op_rem(a, b):
    return a % b


def arith_edges():
    return table(op_arith)


def div_edges():
    return table(op_div)


def rem_edges():
    return table(op_rem)


def unary_edges():
    out = []
    for a in EDGES:
        if isinstance(a, float):
            out.append((repr(-a), repr(abs(a)), bool(a), not a))
    return out


def bool_in_jumps(n):
    # nb_bool on floats, used by if/while/and/or
    out = []
    x = 2.0
    while x:
        out.append(x if x else 'zero')
        x = x - 0.5
        if len(out) > n:
            break
    return out, (0.0 and 1), (0.5 or 2), (-0.0 or 'neg zero')


def float_loop(n):
    # values stay virtual through % - abs
    t = 0.0
    i = 0.5
    while i < n:
        t = t + abs(-(i % 3.0)) / 2.0
        i = i + 1.0
    return t


class MyFloat(float):
    pass


def subclasses():
    a = MyFloat(2.5)
    return (a + 1.0, type(a + 1.0), -a, type(-a), abs(-a), a % 2.0, bool(a))


def unary_plus_identity(x):
    # float_float returns an exact float itself (F22); a subclass gets a
    # new exact float
    y = +x
    m = MyFloat(x)
    w = x * 2.0              # a float computed here
    z = +w
    return y is x, y == x, z is w, type(+m).__name__, +m is m


CASES = [
    ('arith_edges', [()], {}),
    ('div_edges', [()], {}),
    ('rem_edges', [()], {}),
    ('unary_edges', [()], {}),
    ('bool_in_jumps', [(10,)], {}),
    ('float_loop', [(20.0,)], {}),
    ('subclasses', [()], {}),
    ('unary_plus_identity', [(2.5,), (-0.0,)], {}),
]
