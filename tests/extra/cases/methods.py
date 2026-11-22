# Method calls (LOAD_METHOD/CALL_METHOD) and augmented subscripts (DUP_TOP_TWO).

class C:
    def __init__(self, x):
        self.x = x
    def get(self):
        return self.x
    def add(self, a, b=10):
        return self.x + a + b
    @staticmethod
    def st(a):
        return a * 2
    @classmethod
    def cm(cls, a):
        return cls.__name__, a

def list_methods(n):
    r = []
    for i in range(n):
        r.append(i * i)
    r.reverse()
    return r, r.index(0) if n else None, r.count(1)

def str_methods(s):
    return s.upper(), s.split(','), '-'.join(s.split(',')), s.startswith('a')

def dict_methods(d):
    k = sorted(d.keys())
    return k, d.get('a'), d.get('zz', 5)

def user_methods(c):
    return c.get(), c.add(1), c.add(1, 2), c.st(3), c.cm(4)

def method_raises(s):
    return s.index('q')

def attr_not_method(c):
    c.f = len
    return c.f('abc')

def no_such_method(c):
    return c.nope()

def aug_subscript(n):
    a = [0, 0, 0]
    for i in range(n):
        a[i % 3] += i
    d = {'k': 1}
    d['k'] *= 5
    return a, d

def chained_calls(s):
    return s.strip().lower().replace('a', 'b')

CASES = [
    ('list_methods', [(0,), (5,)], {}),
    ('str_methods', [('a,b,c',), ('x',)], {}),
    ('dict_methods', [({'a': 1, 'b': 2},), ({},)], {}),
    ('user_methods', '[(C(5),)]', {}),
    ('method_raises', [('abc',)], {}),
    ('attr_not_method', '[(C(1),)]', {}),
    ('no_such_method', '[(C(1),)]', {}),
    ('aug_subscript', [(0,), (10,)], {}),
    ('chained_calls', [('  AbA ',)], {}),
]
