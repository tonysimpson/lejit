# Generator expressions consumed by list(), set(), tuple(), any() and
# all(), which some CPythons compile into a loop in the caller over the
# generator (3.14 for tuple, any and all; 3.15 for list and set too),
# others into a call that LeJIT fuses. Where the generator expression takes
# the iterator of its argument before it is a generator (3.15), the
# consumer takes it instead, only for an iterable whose iter() can't raise
# or run Python: what Python sees must be CPython's either way, errors and
# tracebacks from that iter() included.
import sys

LOG = []


class Noted:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        LOG.append(('del', self.name))


class PyIter:
    def __init__(self, n):
        self.n = n

    def __iter__(self):
        LOG.append('iter')
        return iter(range(self.n))


class Refuses:
    def __iter__(self):
        raise ValueError('no')


class ListKind(list):
    pass


def tb_names(e):
    out = []
    tb = e.__traceback__
    while tb is not None:
        out.append(tb.tb_frame.f_code.co_name)
        tb = tb.tb_next
    return out


def run(f):
    try:
        return f()
    except Exception as e:
        return ('raised', type(e).__name__, str(e), tb_names(e)[1:])


def consumers(n):
    xs = list(range(n))
    d = {i: -i for i in xs}
    out = [list(x * 2 for x in xs), sorted(set(x % 3 for x in xs)),
           tuple(x + 1 for x in xs), any(x > 1 for x in xs),
           all(x < n for x in xs)]
    # each kind of iterable the consumer takes the iterator of
    out.append(list(x for x in tuple(xs)))
    out.append(list(x for x in range(n)))
    out.append(sorted(x for x in d))
    out.append(sorted(list(v for v in d.values())))
    out.append(sorted(list(k for k in d.items())))
    out.append(sorted(set(x for x in set(xs))))
    out.append(list(x for x in ListKind(xs)))
    out.append(list(x for x in iter(xs)))
    # one over another
    out.append(list(y + 1 for y in (x * 3 for x in xs)))
    return out


# where CPython keeps an index for these rather than making an iterator
def strings(n):
    return [list(c for c in 'abc'[:n]), list(b for b in b'abc'[:n])]


def closures(n):
    base = [10, 20, 30, 40]
    out = [list(base[i] + i for i in range(n)),
           set(base[i] - i for i in range(n)),
           tuple(base[i] for i in list(range(n))[::-1])]
    return out


def not_taken_early(n):
    # an iterable whose __iter__ is Python, one that raises, and one that
    # isn't iterable: the generator is CPython's, made at the call
    LOG.clear()
    out = [list(x for x in PyIter(n)), list(LOG)]
    out.append(run(lambda: list(x for x in Refuses())))
    out.append(run(lambda: any(x for x in n)))
    out.append(run(lambda: tuple(x for x in None)))
    return out


def raiser(x):
    if x == 2:
        raise KeyError(x)
    return x


def body_raises(n):
    xs = list(range(n))
    return [run(lambda: list(raiser(x) for x in xs)),
            run(lambda: set(raiser(x) for x in xs)),
            run(lambda: any(raiser(x) > 5 for x in xs)),
            run(lambda: list(1 // (x - 1) for x in xs))]


def body_frames(n):
    return list(sys._getframe(1).f_code.co_name for x in range(n))


def stops_early(n):
    # any() and all() stop at the first answer, dropping the generator
    # paused, its iterable's items still held
    LOG.clear()
    ws = [Noted('w%d' % i) for i in range(n)]
    out = [any(w.name == 'w1' for w in ws), all(w.name != 'w1' for w in ws),
           list(LOG)]
    del ws
    out.append(list(LOG))
    return out


def iterable_dies(n):
    # the iterable made only for the generator dies with it, as in CPython
    LOG.clear()
    out = [list(len(w.name) for w in [Noted('a'), Noted('b')][:n]),
           list(LOG)]
    LOG.clear()
    out.append(any(True for w in [Noted('c'), Noted('d')]))
    out.append(list(LOG))
    return out


CASES = [
    ('consumers', [(0,), (1,), (3,)], {}),
    ('strings', [(0,), (3,)], {}),
    ('closures', [(3,)], {}),
    ('not_taken_early', [(3,)], {}),
    ('body_raises', [(4,)], {}),
    ('body_frames', [(2,)], {}),
    ('stops_early', [(3,)], {}),
    ('iterable_dies', [(2,)], {}),
]
