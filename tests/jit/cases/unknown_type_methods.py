# x.m(...) where the site has seen too many types for LeJIT to know x's:
# CPython's method load at run time. A function found on the type is called
# with x first, unless the instance's dict shadows it; anything else
# (classmethods, staticmethods, other descriptors, __getattr__ and
# __getattribute__, modules, built-in types) is the attribute, called as it
# is.
import math
import sys


def make(i):
    def m(self, *a, **k):
        return ('m', i, a, sorted(k.items()))
    return type('C%d' % i, (), {'m': m, 'n': i})


PLAIN = [make(i)() for i in range(8)]


class Shadowed:
    def m(self, *a, **k):
        return 'class'


class ClassM:
    @classmethod
    def m(cls, *a, **k):
        return ('classmethod', cls.__name__, a)


class StaticM:
    @staticmethod
    def m(*a, **k):
        return ('staticmethod', a)


class Prop:
    @property
    def m(self):
        return lambda *a, **k: ('property', a)


class GetAttr:
    def __getattr__(self, name):
        return lambda *a, **k: ('getattr', name, a)


class GetAttribute:
    def m(self, *a, **k):
        return 'never'

    def __getattribute__(self, name):
        return lambda *a, **k: ('getattribute', name, a)


class Slots:
    __slots__ = ('m',)


class CallableAttr:
    class m:
        def __init__(self, *a, **k):
            self.a = a

        def __repr__(self):
            return 'CallableAttr.m%r' % (self.a,)


class Missing:
    pass


def objects():
    sh = Shadowed()
    sh.m = lambda *a, **k: ('instance', a)
    sl = Slots()
    sl.m = lambda *a, **k: ('slot', a)
    return PLAIN + [sh, Shadowed(), ClassM(), ClassM, StaticM(), Prop(),
                    GetAttr(), GetAttribute(), sl, CallableAttr(),
                    sys.modules[__name__]]


def m(*a, **k):
    # this module's m, for the module in objects()
    return ('module', a)


def calls(n):
    objs = objects()
    out = []
    for i in range(n):
        for o in objs:
            out.append((repr(o.m()) if isinstance(o, CallableAttr)
                        else o.m(), o.m(i), o.m(i, k=i)))
    return out


def builtins(n):
    objs = [[3, 1, 2], (1, 2, 1), 'a,b', b'x,y', {1: 2}, {3}, 7, 2.5,
            bytearray(b'q')]
    return [[o.__len__() if hasattr(o, '__len__') else o.__hash__(),
             o.__class__.__name__, o.__repr__(), o.__sizeof__() > 0]
            for i in range(n) for o in objs]


def errors(n):
    objs = PLAIN[:6] + [Missing(), Shadowed(), 5, 'x', math]
    out = []
    for i in range(n):
        for o in objs:
            try:
                out.append(o.nope(i))
            except AttributeError as e:
                out.append(str(e))
            try:
                out.append(o.m(i, k=1) if not isinstance(o, (int, str))
                           else o.m())
            except (AttributeError, TypeError) as e:
                out.append(str(e))
    return out


def changed_class(n):
    objs = [make(i)() for i in range(7)]
    out = []
    for i in range(n):
        out.append([o.m(i) for o in objs])
        if i == 1:
            type(objs[0]).m = lambda self, *a: ('replaced', a)
        if i == 2:
            del type(objs[1]).m
            objs[1].m = lambda *a: ('now on the instance', a)
        if i == 3:
            objs[2].__class__ = Shadowed
    return out


def raising(n):
    class E(Exception):
        pass

    def boom(self, x):
        raise E(x)
    objs = [type('R%d' % i, (), {'m': boom})() for i in range(7)]
    out = []
    for i in range(n):
        for o in objs:
            try:
                o.m(i)
            except E as e:
                out.append((e.args, e.__traceback__.tb_next.tb_frame.f_code
                            .co_name))
    return out


CASES = [
    ('calls', [(0,), (3,)], {}),
    ('builtins', [(2,)], {}),
    ('errors', [(2,)], {}),
    ('changed_class', [(5,)], {}),
    ('raising', [(2,)], {}),
]
