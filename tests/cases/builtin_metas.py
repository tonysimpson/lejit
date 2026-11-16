# Builtins with Psyco metas (KNOWN_BUGS F2): len, abs, id.

class L:
    def __len__(self):
        return 7

class BadLen:
    def __len__(self):
        return -1

class A:
    def __abs__(self):
        return 'abs!'

def lens(a, b, c, d):
    return len(a), len(b), len(c), len(d)

def len_user(x):
    return len(x)

def len_loop(n):
    t = 0
    s = 'abc'
    for i in range(n):
        t = t + len(s)
    return t

def abss(a, b):
    return abs(a), abs(b), abs(-a), abs(-b)

def abs_user(x):
    return abs(x)

def id_same(a):
    b = a
    return id(a) == id(b), id(a) == id([])

CASES = [
    ('lens', "[([1, 2], (1,), 'abcd', {'k': 1}), ([], (), '', {})]", {}),
    ('len_user', '[(L(),), (BadLen(),), (5,), (None,)]', {}),
    ('len_loop', [(0,), (10,)], {}),
    ('abss', [(3, -2.5), (0, 0.0), (-(2**70), 1e300)], {}),
    ('abs_user', "[(A(),), ('s',)]", {}),
    ('id_same', '[([1],)]', {}),
]
