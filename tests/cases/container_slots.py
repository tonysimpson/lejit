# Slots generated from the container files (D84): tuple's tp_richcompare
# (a field of the type, not of a slot table) unrolled for tuples of known
# length, CPython's slot otherwise; len() of a list, dict and bytes read
# as a field through policy_table.TYPES' descriptors.


def tup_eq(a, b):
    return (a, b) == (1, 2)


def tup_ops(a, b):
    x = (a, b)
    y = (b, a)
    return [x == y, x != y, x < y, x <= y, x > y, x >= y]


def tup_short(a):
    return [(a,) < (a, 1), (a, 1) > (a,), (a,) == (a, 1), (a,) != (a, 1),
            () == (), () < ()]


def tup_runtime(t, u):
    return [t == u, t != u, t < u, t >= u]


def tup_mixed(a):
    return (a, 'x') == (a, 'x'), (a, 'x') < (a, 'y')


class Eq:
    def __init__(self, v):
        self.v = v

    def __eq__(self, o):
        if self.v == 'boom':
            raise ValueError('boom')
        return isinstance(o, Eq) and self.v == o.v

    def __lt__(self, o):
        return self.v < o.v

    __hash__ = object.__hash__


def tup_user(a, b):
    try:
        return [(Eq(a), 1) == (Eq(b), 1), (Eq(a),) < (Eq(b),)]
    except ValueError as e:
        return 'ValueError: %s' % e


def tup_uncomparable(a):
    try:
        return (a, 1) < ('x', 1)
    except TypeError as e:
        return 'TypeError: %s' % e


class T(tuple):
    pass


def tup_subclass(a):
    return T((a, 2)) == (a, 2), (a, 2) == T((a, 2)), T((a,)) < (a, 0)


def tup_notimpl(a):
    return (a, 1) == [a, 1], (a, 1) != None


def lens(l, d, b):
    return len(l), len(d), len(b)


def len_mutated(n):
    l = []
    d = {}
    out = []
    for i in range(n):
        l.append(i)
        d[i] = i
        out.append((len(l), len(d)))
    del l[0]
    del d[0]
    out.append((len(l), len(d)))
    return out


class L(list):
    def __len__(self):
        return 99


class D(dict):
    pass


class B(bytes):
    pass


def len_subclass(l, d, b):
    return len(l), len(d), len(b)


CASES = [
    ('tup_eq', [(1, 2), (2, 1), (1, 2.0), ('a', 2)], {}),
    ('tup_ops', [(1, 2), (2, 1), (1, 1), ('a', 'b')], {}),
    ('tup_short', [(0,), ('a',), (None,)], {}),
    ('tup_runtime', [((1, 2), (1, 2)), ((1, 2), (1, 3)), ((), (1,)),
                     ((1, 2, 3), (1, 2))], {}),
    ('tup_mixed', [(1,), ('q',)], {}),
    ('tup_user', [(1, 1), (1, 2), ('boom', 1)], {}),
    ('tup_uncomparable', [(1,), ('x',)], {}),
    ('tup_subclass', [(1,), ('a',)], {}),
    ('tup_notimpl', [(1,), (None,)], {}),
    ('lens', [([], {}, b''), ([1, 2], {1: 2}, b'abc'),
              (list(range(1000)), dict.fromkeys(range(100)), b'x' * 70000)], {}),
    ('len_mutated', [(0,), (3,), (10,)], {}),
    ('len_subclass', "[(L([1]), D(a=1), B(b'xy')), (L(), D(), B())]", {}),
]
