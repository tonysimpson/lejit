# super().name loaded as an attribute (not called straight away as a
# method): super().__init__(*a, **k), a bound method kept or passed on,
# with the function found while compiling, and the classes changing after
# the load is compiled.


def classes():
    class A:
        def __init__(self, *a, **k):
            self.got = ('A', a, sorted(k.items()))

        def m(self, x):
            return ('A.m', x)

    class A2:
        def __init__(self, *a, **k):
            self.got = ('A2', a)

        def m(self, x):
            return ('A2.m', x)

    class B(A):
        def __init__(self, *a, **k):
            super().__init__(*a, **k)

        def kept(self):
            f = super().m
            return (type(f).__name__, f.__self__ is self, f.__func__.__name__,
                    f(1), f == super().m, f is super().m)

        def passed(self, x):
            return list(map(super().m, [x, x + 1]))

    class C(B):
        def __init__(self, *a, **k):
            super().__init__(*a, k=len(a), **k)
    return A, A2, B, C


A, A2, B, C = classes()
SUBS = [type('S%d' % k, (C,), {}) for k in range(8)]


def forwarding(n):
    return [(B(i, j=i).got, C(i).got, B().got) for i in range(n)]


def kept(n):
    return [(B().kept(), C().kept(), C().passed(i)) for i in range(n)]


NEW_CLASSES = (40, "each round's classes are new: their __init__ is compiled "
                   'for some rounds before the site stops')


def base_changes(n):
    A, A2, B, C = classes()
    out = []
    for i in range(n):
        o = C(i)
        out.append((o.got, o.kept()[3]))
        if i == 1:
            A.__init__ = lambda self, *a, **k: setattr(self, 'got', ('new', a))
            A.m = lambda self, x: ('new A.m', x)
        if i == 3:
            del A.m
            A.m = staticmethod(lambda x: ('static', x))
        if i == 5:
            A.m = classmethod(lambda cls, x: ('class', cls.__name__, x))
        if i == 7:
            A.m = property(lambda self: lambda x: ('property', x))
    return out


def bases_changed(n):
    A, A2, B, C = classes()
    out = []
    for i in range(n):
        o = C(i)
        out.append((o.got, o.kept()[3]))
        if i == 2:
            B.__bases__ = (A2,)
        if i == 4:
            B.__bases__ = (A,)
    return out


def class_cell(n):
    A, A2, B, C = classes()
    cell = [c for c in B.kept.__closure__ if c.cell_contents is B][0]
    o = C()
    out = []
    for i in range(n):
        try:
            out.append(o.kept()[3])
        except (TypeError, AttributeError) as e:
            out.append((type(e).__name__, str(e)))
        if i == 2:
            cell.cell_contents = A2
        if i == 4:
            cell.cell_contents = A
        if i == 6:
            cell.cell_contents = B
    return out


def getattribute_added(n):
    A, A2, B, C = classes()
    out = []
    for i in range(n):
        out.append((C(i).got, C().kept()[3]))
        if i == 2:
            C.__getattribute__ = lambda self, name: (
                object.__getattribute__(self, name))
    return out


def many_types(n):
    return [[(s(i).got, s().kept()[3]) for s in SUBS] for i in range(n)]


CASES = [
    ('forwarding', [(0,), (3,)], {}),
    ('kept', [(2,)], {}),
    ('base_changes', [(9,)], {'warmup': NEW_CLASSES}),
    ('bases_changed', [(6,)], {'warmup': NEW_CLASSES}),
    ('class_cell', [(8,)], {'warmup': NEW_CLASSES}),
    ('getattribute_added', [(5,)], {'warmup': NEW_CLASSES}),
    ('many_types', [(3,)], {}),
]
