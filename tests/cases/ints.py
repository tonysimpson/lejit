# Virtual ints (ni/Objects/plongobject.c): + - * unary - and comparisons
# take 3.7's one-digit fast path (|value| <= 2**30 - 1) and fall back to
# CPython's slot otherwise. Results, types and identity of small ints must
# be CPython's, on both sides of the one-digit boundary.

M = 2 ** 30 - 1          # PyLong_MASK: the largest one-digit value
EDGES = [0, 1, -1, 2, M - 1, M, -M, M + 1, -(M + 1), 2 ** 31, 2 ** 62,
         2 ** 63, -2 ** 63, 10 ** 30, -10 ** 30]


def table(op):
    out = []
    for a in EDGES:
        for b in EDGES:
            out.append(op(a, b))
    return out


# module-level, not lambdas: a new function object each call would be
# compiled each time (KNOWN_BUGS C14), which the leak check sees
def op_add(a, b):
    return a + b


def op_sub(a, b):
    return a - b


def op_mul(a, b):
    return a * b


def op_invert(a, b):
    return ~a, ~b, ~(a + b)


def op_cmp(a, b):
    return (a < b, a <= b, a == b, a != b, a > b, a >= b)


def add_edges():
    return table(op_add)


def sub_edges():
    return table(op_sub)


def mul_edges():
    return table(op_mul)


def invert_edges():
    return table(op_invert)


def cmp_edges():
    return table(op_cmp)


# the operations inline in one function, so results stay virtual and feed
# each other: the range test on virtual operands decides each step
def chain(a, b):
    c = a + b
    d = c * c
    e = d - a
    f = -e
    return c, d, e, f, f < a, -(-a) == a


def chain_edges():
    return [chain(a, b) for a in EDGES for b in EDGES]


def grow_past_one_digit(n):
    # starts small, leaves one digit, ends huge: every fallback is taken
    x = 1
    out = []
    for i in range(n):
        x = x * 3 + i
        out.append(x)
    return out


def shrink_back(n):
    x = 3 ** 40
    out = []
    while x > 1:
        x = x - x // 2 - 1 if x > 10 else x - 1
        out.append(x)
    return out[-5:], len(out)


def zero_and_signs(n):
    out = []
    for i in range(-n, n + 1):
        out.append((i * 0, 0 - i, -i, i * -1, i - i, bool(i), not i))
    return out


def bools_are_ints():
    t, f = True, False
    return (t + t, t - f, t * 5, -t, f * 7, t < 2, f == 0, type(t + t),
            t + 1.5)


class MyInt(int):
    pass


class AddInt(int):
    def __add__(self, other):
        return 'AddInt.__add__'


def subclasses():
    a = MyInt(5)
    b = AddInt(5)
    return (a + 1, type(a + 1), a * a, -a, a < 7, b + 1, 1 + b, type(1 + b))


def with_floats(n):
    t = 0
    f = 0.5
    for i in range(n):
        t = t + i
        f = f + i
    return t, f, t * f, t < f, t + 2 ** 70 * 1.0


def small_int_identity():
    a = 100
    b = a + 1
    c = b - 1
    return c is a, (a * 2 - 100) is a, (-(-a)) is a, b + 2 ** 40 - 2 ** 40 == b


def escapes(n):
    # virtual results stored, passed, returned and used as keys and indexes
    seen = {}
    lst = []
    for i in range(n):
        k = i * 7 - 3
        seen[k] = i
        lst.append(k)
    prev = []
    for i in range(1, n):
        prev.append(lst[i - 1])
    return seen, lst, prev, max(lst), str(k)


def loop_counter(n):
    i = 0
    total = 0
    while i < n:
        total = total + i * i
        i = i + 1
    return total


def mixed_errors():
    out = []
    for a, b in ((1, 'x'), ('x', 1), (1, None), (1, [1])):
        for op in (op_add, op_mul, op_cmp):
            try:
                out.append(op(a, b))
            except TypeError as e:
                out.append(type(e).__name__)
    return out


# comparison results are virtual bools (pboolobject.h): used in jumps,
# stored, compared by identity, and used as ints
def bool_results(n):
    out = []
    flags = []
    for i in range(-n, n):
        b = i < 0
        f = i * 0.5 >= 1.0
        flags.append(b)
        out.append((b, f, b is True, f is False, b + f, -b, b * 3, b == f,
                    type(b), not b, b and f, b or f))
    return out, sum(flags), flags.count(True)


CASES = [
    ('add_edges', [()], {}),
    ('sub_edges', [()], {}),
    ('mul_edges', [()], {}),
    ('invert_edges', [()], {}),
    ('cmp_edges', [()], {}),
    ('chain_edges', [()], {}),
    ('grow_past_one_digit', [(40,)], {}),
    ('shrink_back', [(0,)], {}),
    ('zero_and_signs', [(5,)], {}),
    ('bools_are_ints', [()], {}),
    ('subclasses', [()], {}),
    ('with_floats', [(10,)], {}),
    ('small_int_identity', [()], {}),
    ('escapes', [(12,)], {}),
    ('loop_counter', [(100,), (0,)], {}),
    ('mixed_errors', [()], {}),
    ('bool_results', [(4,)], {}),
]
