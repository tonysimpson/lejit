# super() with no arguments, which finds its class and self in the frame:
# in a nested function, in code with no arguments or no __class__ cell,
# with the __class__ cell emptied or holding a non-type, with self in a
# cell, with the builtin super rebound, for a class whose metaclass has
# __getattribute__, and for a descriptor on a base.
import builtins


class A:
    def m(self, *a):
        return ('A.m', type(self).__name__, a)


class B(A):
    def m(self, *a):
        return ('B.m', super().m(*a))

    def nested(self, x):
        def no_args():
            return super().m(x)

        def with_arg(other):
            return super().m(x)
        out = []
        for f in (no_args, lambda: with_arg(self), lambda: with_arg(5)):
            try:
                out.append(f())
            except (RuntimeError, TypeError) as e:
                out.append((type(e).__name__, str(e)))
        return out

    def captured(self, x):
        # self is also a cell, read by the lambda
        f = lambda: type(self).__name__
        return (super().m(x), f(), super().m)


def outside(self):
    return super().m()


class Star(A):
    def m(*a):
        return super().m(*a)

    n = outside


def nested(n):
    return [B().nested(i) for i in range(n)]


def no_arguments(n):
    out = []
    for i in range(n):
        try:
            out.append(Star().m(i))
        except RuntimeError as e:
            out.append((type(e).__name__, str(e)))
        try:
            out.append(Star().n())
        except RuntimeError as e:
            out.append((type(e).__name__, str(e)))
    return out


def class_cell_changes(n):
    class C(A):
        def m(self, x):
            return ('C.m', super().m(x))
    cell = [c for c in C.m.__closure__ if c.cell_contents is C][0]
    o = C()
    out = []
    for i in range(n):
        try:
            out.append(o.m(i))
        except RuntimeError as e:
            out.append((type(e).__name__, str(e)))
        if i == 2:
            del cell.cell_contents
        if i == 3:
            cell.cell_contents = 5
        if i == 4:
            cell.cell_contents = C
    return out


def self_in_cell(n):
    out = []
    for i in range(n):
        r = B().captured(i)
        out.append((r[0], r[1], r[2](i)))
    return out


def builtin_rebound(n):
    real = builtins.super

    def fake(*a):
        return real(B, a[1] if a else B())
    out = []
    try:
        for i in range(n):
            out.append(B().m(i))
            if i == 1:
                builtins.super = fake
            if i == 3:
                builtins.super = real
    finally:
        builtins.super = real
    return out


class Meta(type):
    def __getattribute__(cls, name):
        if name == 'tag':
            return 'from Meta'
        return type.__getattribute__(cls, name)


class MA(metaclass=Meta):
    tag = 'MA.tag'

    def m(self, x):
        return ('MA.m', x)

    @classmethod
    def c(cls, x):
        return ('MA.c', cls.__name__, x)


class MB(MA):
    def m(self, x):
        return ('MB.m', super().m(x), super().tag)

    @classmethod
    def c(cls, x):
        return ('MB.c', super().c(x), super().tag)


def metaclass_getattribute(n):
    return [(MB().m(i), MB.c(i), MB().c(i)) for i in range(n)]


class Getter:
    def __get__(self, obj, owner):
        return ('Getter', type(obj).__name__, owner.__name__)


class DA:
    d = Getter()

    def __call__(self):
        return 'DA()'


class DB(DA):
    def get(self):
        return (super().d, super().__call__())


def descriptor(n):
    return [DB().get() for i in range(n)]


CASES = [
    ('nested', [(3,)], {}),
    ('no_arguments', [(3,)], {}),
    ('class_cell_changes', [(7,)], {'warmup': (40, "each round's class is "
                                                   'new: the site promotes '
                                                   'it for some rounds')}),
    ('self_in_cell', [(3,)], {}),
    ('builtin_rebound', [(5,)], {}),
    ('metaclass_getattribute', [(3,)], {}),
    ('descriptor', [(3,)], {}),
]
