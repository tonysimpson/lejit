# The int slots generated from Objects/longobject.c with the word
# representation (D83): + - * unary - ~ (one word, else CPython's slot),
# unary +, abs() (sign test, then long_neg or long_long), bool (the sign
# against 0), / and ** (CPython's slot with the result type). Both sides of
# the one-digit boundary, signs, zero, subclasses and errors.

M = 2 ** 30 - 1
EDGES = [0, 1, -1, 2, -2, M, -M, M + 1, -(M + 1), 2 ** 31, -2 ** 31,
         2 ** 63, -2 ** 63, 10 ** 30, -10 ** 30]


class I(int):
    pass


class J(int):
    def __abs__(self):
        return 'J.abs'

    def __bool__(self):
        return False


def unary(a):
    return (+a, -a, ~a, abs(a), bool(a), not a, type(+a), type(abs(a)))


def unary_edges():
    return [unary(a) for a in EDGES]


def binary(a, b):
    out = [a + b, a - b, a * b]
    try:
        out.append(a / b)
    except ZeroDivisionError as e:
        out.append(str(e))
    try:
        out.append(a ** (b % 7))
        out.append(type(a ** -2))
    except ZeroDivisionError as e:
        out.append(str(e))
    out.append(pow(a, 3, 7) if a else pow(a, 2))
    return out


def binary_edges():
    return [binary(a, b) for a in EDGES for b in EDGES[:9]]


def bool_branch(n):
    t = 0
    for i in range(-n, n):
        if i:
            t += 1
        if i - i:
            t += 100
        if abs(i) == -i:
            t += 1000
    return t


def abs_loop(n):
    t = 0
    for i in range(-n, n):
        t += abs(i) + abs(i * M)
    return t


def subclasses():
    i, k = I(-5), I(10 ** 20)
    j = J(-7)
    return (+i, type(+i), abs(i), type(abs(i)), abs(k), type(abs(k)),
            bool(I(0)), bool(i), -i, type(-i), ~i, i / 2, i ** 2,
            type(i ** 2), abs(j), bool(j), +j, type(+j), not j)


def err_op(k, a, b):
    if k == 0:
        return a / b
    if k == 1:
        return a ** b
    if k == 2:
        return a + b
    if k == 3:
        return -b
    if k == 4:
        return pow(a, 3, b)
    return pow(a, b, 4)


def errors():
    out = []
    for k, a, b in ((0, 1, 0), (1, 0, -1), (0, 10 ** 400, 3), (2, 1, 'a'),
                    (3, 1, 'a'), (4, 2, 0), (5, 2, -1), (0, M + 1, 0)):
        try:
            out.append(err_op(k, a, b))
        except Exception as e:
            out.append((type(e).__name__, str(e)))
    return out


def overflow(n):
    a = M
    out = []
    for i in range(n):
        a = a * 3 + 1
        out.append((a, -a, abs(-a), ~a, bool(a - a), a / 7))
    return out


CASES = [
    ('unary_edges', [()], {}),
    ('binary_edges', [()], {}),
    ('bool_branch', [(50,)], {}),
    ('abs_loop', [(100,)], {}),
    ('subclasses', [()], {}),
    ('errors', [()], {}),
    ('overflow', [(30,)], {}),
]
