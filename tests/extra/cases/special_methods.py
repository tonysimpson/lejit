# Special methods defined in Python, called from compiled code through
# typeobject.c's slot functions (ptypeobject.c): SLOT1BINFULL's order
# (the reflected method first only for a subclass that overrides it,
# NotImplemented passing to the other operand), SLOT0's AttributeError,
# slot_tp_richcompare's NotImplemented for a missing method, results
# that aren't bools, methods that aren't plain functions, and a class
# whose methods change after the code using them was compiled.
import traceback


class V:
    def __init__(self, x, log=None):
        self.x = x
        self.log = log

    def __add__(self, o):
        if self.log is not None:
            self.log.append('V.__add__')
        if isinstance(o, V):
            return V(self.x + o.x)
        if isinstance(o, int):
            return V(self.x + o)
        return NotImplemented

    def __radd__(self, o):
        if self.log is not None:
            self.log.append('V.__radd__')
        if isinstance(o, int):
            return V(o + self.x)
        return NotImplemented

    def __mul__(self, k):
        return V(self.x * k)

    __rmul__ = __mul__

    def __neg__(self):
        return V(-self.x)

    def __abs__(self):
        return abs(self.x)

    def __eq__(self, o):
        return 'eq %r' % (o,)

    def __lt__(self, o):
        return self.x < o.x

    def __repr__(self):
        return 'V(%r)' % (self.x,)


class SubOver(V):
    def __radd__(self, o):
        if self.log is not None:
            self.log.append('SubOver.__radd__')
        return V(1000)


class SubSame(V):
    pass


class Never:
    def __init__(self, log):
        self.log = log

    def __add__(self, o):
        self.log.append('Never.__add__')
        return NotImplemented

    def __radd__(self, o):
        self.log.append('Never.__radd__')
        return NotImplemented


def arithmetic(n):
    out = []
    for i in range(n):
        a, b = V(i), V(2)
        out.append((a + b, a + 3, 3 + a, a * 2.5, 2 * a, -a, abs(-a)))
    return out


def reflected(n):
    out = []
    for i in range(n):
        log = []
        a, s, t = V(i, log), SubOver(5, log), SubSame(7, log)
        out.append((a + s, a + t, s + a, log))
    return out


def not_implemented(n):
    out = []
    for i in range(n):
        log = []
        a, b = Never(log), V(i, log)
        r = []
        try:
            a + b
        except TypeError as e:
            r.append(str(e))
        try:
            a + a
        except TypeError as e:
            r.append(str(e))
        try:
            b + 'x'
        except TypeError as e:
            r.append(str(e))
        out.append((r, log))
    return out


def compare(n):
    out = []
    for i in range(n):
        a, b = V(i), V(1)
        r = [a == b, b == 3, a != b, a < b, b > a]
        if a == b:
            r.append('truthy')
        try:
            a <= b
        except TypeError as e:
            r.append(str(e))
        out.append(r)
    return out


class Odd:
    def __init__(self, x):
        self.x = x

    def __add__(self, o):
        return self.x + o


def missing(n):
    out = []
    for i in range(n):
        o = Odd(i)
        r = [o + 1]
        for f in (lambda: -o, lambda: abs(o), lambda: o - 1, lambda: o < o):
            try:
                r.append(f())
            except TypeError as e:
                r.append(str(e))
        out.append(r)
    return out


class Raises:
    def __add__(self, o):
        return 1 / o


def raising(n):
    out = []
    for i in range(n):
        try:
            Raises() + 0
        except ZeroDivisionError as e:
            tb = traceback.extract_tb(e.__traceback__)
            out.append([(f.name, f.line) for f in tb])
    return out


class Holder:
    def __init__(self, x):
        self.x = x

    def __add__(self, o):
        return ('first', self.x, o)


def first_add(self, o):
    return ('first', self.x, o)


def second_add(self, o):
    return ('second', self.x, o)


class Callable:
    def __call__(self, a, b):
        return ('callable', a.x, b)


def changed(n):
    out = []
    for i in range(n):
        h = Holder(i)
        r = [h + 1]
        Holder.__add__ = second_add
        r.append(h + 2)
        Holder.__add__ = staticmethod(lambda a, b: ('static', b))
        r.append(h + 3)
        Holder.__add__ = Callable()
        try:
            r.append(h + 4)
        except TypeError as e:
            r.append(str(e))
        del Holder.__add__
        try:
            r.append(h + 5)
        except TypeError as e:
            r.append(str(e))
        Holder.__add__ = first_add
        r.append(h + 6)
        out.append(r)
    return out


class Fickle:
    def __init__(self, x):
        self.x = x

    def __add__(self, o):
        # changes the class during the call, then declines
        Fickle.__add__ = fickle_second
        return NotImplemented

    def __radd__(self, o):
        return ('radd', o)


def fickle_second(self, o):
    Fickle.__add__ = fickle_first
    return ('second', o)


fickle_first = Fickle.__add__


def fickle(n):
    out = []
    for i in range(n):
        a = Fickle(i)
        r = []
        for j in range(3):
            try:
                r.append(a + j)
            except TypeError as e:
                r.append(str(e))
        r.append(j + a)
        out.append(r)
    Fickle.__add__ = fickle_first
    return out


class Morph:
    def __init__(self, x):
        self.x = x

    def __add__(self, o):
        # makes the other operand a Target, then declines: CPython calls
        # Target's __radd__
        if type(o) is Plain:
            o.__class__ = Target
        return NotImplemented

    def __sub__(self, o):
        self.__class__ = Morphed
        return ('Morph.__sub__', o.x)


class Morphed(Morph):
    def __sub__(self, o):
        self.__class__ = Morph
        return ('Morphed.__sub__', o.x)


class Plain:
    def __init__(self, x):
        self.x = x

    def __radd__(self, o):
        return ('Plain.__radd__', self.x)


class Target(Plain):
    def __radd__(self, o):
        self.__class__ = Plain
        return ('Target.__radd__', self.x)


def class_assigned(n):
    out = []
    for i in range(n):
        a, b = Morph(i), Plain(i)
        out.append((a + b, a - b, a - b, type(a).__name__, type(b).__name__))
    return out


class Point:
    __slots__ = ('x', 'y')

    def __init__(self, x, y=0):
        self.x = x
        self.y = y


class Bare:
    pass


class ReturnsValue:
    def __init__(self, x):
        self.x = x
        return x


class InitRaises:
    def __init__(self, x):
        raise ValueError(x)


class WithNew:
    def __new__(cls, x):
        return Point(x)

    def __init__(self, x):
        raise AssertionError('not called: __new__ gave another class')


class MyList(list):
    def __init__(self, n):
        list.__init__(self, range(n))


import abc


class Abstract(abc.ABC):
    @abc.abstractmethod
    def f(self):
        pass


def init_first(self, x, y=0):
    self.x = ('first', x)
    self.y = y


def init_second(self, x, y=0):
    self.x = ('second', x)
    self.y = y


class Swapped:
    __init__ = init_first


def instances(n):
    out = []
    for i in range(n):
        p = Point(i)
        r = [(type(p).__name__, p.x, p.y), Point(i, 2).y, type(Bare()).__name__]
        for f in (lambda: Bare(i), lambda: ReturnsValue(i), lambda: InitRaises(i),
                  lambda: Point(), lambda: Abstract()):
            try:
                r.append(f())
            except (TypeError, ValueError) as e:
                r.append((type(e).__name__, str(e)))
        w = WithNew(i)
        r.append((type(w).__name__, w.x))
        r.append(list(MyList(3)))
        r.append(Point(x=i).x)
        r.append(Swapped(i).x)
        Swapped.__init__ = init_second
        r.append(Swapped(i).x)
        del Swapped.__init__
        try:
            r.append(Swapped(i))
        except TypeError as e:
            r.append(str(e))
        Swapped.__init__ = init_first
        r.append(Swapped(i).x)
        out.append(r)
    return out


class Deep:
    def __init__(self, n):
        self.n = n

    def __add__(self, o):
        if self.n == 0:
            return o
        return Deep(self.n - 1) + (o + 1)


def recursion(n):
    return [Deep(i) + 0 for i in range(n)]


CASES = [
    ('arithmetic', [(1,), (4,)], {}),
    ('reflected', [(3,)], {}),
    ('not_implemented', [(3,)], {}),
    ('compare', [(3,)], {}),
    ('missing', [(3,)], {}),
    ('raising', [(2,)], {}),
    ('changed', [(3,)], {}),
    ('fickle', [(3,)], {}),
    ('class_assigned', [(3,)], {}),
    ('instances', [(3,)], {}),
    ('recursion', [(30,)], {}),
]
