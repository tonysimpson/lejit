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
