# A frame object kept from a generator's body, read after the generator
# has ended: its line and f_lasti are where the body last ran (the for
# line of the loop that ran out, where CPython gives the implicit return
# that line; the raising line), and its f_back is the frame the
# generator was run from, as CPython links it, or None where CPython
# leaves it unlinked. Run by a for loop, a comprehension, a generator
# expression, next() until StopIteration, and a body that raises, each in
# a function a compiled function calls, which has no frame of its own
# until one is asked for.
import sys

FRAMES = []


def grab(x):
    FRAMES.append(sys._getframe(1))
    return x


def body(xs):
    for x in xs:
        yield grab(x)


def raising(xs):
    for x in xs:
        yield 1 // (grab(x) - 1)


def seen():
    f = FRAMES[0]
    back = f.f_back
    out = (f.f_lineno - f.f_code.co_firstlineno, f.f_lasti,
           back.f_code.co_name if back is not None else None,
           all(g is f for g in FRAMES))
    FRAMES.clear()
    return out


def in_for_loop(n):
    for y in body(range(n)):
        pass
    return seen()


def in_comprehension(n):
    ys = [y for y in body(range(n))]
    return ys, seen()


def in_generator_expression(n):
    ys = [y for y in (grab(x) for x in range(n))]
    return ys, seen()


def in_nexts(n):
    g = body(range(n))
    ys = []
    try:
        while True:
            ys.append(next(g))
    except StopIteration:
        pass
    return ys, seen()


def in_raises(n):
    ys = []
    try:
        for y in raising(range(n)):
            ys.append(y)
    except ZeroDivisionError:
        pass
    return ys, seen()


def for_loop(n):
    return in_for_loop(n)


def comprehension(n):
    return in_comprehension(n)


def generator_expression(n):
    return in_generator_expression(n)


def nexts(n):
    return in_nexts(n)


def raises(n):
    return in_raises(n)


CASES = [
    ('for_loop', [(3,)], {}),
    ('comprehension', [(3,)], {}),
    ('generator_expression', [(3,)], {}),
    ('nexts', [(3,)], {}),
    ('raises', [(3,)], {}),
]
