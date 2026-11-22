# Instance attribute loads inline on a class's shared keys (PEP 412,
# pobject.c:instance_dict_get): every way an instance's dict can stop
# matching the keys the code last saw must fall back to the right answer.


class P:
    k = 'class'

    def __init__(self, a, b):
        self.a = a
        self.b = b


def hits(n):
    ps = [P(i, -i) for i in range(5)]
    t = 0
    for i in range(n):
        for p in ps:
            t += p.a - p.b
    return t


def _odd(i):
    q = P.__new__(P)
    if i % 3 == 0:
        q.b = i                 # other order: a combined dict
        q.a = i
    elif i % 3 == 1:
        q.a = i                 # no 'b' and no class 'b'
    return q                    # i % 3 == 2: no dict at all


def misses(n):
    ps = [P(1, 2)]
    out = []
    for i in range(n):
        ps.append(_odd(i))
        for p in ps:
            out.append((getattr(p, 'a', None), getattr(p, 'b', None), p.k))
    return out[-12:], len(out)


def shadow(n):
    p = P(1, 2)
    out = []
    for i in range(n):
        out.append(p.k)
        if i == 2:
            p.k = 'instance'    # a new key: past the shared keys' end
        if i == 4:
            del p.k
    return out


class Grow:
    def __init__(self):
        self.a, self.b, self.c = 1, 2, 3
        self.d, self.e, self.f = 4, 5, 6

    def total(self):
        return self.a + self.f + getattr(self, 'z', 0)


def grows(n):
    g = Grow()
    out = []
    for i in range(n):
        out.append(g.total())
        if i == 2:
            g.z = 100           # grows, or splits from, the shared keys
            g.y = 7
            g.x = 8
    out.append(Grow().total())
    return out


class D:
    def __init__(self, v):
        self.v = v


def keys_dropped(n):
    ds = [D(0), D(1), D(2)]     # the keys are dropped on the first call
    t = []
    for i in range(n):
        s = 0
        for d in ds:
            s += d.v
        t.append(s)
        if i == 2:
            del ds[0].v         # deletion drops the class's shared keys
            ds[0].v = 10
            ds.append(D(20))
    return t


CASES = [
    ('hits', [(20,)], {}),
    ('misses', [(9,)], {}),
    ('shadow', [(7,)], {}),
    ('grows', [(5,)], {}),
    ('keys_dropped', [(5,)], {}),
]
