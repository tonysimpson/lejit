# A class's __getitem__, __setitem__ and __delitem__ called from compiled
# code through their slot functions: plain functions are called directly,
# anything else is the slot function. Replaced or deleted methods, a
# missing __delitem__, errors raised inside, results __setitem__ returns,
# deep recursion and a callable that isn't a function.
import functools
import traceback


class Grid:
    def __init__(self, w):
        self.w = w
        self.data = [0.0] * (w * w)

    def __getitem__(self, xy):
        x, y = xy
        if not (0 <= x < self.w and 0 <= y < self.w):
            raise IndexError(xy)
        return self.data[y * self.w + x]

    def __setitem__(self, xy, v):
        x, y = xy
        self.data[y * self.w + x] = v
        return 'ignored'


def smooth(n):
    g = Grid(n)
    for i in range(n):
        g[i, i] = float(i)
    for y in range(1, n - 1):
        for x in range(1, n - 1):
            g[x, y] = 0.25 * (g[x - 1, y] + g[x + 1, y] + g[x, y - 1] +
                              g[x, y + 1])
    return g.data


def out_of_range():
    g = Grid(2)
    try:
        return g[5, 0]
    except IndexError as e:
        return 'IndexError', e.args, \
            [f.name for f in traceback.extract_tb(e.__traceback__)]


class Store:
    def __init__(self):
        self.log = []

    def __setitem__(self, k, v):
        self.log.append(('set', k, v))

    def __getitem__(self, k):
        return ('get', k)


def delete(s, k):
    del s[k]
    return s.log


def store(s, k, v):
    s[k] = v
    return s.log


def get(s, k):
    return s[k]


def attempt(f, *args):
    try:
        return f(*args)
    except (AttributeError, TypeError, KeyError) as e:
        return '%s: %s' % (type(e).__name__, e)


def missing_delitem():
    s = Store()
    r = [attempt(delete, s, 1), store(s, 1, 2)[:]]
    Store.__delitem__ = lambda self, k: self.log.append(('del', k))
    r.append(delete(s, 3)[:])
    del Store.__delitem__
    r.append(attempt(delete, s, 4))
    return r


class Swap:
    def __getitem__(self, k):
        return 'first'


def replaced_method():
    s = Swap()
    r = [get(s, 0)]
    Swap.__getitem__ = lambda self, k: 'second'
    r.append(get(s, 0))
    Swap.__getitem__ = staticmethod(lambda k: ('static', k))
    r.append(get(s, 1))
    Swap.__getitem__ = functools.partial(lambda self, k: ('partial', k))
    r.append(attempt(get, s, 2))
    del Swap.__getitem__
    r.append(attempt(get, s, 3))
    return r


def instance_attribute_ignored():
    s = Store()
    s.__getitem__ = lambda k: 'instance'
    return get(s, 'k')


class Deep:
    def __getitem__(self, n):
        return n if n == 0 else self[n - 1] + 1


def deep(n):
    try:
        return Deep()[n]
    except RecursionError:
        return 'RecursionError'


class Missing(dict):
    def __missing__(self, k):
        return ('missing', k)


class Over(dict):
    def __getitem__(self, k):
        return ('over', dict.get(self, k))

    def __setitem__(self, k, v):
        dict.__setitem__(self, k, v * 2)

    def __delitem__(self, k):
        dict.__setitem__(self, k, None)


def dict_subclasses():
    m, o = Missing(a=1), Over(a=1)
    r = [get(m, 'a'), get(m, 'b'), get(o, 'a')]
    store(Store(), 0, 0)
    o['b'] = 3
    del o['a']
    r.append(sorted(dict.items(o)))
    return r


CASES = [
    ('smooth', [(6,), (8,)], {}),
    ('out_of_range', [()], {}),
    ('missing_delitem', [()], {}),
    ('replaced_method', [()], {}),
    ('instance_attribute_ignored', [()], {}),
    ('deep', [(10,), (100000,)], {}),
    ('dict_subclasses', [()], {}),
]
