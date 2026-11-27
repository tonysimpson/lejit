# Stores into __slots__ (object members) are written inline: the old value
# is released after the new one is stored, as PyMember_SetOne does, so a
# __del__ runs at CPython's moment; a deletion, a deleted slot, a read-only
# member and a slot replaced on the class keep CPython's behaviour and
# messages. A slot placed after a variable-size object's items (a tuple
# subclass, where CPython allows it) is found from the size.
import gc


class Logged:
    def __init__(self, log, n):
        self.log = log
        self.n = n

    def __del__(self):
        self.log.append(('del', self.n))


class Pair:
    __slots__ = ('a', 'b')

    def __init__(self, a, b):
        self.a = a
        self.b = b


def stores(n):
    out = []
    for i in range(n):
        p = Pair(i, None)
        p.a = p.a + 1
        p.b = str(i)
        out.append((p.a, p.b))
    return out


def release_order(n):
    log = []
    for i in range(n):
        p = Pair(Logged(log, i), None)
        log.append(('store', i))
        p.a = Logged(log, -i)   # the old value dies here
        log.append(('stored', i))
        p.a = p.a               # the same object: kept
        log.append(('again', i))
        del p
    gc.collect()
    return log


def attempt(f, *args):
    try:
        return f(*args)
    except (AttributeError, TypeError) as e:
        return type(e).__name__, str(e)


def get_a(p):
    return p.a


def del_a(p):
    del p.a


def set_a(p, v):
    p.a = v
    return 'ok'


def deleted(n):
    out = []
    for i in range(n):
        p = Pair(i, i)
        out.append(attempt(del_a, p))
        out.append(attempt(get_a, p))
        out.append(attempt(del_a, p))
        out.append(set_a(p, i + 1))
        out.append(get_a(p))
    return out


class Triple(Pair):
    __slots__ = ('c',)


def subclass(n):
    out = []
    for i in range(n):
        t = Triple(i, i)
        t.c = t.a + t.b
        out.append((set_a(t, 'x'), t.a, t.c))
    return out


class Replaced:
    __slots__ = ('a',)


def replaced_by_property(n):
    log = []
    r = Replaced()
    out = [set_a(r, 1), get_a(r)]
    Replaced.a = property(lambda s: 'property',
                          lambda s, v: log.append(v))
    for i in range(n):
        out.append(set_a(r, i))
    out.append(get_a(r))
    out.append(log)
    return out


class Shared:
    __slots__ = ('a',)


class Other:
    __slots__ = ('a',)


def set_and_get(o, v):
    o.a = v
    return o.a


def class_assigned(n):
    out = []
    for i in range(n):
        o = Shared()
        out.append(set_and_get(o, i))
        o.__class__ = Other
        out.append((set_and_get(o, -i), type(o).__name__))
    return out


class Prop(property):
    pass


def set_fget(p, v):
    p.fget = v


def read_only(n):
    # property's fget is a read-only member
    p = Prop(lambda s: 1)
    return [attempt(set_fget, p, i) for i in range(n)]


try:
    class Items(tuple):
        __slots__ = ('a',)
except TypeError:
    Items = None


def after_items(n):
    if Items is None:
        return None
    out = []
    for i in range(n):
        t = Items(range(i))
        t.a = len(t)
        out.append((t.a, t))
    return out


CASES = [
    ('stores', [(3,)], {}),
    ('release_order', [(3,)], {}),
    ('deleted', [(3,)], {}),
    ('subclass', [(3,)], {}),
    ('replaced_by_property', [(3,)], {}),
    ('class_assigned', [(3,)], {}),
    ('read_only', [(3,)], {}),
    ('after_items', [(4,)], {}),
]
