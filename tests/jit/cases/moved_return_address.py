# A kept frame is let go when its call returns, wherever the callee keeps
# its return address then. Six types of 'b' fill the loop's merge point in
# 'callee' with blocks entered with three words pushed; a call with two
# of them constants is entered with one, and goes on in one of those
# blocks, which keeps the return address two words further down and
# leaves the word it came from unused. Each call's frame is its own.
import sys


class Int(int):
    pass


class Float(float):
    pass


def callee(a, b, c):
    t = b * c
    b = c = None
    for i in range(a):
        t += i
    return sys._getframe(), t


def all_run_time(a, b, c):
    return callee(a, b, c)


def two_constant(a):
    return callee(a, 2, 3)


def frames_distinct(n):
    for b in [1, 1.5, Int(2), Float(2.5), True, 3 + 0j, 10 ** 30]:
        all_run_time(n, b, b * 1)
    frames, totals = [], []
    for k in range(2):
        f, t = two_constant(n + k)
        frames.append(f)
        totals.append(t)
    return frames[0] is frames[1], totals


CASES = [
    ('frames_distinct', [(3,)], {}),
]
