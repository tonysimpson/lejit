# `in` on a tuple goes through the meta generated from tupleobject.c's
# tuplecontains (D78): a tuple of known length (constant or virtual) is
# unrolled, one comparison per item with a run-time test between them;
# any other tuple calls CPython's slot. Also PySequence_Tuple/List and
# _PySequence_IterSearch, generated from abstract.c, and the int index of
# list and tuple subscripts (KNOWN_BUGS F102).


def in_const(x):
    return x in (1, 2, 3)


def in_const_str(x):
    return x in ('a', 'bc', 'd')


def in_virtual(x, a, b):
    return x in (a, b)


def in_runtime(x, t):
    return x in t


def not_in_const(x):
    return x not in (1, 2.5, 'z')


class Eq:
    def __init__(self, v):
        self.v = v

    def __eq__(self, o):
        if self.v == 'boom':
            raise ValueError('boom')
        return self.v == o

    __hash__ = object.__hash__


class T(tuple):
    pass


class CountEq:
    n = 0

    def __eq__(self, o):
        CountEq.n += 1
        return False

    __hash__ = object.__hash__


def eq_raises(x):
    try:
        return Eq(x) in (1, 2, 3)
    except ValueError as e:
        return 'raised %s' % e


def eq_called_once_per_item():
    CountEq.n = 0
    r = CountEq() in (1, 2, 3)
    return r, CountEq.n


def in_subclass(x):
    return x in T((1, 2, 3))


def in_long_const(x):
    return x in (0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16)


def in_nested(x):
    return x in ((1, 2), (3, 4))


def seq_tuple(x):
    return tuple(x)


def seq_list(x):
    return list(x)


def iter_search(x, it):
    return x in it


class It:
    def __init__(self, n):
        self.n = n

    def __iter__(self):
        return iter(range(self.n))


def index_big(seq, i):
    try:
        return seq[i]
    except (IndexError, OverflowError) as e:
        return '%s: %s' % (type(e).__name__, e)


def store_big(i):
    l = [1, 2, 3]
    try:
        l[i] = 0
    except (IndexError, OverflowError) as e:
        return '%s: %s' % (type(e).__name__, e)
    return l


CASES = [
    ('in_const', [(0,), (1,), (2,), (3,), (4,), (2.0,), ('a',), (None,)], {}),
    ('in_const_str', [('a',), ('bc',), ('x',), (1,)], {}),
    ('in_virtual', [(1, 1, 2), (2, 1, 2), (3, 1, 2), (2.0, 1, 2),
                    ('a', 'a', 1)], {}),
    ('in_runtime', [(1, (1, 2)), (5, (1, 2)), (1, ()), ('a', ('b', 'a'))], {}),
    ('not_in_const', [(1,), (2.5,), ('z',), (3,)], {}),
    ('eq_raises', [(1,), (3,), (5,), ('boom',)], {}),
    ('eq_called_once_per_item', [(), (), ()], {}),
    ('in_subclass', [(1,), (3,), (4,)], {}),
    ('in_long_const', [(0,), (16,), (17,)], {}),
    ('in_nested', [((1, 2),), ((3, 4),), ((1, 3),)], {}),
    ('seq_tuple', [((1, 2),), ([1, 2],), ('ab',), (range(3),)], {}),
    ('seq_list', [((1, 2),), ([1, 2],), ('ab',), (range(3),)], {}),
    ('iter_search', "[(1, It(3)), (5, It(3)), (0, iter([0])), (1, {1: 2})]",
     {}),
    ('index_big', [((1, 2), 1), ([1, 2], -1), ((1, 2), 5), ([1, 2], 5),
                   ((1, 2), 2 ** 70), ([1, 2], -2 ** 70)], {}),
    ('store_big', [(1,), (5,), (2 ** 70,)], {}),
]
