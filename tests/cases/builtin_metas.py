# Builtins with Psyco metas (KNOWN_BUGS F2): len, abs, id, and the
# METH_FASTCALL ones (divmod, pow), whose metas take a tuple
# (pmethodobject.h); hash, min, max, isinstance.

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
    ('pow_bad_calls', [((2, 3),), ((2,),), ((2, 3, 4, 5),)], {}),
]
