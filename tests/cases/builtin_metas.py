# Builtins with Psyco metas (KNOWN_BUGS F2): len, abs, id, and the
# METH_FASTCALL ones (divmod, pow), whose metas take a tuple
# (pmethodobject.h); hash, min, max, isinstance, round, sum; and calls of the
# types int, float and type (a meta for type_call).

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

LOG = []

class Cmp:
    # records the comparisons min/max make, in order
    def __init__(self, v):
        self.v = v
    def __lt__(self, other):
        LOG.append(('lt', self.v, other.v))
        return self.v < other.v
    def __gt__(self, other):
        LOG.append(('gt', self.v, other.v))
        return self.v > other.v
    def __repr__(self):
        return 'Cmp(%r)' % self.v

class H:
    def __hash__(self):
        return -1          # CPython turns -1 into -2

class BadH:
    def __hash__(self):
        return 'x'

def hashes(a):
    try:
        return hash(a)
    except TypeError as e:
        return str(e)

def str_hash(s):
    # str hashes are randomised per process: compare within one
    return hash(s) == hash(s[:1] + s[1:]), hash(s) == hash(s + 'x')

def hash_loop(n):
    t = 0
    x = 0.5
    for i in range(-n, n):
        t = t + hash(i) + hash(x * i)
    return t

def min_max(a, b, c):
    try:
        return (max(a, b), min(a, b), max(a, b, c), min(c, b, a),
                max(a, b) is a, min(a, b) is a)
    except TypeError as e:
        return str(e)

def min_max_order():
    del LOG[:]
    a, b, c = Cmp(2), Cmp(5), Cmp(5)
    r = (max(a, b, c), min(c, a, b), max(b, c) is b)
    return r, list(LOG)

def min_max_loop(n):
    hi = -1
    lo = n
    for i in range(n):
        v = (i * 7) % 11
        hi = max(hi, v)
        lo = min(v, lo)
    return hi, lo

def min_max_fallbacks(seq):
    out = []
    for call in range(5):
        try:
            if call == 0:
                out.append(max(seq))
            elif call == 1:
                out.append(min(seq, key=abs))
            elif call == 2:
                out.append(max(seq, default='empty'))
            elif call == 3:
                out.append(max(*seq))
            else:
                out.append(min(1))
        except (TypeError, ValueError) as e:
            out.append(str(e))
    return out

def pows(a, b):
    out = []
    for z in (None, 5, 0, 2.5):
        try:
            out.append(pow(a, b) if z is None else pow(a, b, z))
        except (TypeError, ValueError, ZeroDivisionError) as e:
            out.append(type(e).__name__ + ': ' + str(e))
    return out

def pow_bad_calls(t):
    out = []
    try:
        out.append(pow(2))
    except TypeError as e:
        out.append(str(e))
    try:
        out.append(pow(*t))
    except TypeError as e:
        out.append(str(e))
    return out

import collections.abc
import weakref

class Base:
    pass

class Derived(Base):
    pass

class Other:
    pass

class MyInt(int):
    pass

class FakeInt:
    # isinstance consults __class__ when the type check fails
    @property
    def __class__(self):
        return int

class Meta(type):
    def __instancecheck__(cls, inst):
        return inst == 'yes'

class ByMeta(metaclass=Meta):
    pass

def isinstances(x):
    try:
        return (isinstance(x, int), isinstance(x, (str, float)),
                isinstance(x, Base), isinstance(x, (Other, (Base, int))),
                isinstance(x, object), isinstance(x, ()),
                isinstance(x, collections.abc.Sequence),
                isinstance(x, ByMeta), isinstance(x, (Base, MyInt)))
    except TypeError as e:
        return str(e)

def isinstance_bad(x):
    out = []
    for cls in (int, 5, (int, 5), (5, int)):
        try:
            out.append(isinstance(x, cls))
        except TypeError as e:
            out.append(str(e))
    try:
        out.append(isinstance(x))
    except TypeError as e:
        out.append(str(e))
    return out

def isinstance_proxy():
    b = Derived()
    p = weakref.proxy(b)
    return isinstance(p, Base), isinstance(p, weakref.ProxyType), isinstance(p, int)

class C1:
    pass

class D1:
    pass

def isinstance_bases_change():
    # a heap type's subtype relation can change between two calls
    x = C1()
    before = isinstance(x, D1)
    C1.__bases__ = (D1,)
    during = isinstance(x, D1)
    C1.__bases__ = (object,)
    return before, during, isinstance(x, D1)

def check_against(x, classes):
    return [isinstance(x, c) for c in classes]

def isinstance_many_classes():
    # one call site sees many classes: the promotion goes megamorphic
    classes = [int, str, float, list, dict, tuple, set, bytes, Base, Derived,
               Other, MyInt, bool, complex, frozenset, type, object]
    out = []
    for v in (1, 'a', 2.5, [], Derived(), True, MyInt(3)):
        out.append(check_against(v, classes))
    return out

def isinstance_globals_tuple(x):
    # (Base, int) is built at run time from globals: a virtual tuple
    return isinstance(x, (Base, int)), isinstance(x, (Other, str))

class MyFloat(float):
    pass

class K:
    def __init__(self, a):
        self.a = a

def conversions(x):
    out = []
    for fn in (int, float, round, type):
        try:
            r = fn(x)
            out.append((repr(r), type(r).__name__, r is x))
        except (TypeError, ValueError, OverflowError) as e:
            out.append(type(e).__name__ + ': ' + str(e))
    return out

def conversion_loop(n):
    t = 0
    x = 0.25
    for i in range(n):
        t = t + int(x) + round(x) + int(float(i))
        x = x + 0.5
    return t, float(t), type(t)

def type_calls_other():
    # forms the type_call meta leaves to CPython
    out = [int('ff', 16), int('12'), int(), float(), float('1.5'),
           int(3.9), type('T', (), {}).__name__,
           K(5).a, MyInt(7), MyFloat(1.5), bool(2)]
    try:
        out.append(int(1.5, 10))
    except TypeError as e:
        out.append(str(e))
    try:
        out.append(round(2.567, 2))
        out.append(round(25, -1))
        out.append(round(2.5, None))
        out.append(round(number=3.5))
    except TypeError as e:
        out.append(str(e))
    return out

def make(cls, v):
    return cls(v)

def type_call_many_classes():
    # one call site sees many types: the type promotion goes megamorphic
    out = []
    for cls in (int, float, str, list, tuple, K, MyInt, MyFloat, bool, type,
                int, float):
        out.append(repr(make(cls, 3)))
    return out

def type_call_alternating(n):
    # one site switches between heap classes (never constants) and static
    # types (promoted), in both orders, past MEGAMORPHIC_MAX
    out = []
    for i in range(n):
        cls = (K, int, MyFloat, float, type, K, str, MyInt)[i % 8]
        r = make(cls, i)
        out.append(r.a if cls is K else repr(r))
        out.append(isinstance(i, cls))
    return out

class Add:
    # records additions, to check sum's order and that none runs twice
    def __init__(self, v):
        self.v = v
    def __add__(self, other):
        LOG.append(('add', self.v, getattr(other, 'v', other)))
        return Add(self.v + getattr(other, 'v', other))
    def __radd__(self, other):
        LOG.append(('radd', self.v, other))
        return Add(other + self.v)

def sums(a, b, c):
    out = []
    for call in range(7):
        try:
            if call == 0:
                r = sum((a, b, c))
            elif call == 1:
                r = sum((a, b), c)
            elif call == 2:
                r = sum((a,))
            elif call == 3:
                r = sum(())
            elif call == 4:
                r = sum((), a)
            elif call == 5:
                r = sum((a, b, c), 0.0)
            else:
                r = sum((a, b), -0.0)
            out.append((repr(r), type(r).__name__))
        except (TypeError, OverflowError) as e:
            out.append(type(e).__name__ + ': ' + str(e))
    return out

def sum_direct(a, b, c):
    # no lambdas: the tuples are virtual in this frame
    try:
        r1 = sum((a, b, c))
        r2 = sum((a, b), c)
        r3 = sum((c, b, a), -0.0)
        return repr(r1), repr(r2), repr(r3), type(r1).__name__
    except (TypeError, OverflowError) as e:
        return type(e).__name__ + ': ' + str(e)

def sum_order():
    del LOG[:]
    r = sum((Add(1), 2, Add(3)), Add(10))
    s = sum((Add(1), 2))
    return r.v, s.v, list(LOG)

def sum_bad_start(x):
    out = []
    for start in ('', b'', bytearray()):
        try:
            out.append(sum((x,), start))
        except TypeError as e:
            out.append(str(e))
    try:
        out.append(sum())
    except TypeError as e:
        out.append(str(e))
    try:
        out.append(sum((x,), 0, 1))
    except TypeError as e:
        out.append(str(e))
    return out

def sum_fallbacks(seq):
    # lists, ranges, constant tuples and iterators
    return (sum(seq), sum([1, 2.5, 3]), sum(range(10)), sum((1, 2, 3)),
            sum(iter(seq), 100), sum(seq, 0.5))

def sum_loop(n):
    t = 0
    for i in range(n):
        t = t + sum((i, i * 2, 1)) + sum((i, 0.5))
    return t

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
    ('hashes', "[(0,), (-1,), (-2,), (2**30 - 1,), (-(2**30),), (2**70,), (True,), (-1.0,), (0.5,), (float('inf'),), (float('nan'),), (1e300,), ((1, 2),), ([1],), (H(),), (BadH(),)]", {}),
    ('str_hash', [('abc',), ('',)], {}),
    ('hash_loop', [(10,)], {}),
    ('min_max', "[(1, 2, 3), (2, 1, 0), (1, True, 1.0), (True, 1, 0), (1.0, 1, 2), (float('nan'), 1, 2), (1, float('nan'), 0), (-(2**70), 2**70, 0), (1, 'a', 2), ('b', 'a', 'c')]", {}),
    ('min_max_order', [()], {}),
    ('min_max_loop', [(50,)], {}),
    ('min_max_fallbacks', [([3, -7, 2],), ([],), ((5,),)], {}),
    ('pows', [(2, 10), (2, -1), (-3, 3), (0, -1), (2.0, 0.5), (2**40, 3)], {}),
    ('isinstances', "[(1,), (True,), (2.5,), ('a',), ('yes',), (Base(),), (Derived(),), (Other(),), (MyInt(4),), (FakeInt(),), (None,), ([1],), ((1,),)]", {}),
    ('isinstance_bad', [(1,), ('s',)], {}),
    ('isinstance_proxy', [()], {}),
    ('isinstance_bases_change', [()], {}),
    ('isinstance_many_classes', [()], {}),
    ('isinstance_globals_tuple', "[(1,), (Derived(),), ('s',), (2.5,)]", {}),
    ('conversions', "[(0,), (-7,), (2**62,), (2**63 - 1,), (2**70,), (0.0,), (-0.0,), (2.5,), (3.5,), (-2.5,), (-0.5,), (0.49999999999999994,), (9.2e18,), (9.3e18,), (-9.3e18,), (1e300,), (inf,), (-inf,), (nan,), (True,), (MyInt(5),), (MyFloat(1.5),), ('12',), (None,)]", {}),
    ('conversion_loop', [(20,)], {}),
    ('type_calls_other', [()], {}),
    ('type_call_many_classes', [()], {}),
    ('type_call_alternating', [(3,), (40,)], {}),
    ('sums', "[(1, 2, 3), (1, 2.5, 3), (2**62, 2**62, 1), (True, True, 1), (-0.0, -0.0, -0.0), (1e308, 1e308, 1), (inf, -inf, 1), ('a', 'b', 'c'), ([1], [2], [3]), (1, 'a', 2), (MyInt(1), MyFloat(2.0), 3)]", {}),
    ('sum_direct', "[(1, 2, 3), (1, 2.5, 3), (2**62, 2**62, 2**62), (True, 1, 0.5), (-0.0, -0.0, -0.0), ('a', 1, 2), ([1], [2], [3])]", {}),
    ('sum_order', [()], {}),
    ('sum_bad_start', "[(1,), ('a',)]", {}),
    ('sum_fallbacks', [([1, 2, 3],), ((4, 5),)], {}),
    ('sum_loop', [(30,)], {}),
    ('pow_bad_calls', [((2, 3),), ((2,),), ((2, 3, 4, 5),)], {}),
]
