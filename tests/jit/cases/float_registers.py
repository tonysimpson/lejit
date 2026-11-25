# Floats kept in XMM registers: + - * / and int-to-float done inline, bit
# for bit as plain CPython gives them (signed zeros, infinities, NaN
# payloads and signs, overflow, denormals, rounding of big ints), the
# division-by-zero errors, and float values still right after every kind
# of call that can come between two operations: C calls, a dealloc, an
# attribute store's slow path, a promotion, a generator's yield, a handler
# run by the eval breaker, frames read from C, more live floats than there
# are registers.
import gc, math, random, signal, struct, sys

NAN1 = struct.unpack('<d', struct.pack('<Q', 0x7FF8000000000001))[0]
NEG_NAN = struct.unpack('<d', struct.pack('<Q', 0xFFF8000000000002))[0]
VALUES = [0.0, -0.0, 1.0, -1.5, 0.1, 3.0, math.inf, -math.inf, NAN1, NEG_NAN,
          1e308, -1e308, 5e-324, 2.2250738585072014e-308, 0, 7, -3,
          2 ** 53 + 1, -(2 ** 62), 2 ** 63 - 1]


def bits(x):
    if isinstance(x, float):
        return hex(struct.unpack('<Q', struct.pack('<d', x))[0])
    return repr(x)


def shown(f, a, b):
    try:
        return bits(f(a, b))
    except ArithmeticError as e:
        return type(e).__name__ + ': ' + str(e)


def add(a, b):
    return a + b


def sub(a, b):
    return a - b


def mul(a, b):
    return a * b


def div(a, b):
    return a / b


def rem(a, b):
    return a % b


def lt(a, b):
    return a < b


def le(a, b):
    return a <= b


def gt(a, b):
    return a > b


def ge(a, b):
    return a >= b


def eq(a, b):
    return a == b


def ne(a, b):
    return a != b


def branches(a, b):
    # each compare as a branch, as well as a value
    n = 0
    if a < b:
        n += 1
    if a <= b:
        n += 2
    if a > b:
        n += 4
    if a >= b:
        n += 8
    if a == b:
        n += 16
    if a != b:
        n += 32
    if a:
        n += 64
    if not b:
        n += 128
    return n


def chain(a, b):
    x = a * 0.5 + b
    y = (x - a) * (b + 2.0)
    return y / 3.0 - x * a


def table(n):
    out = []
    for f in (add, sub, mul, div, rem, chain, lt, le, gt, ge, eq, ne,
              branches):
        for a in VALUES:
            for b in VALUES:
                if isinstance(a, float) or isinstance(b, float):
                    if not (a != a and b != b):   # one NaN at a time
                        out.append(shown(f, a, b))
    return out


def unary(n):
    out = []
    for a in VALUES:
        if isinstance(a, float):
            x = a * n               # a run-time float
            out.append((bits(-x), bits(abs(x)), bool(x), not x,
                        bits(-(x + 1.0)), bits(abs(x - 2.0))))
    return out


def known_ints(n):
    # a float against an int literal, small and big
    out = []
    for a in VALUES:
        if isinstance(a, float):
            x = a * n
            out.append((x < 0, x <= 0, x > 0, x >= 0, x == 0, x != 0,
                        x < 3, x == -1, x > -7, x >= 2 ** 47,
                        x < 2 ** 53 + 1, x == 2 ** 60, x != -(2 ** 49)))
    return out


def from_ints(n):
    out = []
    for i in (0, 1, -1, 2 ** 53 + 1, 2 ** 53 + 3, -(2 ** 62) - 1, 2 ** 63 - 1,
              -(2 ** 63), n):
        out.append(bits(i * 1.0))
        out.append(bits(0.5 + i))
        out.append(bits(i / 4.0))
    return out


class Holder:
    pass


def g():
    pass


def across_calls(n):
    x = n * 1.25
    y = x + 0.5
    s = str(x)                      # a C call with x and y live
    z = x * y
    h = Holder()
    h.a = [1, 2]
    w = z - x
    h.a = None                      # the old list is freed: a dealloc
    v = w * 2.0 + y
    h.b = 3                         # a new attribute: the store's slow path
    u = v / 7.0 + w
    g()                             # a compiled call
    t = u * u
    k = len(s) + t
    return [bits(q) for q in (x, y, z, w, v, u, t, k)] + [s]


def dealloc_few_live(n):
    out = []
    h = Holder()
    for i in range(3):
        x = n * 0.5 + i
        y = x * 3.0
        h.a = [i]                   # frees the last list: x and y live
        out.append(bits(x + y))
        z = y - x
        h.a = None
        out.append(bits(z / 2.0))
    return out


def respawned(n):
    # triangular's branches are compiled late, by replaying the compilation
    # from an earlier point, which must choose the same registers
    g = random.Random(12345)
    out = []
    for k in range(20):
        for low, high, mode in [(0.0, 1.0, None), (1.0, 0.0, None),
                                (0.0, n * 1.0, 3.0), (5.0, 5.0, 5.0),
                                (-3.0, 2.0, 2.0), (0.0, 1.0, 0.0)]:
            out.append(bits(g.triangular(low, high, mode)))
    return out


class A:
    pass


class B:
    def __float__(self):
        return 2.5


def promoted(n):
    out = []
    for obj in (A(), B(), 1, 2.0, A()):
        x = n * 1.5
        y = x - 0.25
        kind = type(obj).__name__     # promotes obj's type
        if isinstance(obj, B):
            z = y + float(obj)
        elif isinstance(obj, (int, float)):
            z = y * obj
        else:
            z = y / 3.0
        out.append((kind, bits(x), bits(y), bits(z)))
    return out


def gen(n):
    x = n * 0.5
    for i in range(3):
        y = x + i
        yield y
        x = y * 1.5


def yields(n):
    return [bits(v) for v in gen(n)]


def many_live(n):
    a = n * 1.1
    b = a + 1.2
    c = b * 1.3
    d = c - 1.4
    e = d / 1.5
    f = e + 1.6
    g = f * 1.7
    h = g - 1.8
    i = h / 1.9
    j = i + 2.0
    k = len(str(j))
    return [bits(x) for x in (a, b, c, d, e, f, g, h, i, j, k + a)]


def seen_from_c(n):
    x = n * 0.75
    y = x + 1.0
    loc = sys._getframe().f_locals
    z = x / y
    return [bits(loc['x']), bits(loc['y']), bits(z)]


def traceback_mid_chain(n):
    def inner(a, b):
        x = a * 2.0
        y = x + b
        return y / (x - x)
    try:
        inner(n * 0.5, 1.5)
    except ZeroDivisionError as e:
        tb = e.__traceback__
        while tb.tb_next is not None:
            tb = tb.tb_next
        return sorted((k, bits(v)) for k, v in tb.tb_frame.f_locals.items())


SEEN = []


def handler(signum, frame):
    SEEN.append(signum)
    gc.collect()


def signal_between(n):
    old = signal.signal(signal.SIGUSR1, handler)
    try:
        x = n * 0.5
        total = 0.0
        for i in range(200):
            y = x * i + 0.25
            if i == 100:
                signal.raise_signal(signal.SIGUSR1)
            total = total + y / 2.0
        return bits(total), len(SEEN) > 0
    finally:
        signal.signal(signal.SIGUSR1, old)


CASES = [
    ('table', [(0,)], {}),
    ('unary', [(1,), (-1,)], {}),
    ('known_ints', [(1,), (-1,)], {}),
    ('from_ints', [(5,), (-(2 ** 61),)], {}),
    ('across_calls', [(3,), (-2,)], {}),
    ('dealloc_few_live', [(3,), (-1.5,)], {}),
    ('respawned', [(3,)], {}),
    ('promoted', [(3,)], {}),
    ('yields', [(1,), (2.5,)], {}),
    ('many_live', [(3,), (0.5,)], {}),
    ('seen_from_c', [(3,)], {}),
    ('traceback_mid_chain', [(3,)], {}),
    ('signal_between', [(3,)], {}),
]
