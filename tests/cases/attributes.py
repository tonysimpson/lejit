# Attribute lookup specialised on the type (pobject.c:PsycoObject_GenericGetAttr):
# the class attribute is found at compile time and guarded by the type's
# version tag, so a class changed after compiling must be seen, and the
# order of _PyObject_GenericGetAttrWithDict kept (data descriptor, then
# instance dict, then non-data descriptor or class attribute).


class Point:
    scale = 2

    def __init__(self, x):
        self.x = x

    def get(self):
        return self.x * self.scale


def method_calls(n):
    p = Point(3)
    t = 0
    for i in range(n):
        t += p.get() + p.x + p.scale
    return t


class Changes:
    pass


def _f10(self):
    return 10


def class_changes(n):
    C = Changes
    C.k = 1
    C.f = _f10
    c = C()
    out = []
    for i in range(n):
        out.append((c.f(), c.k))
        if i == 2:
            C.f = lambda self: 20       # replace a method
        if i == 4:
            C.k = 5                     # replace a class attribute
        if i == 6:
            c.f = lambda: 30            # shadow it in the instance
        if i == 8:
            del c.f
            del C.f                     # gone: AttributeError
    try:
        c.f()
    except AttributeError as e:
        out.append(str(e))
    return out


class B:
    pass


class D(B):
    pass


def base_changes(n):
    B.f = lambda self: 1
    D.__dict__.get('f') and delattr(D, 'f')
    d = D()
    out = []
    for i in range(n):
        out.append(d.f())
        if i == 2:
            B.f = lambda self: 2        # a base change invalidates D
        if i == 4:
            D.f = lambda self: 3
    return out


class Outlives:
    pass


def bound_method_outlives(n):
    C = Outlives
    C.f = lambda self: 'f'
    c = C()
    m = c.f
    del C.f
    return m(), [m() for _ in range(n)]


class Slots:
    __slots__ = ('a', 'b')
    k = 7

    @property
    def p(self):
        return self.a * 2

    @staticmethod
    def s():
        return 's'

    @classmethod
    def c(cls):
        return cls.__name__


def descriptors(n):
    o = Slots()
    o.a = 4
    out = []
    for i in range(n):
        out.append((o.a, o.p, o.k, o.s(), o.c()))
    try:
        o.b
    except AttributeError as e:
        out.append(str(e))
    return out


class Empty:
    pass


class Empty2:
    pass


def property_added_later(n):
    C = Empty
    C.__dict__.get('v') and delattr(C, 'v')
    c = C()
    c.v = 1
    out = []
    for i in range(n):
        out.append(c.v)
        if i == 2:
            C.v = property(lambda self: 'prop')  # data descriptor wins
    return out


class Get:
    def __get__(self, obj, tp):
        return 'get'


class HasGet:
    d = Get()


def heap_descriptor(n):
    Get.__dict__.get('__set__') and delattr(Get, '__set__')
    C = HasGet
    c = C()
    out = []
    for i in range(n):
        out.append(c.d)
        if i == 2:
            c.__dict__['d'] = 'inst'    # non-data: the instance wins
        if i == 4:
            Get.__set__ = lambda self, o, v: None  # now a data descriptor
    return out


class A:
    __slots__ = ('x',)


class Foreign:
    x = A.__dict__['x']                # a member descriptor of another class


def foreign_member():
    try:
        return Foreign().x
    except TypeError as e:
        return str(e)


def getattr_added_later(n):
    C = Empty2
    C.__dict__.get('__getattr__') and delattr(C, '__getattr__')
    c = C()
    out = []
    for i in range(n):
        try:
            out.append(c.missing)
        except AttributeError as e:
            out.append(str(e))
        if i == 1:
            C.__getattr__ = lambda self, name: 'hook ' + name
    return out


def builtin_methods(xs):
    t = 0
    for x in xs:
        t += x.bit_length() + x.real
    return t, (3.5).is_integer(), 'ab'.upper()


CASES = [
    ('method_calls', [(50,)], {}),
    ('class_changes', [(10,)], {}),
    ('base_changes', [(6,)], {}),
    ('bound_method_outlives', [(3,)], {}),
    ('descriptors', [(3,)], {}),
    ('property_added_later', [(5,)], {}),
    ('heap_descriptor', [(7,)], {}),
    ('foreign_member', [()], {}),
    ('getattr_added_later', [(4,)], {}),
    ('builtin_methods', [([1, 255, 2 ** 70],)], {}),
]
