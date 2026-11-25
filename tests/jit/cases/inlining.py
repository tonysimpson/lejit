# Bodies of up to 16 instructions that make code, with no loop or join, are
# inlined into their compiled caller: their results, errors, tracebacks and
# frames must be CPython's.
import sys


def eval_a(i, j):                      # 8 operations
    return 1.0 / ((i + j) * (i + j + 1) // 2 + i + 1)


def sums(n):
    t = 0.0
    for i in range(n):
        for j in range(n):
            t += eval_a(i, j) * j
    return t


class Vec:
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z

    def dot(self, other):              # 11 operations
        return self.x * other.x + self.y * other.y + self.z * other.z


def dots(n):
    a = Vec(1.5, 2, 3)
    b = Vec(4, 5.5, 6)
    t = 0
    for i in range(n):
        t += a.dot(b)
        b.x = i                        # an int after a float
    return t


def divide(a, b, c):                   # raises inside the inlined body
    return (a + b) * (a - b) // (c - 1) + a * b


def errors(n):
    out = []
    for i in range(n):
        try:
            out.append(divide(i, 2, i % 3))
        except ZeroDivisionError as e:
            tb = e.__traceback__
            names = []
            while tb is not None:
                names.append((tb.tb_frame.f_code.co_name,
                              tb.tb_lineno - tb.tb_frame.f_code.co_firstlineno))
                tb = tb.tb_next
            out.append(names)
    return out


def locals_at_raise(a, b):             # its frame's locals, from a traceback
    s = a * b + a
    return s // (b - b)


def traceback_locals(n):
    out = []
    for i in range(n):
        try:
            locals_at_raise(i, 3)
        except ZeroDivisionError as e:
            f = e.__traceback__.tb_next.tb_frame
            out.append(sorted(f.f_locals.items()))
    return out


def caller_frame(a, b):                # reads its caller's frame
    t = a * b - a + b
    return t, sys._getframe(1).f_code.co_name, sys._getframe(0).f_lineno


def frames(n):
    return [caller_frame(i, i + 1) for i in range(n)]


CASES = [
    ('sums', [(30,)], {}),
    ('dots', [(50,)], {}),
    ('errors', [(7,)], {}),
    ('traceback_locals', [(4,)], {}),
    ('frames', [(5,)], {}),
]
