# Stores in __init__ that add a name to a new instance. Where a class's
# instances keep their attributes in values indexed by the class's keys,
# a compiled store into an empty slot fills it and appends its index to
# the values' insertion order (x64/ipyencoding.c), which __dict__ shows;
# anything else goes to CPython's store.
import gc


class Pair:
    def __init__(self, x, y):
        self.x = x
        self.y = y


class Either:
    def __init__(self, first, a, b):
        if first:
            self.a = a
            self.b = b
        else:
            self.b = b
            self.a = a


def insertion_order(n):
    out = []
    for i in range(n):
        p = Pair(i, -i)
        e = Either(i % 2, i, -i)
        out.append((list(p.__dict__.items()), list(e.__dict__), e.a, e.b))
    return out


class Growing:
    def __init__(self, i):
        self.a = i
        if i % 3 == 0:
            self.c = i * 2
        self.b = -i
        if i % 4 == 1:
            self.d = 'd'


def shared_keys_grow(n):
    # each new name grows the class's keys; later instances fill them
    out = []
    for i in range(n):
        g = Growing(i)
        out.append(list(g.__dict__.items()))
    return out


class Wide:
    def __init__(self, i):
        for k in range(40):
            setattr(self, 'w%d' % k, k)
        self.x = i
        self.y = i + 1


def keys_full(n):
    # past the keys' 30 names the rest go to a dict of the instance's own
    out = []
    for i in range(n):
        w = Wide(i)
        out.append((w.x, w.y, list(w.__dict__)[-3:], len(w.__dict__)))
    return out


class Seen:
    def __init__(self, i, look):
        self.a = i
        if look:
            self.d = self.__dict__
        self.b = i + 1
        self.c = i + 2


def dict_made_in_init(n):
    # a dict made between stores holds what follows too
    out = []
    for i in range(n):
        s = Seen(i, i % 2)
        d = s.__dict__
        out.append((list(d), d is s.__dict__,
                    'd' in d and d['d'] is d, s.b, s.c))
    return out


def dict_made_later(n):
    out = []
    for i in range(n):
        p = Pair(i, str(i))
        d = p.__dict__
        p.z = i * 3
        out.append((list(d.items()), d is p.__dict__, sorted(p.__dict__)))
    return out


class Deleting:
    def __init__(self, i):
        self.a = i
        self.b = i + 1
        del self.a
        self.c = i + 2
        self.a = i + 3


def delete_then_add(n):
    out = []
    for i in range(n):
        o = Deleting(i)
        out.append(list(o.__dict__.items()))
    return out


class Again:
    def __init__(self, i):
        self.a = i
        self.b = [i]


def init_twice(n):
    # a second __init__ replaces values that are there, in the same order
    out = []
    for i in range(n):
        o = Again(i)
        o.__init__(i + 10)
        out.append(list(o.__dict__.items()))
    return out


class Slotted:
    __slots__ = ('s', '__dict__')

    def __init__(self, i):
        self.s = i
        self.x = i + 1
        self.y = i + 2


class SlottedOnly:
    __slots__ = ('s', 't')

    def __init__(self, i):
        self.t = i
        self.s = i + 1


class SlotsBelow(SlottedOnly):
    def __init__(self, i):
        SlottedOnly.__init__(self, i)
        self.y = i + 3
        self.x = i + 2


def slots(n):
    out = []
    for i in range(n):
        a = Slotted(i)
        b = SlottedOnly(i)
        c = SlotsBelow(i)
        out.append((a.s, list(a.__dict__.items()), b.s, b.t,
                    hasattr(b, '__dict__'), c.s, c.t,
                    list(c.__dict__.items())))
    return out


class Other:
    pass


class Moving:
    def __init__(self, i):
        self.a = i
        if i % 2:
            self.__class__ = Other
        self.b = i + 1


def class_changed_in_init(n):
    # a new class leaves the values no longer the attributes
    out = []
    for i in range(n):
        m = Moving(i)
        out.append((type(m).__name__, list(m.__dict__.items())))
    return out


class Replaced:
    def __init__(self, i):
        self.a = i
        if i % 2:
            self.__dict__ = {'z': 0}
        self.b = i + 1


def dict_replaced_in_init(n):
    out = []
    for i in range(n):
        r = Replaced(i)
        out.append(list(r.__dict__.items()))
    return out


class Holder:
    def __init__(self, v):
        self.v = v
        self.me = self


def values_owned(n):
    # each stored value is held once by the instance: freeing it frees them
    gc.collect()
    out = []
    for i in range(n):
        h = Holder([i])
        out.append((h.v, h.me is h, gc.is_tracked(h)))
        del h
    gc.collect()
    return out


CASES = [
    ('insertion_order', [(6,)], {}),
    ('shared_keys_grow', [(12,)], {}),
    ('keys_full', [(3,)], {}),
    ('dict_made_in_init', [(4,)], {}),
    ('dict_made_later', [(3,)], {}),
    ('delete_then_add', [(3,)], {}),
    ('init_twice', [(3,)], {}),
    ('slots', [(3,)], {}),
    ('class_changed_in_init', [(4,)], {}),
    ('dict_replaced_in_init', [(4,)], {}),
    ('values_owned', [(3,)], {}),
]
