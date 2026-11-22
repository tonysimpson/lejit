# An immutable heap type (array.array from 3.10, Py_TPFLAGS_IMMUTABLETYPE)
# is read as a static type is: its slots and its instances' class are
# fixed (pobject.h:Psyco_TypeMutable). Its subclasses defined in Python
# stay mutable: a method replaced on one is seen by compiled code.
from array import array


def subscripts(n):
    a = array('d', [0.5]) * n
    ix = array('i', range(n))
    s = 0.0
    for i in range(n):
        a[ix[i]] = a[i] * 2.0 + i
        s += a[ix[n - 1 - i]]
    return s, bool(a), bool(array('d')), [x for x in a][:3]


def refused():
    out = []
    try:
        array.__getitem__ = lambda self, i: 0
    except TypeError:
        out.append('type attribute refused')
    a = array('i', [1, 2])

    class Other:
        pass
    try:
        a.__class__ = Other
    except TypeError:
        out.append('class refused')
    out.append(a[1])
    return out


def doubled(self, i):
    return 2 * array.__getitem__(self, i)


class Doubled(array):
    pass


def read_all(a):
    return [a[i] for i in range(len(a))]


def subclass_replaced():
    Doubled.__getitem__ = doubled
    d = Doubled('i', [1, 2, 3])
    r = [read_all(d)]
    Doubled.__getitem__ = lambda self, i: -array.__getitem__(self, i)
    r.append(read_all(d))
    del Doubled.__getitem__
    r.append(read_all(d))
    r.append(read_all(array('i', [4, 5])))
    return r


CASES = [
    ('subscripts', [(10,), (50,)], {}),
    ('refused', [()], {}),
    ('subclass_replaced', [()], {}),
]
