# Load guards shared per (type, name, kind) and repaired at run time
# (pobject.c:load_guard, load_type_changed): a class change that
# leaves a load's lookup as compiled re-arms the guard, one that doesn't
# must be seen. Classes with a metaclass get a version tag
# (enable_version_tag), so their guards hold, and their changes must be
# seen too. Counts of failed guards come from lejit._core._stats() and are 0 in
# the plain run, so a case returns them only where LeJIT must also give 0.
import abc
import enum
import sys


def _fails():
    lejit = sys.modules.get('lejit')
    return lejit._core._stats()['load_type_changes'] if lejit is not None else 0


def _m1(self):
    return 1


def _m2(self):
    return 2


class Same:
    k = 10
    f = _m1

    def __init__(self):
        self.x = 1


def _loads(o, n):
    t = 0
    for i in range(n):
        t += o.x + o.k + o.f()
    return t


def same_value(n):
    # the class changes, but every load still finds what it was compiled
    # for: after one failure per site, the guards hold again
    C = Same
    o = C()
    a = _loads(o, n)
    C.k = 10                    # the same object: a new version tag only
    C.f = _m1
    b = _loads(o, n)
    f = _fails()
    c = _loads(o, n)
    return a, b, c, _fails() - f


class Differs:
    k = 10
    f = _m1

    def __init__(self):
        self.x = 1


def different_value(n):
    C = Differs
    C.k = 10
    C.f = _m1
    o = C()
    out = [_loads(o, n)]
    C.k = 20
    out.append(_loads(o, n))
    C.f = _m2
    out.append(_loads(o, n))
    C.f = _m1                   # back to the kind compiled first
    C.k = 10
    out.append(_loads(o, n))
    f = _fails()
    out.append(_loads(o, n))
    out.append(_fails() - f)
    return out


class Unrelated:
    k = 3

    def __init__(self):
        self.x = 1

    def f(self):
        return 5


def unrelated_attribute(n):
    # a new class attribute nothing loads moves the version tag
    C = Unrelated
    o = C()
    out = [_loads(o, n)]
    for j in range(3):
        setattr(C, 'other%d' % j, j)
        out.append(_loads(o, n))
    f = _fails()
    out.append(_loads(o, n))
    out.append(_fails() - f)
    return out


class Later:
    def __init__(self):
        self.x = 1
        self.k = 2

    def f(self):
        return 3


def descriptor_added_later(n):
    C = Later
    for k in ('k', 'x', '__getattr__', '__getattribute__'):
        if k in C.__dict__:
            delattr(C, k)
    o = C()
    out = [_loads(o, n)]
    C.k = property(lambda self: 100)         # a data descriptor wins
    out.append(_loads(o, n))
    del C.k
    out.append(_loads(o, n))
    C.f = property(lambda self: lambda: 7)   # a method becomes a property
    out.append(_loads(o, n))
    del C.f
    out.append(_loads(o, n))
    C.__getattribute__ = lambda self, name: 1 if name == 'f' else \
        object.__getattribute__(self, name)
    try:
        out.append(_loads(o, n))
    except TypeError as e:
        out.append(type(e).__name__)
    del C.__getattribute__
    out.append(_loads(o, n))
    return out


def _counter_loop(o, n, every):
    C = type(o)
    t = 0
    for i in range(n):
        t += o.x + o.k + o.f()
        if i % every == 0:
            C.count = C.count + 1
    return t, C.count


class Counter:
    k = 1
    count = 0

    def __init__(self):
        self.x = 1

    def f(self):
        return self.count


def class_counter(n):
    # a class attribute used as a counter: the guards fail at each bump
    Counter.count = 0
    o = Counter()
    return [_counter_loop(o, n, 1), _counter_loop(o, n, 3)]


def many_classes(n):
    classes = []
    for j in range(12):
        classes.append(type('M%d' % j, (Same,), {'k': j}))
    out = []
    for r in range(3):
        t = 0
        for C in classes:
            t += _loads(C(), n)
            if r == 1:
                C.k = C.k + 1
            if r == 2:
                C.unrelated = r
        out.append(t)
    return out


class Meta(type):
    pass


class WithMeta(metaclass=Meta):
    k = 1

    def __init__(self):
        self.x = 1

    def f(self):
        return 2


class Abstract(abc.ABC):
    k = 5

    @abc.abstractmethod
    def f(self):
        pass


class Concrete(Abstract):
    def __init__(self):
        self.x = 1

    def f(self):
        return 3


def metaclass_classes(n):
    # CPython 3.7 gives these no version tag; LeJIT sets the flag, so their
    # guards hold, and a change to the class or to a base is still seen
    out = []
    for C, B in ((WithMeta, WithMeta), (Concrete, Abstract)):
        B.k = 1
        o = C()
        out.append(_loads(o, n))
        f = _fails()
        out.append(_loads(o, n))
        out.append(_fails() - f)
        B.k = 4                             # the base changes
        out.append(_loads(o, n))
        f = C.__dict__['f']
        C.f = lambda self: 9
        out.append(_loads(o, n))
        C.f = f
        out.append(_loads(o, n))
    return out


class OtherBase:
    k = 50

    def f(self):
        return 60


class Rebased(metaclass=Meta):
    k = 7

    def __init__(self):
        self.x = 1

    def f(self):
        return 8


class RebasedChild(Rebased):
    pass


def bases_assigned(n):
    # __bases__ assigned: CPython clears the flag again (and the tag)
    C = RebasedChild
    C.__bases__ = (Rebased,)
    o = C()
    out = [_loads(o, n)]
    C.__bases__ = (OtherBase,)
    out.append(_loads(o, n))
    C.__bases__ = (Rebased,)
    out.append(_loads(o, n))
    return out


class MroMeta(type):
    def mro(cls):
        return type.mro(cls)


class CustomMro(metaclass=MroMeta):
    k = 1

    def __init__(self):
        self.x = 2

    def f(self):
        return 3


def custom_mro(n):
    # a metaclass that overrides mro: no version tag, the generic path
    C = CustomMro
    C.k = 1
    o = C()
    a = _loads(o, n)
    C.k = 2
    return a, _loads(o, n)


class Color(enum.Enum):
    RED = 1
    BLUE = 2


def enum_members(n):
    t = 0
    for i in range(n):
        c = Color.RED if i % 2 else Color.BLUE
        t += c.value + len(c.name)
    return t


# Instance attributes read and stored inline where the instance keeps
# them (shared keys' split dicts; from 3.11 values with no dict, indexed by
# the class keys, which only grow), with every way out of that shape.


class Point:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def norm(self):
        return self.x * self.x + self.y * self.y


def _z_or_error(p):
    try:
        return p.z
    except AttributeError as e:
        return str(e)


def attributes_present_and_absent(n):
    ps = [Point(i, i + 1) for i in range(n)]
    total = 0
    for p in ps:
        total = total + p.norm()
        p.x = p.x + 1
    return total, [_z_or_error(p) for p in ps[:2]]


def name_added_after_absent(n):
    # z is absent from the class keys while the load compiles, then an
    # instance adds it
    class Late:
        def __init__(self):
            self.a = 1
    ls = [Late() for _ in range(n)]
    out = [_z_or_error(x) for x in ls]
    ls[1].z = 'zed'
    out.append([_z_or_error(x) for x in ls])
    return out


def method_shadowed_then_deleted(n):
    ps = [Point(i, 1) for i in range(n)]
    out = [p.norm() for p in ps]
    ps[1].norm = lambda: -1
    out.append([p.norm() for p in ps])
    del ps[1].norm
    out.append([p.norm() for p in ps])
    return out


def attribute_deleted(n):
    ps = [Point(i, 1) for i in range(n)]
    del ps[0].x
    out = []
    for p in ps:
        try:
            out.append(p.x)
        except AttributeError as e:
            out.append(str(e))
    ps[0].x = 7
    out.append(ps[0].x)
    return out


def dict_made(n):
    ps = [Point(i, 1) for i in range(n)]
    d = ps[0].__dict__
    d['x'] = 100
    ps[0].y = 5
    d['w'] = 'w'
    return [p.norm() for p in ps], ps[0].w, sorted(d.items())


class _Many:
    pass


def many_attributes(n):
    # more names than shared keys hold
    qs = [_Many() for _ in range(n)]
    for q in qs:
        for i in range(40):
            setattr(q, 'a%d' % i, i)
    return [q.a0 + q.a1 + q.a39 for q in qs]


def stores_in_loop(n):
    p = Point(0, 0)
    i = 0
    while i < n:
        p.y = p.y + i
        i = i + 1
    return p.x, p.y


# Stores in __init__ that fill an empty slot of the instance's values add
# its name to their insertion order, which __dict__ shows.


class _Ordered:
    def __init__(self, flip):
        if flip:
            self.b = 'b'
            self.a = 'a'
        else:
            self.a = 'a'
            self.b = 'b'
        self.c = [flip]


def init_order(n):
    os = [_Ordered(i % 3 == 1) for i in range(n)]
    return [list(o.__dict__) for o in os], [o.c for o in os]


class _Refilled:
    def __init__(self, k):
        self.x = k
        self.y = k
        del self.x
        self.x = -k
        self.y = self.y + 1


def init_delete_and_refill(n):
    return [list(_Refilled(i).__dict__.items()) for i in range(n)]


class _Grows:
    def __init__(self, k):
        self.first = k
        setattr(self, 'n%d' % k, k)  # a new name for each instance
        self.last = k


def init_keys_grow(n):
    # the class keys fill up, then instances get a dict
    return [list(_Grows(i).__dict__.items()) for i in range(n)]


class _DictFirst:
    def __new__(cls, k):
        o = object.__new__(cls)
        if k % 2:
            o.__dict__  # made before __init__ stores
        return o

    def __init__(self, k):
        self.a = k
        self.b = [k]


def init_dict_made_first(n):
    return [list(_DictFirst(i).__dict__.items()) for i in range(n)]


class _Sub(_Ordered):
    def __init__(self, flip):
        self.d = 'd'
        _Ordered.__init__(self, flip)


def init_through_base(n):
    return [list(_Sub(i % 2 == 0).__dict__) for i in range(n)]


CASES = [
    ('init_order', [(9,)], {}),
    ('init_delete_and_refill', [(4,)], {}),
    ('init_keys_grow', [(40,)], {}),
    ('init_dict_made_first', [(6,)], {}),
    ('init_through_base', [(5,)], {}),
    ('attributes_present_and_absent', [(5,)], {}),
    ('name_added_after_absent', [(4,)], {}),
    ('method_shadowed_then_deleted', [(3,)], {}),
    ('attribute_deleted', [(3,)], {}),
    ('dict_made', [(3,)], {}),
    ('many_attributes', [(3,)], {}),
    ('stores_in_loop', [(100,)], {}),
    ('same_value', [(5,)], {'checked': 'reads lejit._core._stats'}),
    ('different_value', [(5,)], {'checked': 'reads lejit._core._stats'}),
    ('unrelated_attribute', [(5,)], {'checked': 'reads lejit._core._stats'}),
    ('descriptor_added_later', [(4,)], {}),
    ('class_counter', [(10,)], {}),
    ('many_classes', [(3,)], {}),
    ('metaclass_classes', [(4,)], {'checked': 'reads lejit._core._stats'}),
    ('bases_assigned', [(4,)], {}),
    ('custom_mro', [(4,)], {}),
    ('enum_members', [(10,)], {}),
]
