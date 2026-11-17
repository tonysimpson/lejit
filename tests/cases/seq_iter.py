# Sequence iterators (Objects/iterobject.c:iter_iternext, piterobject.c): an
# exhausted iterator drops its sequence (it_seq = NULL), so it stays
# exhausted when the sequence grows (KNOWN_BUGS F48), whether it is still
# virtual, materialised by a call, or a run-time iterator passed in.
# IndexError and StopIteration from __getitem__ both end the iteration.


class Seq:
    def __init__(self, n):
        self.n = n

    def __getitem__(self, i):
        if i < self.n:
            return i
        raise IndexError(i)


class StopSeq:
    def __getitem__(self, i):
        if i < 2:
            return i
        raise StopIteration


def grown_after_exhaustion(n):
    a = Seq(n)
    it = iter(a)
    first = [x for x in it]
    a.n = n + 2
    return first, list(it), list(it)


def loop_then_loop(n):
    a = Seq(n)
    it = iter(a)
    first = []
    for x in it:
        first.append(x)
    a.n = n + 2
    second = []
    for x in it:
        second.append(x)
    return first, second


def runtime_iter(it, a):
    first = []
    for x in it:
        first.append(x)
    a.n += 2
    second = []
    for x in it:
        second.append(x)
    return first, second, next(it, 'end')


def exhausted_elsewhere(n):
    # a call materialises the iterator, a residual list() exhausts it, and
    # the compiled loop then must not trust its cached it_seq
    a = Seq(n)
    it = iter(a)
    r = [next(it, 'empty'), list(it)]
    a.n += 2
    for x in it:
        r.append(x)
    return r


def lists(n):
    a = list(range(n))
    it = iter(a)
    first = []
    for x in it:
        first.append(x)
    a.append(99)
    second = []
    for x in it:
        second.append(x)
    return first, second, list(it)


def stop_iteration():
    return list(StopSeq()), [x for x in StopSeq()]


def reduce_exhausted(n):
    it = iter(Seq(n))
    for x in it:
        pass
    return it.__reduce__()[1], it.__length_hint__()


CASES = [
    ('grown_after_exhaustion', [(0,), (3,)], {}),
    ('loop_then_loop', [(0,), (3,)], {}),
    ('runtime_iter', '[(lambda a: (iter(a), a))(Seq(3))]', {}),
    ('runtime_iter', '[(lambda a: (iter(a), a))(Seq(3))] * 2', {}),
    ('exhausted_elsewhere', [(0,), (3,)], {}),
    ('lists', [(0,), (3,)], {}),
    ('stop_iteration', [()], {}),
    ('reduce_exhausted', [(2,)], {}),
]
