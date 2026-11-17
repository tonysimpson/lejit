# Class statements in a function (LOAD_BUILD_CLASS), del of a cell
# variable (DELETE_DEREF) and the @ operator (BINARY/INPLACE_MATRIX_MULTIPLY).
import builtins
import gc
import weakref


def make_class(x):
    class C:
        k = x

        def get(self):
            return self.k * 2
    return C.__name__, C.__qualname__, C().get()


def class_with_base(x):
    class Base:
        def f(self):
            return 'base'

    class D(Base, metaclass=type):
        def f(self):
            return 'd+' + super().f()
    return D().f(), [c.__name__ for c in D.__mro__]


def class_closure(x):
    y = x + 1

    class C:
        def get(self):
            return y
    return C().get()


def classes_die(n):
    # a fresh class per iteration: the class statement itself, an instance
    # and an isinstance test mustn't keep any of them alive
    refs = []
    out = []
    for i in range(n):
        class C:
            def get(self):
                return 1
        c = C()
        out.append(isinstance(c, C))
        refs.append(weakref.ref(C))
    del C, c
    gc.collect()
    return out, [r() is None for r in refs]


def make_and_load(i):
    class C:
        k = i

        def get(self):
            return self.k + 1
    c = C()
    return c.get() + c.k, weakref.ref(C)


def make_and_store(i):
    class C:
        def __init__(self, v):
            self.v = v
    return C(i).v, weakref.ref(C)


def classes_die_after_cutoff(make, n):
    # an attribute site on an instance promotes its type, and the code then
    # holds that class, but only for the first MEGAMORPHIC_MAX (5) types per
    # site (F25, D30): after a first batch, no class may stay alive
    out = []
    for i in range(n):
        out.append(make(i)[0])
    refs = []
    for i in range(n):
        r, ref = make(i)
        out.append(r)
        refs.append(ref)
    gc.collect()
    return out, [r() is None for r in refs]


def no_build_class(x):
    saved = builtins.__build_class__
    del builtins.__build_class__
    try:
        class C:
            pass
    except NameError as e:
        return str(e)
    finally:
        builtins.__build_class__ = saved


def del_cell(x):
    v = x

    def get():
        return v
    a = get()
    del v
    try:
        get()
    except NameError as e:
        return a, str(e)


def del_cell_unbound(x):
    v = x

    def get():
        return v
    del v
    try:
        del v
    except NameError as e:
        return str(e)


def del_cell_twice_in_loop(n):
    out = []
    for i in range(n):
        v = i

        def get():
            return v
        out.append(get())
        del v
    return out


class Mat:
    def __init__(self, v):
        self.v = v

    def __matmul__(self, other):
        return Mat(self.v * (other.v if isinstance(other, Mat) else other))

    def __rmatmul__(self, other):
        return Mat(other * self.v + 1000)

    def __repr__(self):
        return 'Mat(%r)' % (self.v,)


class IMat(Mat):
    def __imatmul__(self, other):
        self.v += other.v
        return self


def matmul(a, b):
    return a @ b


def imatmul(a, b):
    a @= b
    return a


def matmul_loop(n):
    m = Mat(1)
    for i in range(1, n):
        m = m @ Mat(2)
    return m.v


CASES = [
    ('make_class', [(1,), (2,)], {}),
    ('class_with_base', [(0,)], {}),
    ('class_closure', [(5,)], {}),
    ('classes_die', [(20,)], {}),
    ('classes_die_after_cutoff', "[(make_and_load, 20), (make_and_store, 20)]",
     {}),
    ('no_build_class', [(0,)], {}),
    ('del_cell', [(3,)], {}),
    ('del_cell_unbound', [(3,)], {}),
    ('del_cell_twice_in_loop', [(4,)], {}),
    ('matmul', "[(Mat(2), Mat(3)), (Mat(2), 5), (5, Mat(2)), (1, 2), "
               "(Mat(1), 'x'), ([1], [2])]", {}),
    ('imatmul', "[(IMat(2), Mat(3)), (Mat(2), Mat(3)), (1, 2), (Mat(1), 'x')]",
     {}),
    ('matmul_loop', [(10,)], {}),
]
