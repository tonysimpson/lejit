# A class's shared keys (PEP 412) stay CPython's: compiled loads hold no
# reference to them, so when the only instance outgrows them CPython can
# give the class the grown keys (_PyObjectDict_SetItem), and later
# instances still get split dicts. sys.getsizeof tells a split dict from
# a combined one.
import sys


class Grower:
    def __init__(self):
        self.a = 1

    def get_a(self):
        return self.a

    def fill(self):
        self.b = 2
        self.c = 3
        self.d = 4
        self.e = 5
        self.f = 6
        self.g = 7


def regrown_after_load(n):
    # loads compiled while one instance uses the keys, then it outgrows them
    out = []
    for i in range(n):
        x = Grower()
        for j in range(3):
            x.get_a()
        x.fill()
        y = Grower()
        y.fill()
        out.append((sys.getsizeof(x.__dict__), sys.getsizeof(y.__dict__)))
    return out


class HalfBuilt:
    total = 0

    def __init__(self, i):
        self.a = i
        self.note()             # compiled while the first instance is half built
        self.b = i
        self.c = i
        self.d = i
        self.e = i
        self.f = i

    def note(self):
        t = self.a + 1          # the type of a promoted: compiling pauses
        HalfBuilt.total += t + self.a   # this load is compiled on resuming


def half_built(n):
    objs = [HalfBuilt(i) for i in range(n)]
    return [sys.getsizeof(o.__dict__) for o in objs]


class Absent:
    def m(self):
        return 1


def absent_probe(n):
    # a method load proves the name absent from the instance dict by its
    # probe sequence: right for split, combined and resized dicts, and for
    # an instance attribute that shadows the method
    out = []
    for i in range(n):
        o = Absent()
        for k in range(i % 12 if i % 11 else 120):   # 120: wider indices
            setattr(o, 'x%d' % k, k)
        if i % 3 == 1:
            o.__dict__ = dict(o.__dict__)
        if i % 5 == 4:
            o.m = lambda: 2
        if i % 7 == 6:
            del o.__dict__
        out.append(o.m())
    return out


CASES = [
    ('regrown_after_load', [(3,)], {}),
    ('half_built', [(4,)], {}),
    ('absent_probe', [(40,)], {}),
]
