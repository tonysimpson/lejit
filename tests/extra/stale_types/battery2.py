from hdr import *
import math
def case(name, setup, mutate, fn):
    objs = setup()
    hot(fn, *objs)
    show(name + ' before', fn, *objs)
    mutate(objs)
    show(name + ' after', fn, *objs)
def cls(**kw):
    return type('C', (), kw)
case('class_getitem', lambda: (cls(),), lambda o: setattr(o[0], '__class_getitem__', classmethod(lambda c, i: 'cgi')), lambda C: C[int])
case('round', lambda: (cls(__round__=lambda s, n=None: 1)(),), lambda o: setattr(type(o[0]), '__round__', lambda s, n=None: 2), lambda o: round(o))
case('floor', lambda: (cls(__floor__=lambda s: 1)(),), lambda o: setattr(type(o[0]), '__floor__', lambda s: 2), lambda o: math.floor(o))
case('unpack', lambda: (cls(__iter__=lambda s: iter((1, 2)))(),), lambda o: setattr(type(o[0]), '__iter__', lambda s: iter((3, 4))), lambda o: tuple(a for a in [o]) and (lambda a, b: (a, b))(*o))
def fu(o):
    a, b = o
    return a + b
case('unpack2', lambda: (cls(__iter__=lambda s: iter((1, 2)))(),), lambda o: setattr(type(o[0]), '__iter__', lambda s: iter((3, 4))), fu)
class CM:
    def __enter__(self): return 1
    def __exit__(self, *a): return False
def fw(o):
    with o as v:
        return v
case('with', lambda: (type('W', (CM,), {})(),), lambda o: setattr(type(o[0]), '__enter__', lambda s: 2), fw)
case('format', lambda: (cls(__format__=lambda s, f: 'f1')(),), lambda o: setattr(type(o[0]), '__format__', lambda s, f: 'f2'), lambda o: f'{o}')
case('str', lambda: (cls(__str__=lambda s: 's1')(),), lambda o: setattr(type(o[0]), '__str__', lambda s: 's2'), lambda o: str(o))
case('repr', lambda: (cls(__repr__=lambda s: 'r1')(),), lambda o: setattr(type(o[0]), '__repr__', lambda s: 'r2'), lambda o: repr(o))
case('list-contains', lambda: (cls(__eq__=lambda s, x: False, __hash__=None)(),), lambda o: setattr(type(o[0]), '__eq__', lambda s, x: True), lambda o: 5 in [1, 2, o] and 'yes' or 'no')
case('lt', lambda: (cls(__lt__=lambda s, x: True)(),), lambda o: setattr(type(o[0]), '__lt__', lambda s, x: False), lambda o: o < 1)
case('while', lambda: (cls(__bool__=lambda s: False)(),), lambda o: setattr(type(o[0]), '__bool__', lambda s: True), lambda o: 'loop' if o else 'no')
def sE():
    class E(KeyError): pass
    return (E,)
def fE(E):
    try:
        raise E
    except ValueError:
        return 'VE'
    except KeyError:
        return 'KE'
case('exc-bases', sE, lambda o: setattr(o[0], '__bases__', (ValueError,)), fE)
def fE2(E):
    try:
        {}['x']
    except E:
        return 'E'
    except KeyError:
        return 'KE'
def sE2():
    class E(IndexError): pass
    return (E,)
case('exc-bases2', sE2, lambda o: setattr(o[0], '__bases__', (LookupError,)), fE2)
def sSub():
    class B: pass
    class B2: pass
    class C(B): pass
    return (C, B, B2)
case('issubclass', sSub, lambda o: setattr(o[0], '__bases__', (o[2],)), lambda C, B, B2: issubclass(C, B))
case('isinstance-heap', sSub, lambda o: setattr(o[0], '__bases__', (o[2],)), lambda C, B, B2: isinstance(C(), B))
case('type-is', sSub, lambda o: None, lambda C, B, B2: type(C()) is C)
def sInitDel():
    class B:
        def __init__(self): self.v = 'B'
    class C(B):
        def __init__(self): self.v = 'C'
    return (C,)
case('init-del', sInitDel, lambda o: delattr(o[0], '__init__'), lambda C: C().v)
def sInitCallable():
    class C:
        def __init__(self): self.v = 'f'
    return (C,)
class Init:
    def __call__(self, *a): pass
    def __get__(self, obj, t):
        def b(): obj.v = 'callable'
        return b
case('init-noncallable', sInitCallable, lambda o: setattr(o[0], '__init__', Init()), lambda C: C().v)
def sSM():
    class C:
        @staticmethod
        def m(): return 1
    return (C(),)
case('staticmethod-reinit', sSM, lambda o: type(o[0]).__dict__['m'].__init__(lambda: 2), lambda o: o.m())
def sCM():
    class C:
        @classmethod
        def m(cls): return 1
    return (C(),)
case('classmethod-reinit', sCM, lambda o: type(o[0]).__dict__['m'].__init__(lambda cls: 2), lambda o: o.m())
def sProp():
    class C:
        @property
        def p(self): return 1
    return (C(),)
case('property-reinit', sProp, lambda o: type(o[0]).__dict__['p'].__init__(lambda s: 2), lambda o: o.p)
def sMetaAttr():
    class M(type): pass
    class C(metaclass=M):
        x = 1
    return (C,)
case('meta-data-descr', sMetaAttr, lambda o: setattr(type(o[0]), 'x', property(lambda c: 'meta')), lambda C: C.x)
def sMetaClassAssign():
    class M(type):
        def m(cls): return 'M'
    class M2(type):
        def m(cls): return 'M2'
    class C(metaclass=M): pass
    return (C, M2)
case('meta-class-assign', sMetaClassAssign, lambda o: setattr(o[0], '__class__', o[1]), lambda C, M2: C.m())
def sSlots():
    class C:
        __slots__ = ('a',)
        def __init__(self): self.a = 1
    return (C(),)
case('slots-replace', sSlots, lambda o: setattr(type(o[0]), 'a', 'classattr'), lambda o: o.a)
def sMod():
    import types
    m = types.ModuleType('m'); m.x = 1
    class MM(types.ModuleType):
        @property
        def x(self): return 'prop'
    return (m, MM)
case('module-class', sMod, lambda o: setattr(o[0], '__class__', o[1]), lambda m, MM: m.x)
