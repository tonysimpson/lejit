# Integer arithmetic, comparisons, branches and loops (the README's examples).

def add(a, b):
    return a + b

def sub(a, b):
    return a - b

def mul(a, b):
    return a * b

def floordiv(a, b):
    return a // b

def mod(a, b):
    return a % b

def neg(a):
    return -a

def bitops(a, b):
    return (a & b, a | b, a ^ b, a << 3, a >> 1, ~a)

def const_fold():
    return 2 * 3 + 4

def while_count(n):
    i = 0
    while i < n:
        i = i + 1
    return i

def while_sum(n):
    t = 0
    i = 0
    while i < n:
        t = t + i
        i = i + 1
    return t

def if_else(a):
    if a > 10:
        return 1
    else:
        return 2

def compare_all(a, b):
    return (a < b, a <= b, a == b, a != b, a > b, a >= b)

def truthiness(a):
    if a:
        return 'yes'
    return 'no'

def inplace(n):
    t = 0
    t += n
    t *= 3
    t -= 1
    return t

def nested_loops(n):
    t = 0
    i = 0
    while i < n:
        j = 0
        while j < n:
            t += i * j
            j += 1
        i += 1
    return t

def big_ints(a):
    return a * a * a

INTS = [(0, 1), (3, 4), (-5, 7), (100, -3), (2**40, 3)]

def boxed_small(lo, hi):
    # ints made at run time and boxed: those of CPython's small-int cache
    # are its objects, the rest new ones
    out = []
    for i in range(lo, hi):
        out.append(i + 1)
    return [x is int(str(x)) for x in out], out

def boxed_beside_float(n):
    f = 0.5
    out = []
    for i in range(n):
        f = f * 1.5 + 0.25
        out.append(i * 7 - 3)
    return f, out

CASES = [
    ('add', INTS, {}),
    ('sub', INTS, {}),
    ('mul', INTS, {}),
    ('floordiv', [(7, 2), (-7, 2), (7, -2), (1, 0)], {}),
    ('mod', [(7, 2), (-7, 2), (7, -2), (1, 0), (5, 'a'), ('%d', 5)], {}),
    ('neg', [(0,), (5,), (-5,)], {}),
    ('bitops', [(12, 10), (-1, 255), (2**33, 5)], {}),
    ('const_fold', [()], {}),
    ('while_count', [(0,), (10,), (1000,)], {}),
    ('while_sum', [(0,), (100,), (10000,)], {}),
    ('if_else', [(5,), (11,), (10,)], {}),
    ('compare_all', [(1, 2), (2, 2), (3, 2), (-1, 2**40)], {}),
    ('truthiness', [(0,), (1,), (-1,), (2**32,), (2**64,)], {}),
    ('inplace', [(0,), (7,), (-3,)], {}),
    ('nested_loops', [(0,), (5,), (30,)], {}),
    ('big_ints', [(10,), (2**21,), (2**40,)], {}),
    ('boxed_small', [(-12, 1040), (2**40, 2**40 + 3)], {}),
    ('boxed_beside_float', [(0,), (300,)], {}),
]
