# Attribute stores (pobject.c:PsycoObject_SetAttr): on a heap type with an
# instance dict the store is guarded by the type's version tag and calls
# _PyObjectDict_SetItem directly (or a known-hash store for an unshared
# dict). A class changed after compiling must be seen, and
# data descriptors, missing dicts and errors must behave as in CPython.


class Hooked:
    pass


def setattr_added_later(n):
    # a __setattr__ added to the class after compiling must be called
    C = Hooked
    if '__setattr__' in C.__dict__:
        del C.__setattr__
    c = C()
    seen = []
    for i in range(n):
        c.x = i
        if i == 1:
            def hook(self, name, value, seen=seen):
                seen.append((name, value))
                object.__setattr__(self, name, value * 10)
            C.__setattr__ = hook
    return c.x, seen


def delattr_added_later(n):
    C = Hooked
    if '__delattr__' in C.__dict__:
        del C.__delattr__
    c = C()
    seen = []
    for i in range(n):
        c.y = i
        del c.y
        if i == 1:
            C.__delattr__ = lambda self, name: seen.append(name)
    if '__delattr__' in C.__dict__:
        del C.__delattr__
    return seen


class Prop:
    pass


def property_added_later(n):
    C = Prop
    if 'v' in C.__dict__:
        del C.v
    c = C()
    seen = []
    for i in range(n):
        c.v = i
        if i == 1:
            C.v = property(lambda self: -1,
                           lambda self, value: seen.append(value))
    r = (c.v, sorted(c.__dict__.items()), seen)
    del C.v
    return r


class Setter:
    def __set__(self, obj, value):
        obj.__dict__['got'] = value


class HeapDescr:
    pass


class Later:
    pass


def heap_descriptor_set_later(n):
    # a class attribute whose type is a heap type: its type may gain
    # __set__ without the class's version tag changing
    if '__set__' in Later.__dict__:
        del Later.__set__
    C = HeapDescr
    C.w = Later()
    c = C()
    for i in range(n):
        c.w = i
        if i == 1:
            Later.__set__ = Setter.__set__
    return sorted(c.__dict__.items())


class Shared:
    def __init__(self, a, b):
        self.a = a
        self.b = b


def shared_keys(n):
    objs = [Shared(i, -i) for i in range(n)]
    for o in objs:
        o.a = o.a + 1
        o.b = o.b * 2
    return [(o.a, o.b) for o in objs]


class Combined:
    def __init__(self):
        self.p = 1
        self.q = 2


def combined_dict(n):
    out = []
    for i in range(n):
        c = Combined()
        del c.p                 # converts the dict to combined
        c.p = i
        c.r = i * 2
        c.q = c.q + c.p
        out.append(sorted(c.__dict__.items()))
    return out


class NoKeys:
    pass


class D(dict):
    def __setitem__(self, k, v):    # not called by attribute stores
        raise RuntimeError


def replaced_dict(n):
    out = []
    for i in range(n):
        o = NoKeys()
        o.__dict__ = D(z=0) if i % 2 else {1: 'int key'}
        o.z = i
        o.k = 'x'
        out.append((type(o.__dict__).__name__, sorted(map(str, o.__dict__.items()))))
    return out


class BadEq(str):
    def __hash__(self):
        return hash('m')

    def __eq__(self, other):
        raise KeyError('from __eq__')


def key_eq_raises(n):
    out = []
    for i in range(n):
        o = NoKeys()
        o.__dict__[BadEq('m')] = 0
        try:
            o.m = i
            out.append('stored')
        except Exception as e:
            out.append('%s: %s' % (type(e).__name__, e))
    return out


class Fresh:
    pass


def no_dict_yet(n):
    out = []
    for i in range(n):
        o = Fresh()
        o.a = i
        o.b = i + 1
        out.append(sorted(o.__dict__.items()))
    return out


class Slotted:
    __slots__ = ('a', 'b')


def slots(n):
    out = []
    o = Slotted()
    for i in range(n):
        o.a = i
        o.b = o.a * 2
        try:
            o.c = 1
        except AttributeError as e:
            out.append(str(e))
    out.append((o.a, o.b))
    return out


class SlotsAndDict:
    __slots__ = ('a', '__dict__')


def slots_and_dict(n):
    o = SlotsAndDict()
    for i in range(n):
        o.a = i
        o.d = i * 3
    return o.a, o.d, sorted(o.__dict__.items())


class MyList(list):
    pass


class MyInt(int):
    pass


class MyStr(str):
    pass


def builtin_subclasses(n):
    l, m, s = MyList(), MyInt(5), MyStr('s')
    for i in range(n):
        l.tag = i
        m.tag = i + 1
        s.tag = i + 2
    try:
        l.append = 1
    except AttributeError as e:
        err = str(e)
    return l.tag, m.tag, s.tag, sorted(l.__dict__), err


class ReadOnly:
    @property
    def ro(self):
        return 1


def read_only(n):
    out = []
    o = ReadOnly()
    for i in range(n):
        try:
            o.ro = i
        except AttributeError as e:
            out.append(str(e))
        try:
            o.__class__.__name__
            object().x = 1
        except AttributeError as e:
            out.append(str(e))
    return out


def delete_missing(n):
    out = []
    for i in range(n):
        o = Fresh()
        try:
            del o.nope
        except AttributeError as e:
            out.append(str(e))
        o.nope = 1
        del o.nope
        try:
            del o.nope
        except AttributeError as e:
            out.append(str(e))
    return out


class Counted:
    pass


class Dies:
    def __init__(self, log):
        self.log = log

    def __del__(self):
        self.log.append('del')


def old_value_freed(n):
    log = []
    o = Counted()
    for i in range(n):
        o.v = Dies(log)
        o.v = o.v
    o.v = None
    return log


class Refreshed:
    pass


def _hooked_store(self, name, value):
    object.__setattr__(self, name, ('hooked', value))


def _store_a(o, v):
    o.s = v


def _store_b(o, v):
    o.s = v


def guard_refresh(n):
    # a guard that failed on an unrelated class change must pass again, and
    # still fail on a later change that matters; two sites of the same name
    # compiled for different __setattr__s must each stay right
    C = Refreshed
    for k in ('__setattr__', 'const'):
        if k in C.__dict__:
            delattr(C, k)
    o = C()
    out = []
    for i in range(n):
        _store_a(o, i)
        out.append(o.s)
        if i == 1:
            C.const = i                 # unrelated: the store's kind is unchanged
        if i == 3:
            C.__setattr__ = _hooked_store
            _store_b(o, i)              # compiled with the hook
            out.append(o.s)
        if i == 5:
            del C.__setattr__
            _store_b(o, i)
            out.append(o.s)
    return out


CASES = [
    ('guard_refresh', [(8,)], {}),
    ('setattr_added_later', [(4,)], {}),
    ('delattr_added_later', [(4,)], {}),
    ('property_added_later', [(4,)], {}),
    ('heap_descriptor_set_later', [(4,)], {}),
    ('shared_keys', [(5,)], {}),
    ('combined_dict', [(4,)], {}),
    ('replaced_dict', [(4,)], {}),
    ('key_eq_raises', [(3,)], {}),
    ('no_dict_yet', [(4,)], {}),
    ('slots', [(3,)], {}),
    ('slots_and_dict', [(3,)], {}),
    ('builtin_subclasses', [(3,)], {}),
    ('read_only', [(3,)], {}),
    ('delete_missing', [(3,)], {}),
    ('old_value_freed', [(4,)], {}),
]
