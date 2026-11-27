# Pieces lists (plistobject.c): slices of an exact list, and concatenations
# of them with lists and short literal lists, made only where they are used.
# Stored into a slice of an exact list, their items go straight in. Results,
# the order objects die in, sizes, lists changed while a slice of them is
# pending, subclasses and bounds of other types.
import sys

from ref_tracer import count_made, made

BOUNDS = [None, -1000, -9, -8, -5, -1, 0, 1, 3, 7, 8, 9, 1000, 2 ** 70,
          -2 ** 70]


def reverse_prefix(n):
    # fannkuch's flip
    out = []
    for k in range(-2, n + 2):
        perm = list(range(n))
        perm[:k + 1] = perm[k::-1]
        out.append(perm)
    return out


def rotate(n):
    # nqueens' rotation
    out = []
    for i in range(-2, n + 2):
        indices = list(range(n))
        indices[i:] = indices[i + 1:] + indices[i:i + 1]
        out.append(indices)
    return out


def move_to_front(n):
    # pyflate's move to front
    out = []
    for c in range(-2, n + 2):
        l = list(range(n))
        l[:] = l[c:c + 1] + l[0:c] + l[c + 1:]
        out.append(l)
    return out


def bounds(n):
    out = []
    l = list(range(n))
    for a in BOUNDS:
        for b in BOUNDS:
            out.append((l[a:b], l[a:b:-1], l[a:] + l[:b]))
    return out


def stores(n):
    # into other lists, of other lengths
    out = []
    src = list('abcdefgh'[:n])
    for a in BOUNDS:
        for b in BOUNDS:
            t = list(range(n))
            t[a:b] = src[1:] + src[:1]
            u = list(range(n))
            u[a:b] = src[::-1]
            v = list(range(n))
            v[b:a] = src[a:b]
            out.append((t, u, v, len(t), len(u), len(v)))
    return out


def concatenations(n):
    out = []
    a = list(range(n))
    b = list('xyz')
    x = 'item'
    out.append(a[1:] + b)
    out.append(b + a[1:])
    out.append(a[1:] + [x])
    out.append([x, x] + a[:2])
    out.append(a[1:] + a[:1] + b[1:] + [x])
    out.append(a[1:] + a[:1] + b[1:] + b[:1] + a[2:])
    out.append(a[2:] + (b[1:] + [x]))
    out.append(a[::-1] + a[1:] + a)
    out.append((a[:2] + b) * 2)
    c = list(range(n))
    c[1:] = c[1:] + c[:1] + [x, x, x]
    out.append(c)
    return out


def sizes(n):
    out = []
    a = list(range(n))
    for i in range(n + 1):
        s = a[i:]
        r = a[i::-1]
        c = a[i:] + a[:i]
        t = list(range(n))
        t[:i] = a[i:] + a[:1]
        out.append((sys.getsizeof(s), sys.getsizeof(r), sys.getsizeof(c),
                    sys.getsizeof(t)))
    return out


LOG = []


class Noisy:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        LOG.append(self.name)


def death_order(n):
    del LOG[:]
    l = [Noisy(i) for i in range(n)]
    l[1:4] = l[0:1] + [Noisy('a'), Noisy('b')]
    LOG.append('same length')
    l[0:2] = l[3:4] + l[2:3] + [Noisy('c'), Noisy('d')]
    LOG.append('longer')
    l[:] = l[5:6] + l[0:1]
    LOG.append('shorter')
    del l
    return list(LOG)


class Evil:
    # its death changes the list it was taken out of
    def __init__(self, target):
        self.target = target

    def __del__(self):
        self.target.append('evil')

    def __repr__(self):
        return 'Evil'


def death_changes_source(n):
    out = []
    for i in range(n):
        l = list(range(n))
        l[i] = Evil(l)
        l[i:i + 1] = l[:1] + l[n - 1:]
        out.append(l)
    return out


def changed_by_store(n):
    out = []
    l = list(range(n))
    s = l[1:]
    l[0] = 'changed'
    r = l[2:] + l[:1]
    l[1] = r
    out.append((s, r, l))
    return out


def mutate(l):
    l.reverse()
    l.append('m')


def changed_by_call(n):
    l = list(range(n))
    s = l[1:] + l[:1]
    mutate(l)
    t = l[2:] + l[:2]
    return s, t, l


class Key:
    def __init__(self, l):
        self.l = l

    def __hash__(self):
        self.l.append('hashed')
        return 1


def changed_by_hash(n):
    l = list(range(n))
    d = {}
    d[Key(l)] = l[1:] + l[:1]
    d2 = {}
    d2[l[2:] + l[:2] and 0] = 0
    return list(d.values()), l


def across_branches(n):
    out = []
    l = list(range(n))
    for i in range(n):
        s = l[i:] + l[:i]
        if i % 2:
            l[0] = i
        out.append(s)
        out.append(l[:i] if i % 3 else l[i:][::-1])
    return out


def seen_by_frame(n):
    l = list(range(n))
    s = l[1:] + l[:1]
    return sys._getframe().f_locals['s'], s


class L(list):
    def __getitem__(self, k):
        return ('L', list.__getitem__(self, k))

    def __add__(self, o):
        return ('add', list(self), o)


class I(int):
    pass


class Index:
    def __init__(self, v):
        self.v = v

    def __index__(self):
        return self.v


def other_types(n):
    out = []
    l = L(range(n))
    m = list(range(n))
    out.append(l[1:])
    out.append(l[1:] + m[1:])
    out.append(m[1:] + l)
    out.append(m[I(1):I(3)] + m[:1])
    out.append(m[Index(1):Index(3)] + m[:1])
    out.append(m[1:3:2] + m[::-2])
    t = L(range(n))
    t[1:3] = m[2:] + m[:1]
    out.append(list(t))
    u = list(range(n))
    u[::2] = m[1:1 + len(u[::2])][::-1]
    out.append(u)
    out.append(tuple(range(n))[1:] + tuple(m[:1]))
    return out


def errors(n):
    out = []
    l = list(range(n))
    for v in [l[1:] + l[:1], l[::-1]]:
        try:
            l[::2] = v
        except ValueError as e:
            out.append(str(e))
        try:
            v + (1,)
        except TypeError as e:
            out.append(str(e))
    try:
        l[1:'x'] + l
    except TypeError as e:
        out.append(str(e))
    return out


class Sliced:
    # sees the slice object itself: a constant slice is one object here
    def __getitem__(self, k):
        return k

    def __setitem__(self, k, v):
        self.k = k


class SubList(list):
    pass


def constant_slices(n):
    # slices whose bounds are all constants, which the compiler may fold
    # into one slice object
    out = []
    for k in range(n):
        l = list(range(8))
        t = tuple(l)
        s = 'abcdefgh'
        b = b'abcdefgh'
        out.append((l[:], l[2:], l[:3], l[1:6:2], l[::-1], l[-3:]))
        out.append((t[1:4], s[2:], s[::2], b[:5], b[5:1:-1]))
        l[:] = l[3:] + l[:3]
        l[1:3] = 'xy'
        l[::2] = l[1::2]
        del l[:2]
        del l[::3]
        out.append(l)
        m = SubList(range(5))
        m[1:] = [9]
        out.append((m, m[:2], type(m[:2]).__name__))
        z = Sliced()
        z[1:2] = 0
        out.append((z[1:2], z[1:2] is z[1:2], z.k == z[1:2], z[::] ))
    return out


def constant_slices_made(n):
    # where the compiler folds a slice of constants into one slice object,
    # it is used, not made again: slices of a tuple, and steps the compiled
    # slicing leaves to the real slice; slices made per round (after the
    # first, which compiles). Without such constants CPython makes a slice
    # for each, and LeJIT may make fewer (it copies l[:] without one), so
    # there is nothing to compare.
    folded = any(type(c) is slice for c in constant_slices_made.__code__.co_consts)
    out = []
    l = list(range(9))
    t = tuple(l)
    s = 'abcdefghi'
    b = b'abcdefghi'
    for i in range(n):
        count_made(slice)
        for k in range(100):
            r = (t[1:], t[:3], t[::2], l[::2], l[1::3], s[::2], b[::3], l[:])
        count = made()
        out.append((r, count // 100 if i and folded and count is not None else None))
    return out


def reverse_prefix_made(n):
    # perm[k::-1] stored into perm makes no list of its own; per round,
    # after the first, which compiles
    perm = list(range(n))
    out = []
    for i in range(3):
        count_made(list)
        for j in range(10):
            for k in range(n):
                perm[:k + 1] = perm[k::-1]
        count = made()
        if i and count is not None:
            out.append(count)
    return out == [0] * len(out)


CASES = [
    ('constant_slices', [(1,), (3,)], {}),
    ('constant_slices_made', [(3,)], {}),
    ('reverse_prefix', [(0,), (1,), (5,), (12,)], {}),
    ('reverse_prefix_made', [(8,)], {'expected': [True]}),
    ('rotate', [(0,), (1,), (8,)], {}),
    ('move_to_front', [(0,), (3,), (9,)], {}),
    ('bounds', [(0,), (8,)], {}),
    ('stores', [(0,), (3,), (8,)], {}),
    ('concatenations', [(0,), (5,)], {}),
    ('sizes', [(0,), (9,)], {}),
    ('death_order', [(8,)], {}),
    ('death_changes_source', [(1,), (4,)], {}),
    ('changed_by_store', [(4,)], {}),
    ('changed_by_call', [(5,)], {}),
    ('changed_by_hash', [(5,)], {}),
    ('across_branches', [(6,)], {}),
    ('seen_by_frame', [(4,)], {}),
    ('other_types', [(5,)], {}),
    ('errors', [(5,)], {}),
]
