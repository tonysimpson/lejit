# Tuples, lists, dicts, subscripts, iteration.

def make_tuple(a, b):
    return (a, b, a + b)

def unpack(t):
    a, b = t
    return b - a

def swap(a, b):
    a, b = b, a
    return a - b

def make_list(a):
    return [a, a * 2, a * 3]

def index_list(l, i):
    return l[i]

def index_tuple(t, i):
    return t[i]

def store_subscr(l, i, v):
    l[i] = v
    return l

def for_range(n):
    t = 0
    for i in range(n):
        t += i
    return t

def for_list(l):
    t = 0
    for x in l:
        t += x
    return t

def for_tuple(tp):
    t = 0
    for x in tp:
        t += x
    return t

def list_append(n):
    l = []
    for i in range(n):
        l.append(i * i)
    return l

def dict_get(d, k):
    return d[k]

def make_dict(a):
    return {'a': a, 'b': a + 1}

def slicing(l):
    return l[1:3]

def contains(l, x):
    return x in l

def list_comp(n):
    return [i * 2 for i in range(n)]

CASES = [
    ('make_tuple', [(1, 2), (1.5, 2.5)], {}),
    ('unpack', [((1, 5),), ([3, 4],)], {}),
    ('swap', [(1, 2)], {}),
    ('make_list', [(2,), ('x',)], {}),
    ('index_list', [([1, 2, 3], 0), ([1, 2, 3], -1), ([1, 2, 3], 5)], {}),
    ('index_tuple', [((1, 2, 3), 1), ((1, 2, 3), 3)], {}),
    ('store_subscr', [([1, 2, 3], 1, 9)], {}),
    ('for_range', [(0,), (10,), (1000,)], {}),
    ('for_list', [([],), ([1, 2, 3],)], {}),
    ('for_tuple', [((),), ((1, 2, 3),)], {}),
    ('list_append', [(0,), (5,)], {}),
    ('dict_get', [({'a': 1}, 'a'), ({'a': 1}, 'b')], {}),
    ('make_dict', [(1,)], {}),
    ('slicing', [([1, 2, 3, 4],), ((1, 2, 3, 4),)], {}),
    ('contains', [([1, 2], 2), ([1, 2], 3)], {}),
    ('list_comp', [(0,), (4,)], {}),
]

def dict_literals(a, b):
    one = {a: b}
    two = {'x': a, 'y': b}
    computed = {a: 1, b: 2}
    comp = {k: k * 2 for k in range(3)}
    return one, two, computed, comp

def unhashable_key(a):
    return {a: 1}

CASES += [
    ('dict_literals', [(1, 2), ('p', 'q')], {}),
    ('unhashable_key', [([1],)], {}),
]


def unpack_empty(x):
    # UNPACK_SEQUENCE 0: the helper's output array is empty (a run-time
    # call with an empty 'A' array read a vinfo that isn't there)
    [] = x
    [] = iter(x)
    return 'ok'


CASES += [
    ('unpack_empty', [([],), ((),), ('',)], {}),
]


def unpack_errors(x):
    # ceval's unpack_iterable messages (KNOWN_BUGS F30), for tuples, lists,
    # iterators and non-iterables, too few and too many
    out = []
    for f in (unpack0, unpack2):
        for v in (x, list(x), tuple(x), iter(x)):
            try:
                out.append(f(v))
            except (ValueError, TypeError) as e:
                out.append((type(e).__name__, str(e)))
    try:
        unpack2(5)
    except TypeError as e:
        out.append(str(e))
    return out


def unpack0(v):
    [] = v
    return 0


def unpack2(v):
    a, b = v
    return a, b


CASES += [
    ('unpack_errors', [((),), ((1,),), ((1, 2),), ((1, 2, 3),)], {}),
]


# list.__getitem__ is a C method as well as list's mp_subscript slot, so it
# can receive a subclass instance whose own __getitem__ is Python code: the
# meta must not dispatch on the instance's type (KNOWN_BUGS F41; it recursed
# into the override, CPython's test_code test_closure_injection)
class OwnGetitem(list):
    def __getitem__(self, i):
        return ('own', super().__getitem__(i))


class OwnLen(list):
    def __len__(self):
        return 100


def super_getitem(i):
    return OwnGetitem([5, 6, 7])[i]


def list_getitem_on_subclass(i):
    return (list.__getitem__(OwnGetitem([5, 6, 7]), i),
            list.__getitem__(OwnLen([5, 6, 7]), i))


CASES += [
    ('super_getitem', [(0,), (-1,), (slice(1, None),)], {}),
    ('list_getitem_on_subclass', [(1,), (-1,), (slice(None, 2),)], {}),
]


# PEP 560 (F39) with a heap metaclass: every heap type has a
# tp_as_sequence, with no sq_item, so Ni took the sequence path and raised
# "'Meta' object does not support indexing" (KNOWN_BUGS F42)
class WithClassGetitem:
    def __class_getitem__(cls, item):
        return (cls.__name__, item)


class Meta(type):
    pass


class MetaClassGetitem(metaclass=Meta):
    def __class_getitem__(cls, item):
        return ('meta', item)


def class_getitem(k):
    return WithClassGetitem[k], MetaClassGetitem[k]


def class_not_subscriptable(k):
    return int[k]


CASES += [
    ('class_getitem', '[(1,), (str,)]', {}),
    ('class_not_subscriptable', [(1,)], {}),
]
