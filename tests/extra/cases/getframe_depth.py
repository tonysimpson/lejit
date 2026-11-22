# sys._getframe's depth as CPython counts it: a negative depth gives the top
# frame, a depth past the bottom raises ValueError.
import sys


def msg(f, *a):
    try:
        return ('ok', f(*a))
    except Exception as e:
        return (type(e).__name__, str(e))


def name_at(d):
    return msg(lambda: sys._getframe(d).f_code.co_name)


def inner(d):
    return name_at(d), msg(lambda: sys._getframe(d).f_back.f_code.co_name)


def outer(d):
    return inner(d)


CASES = [
    ('outer', [(0,), (1,), (-1,), (-2,), (-2147483648,), (10 ** 6,)], {}),
]
