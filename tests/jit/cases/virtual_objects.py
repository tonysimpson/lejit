# Virtual objects generated from CPython's constructors: tuples,
# slices and bound methods built by gen_virtual.py. Tuple concatenation,
# repetition and comparison of them: an operand given back or the empty
# tuple are known now, the rest is CPython's own at run time. Forcing: a
# constant result is built at compile time, any other by CPython's
# constructor at run time.


class T(tuple):
    pass


class Seq:
    def __radd__(self, o):
        return 'radd'

    def __rmul__(self, o):
        return 'rmul'


class Idx:
    def __index__(self):
        return 2


class Eq:
    def __init__(self, v):
        self.v = v

    def __eq__(self, o):
        if self.v == 'raise':
            raise ValueError('eq')
        return isinstance(o, Eq) and self.v == o.v

    def __lt__(self, o):
        return self.v < o.v

    __hash__ = None


# ---- tuple comparisons ----
def compare_virtual(a, b, c):
    return ((a, b) == (a, b), (a, b) != (a, c), (a, b) < (a, c),
            (a, b) <= (a,), (a, b) > (a,), (a,) >= (a, b), (a, b) == [a, b])


def compare_items(x, y):
    try:
        return (x, 1) == (y, 1), (x, 1) < (y, 2)
    except Exception as e:
        return type(e).__name__, str(e)


def compare_nan(x):
    return (x,) == (x,), (x,) == (float(x),), (x, 1) < (x, 2)


def compare_subclass(a):
    return T((a, 1)) == (a, 1), (a, 1) < T((a, 2)), T((a,)) != T((a,))


# ---- tuple + tuple ----

def concat_virtual(a, b, c):
    t = (a, b) + (c,)
    return t, len(t), t[2]


def concat_const(x):
    return (1, 2) + (3, x)


def concat_runtime(t, u):
    return t + u


def concat_identity(t):
    # an empty operand gives the other one, if that is an exact tuple
    r = t + ()
    s = () + t
    return r is t, s is t, r


def concat_virtual_identity(a):
    t = (a, a)
    return (t + ()) is t, (() + t) is t


def concat_subclass(x):
    t = T((1, 2))
    r = t + ()
    s = () + t
    return type(r).__name__, r is t, type(s).__name__, r, t + (x,)


def concat_error(x):
    try:
        return (1, 2) + x
    except TypeError as e:
        return 'TypeError %s' % e


def concat_long(x):
    # past the item limit: CPython's tupleconcat at run time
    t = (x, x, x, x, x, x, x, x, x)
    return t + t, len(t + t)


def concat_escapes(a, b):
    t = (a,) + (b,)
    return repr(t), hash(t) == hash((a, b))


def concat_in_loop(n):
    t = ()
    for i in range(n):
        t = t + (i,)
    return t


# ---- tuple * int ----

def repeat_const(x):
    return (x, 1) * 3


def repeat_virtual(a, b):
    t = (a, b) * 2
    return t, len(t), t[3]


def repeat_runtime_n(a, n):
    return (a, 1) * n


def repeat_runtime_tuple(t):
    return t * 2


def repeat_identity(t):
    r = t * 1
    return r is t, (t * 0) is t, t * 0


def repeat_empty(n):
    e = ()
    return e * n, (e * n) is e


def repeat_negative(a):
    return (a,) * -3, (a,) * 0


def repeat_subclass(x):
    t = T((x,))
    r = t * 1
    e = T(()) * 5
    return type(r).__name__, r is t, type(e).__name__, e, t * 2


def repeat_long(x):
    return (x, x) * 9


def repeat_index(x):
    return (x,) * Idx()


def repeat_errors(x):
    out = []
    for n in (2.0, 'a', None):
        try:
            out.append((x,) * n)
        except TypeError as e:
            out.append('TypeError %s' % e)
    return out


def repeat_reflected(x):
    return (x,) + Seq(), (x,) * Seq()


def repeat_big():
    try:
        return (1, 2) * (2 ** 62)
    except MemoryError:
        return 'MemoryError'
    except OverflowError as e:
        return 'OverflowError %s' % e


def repeat_escapes(a):
    t = (a,) * 3
    return repr(t), sorted(t)


# ---- slices ----

def slice_const(l):
    s = slice(1, 3)
    return l[s], s, s.start, s.stop, s.step


def slice_runtime(l, i, j):
    return l[i:j], l[i:j:2]


def slice_escapes(i):
    s = slice(i, None)
    return s, s.start, repr(s)


def slice_forced_const():
    s = slice(1, 2, 3)
    return s, type(s).__name__, s == slice(1, 2, 3)


def slice_errors(l, i):
    try:
        return l[i:]
    except TypeError as e:
        return 'TypeError %s' % e


# ---- bound methods ----

class C:
    def __init__(self, v):
        self.v = v

    def get(self):
        return self.v

    def add(self, x):
        return self.v + x


class D(C):
    def get(self):
        return -self.v


SHARED = C(7)


def method_call(c, x):
    return c.get(), c.add(x)


def method_escapes(c):
    m = c.get
    return m(), m.__self__ is c, m.__func__ is C.get


def method_const():
    m = SHARED.get
    return m(), m.__self__ is SHARED


def method_subclass(c):
    return c.get(), type(c.get).__name__


def method_compare(c):
    return c.get == c.get, c.get is c.get


# a tuple compiled code builds is tracked by the GC as CPython's own: from
# 3.14 one whose items' types the GC doesn't track starts untracked. The
# collector is off meanwhile: a collection untracks such a tuple in CPython
# too, and compiling the first call can start one
def tuples_tracked(n):
    import gc
    o = object()
    on = gc.isenabled()
    gc.disable()
    try:
        built = [(n, n + 1), (o,), (n, 'a', 1.5), ([],), (n, {}),
                 ((n, 2), 3), (n, (o,))]
        return [gc.is_tracked(t) for t in built]
    finally:
        if on:
            gc.enable()


CASES = [
    ('compare_virtual', [(1, 2, 3), ('a', 'b', 'a'), (2.5, 1, 1)], {}),
    ('compare_items', "[(Eq(1), Eq(1)), (Eq(1), Eq(2)), (Eq('raise'), Eq(1)), (1, 'x')]", {}),
    ('compare_nan', "[(nan,), (1.0,)]", {}),
    ('compare_subclass', [(1,), ('z',)], {}),
    ('concat_virtual', [(1, 2, 3), ('a', None, 2.5)], {}),
    ('concat_const', [(4,), ('x',)], {}),
    ('concat_runtime', [((1,), (2,)), ((), (1, 2)), ((1, 2), ()), ((), ()),
                        ((1,), [2]), ((1,), 'ab')], {}),
    ('concat_identity', [((1, 2),), ((),)], {}),
    ('concat_virtual_identity', [(1,), ('a',)], {}),
    ('concat_subclass', [(3,)], {}),
    ('concat_error', "[([3],), (None,), ((3,),), (T((4,)),), (Seq(),)]", {}),
    ('concat_long', [(1,), ('a',)], {}),
    ('concat_escapes', [(1, 2), ('a', 'b')], {}),
    ('concat_in_loop', [(0,), (3,), (20,)], {}),
    ('repeat_const', [(5,), ('a',)], {}),
    ('repeat_virtual', [(1, 2), ('a', None)], {}),
    ('repeat_runtime_n', [(1, 0), (1, 1), (1, 3), (1, -1), ('a', 20)], {}),
    ('repeat_runtime_tuple', [((1,),), ((),), ((1, 2, 3),)], {}),
    ('repeat_identity', [((1, 2),), ((),)], {}),
    ('repeat_empty', [(0,), (3,), (-1,)], {}),
    ('repeat_negative', [(1,), ('a',)], {}),
    ('repeat_subclass', [(1,), ('a',)], {}),
    ('repeat_long', [(1,), ('z',)], {}),
    ('repeat_index', [(1,), ('a',)], {}),
    ('repeat_errors', [(1,)], {}),
    ('repeat_reflected', [(1,)], {}),
    ('repeat_big', [()], {}),
    ('repeat_escapes', [(3,), (1,)], {}),
    ('slice_const', [([1, 2, 3, 4],), ((1, 2, 3, 4),), ('abcd',)], {}),
    ('slice_runtime', [([1, 2, 3, 4], 1, 3), ('abcdef', 0, 5), ((1, 2), None, 1)], {}),
    ('slice_escapes', [(1,), (None,), ('a',)], {}),
    ('slice_forced_const', [()], {}),
    ('slice_errors', [([1, 2], 1), ([1, 2], 'a'), ((1, 2), 1.5)], {}),
    ('method_call', "[(C(1), 2), (D(3), 4), (C('a'), 'b')]", {}),
    ('method_escapes', "[(C(1),), (D(2),)]", {}),
    ('method_const', [()], {}),
    ('method_subclass', "[(C(1),), (D(2),)]", {}),
    ('method_compare', "[(C(1),), (D(2),)]", {}),
    ('tuples_tracked', [(1,), (5,)], {}),
]
