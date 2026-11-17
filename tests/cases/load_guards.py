# Load guards shared per (type, name, kind) and repaired at run time
# (pobject.c:load_guard, load_type_changed; D60): a class change that
# leaves a load's lookup as compiled re-arms the guard, one that doesn't
# must be seen. Classes with a metaclass get a version tag
# (enable_version_tag), so their guards hold, and their changes must be
# seen too. Counts of failed guards come from ni._stats() and are 0 in the
# plain run, so a case returns them only where Ni must also give 0.
import abc
import enum
import sys


def _fails():
    ni = sys.modules.get('ni')
    return ni._stats()['load_type_changes'] if ni is not None else 0


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
    # CPython 3.7 gives these no version tag; Ni sets the flag, so their
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


CASES = [
    ('same_value', [(5,)], {}),
    ('different_value', [(5,)], {}),
    ('unrelated_attribute', [(5,)], {}),
    ('descriptor_added_later', [(4,)], {}),
    ('class_counter', [(10,)], {}),
    ('many_classes', [(3,)], {}),
    ('metaclass_classes', [(4,)], {}),
    ('bases_assigned', [(4,)], {}),
    ('custom_mro', [(4,)], {}),
    ('enum_members', [(10,)], {}),
]
