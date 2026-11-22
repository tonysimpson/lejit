from . import Block

BLOCKS = [
    Block('type_switch', 'types',
          'promotes the type of a local used by a binary operator, one block per type',
          'type switch: the values change kind after the break',
          '''
P_vals = S_before
P_after = S_after
def P_break():
    global P_vals
    P_vals = P_after
def P_run(r):
    acc = []
    inner = None
    for i in range(6):
        v = P_vals[(r + i) % len(P_vals)]
        try:
            acc.append(v + v)
            acc.append(v * 2)
        except TypeError:
            acc.append('TypeError')
        if i == 2:
            inner = NEXT(r)
    return acc, inner
''', slots={'before': ['(1, 2, 3)', '(1.5, -0.0)', "('ab', 'c')", '(True, False)', '(2**62, 7)'],
            'after': ['(2**64, 1.5, "x")', '(None, 1)', '([1], (2,), 3)', '(2**63-1, -2**63, 7)',
                      "(1, 2.5, 'a', b'b', True, 2**70, 1j)", "(frozenset([1]), 3.0)"]}),

    Block('megamorphic_method', 'types',
          'one promotion site sees instances of more classes than MEGAMORPHIC_MAX (5) '
          'and goes megamorphic',
          'megamorphic overflow: the break adds classes the site has not seen',
          '''
def P_make(k):
    class C:
        def m(self, a):
            return a * k + 1
    C.__name__ = 'C%d' % k
    return C
P_objs = [P_make(k)() for k in range(S_n)]
def P_break():
    global P_objs
    P_objs = P_objs + [P_make(k)() for k in range(100, 100 + S_more)]
def P_run(r):
    out = 0
    names = []
    inner = None
    for j, o in enumerate(P_objs):
        out += o.m(r + j)
        names.append(type(o).__name__)
        if j == 1:
            inner = NEXT(r)
    return out, names[-2:], inner
''', slots={'n': ['2', '5', '6', '7', '12'], 'more': ['1', '3', '8']}),

    Block('class_assign', 'types',
          "promotes an object's type and keeps it across a call, which reassigns "
          '__class__',
          '__class__ assignment, by a call made between two uses',
          '''
class P_A:
    x = 1
    def f(self):
        return 'A'
class P_B:
    x = 2
    def f(self):
        return 'B'
    def __len__(self):
        return 3
P_o = P_A()
P_broken = False
def P_break():
    global P_broken
    P_broken = True
def P_swap(o):
    if P_broken:
        o.__class__ = P_B if o.__class__ is P_A else P_A
    return 0
def P_run(r):
    o = P_o
    a = o.f() + str(o.x)
    inner = NEXT(r)
    P_swap(o)
    b = o.f() + str(o.x) + type(o).__name__
    try:
        n = len(o)
    except TypeError:
        n = 'TypeError'
    return a, b, n, isinstance(o, P_B), inner
'''),

    Block('int_subclass', 'types',
          'translated int arithmetic on exact ints (PyLong_CheckExact in plongobject_gen.h), '
          'then a subclass with its own __add__ reaches the same site',
          'type switch to a subclass that overrides the operator',
          '''
class P_I(int):
    def __add__(self, o):
        return 'I+'
    __radd__ = __add__
    def __mul__(self, o):
        return int.__mul__(self, o) + 1000
P_vals = S_vals
def P_break():
    global P_vals
    P_vals = (P_I(1), 2, P_I(S_k))
def P_run(r):
    acc = []
    for v in P_vals:
        acc.append(v + 1)
        acc.append(1 + v)
        acc.append(v * 2)
        acc.append(-v)
    return acc, NEXT(r)
''', slots={'vals': ['(1, 2, 3)', '(0, -1)', '(2**31, 2**63)'],
            'k': ['0', '-5', '2**64']}),

    Block('abc_register', 'types',
          'isinstance and issubclass of a known class folded at compile time',
          'ABC registration changes the answer after the break',
          '''
import abc
class P_Abc(abc.ABC):
    pass
class P_K:
    pass
P_k = P_K()
def P_break():
    S_how
def P_run(r):
    a = isinstance(P_k, P_Abc)
    b = isinstance(r, P_Abc)
    c = issubclass(P_K, P_Abc)
    d = isinstance(P_k, (int, P_Abc))
    return a, b, c, d, NEXT(r)
''', slots={'how': ['P_Abc.register(P_K)', 'P_Abc.register(int)',
                    'P_Abc.register(bool)']}),

    Block('instancecheck_meta', 'types',
          'isinstance on a class whose metaclass defines __instancecheck__',
          'method replacement: the metaclass\'s __instancecheck__ changes answer',
          '''
class P_M(type):
    def __instancecheck__(cls, o):
        return P_answer(o)
class P_T(metaclass=P_M):
    pass
def P_answer(o):
    return type(o) is int
def P_break():
    global P_answer
    P_answer = S_new
def P_run(r):
    return isinstance(r, P_T), isinstance('s', P_T), isinstance(P_T(), P_T), NEXT(r)
''', slots={'new': ['lambda o: True', 'lambda o: isinstance(o, str)', 'lambda o: 1 // 0']}),

    Block('classes_per_call', 'types',
          'a class made per call: each new type reaches the same promotion sites',
          'megamorphic overflow by fresh classes every call',
          '''
P_base = object
def P_break():
    global P_base
    P_base = S_base
def P_run(r):
    class K(P_base):
        def m(self):
            return r % 7
    o = K()
    return o.m(), type(o).__name__, K.__mro__[-1].__name__, NEXT(r)
''', slots={'base': ['int', 'dict', 'Exception', 'tuple']}, leaks='code compiled for each new class'),
]
