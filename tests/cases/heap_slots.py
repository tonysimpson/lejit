# A class's slots change when a special method is set or deleted on it,
# after code using them is compiled:
# __bool__ or __len__ in a truth test, the number slots, __contains__,
# __getitem__/__setitem__, len() and __call__. Compiled code must see
# the class as it is when it runs.


def truth(x):
    if x:
        return 'true'
    return 'false'


class T:
    pass


def added_bool():
    t = T()
    r = [truth(t)]
    T.__bool__ = lambda self: False
    r.append(truth(t))
    del T.__bool__
    r.append(truth(t))
    T.__len__ = lambda self: 0
    r.append(truth(t))
    del T.__len__
    r.append(truth(t))
    return r


class TL:
    def __len__(self):
        return 0


def deleted_len():
    t = TL()
    r = [truth(t)]
    f = TL.__len__
    del TL.__len__
    r.append(truth(t))
    TL.__len__ = f
    return r


def add(a, b):
    return a + b


def mul(a, b):
    return a * b


def iadd(a, b):
    a += b
    return a


def neg(a):
    return -a


def attempt(f, *args):
    try:
        return f(*args)
    except TypeError as e:
        return str(e)


class N:
    pass


def added_add():
    a = N()
    r = [attempt(add, a, 1), attempt(add, 1, a), attempt(neg, a),
         attempt(iadd, a, 1)]
    N.__add__ = lambda self, o: ('add', o)
    N.__radd__ = lambda self, o: ('radd', o)
    N.__neg__ = lambda self: 'neg'
    r += [attempt(add, a, 1), attempt(add, 1, a), attempt(neg, a),
          attempt(iadd, a, 1)]
    N.__iadd__ = lambda self, o: ('iadd', o)
    r.append(attempt(iadd, a, 1))
    del N.__add__, N.__radd__, N.__neg__, N.__iadd__
    r += [attempt(add, a, 1), attempt(add, 1, a), attempt(neg, a),
          attempt(iadd, a, 1)]
    return r


class I(int):
    pass


def overridden_int():
    a = I(2)
    r = [add(a, 1), mul(a, 3), neg(a)]
    I.__add__ = lambda self, o: 'add'
    I.__mul__ = lambda self, o: 'mul'
    I.__neg__ = lambda self: 'neg'
    r += [add(a, 1), mul(a, 3), neg(a)]
    del I.__add__, I.__mul__, I.__neg__
    r += [add(a, 1), mul(a, 3), neg(a)]
    return r


class L(list):
    pass


def overridden_list():
    a = L([1])
    r = [add(a, [2]), mul(a, 2)]
    L.__add__ = lambda self, o: 'add'
    L.__mul__ = lambda self, o: 'mul'
    r += [add(a, [2]), mul(a, 2)]
    L.__add__ = L.__mul__ = lambda self, o: NotImplemented
    r += [attempt(add, a, [2]), attempt(mul, a, 2), attempt(mul, 2, a)]
    del L.__add__, L.__mul__
    r += [add(a, [2]), mul(a, 2), mul(2, a)]
    return r


def getitem(a, i):
    return a[i]


def setitem(a, i, x):
    a[i] = x
    return a


def contains(a, x):
    return x in a


def length(a):
    return len(a)


def attempt_any(f, *args):
    try:
        return f(*args)
    except (TypeError, IndexError, KeyError) as e:
        return '%s: %s' % (type(e).__name__, e)


class G:
    def __iter__(self):
        return iter([1, 2])


def added_items():
    g = G()
    r = [attempt_any(getitem, g, 0), attempt_any(setitem, g, 0, 5),
         contains(g, 3), attempt_any(length, g)]
    G.__getitem__ = lambda self, i: ('get', i)
    G.__setitem__ = lambda self, i, x: None
    G.__contains__ = lambda self, x: True
    G.__len__ = lambda self: 7
    r += [getitem(g, 0), setitem(g, 0, 5) is g, contains(g, 3), length(g)]
    del G.__getitem__, G.__setitem__, G.__contains__, G.__len__
    r += [attempt_any(getitem, g, 0), attempt_any(setitem, g, 0, 5),
          contains(g, 3), attempt_any(length, g)]
    return r


class D(dict):
    pass


class LL(list):
    pass


class LL2(list):
    pass


def list_len_only():
    # list.__getitem__ uses the list's own length, not a subclass's
    l = LL2([1, 2])
    r = [getitem(l, -1), getitem(l, 0)]
    LL2.__len__ = lambda self: 1
    r += [getitem(l, -1), getitem(l, 0), length(l)]
    del LL2.__len__
    r += [getitem(l, -1), length(l)]
    return r


def overridden_containers():
    d = D(a=1)
    l = LL([1, 2])
    r = [getitem(d, 'a'), contains(d, 'b'), length(d), getitem(l, -1),
         setitem(l, 0, 9)[:], length(l)]
    D.__getitem__ = lambda self, k: 'get'
    D.__contains__ = lambda self, k: 'yes'
    D.__len__ = lambda self: 99
    LL.__getitem__ = lambda self, i: 'get'
    LL.__setitem__ = lambda self, i, x: None
    LL.__len__ = lambda self: 99
    r += [getitem(d, 'a'), contains(d, 'b'), length(d), getitem(l, -1),
          setitem(l, 0, 8)[:], length(l)]
    del D.__getitem__, D.__contains__, D.__len__
    del LL.__getitem__, LL.__setitem__, LL.__len__
    r += [getitem(d, 'a'), contains(d, 'b'), length(d), getitem(l, -1),
          setitem(l, 0, 7)[:], length(l)]
    return r


def call(f, x):
    return f(x)


class K:
    pass


def added_call():
    k = K()
    r = [attempt(call, k, 1)]
    K.__call__ = lambda self, x: ('call', x)
    r.append(call(k, 2))
    del K.__call__
    r.append(attempt(call, k, 3))
    return r


# a __bases__ assignment changes which operand's slot goes first; the
# slots themselves stay the same
class P:
    def __eq__(self, o):
        return 'P'


class R:
    pass


class V(P):
    def __eq__(self, o):
        return 'V'


def eq(a, b):
    return a == b


def changed_bases():
    p, v = P(), V()
    r = [eq(p, v)]
    V.__bases__ = (R,)
    r.append(eq(p, v))
    V.__bases__ = (P,)
    return r


class F(float):
    pass


class F2(float):
    pass


class FG(F):
    def __radd__(self, o):
        return 'FG'


def changed_bases_add():
    f, g = F(1.0), FG(2.0)
    r = [add(f, g)]
    FG.__bases__ = (F2,)
    r.append(add(f, g))
    FG.__bases__ = (F,)
    return r


class Ix:
    pass


def added_index():
    import collections
    d = collections.deque([1, 2])
    i = Ix()
    r = [attempt(getitem, d, i), attempt(setitem, d, i, 5),
         attempt(mul, [1], i)]
    Ix.__index__ = lambda self: 1
    r += [getitem(d, i), list(setitem(d, i, 5)), mul([1], i)]
    del Ix.__index__
    r += [attempt(getitem, d, i), attempt(setitem, d, i, 5),
          attempt(mul, [1], i)]
    return r


CASES = [
    ('added_bool', [()], {}),
    ('deleted_len', [()], {}),
    ('added_add', [()], {}),
    ('overridden_int', [()], {}),
    ('overridden_list', [()], {}),
    ('added_items', [()], {}),
    ('overridden_containers', [()], {}),
    ('list_len_only', [()], {}),
    ('added_call', [()], {}),
    ('changed_bases', [()], {}),
    ('changed_bases_add', [()], {}),
    ('added_index', [()], {}),
]
