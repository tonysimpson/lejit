# super().m() where the method is found while compiling: the classes
# change after the call is compiled (a method assigned or deleted on a
# base, a base's __getattribute__, __bases__ assigned, the __class__ cell
# replaced), and the site sees several types of self.


def classes():
    class A:
        def m(self, x):
            return ('A.m', x)

    class A2:
        def m(self, x):
            return ('A2.m', x)

    class B(A):
        def m(self, x):
            return ('B.m', super().m(x))

    class C(B):
        def m(self, x):
            return ('C.m', super().m(x))
    return A, A2, B, C


def base_changes(n):
    A, A2, B, C = classes()
    objs = [B(), C()]
    out = []
    for i in range(n):
        out.append([o.m(i) for o in objs])
        if i == 2:
            A.m = lambda self, x: ('new A.m', x)
        if i == 4:
            B.m2 = B.m
            del B.m
        if i == 6:
            B.m = B.m2
            A.m = staticmethod(lambda x: ('static', x))
        if i == 8:
            A.m = property(lambda self: lambda x: ('property', x))
    return out


def bases_changed(n):
    A, A2, B, C = classes()
    o = C()
    out = []
    for i in range(n):
        out.append(o.m(i))
        if i == 2:
            B.__bases__ = (A2,)
        if i == 4:
            B.__bases__ = (A,)
    return out


def class_cell(n):
    A, A2, B, C = classes()
    cell = [c for c in B.m.__closure__ if c.cell_contents is B][0]
    o = C()
    out = []
    for i in range(n):
        try:
            out.append(o.m(i))
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
    o = C()
    out = []
    for i in range(n):
        out.append(o.m(i))
        if i == 2:
            C.__getattribute__ = lambda self, name: object.__getattribute__(
                self, name)
        if i == 3:
            B.m = lambda self, x: ('B.m again', super(B, self).m(x))
    return out


def many_types(n):
    A, A2, B, C = classes()
    subs = [type('S%d' % k, (C,), {}) for k in range(8)]
    return [[s().m(i) for s in subs] for i in range(n)]


NEW_CLASSES = (40, "each round's classes are new: the sites promote them for "
                   'some rounds before they stop, in the methods inlined '
                   'into their callers too')

CASES = [
    ('base_changes', [(10,)], {'warmup': NEW_CLASSES}),
    ('bases_changed', [(6,)], {'warmup': NEW_CLASSES}),
    ('class_cell', [(8,)], {'warmup': NEW_CLASSES}),
    ('getattribute_added', [(5,)], {'warmup': NEW_CLASSES}),
    ('many_types', [(3,)], {'warmup': NEW_CLASSES}),
]
