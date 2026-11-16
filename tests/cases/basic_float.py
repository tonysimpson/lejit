# Float arithmetic on matching types (mixed int/float is KNOWN_BUGS F1).

def fadd(a, b):
    return a + b

def fmul(a, b):
    return a * b

def fdiv(a, b):
    return a / b

def fexpr(a, b, c):
    return a + b * c - a / c

def fcompare(a, b):
    return (a < b, a == b, a > b)

def floop(n):
    x = 0.0
    i = 0
    while i < n:
        x = x + 0.5
        i = i + 1
    return x

def mixed(a, b):
    return a + b

FL = [(1.5, 2.25), (-0.5, 4.0), (1e300, 1e300)]

CASES = [
    ('fadd', FL, {}),
    ('fmul', FL, {}),
    ('fdiv', FL, {}),
    ('fdiv_zero', [(1.0, 0.0)], {'xfail': 'F5: pfloat_div has no ZeroDivisionError'}),
    ('fexpr', [(1.0, 2.0, 3.0), (-1.5, 0.25, 8.0)], {}),
    ('fcompare', [(1.0, 2.0), (2.0, 2.0), (float('nan'), 1.0)], {}),
    ('floop', [(0,), (10,), (1000,)], {}),
    ('mixed', [(1, 2.5), (2.5, 1)], {'xfail': 'F1: binary_op1 is the Py2 version'}),
]

def fdiv_zero(a, b):
    return a / b
