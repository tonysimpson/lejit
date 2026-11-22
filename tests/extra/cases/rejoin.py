# Respawned branches rejoin the main path (vcompiler.c:psyco_compile_join):
# many checks in a row, failing one at a time, each branch then joining the main path's block after its
# site. The results must be right whichever block a branch ends in.
import sys


def _joins():
    lejit = sys.modules.get('lejit')
    return lejit._core._stats()['joins'][0] if lejit is not None else 0


class P:
    def __init__(self, i):
        self.a = i
        self.b = i + 1
        self.c = i + 2
        self.d = i + 3
        self.e = i + 4
        self.f = i + 5


def _total(o):
    return o.a + o.b * 2 + o.c * 3 + o.d * 4 + o.e * 5 + o.f * 6


def _shapes(i):
    # one shape per call: shared keys, then dicts that fail the keys check
    o = P(i)
    k = i % 4
    if k == 1:
        o.__dict__ = dict(o.__dict__)           # combined: every load misses
    elif k == 2:
        o.__dict__ = {'f': i, 'e': i, 'd': i, 'c': i, 'b': i, 'a': i}
    elif k == 3:
        del o.c                                 # from the class
    return o


P.c = -100


def load_shapes(n):
    # the loads read the dict as it is when they run: every shape is right
    return [_total(_shapes(i)) for i in range(n)]


def loads_one_at_a_time(n):
    # a class change makes each load's type guard fail, respawn, and the
    # branch join the next load's block, whose guard fails in turn
    j = _joins()
    out = []
    for i in range(n):
        out.append(_total(P(i)))
        P.unrelated = i
    joined = _joins() - j
    return out, 'lejit' not in sys.modules or joined >= 3


class Q:
    x = 'class x'

    def m(self):
        return 'method'


def _use(o):
    a = o.m             # a bound method on the main path (virtual)
    b = o.x
    c = o.m()
    return a(), b, c


def shadowed(n):
    # the branch's value differs from the main path's (an instance
    # attribute where the class had a method): it must not join a block
    # that knows the main path's value
    out = []
    for i in range(n):
        o = Q()
        if i % 3 == 1:
            o.m = lambda: 'instance %d' % i
        if i % 3 == 2:
            o.x = 'instance x %d' % i
        out.append(_use(o))
    return out


class R:
    def __init__(self, a, b, c, d):
        self.a = a
        self.b = b
        self.c = c
        self.d = d


def constructors(n):
    # the stores of a constructor miss one after another on the first
    # instances (no dict yet, keys not yet holding the names, a GC value)
    out = []
    for i in range(n):
        r = R(i, [i], (i,), 'd%d' % i)
        out.append(sorted(r.__dict__.items()))
    return out


class S:
    def __init__(self):
        self.x = 0
        self.y = 0


class S2:
    def __init__(self):
        self.y = 'y'
        self.x = 'x'


class DS(dict):
    pass


def _xy(o, i):
    o.x = i
    o.y = -i


def inlined_two_sites(n):
    # _xy is inlined at four call sites: its split sites are inside an
    # inlined frame, whose caller position must not be generalised
    out = []
    for i in range(n):
        o = S()
        o.__dict__ = {'z': 1}
        _xy(o, i)
        out.append(sorted(o.__dict__.items()))
        o = S()
        o.__dict__.clear()
        _xy(o, i)
        out.append(sorted(o.__dict__.items()))
        o = S()
        p = S2()
        o.__dict__ = p.__dict__     # split, on another class's keys
        _xy(o, i)
        out.append(sorted(o.__dict__.items()))
        o = S()
        o.__dict__ = DS(x=5)
        _xy(o, i)
        out.append(sorted(o.__dict__.items()))
    return out


class T:
    def __init__(self, v):
        self.v = v


def _changes(o, k):
    if k:
        T.w = k                 # the class changes: type guards fail
    return o.v, o.v * 2, getattr(o, 'w', None)


def class_changes(n):
    out = []
    for i in range(n):
        out.append(_changes(T(i), i % 2))
    return out


CASES = [
    ('loads_one_at_a_time', [(24,)], {}),
    ('load_shapes', [(24,)], {}),
    ('shadowed', [(9,)], {}),
    ('constructors', [(12,)], {}),
    ('inlined_two_sites', [(6,)], {}),
    ('class_changes', [(8,)], {}),
]
