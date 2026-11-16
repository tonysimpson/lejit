# Builtins with Psyco metas (KNOWN_BUGS F2): len, abs, id, and the
# METH_FASTCALL ones (divmod), whose metas take a tuple (pmethodobject.h).

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

def divmods(a, b):
    return divmod(a, b)

def divmod_loop(n):
    t = 0
    for i in range(1, n):
        q, r = divmod(n, i)
        t = t + q - r
    return t

def divmod_bad_calls(t):
    # wrong counts, star args, keywords: the meta falls back to the C function
    out = []
    try:
        out.append(divmod(1))
    except TypeError as e:
        out.append(str(e))
    try:
        out.append(divmod(1, 2, 3))
    except TypeError as e:
        out.append(str(e))
    try:
        out.append(divmod(*t))
    except TypeError as e:
        out.append(str(e))
    try:
        out.append(divmod(x=1, y=2))
    except TypeError as e:
        out.append(str(e))
    return out

CASES = [
    ('lens', "[([1, 2], (1,), 'abcd', {'k': 1}), ([], (), '', {})]", {}),
    ('len_user', '[(L(),), (BadLen(),), (5,), (None,)]', {}),
    ('len_loop', [(0,), (10,)], {}),
    ('abss', [(3, -2.5), (0, 0.0), (-(2**70), 1e300)], {}),
    ('abs_user', "[(A(),), ('s',)]", {}),
    ('id_same', '[([1],)]', {}),
    ('divmods', "[(7, 2), (-7, 2), (7, -2), (2**70, 3), (7.5, 2), (7, 2.5), (1, 0), (1.0, 0.0), ('a', 1)]", {}),
    ('divmod_loop', [(30,)], {}),
    ('divmod_bad_calls', [((7, 3),), ((1,),), ((1, 2, 3),)], {}),
]
