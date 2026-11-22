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
# one object (types.resolve_bases does 'is not')
def tuple_identity_through_call(a):
    t = (a, object)
    u = (a, (t, a))
    return give_back(t) is t, give_back(u) is u, give_back(u)[1][0] is t


def new_class_bases():
    import types
    def meta(name, bases, ns, **kw):
        return name, bases, sorted(ns), kw
    return types.new_class('X', (int, object), dict(metaclass=meta, x=0))

# a type is called as ceval calls it: through its vectorcall where it has
# one, whose argument errors differ from tp_new's and tp_init's
def type_call_errors(t):
    out = []
    for args in ((1, 2, 3), (1, 2), (int, 1)):
        try:
            out.append(type(super(*args)).__name__)
        except TypeError as e:
            out.append(str(e))
    for f in (int, float, range, dict, list, t):
        try:
            out.append(f(1, 2, 3, 4))
        except TypeError as e:
            out.append(str(e))
    return out


# functions made on each run, called from a frame with handlers (which
# makes them real objects): each is a new object, so the calls must not
# specialise on it
def nested_function_calls(n):
    def add(a, b=n, *rest):
        return a + b + len(rest)
    sq = (lambda x, y=2: x ** y)
    t = 0
    for i in range(n):
        t += add(i) + add(i, 1) + add(i, 1, 2) + sq(i) + sq(i, 3)
    try:
        add()
    except TypeError as e:
        return t, str(e)
    return t


# a nested function whose code is replaced between two calls
def three():
    return 3

def nested_code_changes(k):
    def f():
        return None
    def g():
        return k
    out = [f(), g()]
    f.__code__ = three.__code__
    g.__code__ = (lambda: -k).__code__
    out += [f(), g()]
    return out


import math


class Ceil:
    def __ceil__(self):
        return 42


# math.ceil and math.floor of what isn't a float go to the real functions
def ceil_floor_objects(x):
    return math.ceil(x), math.floor(2.5), math.ceil(Ceil())


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
    ('type_call_errors', "[(tuple,), (set,)]", {}),
    ('nested_function_calls', [(4,)], {}),
    ('nested_code_changes', [(4,), (2.5,)], {}),
    ('ceil_floor_objects', "[(Ceil(),), (7,), (1.5,)]", {}),
]
