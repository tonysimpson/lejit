# Promotion sites that see several values in turn: the polymorphic inline
# cache (x64/idispatcher.h). Up to 5 values are held at the site; after that
# a megamorphic site jumps to its generic version, and any other site
# replaces the cache's last entry. A full cache gives an entry that goes to
# the generic version (a heap type at isinstance's site) to a value with
# code of its own.

class A:
    k = 1
    def f(self, x):
        return x + 1

class B:
    k = 2
    def f(self, x):
        return x * 2

class C:
    k = 3
    def f(self, x):
        return x - 3

class D:
    k = 4
    def f(self, x):
        return -x

class E(A):
    k = 5

class F(B):
    def f(self, x):
        return x // 2

class G:
    k = 7
    def f(self, x):
        return x % 5

class H(G):
    k = 8

def _make(classes, n):
    return [classes[i % len(classes)]() for i in range(n)]

FOUR = _make([A, B, C, D], 12)
EIGHT = _make([A, B, C, D, E, F, G, H], 24)

def method_four(objs, rounds):
    # LOAD_METHOD on four classes in turn: all held by the cache
    t = 0
    for r in range(rounds):
        for o in objs:
            t += o.f(r)
    return t

def method_eight(objs, rounds):
    # more classes than the cache: the site goes megamorphic
    t = 0
    for r in range(rounds):
        for o in objs:
            t += o.f(r) + o.k
    return t

def attr_phases(rounds):
    # two classes for a while, then six others, then the first two again
    t = 0
    for objs in (FOUR[:2] * 3, EIGHT[2:], FOUR[:2] * 3):
        for r in range(rounds):
            for o in objs:
                t += o.k * r
    return t

class N:
    def __init__(self, v):
        self.v = v
    def __add__(self, other):
        return self.v + other
    def __radd__(self, other):
        return other + self.v

class M(N):
    def __add__(self, other):
        return self.v * 10 + other

class P(N):
    pass

VALUES = [1, 2.5, True, 3 + 1j, N(4), M(5), P(6), 7, 8.0, False]

def add_many_types(vals, rounds):
    # a binary op with no cut-off: more types than the cache holds, so the
    # last entry keeps being replaced; results must stay right
    out = []
    for r in range(rounds):
        for v in vals:
            out.append(v + r)
    return [repr(x) for x in out[-20:]], len(out)

def isinstance_many(rounds):
    # isinstance on a rotating type (a static-type promotion), static and
    # heap types mixed, more than the cache holds
    types = [int, float, str, list, tuple, A, B, dict, set, bytes, C, bool]
    vals = [1, 2.0, 's', [], (), A(), B(), {}, set(), b'', C(), True]
    n = 0
    for r in range(rounds):
        for t in types:
            for v in vals:
                if isinstance(v, t):
                    n += 1
    return n

def isinstance_chain(items, rounds):
    # the shapes kernel (tools/bench.py): an isinstance chain over eight
    # types, two of them heap types, which the site sends to its generic
    # version. The heap types must not use up the room for static types,
    # nor evict them from a full cache.
    t = 0.0
    for r in range(rounds):
        for x in items:
            if isinstance(x, int):
                t = t + x
            elif isinstance(x, float):
                t = t + x * 2.0
            elif isinstance(x, (str, bytes)):
                t = t + len(x)
            elif isinstance(x, A):
                t = t + 3.0 * x.k
            elif isinstance(x, B):
                t = t + x.k
    return t

MIXED = [3, 2.5, 'abc', A(), B(), 7, b'xy', None]
# heap types first: the inline compare and the first chain entries go to
# the generic version, then six static types, then all of them again
HEAP_FIRST = [A(), B(), C(), D(), 1, 2.0, 's', b'b', None, (), 5, A()]

def shift_amounts(rounds):
    t = 0
    for r in range(rounds):
        for s in (1, 2, 3, 4, 5, 6, 7, 8, 9):
            t += (r << s) >> 1
    return t

CASES = [
    ('method_four', '[(FOUR, 1), (FOUR, 50), (FOUR[::-1], 30)]', {}),
    ('method_eight', '[(EIGHT, 1), (EIGHT, 40), (EIGHT[::-1], 20)]', {}),
    ('attr_phases', [(1,), (20,)], {}),
    ('add_many_types', '[(VALUES, 3), (VALUES, 30), (VALUES[::-1], 10)]', {}),
    ('isinstance_many', [(1,), (10,)], {}),
    ('isinstance_chain', '[(MIXED * 3, 1), (MIXED * 10, 20), (MIXED[::-1], 5)]', {}),
    ('isinstance_chain', '[(HEAP_FIRST, 1), (HEAP_FIRST, 20), (MIXED, 10)]', {}),
    ('shift_amounts', [(1,), (30,)], {}),
]
