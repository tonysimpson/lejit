"""Dataflow solver over a graph of node objects of six classes kept in identity-hashed sets and dicts, so the order nodes and their types are first met follows memory addresses and differs from process to process while the fixed point does not: specialisation decided by whichever type arrives first, giving run-to-run variation (Barrett et al., Virtual Machine Warmup Blows Hot and Cold, OOPSLA 2017; Kalibera and Jones, Rigorous Benchmarking in Reasonable Time, ISMM 2013)."""
import zlib

N = 260


class Node:
    __slots__ = ('name', 'succ', 'dist', 'live', '__weakref__')

    def __init__(self, name):
        self.name = name
        self.succ = []
        self.dist = None
        self.live = frozenset()

    def cost(self, other):
        return 1

    def gen(self):
        return frozenset((self.name[0],))


class Load(Node):
    __slots__ = ()

    def cost(self, other):
        return 3 if isinstance(other, Store) else 2


class Store(Node):
    __slots__ = ()

    def cost(self, other):
        return len(other.name) % 4 + 1

    def gen(self):
        return frozenset()


class Branch(Node):
    __slots__ = ()

    def cost(self, other):
        return 5 if other.succ else 1


class Call(Node):
    __slots__ = ('callee_cost',)

    def __init__(self, name):
        super().__init__(name)
        self.callee_cost = len(name) * 2

    def cost(self, other):
        return self.callee_cost

    def gen(self):
        return frozenset(('c', self.name[-1]))


class Phi(Node):
    __slots__ = ()

    def cost(self, other):
        return 0

    def gen(self):
        return frozenset(s.name[0] for s in self.succ[:2])


KINDS = [Node, Load, Store, Branch, Call, Phi]


def build(k, size, variant):
    kinds = KINDS if variant == 1 else [Load, Load, Load, Store]
    nodes = []
    for i in range(size):
        cls = kinds[(i * 7 + k) % len(kinds)]
        nodes.append(cls('%s%d' % ('abcdefgh'[(i + k) % 8], i)))
    for i, nd in enumerate(nodes):
        for d in (1, 3 + (i % 5), (i * 13 + k) % size):
            j = (i + d) % size
            if j != i:
                nd.succ.append(nodes[j])
    pool = set(nodes)
    return nodes, pool


def shortest(start, pool):
    for nd in pool:
        nd.dist = None
    start.dist = 0
    changed = True
    while changed:
        changed = False
        for nd in pool:
            if nd.dist is None:
                continue
            for s in nd.succ:
                c = nd.dist + nd.cost(s)
                if s.dist is None or c < s.dist:
                    s.dist = c
                    changed = True
    return sorted((nd.name, nd.dist) for nd in pool)


def liveness(pool):
    preds = {}
    for nd in pool:
        nd.live = frozenset()
        for s in nd.succ:
            preds.setdefault(s, set()).add(nd)
    work = set(pool)
    while work:
        nd = work.pop()
        out = frozenset().union(*[s.live for s in nd.succ]) if nd.succ else frozenset()
        new = nd.gen() | (out - frozenset((nd.name[0],)))
        if new != nd.live:
            nd.live = new
            work.update(preds.get(nd, ()))
    return sorted((nd.name, ''.join(sorted(nd.live))) for nd in pool)


def reachable_by_type(start):
    seen = {start}
    stack = [start]
    counts = {}
    while stack:
        nd = stack.pop()
        counts[type(nd).__name__] = counts.get(type(nd).__name__, 0) + 1
        for s in nd.succ:
            if s not in seen:
                seen.add(s)
                stack.append(s)
    return sorted(counts.items())


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    out = []
    for k in range(n):
        nodes, pool = build(k, 120 + 10 * (k % 20), variant)
        d = shortest(nodes[0], pool)
        live = liveness(pool)
        out.append((d[:15], d[-5:], sum(x[1] for x in d if x[1] is not None), live[:10],
                    reachable_by_type(nodes[len(nodes) // 2])))
    return digest(out, 3)
