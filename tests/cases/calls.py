# Python-to-Python calls, globals, recursion.

G = 10

def helper(x):
    return x + 1

def call_helper(n):
    return helper(n) * 2

def call_in_loop(n):
    t = 0
    i = 0
    while i < n:
        t = t + helper(i)
        i = i + 1
    return t

def read_global(n):
    return n + G

def fact(n):
    if n <= 1:
        return 1
    return n * fact(n - 1)

def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)

def defaults(a, b=5):
    return a * b

def call_defaults(n):
    return defaults(n) + defaults(n, 2)

def many_args(a, b, c, d, e, f, g, h):
    return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8

def call_many(n):
    return many_args(n, n, n, n, n, n, n, n)

def call_builtin_len(s):
    return len(s)

def call_builtin_abs(n):
    return abs(n)

def down(n):
    return down(n + 1) + 1

# endless recursion in compiled code must raise RecursionError, not
# overflow the C stack; twice, to check the depth is
# given back
def endless_recursion():
    out = []
    for i in range(2):
        try:
            down(0)
        except RecursionError as e:
            out.append(str(e))
    return out

class GenHolder:
    def gen(self):
        yield 1


def renamed_gen():
    yield 2


renamed_gen.__name__ = 'other_name'
renamed_gen.__qualname__ = 'Other.qualname'


# generator functions aren't compiled, so the call falls back to the
# function object, whose name and qualname the generator takes
def generator_names():
    g = GenHolder().gen()
    h = renamed_gen()
    return g.__name__, g.__qualname__, h.__name__, h.__qualname__, list(h)

def give_back(b):
    for x in b:
        if isinstance(x, type):
            continue
    return b


# a tuple built in compiled code and passed to a compiled callee must stay
# one object (KNOWN_BUGS C17; types.resolve_bases does 'is not')
def tuple_identity_through_call(a):
    t = (a, object)
    u = (a, (t, a))
    return give_back(t) is t, give_back(u) is u, give_back(u)[1][0] is t


def new_class_bases():
    import types
    def meta(name, bases, ns, **kw):
        return name, bases, sorted(ns), kw
    return types.new_class('X', (int, object), dict(metaclass=meta, x=0))

CASES = [
    ('call_helper', [(1,), (-4,)], {}),
    ('call_in_loop', [(0,), (50,)], {}),
    ('read_global', [(1,), (-10,)], {}),
    ('fact', [(1,), (5,), (20,), (25,)], {}),
    ('fib', [(0,), (10,), (15,)], {}),
    ('call_defaults', [(3,)], {}),
    ('call_many', [(1,), (7,)], {}),
    ('call_builtin_len', [('abc',), ((1, 2),), ([],)], {}),
    ('call_builtin_abs', [(-3,), (4,), (-2.5,)], {}),
    ('endless_recursion', [()], {}),
    ('generator_names', [()], {}),
    ('tuple_identity_through_call', "[(int,)]", {}),
    ('new_class_bases', [()], {}),
]
