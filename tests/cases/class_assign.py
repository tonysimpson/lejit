# __class__ assigned while compiled code knows the object's type
# (KNOWN_BUGS F55, pobject.c:Psyco_ForgetClasses): every use after the
# assignment must see the new class, wherever the assignment ran.
import types


class A:
    kind = 'A'

    def __init__(self, x):
        self.x = x

    def who(self):
        return 'A' + str(self.x)

    def become_b(self):
        self.__class__ = B
        return self.x


class B:
    kind = 'B'

    def __init__(self, x):
        self.x = x

    def who(self):
        return 'B' + str(self.x)

    def become_b(self):
        return -1

    @property
    def y(self):
        return 'property y'

    @y.setter
    def y(self, v):
        self.__dict__['set_y'] = v


def _to_b(o):
    o.__class__ = B


def _to_a(o):
    o.__class__ = A


def in_called_function(n):
    out = []
    for i in range(n):
        o = A(i)
        out.append((o.who(), o.kind, type(o).__name__))
        _to_b(o)
        o.y = i                 # B's property setter, not the dict
        out.append((o.who(), o.kind, type(o).__name__, o.y,
                    sorted(o.__dict__)))
    return out


def in_own_method(n):
    out = []
    for i in range(n):
        o = A(i)
        o.who()
        r = o.become_b()
        out.append((r, o.who(), o.kind, isinstance(o, B), o.become_b()))
    return out


def _morph(f):
    def wrapper(self, *args):
        r = f(self, *args)
        self.__class__ = B
        return r
    return wrapper


class C(A):
    @_morph
    def step(self):
        return self.x + 1


def via_decorator(n):
    out = []
    for i in range(n):
        o = C(i)
        o.who()
        r = o.step()
        out.append((r, o.who(), type(o).__name__))
    return out


def assigned_back(n):
    out = []
    for i in range(n):
        o = A(i)
        a = o.who()
        _to_b(o)
        b = o.who()
        _to_a(o)
        out.append((a, b, o.who(), o.kind, type(o) is A))
    return out


def in_a_loop(n):
    # the state pattern: the class flips on every iteration
    o = A(0)
    out = []
    for i in range(n):
        o.x = i
        out.append(o.who())
        if i % 2:
            _to_a(o)
        else:
            _to_b(o)
        out.append(o.kind)
    return out


def by_setattr(n):
    out = []
    for i in range(n):
        o = A(i)
        o.who()
        setattr(o, '__class__', B)
        out.append(o.who())
        object.__setattr__(o, '__class__', A)
        out.append(o.who())
    return out


def _raise_after(o):
    o.__class__ = B
    raise ValueError(o.x)


def then_raises(n):
    out = []
    for i in range(n):
        o = A(i)
        o.who()
        try:
            _raise_after(o)
        except ValueError:
            out.append((o.who(), o.kind))
    return out


def _cond(o):
    o.__class__ = B
    return o.x % 2


def in_a_condition(n):
    # the call is in the test of an 'if': the branch sees the new class
    out = []
    for i in range(n):
        o = A(i)
        o.who()
        if _cond(o):
            out.append(o.who())
        else:
            out.append(o.kind)
    return out


def _call_on(o):
    return o.who()


def passed_on(n):
    # the object passed to a compiled callee after the change
    out = []
    for i in range(n):
        o = A(i)
        o.who()
        _to_b(o)
        out.append(_call_on(o))
    return out


class LA(list):
    def tag(self):
        return 'LA', len(self)


class LB(list):
    def tag(self):
        return 'LB', len(self)


def list_subclass(n):
    out = []
    for i in range(n):
        o = LA(range(i))
        o.tag()
        o.__class__ = LB
        o.append(i)
        out.append(o.tag())
    return out


class Mod(types.ModuleType):
    @property
    def answer(self):
        return 42


def _mod_class(m):
    m.__class__ = Mod


def module_class(n):
    # a plain module (a static type) can become a ModuleType subclass
    out = []
    for i in range(n):
        m = types.ModuleType('m%d' % i)
        m.answer = 'attr'
        out.append(m.answer)
        _mod_class(m)
        out.append((m.answer, type(m).__name__))
    return out


def held_in_tuple(n):
    # the object inside a virtual tuple keeps no stale type either
    out = []
    for i in range(n):
        o = A(i)
        t = (o, i)
        t[0].who()
        _to_b(t[0])
        out.append(t[0].who())
    return out


class _Changer:
    def __init__(self, o):
        self.o = o

    def __del__(self):
        self.o.__class__ = B


def by_finaliser(n):
    # a __del__ run by a decref, with the type promoted after the last call
    out = []
    for i in range(n):
        o = A(i)
        c = _Changer(o)
        k = o.kind
        c = None
        out.append((k, o.kind, o.who()))
    return out


G = A(7)


def _flip_global():
    G.__class__ = B if G.__class__ is A else A


def global_instance(n):
    # an object that compiled code holds as a constant (a global that is
    # never rebound): its type is read when the code compiles
    out = []
    for i in range(n):
        out.append(G.kind)
        _flip_global()
        out.append(G.kind)
    if G.__class__ is B:
        _flip_global()
    return out


def global_rebound(n):
    # the same global rebound each time: not a constant
    global G
    out = []
    for i in range(n):
        G = A(i)
        out.append(G.who())
        _flip_global()
        out.append(G.who())
    G = A(7)
    return out


CASES = [
    ('in_called_function', [(4,)], {}),
    ('in_own_method', [(4,)], {}),
    ('via_decorator', [(4,)], {}),
    ('assigned_back', [(4,)], {}),
    ('in_a_loop', [(10,)], {}),
    ('by_setattr', [(4,)], {}),
    ('then_raises', [(4,)], {}),
    ('in_a_condition', [(4,)], {}),
    ('passed_on', [(4,)], {}),
    ('list_subclass', [(4,)], {}),
    ('module_class', [(3,)], {}),
    ('held_in_tuple', [(4,)], {}),
    ('by_finaliser', [(4,)], {}),
    ('global_instance', [(6,)],
     {'xfail': 'F96: a constant object\'s type is read when code compiles'}),
    ('global_rebound', [(4,)], {}),
]
