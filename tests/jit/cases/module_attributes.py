# A module's attribute is read from its dict as a cached global is: a
# constant behind a guard of the dict's entry, then, once it is rebound, a
# load of the entry. Compiled code sees rebinding, deletion (CPython's
# AttributeError), a module __getattr__ (PEP 562), a dict that grows, the
# module's class becoming a subclass with a property, and names that are
# ModuleType's own.
import math
import types

m = types.ModuleType('m')
m.x = 1


def read_x():
    return m.x


def attempt(f):
    try:
        return f()
    except AttributeError as e:
        return 'AttributeError', str(e)


def constant(n):
    return [m.x + math.floor(2.5) for i in range(n)]


def rebound(n):
    out = []
    for i in range(n):
        m.x = i
        out.append(read_x())
    m.x = 1
    return out


def grown(n):
    out = [read_x()]
    for i in range(n):
        setattr(m, 'grow%d' % i, i)
        out.append(read_x())
    for i in range(n):
        delattr(m, 'grow%d' % i)
    return out


def deleted(n):
    out = []
    for i in range(n):
        del m.x
        out.append(attempt(read_x))
        m.x = i
        out.append(read_x())
    m.x = 1
    return out


g = types.ModuleType('g')


def read_y():
    return g.y


def module_getattr(n):
    out = [attempt(read_y)]
    g.__getattr__ = lambda name: 'from __getattr__ ' + name
    for i in range(n):
        out.append(read_y())
    g.y = 'set'
    out.append(read_y())
    del g.y
    out.append(read_y())
    del g.__getattr__
    out.append(attempt(read_y))
    return out


class WithProperty(types.ModuleType):
    @property
    def x(self):
        return 'property'


c = types.ModuleType('c')
c.x = 'dict'


def read_cx():
    return c.x


def class_assigned(n):
    out = []
    for i in range(n):
        out.append(read_cx())
        c.__class__ = WithProperty
        out.append(read_cx())
        c.__class__ = types.ModuleType
    return out


def own_names(n):
    out = []
    for i in range(n):
        out.append((m.__name__, m.__dict__['x'], m.__class__.__name__))
    return out


CASES = [
    ('constant', [(3,)], {}),
    ('rebound', [(5,)], {}),
    ('grown', [(40,)], {}),
    ('deleted', [(3,)], {}),
    ('module_getattr', [(3,)], {}),
    ('class_assigned', [(3,)],
     {'expected': [['dict', 'property'] * 3]}),
    ('own_names', [(3,)], {}),
]
