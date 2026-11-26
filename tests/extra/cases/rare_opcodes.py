# Class statements in a function (LOAD_BUILD_CLASS), del of a cell
# variable (DELETE_DEREF), the @ operator (BINARY/INPLACE_MATRIX_MULTIPLY)
# and an expression shown by sys.displayhook (PRINT_EXPR).
import builtins
import gc
import sys
import types
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
    # site: after a first batch, no class may stay alive
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


# PRINT_EXPR is made only for the interactive prompt ('single' code), which
# is never a function's; a function made of such code is optimised here so
# that it compiles
def _function_of(src):
    c = compile(src, '<single>', 'single')
    flags = c.co_flags | 0x1 | 0x2      # CO_OPTIMIZED | CO_NEWLOCALS
    if hasattr(c, 'replace'):
        c = c.replace(co_flags=flags)
    else:
        c = types.CodeType(c.co_argcount, c.co_kwonlyargcount, c.co_nlocals, c.co_stacksize,
                           flags, c.co_code, c.co_consts, c.co_names, c.co_varnames,
                           c.co_filename, c.co_name, c.co_firstlineno, c.co_lnotab)
    return types.FunctionType(c, globals())


shown = _function_of('6 * 7')


def shown_values(n):
    got = []
    saved = sys.displayhook
    sys.displayhook = got.append
    try:
        for i in range(n):
            shown()
    finally:
        sys.displayhook = saved
    del sys.displayhook
    try:
        shown()
    except RuntimeError as e:
        got.append(str(e))
    finally:
        sys.displayhook = saved
    return got


CASES = [
    ('shown', [()], {}),
    ('shown_values', [(3,)], {}),
    ('make_class', [(1,), (2,)], {}),
    ('class_with_base', [(0,)], {}),
    ('class_closure', [(5,)], {}),
    ('classes_die', [(20,)], {}),
    ('classes_die_after_cutoff', "[(make_and_load, 20), (make_and_store, 20)]",
     {'xfail': "make is inlined into this loop, and some classes promoted "
               "in an inlined body stay held by the loop's code"}),
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
