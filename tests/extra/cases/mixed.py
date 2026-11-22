# Mixed-type binary operations: 3.7's binary_op1 tries both
# operands' slots, the right one first if its type is a subclass.

def arith(a, b):
    return (a + b, a - b, a * b, a / b, b + a, b - a, b * a, b / a)

def floordiv_mod(a, b):
    return (a // b, a % b, b // a, b % a)

def inplace(a, b):
    a += b
    a *= b
    a -= b
    return a

def compare(a, b):
    return (a < b, a <= b, a == b, a != b, a > b, a >= b)

def mandel_row(w):
    n = 0
    for px in range(w):
        x0 = px * 3.0 / w - 2.0
        x = 0.0
        i = 0
        while i < 50 and x * x < 4.0:
            x = x * x + x0
            i = i + 1
        n = n + i
    return n

class F(float):
    def __radd__(self, other):
        return 'F.__radd__'

def subclass_first(a, b):
    return a + b

def str_mod(n):
    return 'n=%d' % n

def seq_repeat(n):
    return ([0] * n, n * [1], 'ab' * n, n * (2,))

def huge(a):
    return a + 0.5

CASES = [
    ('arith', [(1, 2.5), (2.5, 1), (-3, 0.25), (True, 1.5), (7, -2.0)], {}),
    ('arith', [(1, 0.0)], {}),
    ('floordiv_mod', [(7, 2.0), (-7, 2.5), (7.5, -2)], {}),
    ('inplace', [(1, 2.5), (2.5, 3)], {}),
    ('compare', [(1, 1.0), (1, 1.5), (2.5, 2), (float('nan'), 1)], {}),
    ('mandel_row', [(10,), (100,)], {}),
    ('subclass_first', '[(1.0, F(2.0)), (1, F(2.0)), (F(2.0), 1.0)]', {}),
    ('str_mod', [(3,), (-1,)], {}),
    ('seq_repeat', [(0,), (3,)], {}),
    ('huge', '[(10**400,), (2**53,)]', {}),
]
