# super().name and super(C, self).name, as attributes and as method calls:
# functions, classmethods, staticmethods, properties, data on a base,
# multiple inheritance, a missing name, a shadowed super, and a self whose
# class overrides __getattribute__.
import builtins


class A:
    x = 'A.x'

    def m(self, *a, **k):
        return ('A.m', type(self).__name__, a, sorted(k.items()))

    @classmethod
    def c(cls, *a):
        return ('A.c', cls.__name__, a)

    @staticmethod
    def s(*a):
        return ('A.s', a)

    @property
    def p(self):
        return ('A.p', type(self).__name__)


class B(A):
    def m(self, *a, **k):
        return ('B.m', super().m(*a, **k))

    def attrs(self):
        return (super().x, super().p, super().c(1), super().s(2),
                super().m, super(B, self).x)


class C(A):
    x = 'C.x'

    def m(self, *a, **k):
        return ('C.m', super().m(*a, **k))


class D(B, C):
    def m(self, *a, **k):
        return ('D.m', super().m(*a, **k), super(B, self).m(*a),
                super(C, self).m())


class G(A):
    def __getattribute__(self, name):
        return object.__getattribute__(self, name)

    def m(self, *a, **k):
        return ('G.m', super().m(*a))


class Missing(A):
    def m(self, *a, **k):
        return super().nope(*a)

    def attr(self):
        return super().nope


class Two(A):
    def m(self, *a, **k):
        return super(Two, self).m(*a, k=1)

    @classmethod
    def c(cls, *a):
        return ('Two.c', super().c(*a))


def calls(n):
    objs = [B(), C(), D(), G(), Two()]
    out = []
    for i in range(n):
        for o in objs:
            out.append((o.m(i), o.m(i, k=i)))
        out.append((B().attrs()[:4], D().attrs()[:4], Two.c(i), Two().c(i)))
    return out


def bound_method_kept(n):
    # a method loaded as an attribute is a bound method, as in CPython
    out = []
    for i in range(n):
        f = B().attrs()[4]
        out.append((type(f).__name__, f.__self__.__class__.__name__, f(i)))
    return out


def errors(n):
    out = []
    for i in range(n):
        for f in (lambda: Missing().m(i), lambda: Missing().attr(),
                  lambda: super(B, A()).m(), lambda: super(B, 5).x):
            try:
                out.append(f())
            except (AttributeError, TypeError) as e:
                out.append((type(e).__name__, str(e)))
    return out


class Shadowed(A):
    def m(self, *a, **k):
        return super().m(*a)


def shadowed_super(n):
    # super() reads the global or builtin named super
    real = builtins.super

    class Fake:
        def __init__(self, *a):
            self.a = a

        def __getattr__(self, name):
            return lambda *a: ('fake', name, len(self.a))
    out = []
    for i in range(n):
        out.append(Shadowed().m(i))
        if i == 1:
            globals()['super'] = Fake
        if i == 3:
            del globals()['super']
    builtins.super = real
    return out


CASES = [
    ('calls', [(0,), (3,)], {}),
    ('bound_method_kept', [(2,)], {}),
    ('errors', [(2,)], {}),
    ('shadowed_super', [(5,)], {}),
]
