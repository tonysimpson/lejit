# Star and double-star unpacking in calls and displays:
# BUILD_MAP_UNPACK_WITH_CALL, BUILD_MAP_UNPACK and BUILD_SET_UNPACK, a lone
# ** mapping (CALL_FUNCTION_EX), with
# BUILD_TUPLE_UNPACK(_WITH_CALL) and BUILD_LIST_UNPACK fed generators,
# iterators, dicts and other mappings, and every ceval message.
import collections


def target(a=0, b=0, *rest, c=3, **kw):
    return (a, b, rest, c, sorted(kw.items()))


class Mapping:
    def __init__(self, **d):
        self.d = d

    def keys(self):
        return list(self.d)

    def __getitem__(self, k):
        return self.d[k]


class BadMapping:
    # __getitem__ raises KeyError for a key that keys() lists
    def keys(self):
        return ['c']

    def __getitem__(self, k):
        raise KeyError('nope')


class Dsub(dict):
    def keys(self):
        return ['x']


class Point:
    def __init__(self, x=0, y=0, **extra):
        self.p = (x, y, sorted(extra.items()))


def gen(n):
    for i in range(n):
        yield i


def explode(n):
    yield n
    raise ValueError('from the generator')


# f(x, *a) and f(*a, *b): BUILD_TUPLE_UNPACK_WITH_CALL
def star_after_positional(x, a):
    return target(x, *a)


def two_stars(a, b):
    return target(*a, *b)


def star_generator(n):
    return target(n, *gen(n)), target(*gen(2), *iter([7, 8]))


# f(**a, k=1), f(k=1, **a), f(**a, **b): BUILD_MAP_UNPACK_WITH_CALL
def star_star_kw(a):
    return target(**a, c=1)


def kw_star_star(a):
    return target(c=1, **a)


def two_star_stars(a, b):
    return target(**a, **b)


def everything(x, a, b, m):
    return target(x, *a, *b, **m, c=x, z=2)


class Obj:
    def meth(self, a=0, **kw):
        return a, sorted(kw.items())


def method_call(o, a):
    # the messages name the method
    return o.meth(**a, k=1), o.meth(*(), *a)


def class_call(a):
    return Point(**a, y=5).p


def loop(n):
    t = 0
    d = {'b': 1}
    for i in range(n):
        r = target(i, *(i, i), **d, c=i)
        t += r[0] + r[1] + len(r[2]) + r[3]
    return t


# displays: (*a, *b), [*a, 1], {**a, 'k': 1}, {**a}, {*a, *b}
def displays(a, b):
    return (*a, *b), [*a, 1], [*b, *a]


def dict_display(a, b):
    d1 = {**a, 'k': 1}
    d2 = {**a}
    d3 = {'k': 0, **a, **b}
    return sorted(d1.items()), sorted(d2.items()), sorted(d3.items())


def set_display(a, b):
    return (sorted({*a, *b}, key=repr), sorted({*a, 1, *b}, key=repr),
            type({*a, *b}).__name__)


def dict_display_type(a):
    d = {**a}
    d['new'] = 1
    return type(d).__name__, len(d)


# errors, as ceval words them
def err_star(x):
    return target(1, *x)


def err_star_star(x):
    return target(**x, c=1)


class DupMapping:
    # keys() lists a key twice
    def keys(self):
        return ['a', 'a']

    def __getitem__(self, k):
        return 1


def err_star_star_alone(x):
    return target(**x)


def err_multiple(a):
    return target(**a, c=2)


def err_multiple_two(a, b):
    return target(**a, **b)


def err_class(x):
    return Point(**x, y=1).p


def err_map_display(x):
    return {**x, 'k': 1}


def err_set_display(x):
    return sorted({*x, 1}, key=repr)


def err_tuple_display(x):
    return (*x, 1)


def err_list_display(x):
    return [1, *x]


def err_generator(n):
    return target(*explode(n), *explode(n))


def err_set_generator(n):
    return {*explode(n), *gen(2)}


def err_after_some(a):
    # the first parts are fine, a later one fails
    return target(**{'a': 1}, **a), {**{'a': 1}, **a}


CASES = [
    ('star_after_positional', "[(1, (2, 3)), (1, [2]), (1, 'xy'), (1, range(3))]", {}),
    ('two_stars', "[((1,), (2,)), ([1, 2], iter([3])), ('a', {'b': 0})]", {}),
    ('star_generator', [(3,), (0,)], {}),
    ('star_star_kw', "[({'a': 1},), ({},), (Mapping(b=4, z=9),), "
                     "(collections.OrderedDict(a=2),), (Dsub(x=1, y=2),)]", {}),
    ('kw_star_star', "[({'a': 1},), (Mapping(a=5),)]", {}),
    ('two_star_stars', "[({'a': 1}, {'b': 2}), ({}, Mapping(c=4)), "
                       "(Mapping(z=1), collections.OrderedDict(y=2))]", {}),
    ('everything', "[(1, (2,), [3, 4], {'q': 0}), (5, gen(2), iter(()), Mapping(w=1))]", {}),
    ('method_call', "[(Obj(), {'a': 1}), (Obj(), {'k': 2}), (Obj(), 5)]", {}),
    ('class_call', "[({'x': 1},), ({'x': 1, 'q': 3},)]", {}),
    ('loop', [(50,)], {}),
    ('displays', "[((1, 2), [3]), ('ab', range(2)), (gen(3), iter([9]))]", {}),
    ('dict_display', "[({'a': 1}, {'b': 2}), ({'k': 5}, Mapping(k=6)), "
                     "(Dsub(x=1, y=2), collections.OrderedDict(z=3))]", {}),
    ('set_display', "[((1, 2), [2, 3]), ('ab', 'bc'), (gen(3), frozenset([5]))]", {}),
    ('dict_display_type', "[({'a': 1},), (Mapping(a=1),)]", {}),
    ('err_star', "[(5,), (None,), ((2, 3),)]", {}),
    ('err_star_star', "[(5,), ([1],), ({'a': 1},), (BadMapping(),), ({1: 2},)]", {}),
    ('err_star_star_alone', "[(5,), (BadMapping(),), (DupMapping(),), (Mapping(a=1),)]", {}),
    ('err_multiple', "[({'c': 1},), (Mapping(c=1),), ({'a': 1},)]", {}),
    ('err_multiple_two', "[({'a': 1}, {'a': 2}), ({1: 2}, {1: 3}), "
                         "({'a': 1}, Mapping(a=2)), ({}, 7)]", {}),
    ('err_class', "[(5,), ({'y': 2},)]", {}),
    ('err_map_display', "[(5,), ([1],), (BadMapping(),), ({'a': 1},)]", {}),
    ('err_set_display', "[(5,), ([[1]],), ('ab',)]", {}),
    ('err_tuple_display', "[(5,), ('ab',)]", {}),
    ('err_list_display', "[(None,), ((1,),)]", {}),
    ('err_generator', [(1,)], {}),
    ('err_set_generator', [(1,)], {}),
    ('err_after_some', "[({'a': 2},), ({'b': 2},), (3,)]", {}),
]
