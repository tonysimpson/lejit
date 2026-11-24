# Iterators of lists, made in compiled code and made real when passed to a
# call (a generator expression's argument): CPython's list_iterator (type,
# next, __length_hint__, __reduce__), and run-time list iterators iterated
# by compiled code.


def state(it):
    return [type(it).__name__, it.__length_hint__(), it.__reduce__()[1:]]


def argument(gen):
    return gen.gi_frame.f_locals['.0']


def forced(n, taken):
    items = [x * 2 for x in range(n)]
    it = argument(x for x in items)
    out = [type(it).__name__]
    for _ in range(taken):
        out.append(next(it, 'end'))
    out.append(state(it))
    out.append(next(it, 'end'))
    out.append(list(it))
    out.append(state(it))
    return out


def exhausted(a, b):
    items = [a, b]
    gen = (x for x in items)
    it = gen.gi_frame.f_locals['.0']
    out = list(gen)
    out.append(state(it))
    out.append(next(it, 'end'))
    return out


def growing(n):
    items = [0]
    it = iter(items)
    out = []
    for x in it:
        out.append(x)
        if x < n:
            items.append(x + 1)
        out.append(it.__length_hint__())
    items.append('late')
    out.append(next(it, 'end'))
    return out


def split_words(s):
    it = argument(w for w in s.split())
    first = next(it, 'end')
    return [first, state(it), list(it)]


def tuples(n, r):
    indices = list(range(n))
    return tuple(indices[i] * 2 for i in indices[:r])


def run_time(it):
    out = []
    for x in it:
        out.append(x)
    out.append(state(it))
    return out


def make(items, index):
    it = iter(items)
    it.__setstate__(index)
    return it


CASES = [
    ('forced', [(3, 0), (3, 2), (3, 4), (0, 0)], {}),
    ('exhausted', [(1, 2), ('x', None)], {}),
    ('growing', [(0,), (3,)], {}),
    ('tuples', [(4, 2), (3, 3), (2, 0)], {}),
    ('split_words', [('a b c',), ('',), ('one',)], {}),
    ('run_time', "[(iter([1, 2, 3]),), (make([1, 2, 3], 1),),"
                 " (make([1, 2, 3], -1),), (make([1, 2, 3], 9),)]", {}),
]
