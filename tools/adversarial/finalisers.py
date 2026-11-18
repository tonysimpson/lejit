"""Finalisers whose order a program can see: __del__ methods, weakref callbacks and abandoned generators' finally blocks, as resource handles and caches use them; ORDER records the order they run in. Variant 1 drops them in other patterns."""
import weakref

ORDER = []


class Handle:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        ORDER.append('del ' + self.name)


class Node:
    def __init__(self, key):
        self.key = key


def reader(name):
    try:
        yield 1
        yield 2
    finally:
        ORDER.append('close ' + name)


def work(i, every):
    h = Handle('h%d' % i)
    tmp = Handle('t%d' % i) if i % every else None
    total = len(h.name)
    if tmp is not None:
        total += len(tmp.name)
        tmp = None
    g = reader('g%d' % i)
    total += next(g)
    if i % (every + 1) == 0:
        total += next(g)
    g = None
    ORDER.append('end %d' % i)
    return total


def main(n, variant=0):
    del ORDER[:]
    every = 3 if variant == 0 else 4
    cache = weakref.WeakValueDictionary()
    refs = []
    total = 0
    node = None
    for i in range(n):
        total += work(i, every)
        node = Node(i)
        cache[i % 7] = node
        refs.append(weakref.ref(node, lambda r, i=i: ORDER.append('ref %d' % i)))
        pair = [Handle('p%d' % i), Handle('q%d' % i)]
        if variant and i % 2:
            pair.reverse()
        pair = None
        if len(refs) > 50:
            del refs[:25]
        total += len(cache)
    node = None
    return total, len(ORDER), ORDER[:12], ORDER[-6:]


N = 20000

if __name__ == '__main__':
    print(main(N))
