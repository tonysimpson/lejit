"""Promotion points that see a new value on every call: handlers, classes, value types, argument counts and keyword sets. Variant 1 walks the tables with other strides."""
import decimal
import fractions

HANDLERS = [
    lambda x: x + 1, lambda x: x * 3 % 7, lambda x: x - 2, lambda x: x // 3,
    lambda x: x ^ 5, lambda x: x | 8, lambda x: x & 15, lambda x: x << 1,
    lambda x: x >> 1, lambda x: -x, lambda x: x * x % 101, lambda x: abs(x - 50),
    lambda x: max(x, 10), lambda x: min(x, 90), lambda x: x % 13, lambda x: x + 17,
    lambda x: (x * 7 + 3) % 1000, lambda x: x * 2 - 1, lambda x: x % 2, lambda x: 100 - x,
    lambda x: x + len(str(x)), lambda x: hash(x) % 97, lambda x: int(x ** 0.5),
    lambda x: x // 2 + x % 2, lambda x: x * 11 % 31, lambda x: x + 99, lambda x: x - 99,
    lambda x: (x + 1) * (x - 1) % 1009, lambda x: round(x / 3), lambda x: bin(x).count('1'),
    lambda x: x if x > 0 else 0, lambda x: divmod(x, 7)[1], lambda x: sum(range(x % 9)),
    lambda x: len([1] * (x % 5)), lambda x: x * 0, lambda x: x ** 2 % 113,
    lambda x: int(str(abs(x))[::-1]), lambda x: x % 64 << 2, lambda x: ~x & 255,
    lambda x: (x * 2654435761) % 4096,
]

KEYS = ['alpha', 'beta', 'gamma', 'delta', 'eps', 'zeta', 'eta', 'theta', 'iota']


class A: pass
class B(A): pass
class C: pass
class D(C): pass
class E(B, D): pass
class F: pass
class G(F): pass
class H(G): pass


CLASSES = [A, B, C, D, E, F, G, H, int, str, list, tuple, dict, object]
INSTANCES = [A(), B(), C(), D(), E(), F(), G(), H(), 3, 'x', [], (), {}, None, 2.5]
VALUES = [3, 2.5, True, 7 + 1j, fractions.Fraction(3, 4), decimal.Decimal('1.25'),
          'abc', b'xyz', (1, 2), [3, 4], frozenset({5}), None, 10 ** 30]
TUPLES = [tuple(range(k)) for k in range(1, 18)]


def dispatch(handler, value):
    return handler(value)


def call_with(f, args, kw):
    return f(*args, **kw)


def collect(*args, **kw):
    return len(args) + sum(args) + len(kw) + sum(kw.values())


def check(obj, cls):
    return isinstance(obj, cls)


def size(v):
    return len(str(v)) + len(repr(type(v)))


def unpack(t):
    first, *rest = t
    return first + len(rest)


STRIDES = [(7, 200, 13, 10, 5, 1, 1, 3), (11, 170, 9, 8, 3, 5, 7, 4)]


def main(n, variant=0):
    sh, mod, na, nk, si, sc, sv, st = STRIDES[variant]
    keys = KEYS if variant == 0 else KEYS[::-1]
    total = 0
    for i in range(n):
        total += dispatch(HANDLERS[(i * sh) % len(HANDLERS)], i % mod)
        args = tuple(range(i % na))
        kw = {k: i % 5 for k in keys[:i % nk]}
        total += call_with(collect, args, kw)
        total += check(INSTANCES[(i * si) % len(INSTANCES)], CLASSES[(i * sc) % len(CLASSES)])
        total += size(VALUES[(i * sv) % len(VALUES)])
        total += unpack(TUPLES[(i * st) % len(TUPLES)])
    return total


N = 60000

if __name__ == '__main__':
    print(main(N))
