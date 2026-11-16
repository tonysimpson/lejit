# Control flow: boolean operators, try/except, break/continue, attributes.

class P:
    def __init__(self, x):
        self.x = x

def and_or(a, b):
    return (a and b, a or b, not a)

def chained(a):
    return 0 < a < 10

def break_loop(n):
    i = 0
    while True:
        if i >= n:
            break
        i += 1
    return i

def continue_loop(n):
    t = 0
    for i in range(n):
        if i % 2:
            continue
        t += i
    return t

def try_except(a, b):
    try:
        return a // b
    except ZeroDivisionError:
        return -1

def try_finally(a):
    r = 0
    try:
        r = a + 1
    finally:
        r = r * 2
    return r

def raise_it(a):
    if a:
        raise ValueError('bad %d' % a)
    return 0

def get_attr(o):
    return o.x

def set_attr(o, v):
    o.x = v
    return o.x

def is_none(a):
    return a is None

def isinst(a):
    return isinstance(a, int)

CASES = [
    ('and_or', [(0, 1), (2, 3), (0, 0)], {}),
    ('chained', [(5,), (11,), (0,)], {}),
    ('break_loop', [(0,), (5,)], {}),
    ('continue_loop', [(10,)], {}),
    ('try_except', [(5, 2)], {}),
    ('try_except', [(5, 0)], {}),
    ('try_finally', [(3,)], {}),
    ('raise_it', [(0,)], {}),
    ('raise_it', [(3,)], {'xfail': "F1: 'bad %d' % 3 is str % int, mixed types"}),
    ('get_attr', '[(P(3),), (P("s"),)]', {}),
    ('set_attr', '[(P(3), 7)]', {}),
    ('is_none', [(None,), (0,)], {}),
    ('isinst', [(1,), ('a',)], {}),
]
