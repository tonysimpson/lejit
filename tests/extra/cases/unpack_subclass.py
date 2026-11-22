# UNPACK_SEQUENCE takes its fast paths only for exact tuples and lists, as
# ceval does: a subclass's __iter__ is used.


class T(tuple):
    def __iter__(self):
        return iter(('it', 'er'))


class L(list):
    def __iter__(self):
        return iter(('it', 'er'))


class Plain(tuple):
    pass


def unpack(s):
    a, b = s
    return a, b


def unpack_made(x):
    a, b = T((x, x))
    c, d = L([x, x])
    e, f = Plain((x, 1))
    return a, b, c, d, e, f


CASES = [
    ('unpack', '[(T((1, 2)),), (L([1, 2]),), (T((1, 2, 3)),), (Plain((1, 2)),)]', {}),
    ('unpack', [((1, 2),), ([3, 4],), ((1, 2, 3),)], {}),
    ('unpack_made', [(5,), ('s',)], {}),
]
