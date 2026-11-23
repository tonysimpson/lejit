# __class__ assigned while compiled code knows the object's type
# (pobject.c:Psyco_ForgetClasses; for constants, pobject.c:Psyco_ConstantType):
# every use after the
# assignment must see the new class, wherever the assignment ran.
import enum
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


def _module_subclass_misread():
    # Where CPython's specialised attribute load serves a module subclass
    # from the module's dict, ignoring a property the subclass has, plain
    # CPython's answer depends on how warm the site is; run here plain, when
    # run.py reads the cases
    class P(types.ModuleType):
        answer = property(lambda self: 42)

    def read(m):
        return m.answer
    seen = set()
    for i in range(20):
        m = types.ModuleType('probe')
        m.answer = 'attr'
        read(m)
        m.__class__ = P
        seen.add(read(m))
    return 'attr' in seen


MODULE_SUBCLASS_MISREAD = _module_subclass_misread()
MISREAD = ({'xfail': "CPython's specialised load of a module's attribute "
            "ignores a module subclass's property"}
           if MODULE_SUBCLASS_MISREAD else {})


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


# A type forgotten after a call is learnt again by a guard against
# the type it had (pobject.c:Psyco_TypeHintGuard). The cases below run the
# guard both ways: passing for a while, then failing once the class
# changes, in the same compiled code.
def _keep(o):
    return o.x


def _flip_from(o, i, k):
    if i >= k:
        o.__class__ = B if type(o) is A else A


def hint_kept(n):
    out = []
    for i in range(n):
        o = A(i)
        a = o.who()
        _keep(o)
        out.append((a, o.who(), o.kind, type(o).__name__))
        _keep(o)
        out.append(o.kind)
    return out


def hint_fails_later(n):
    out = []
    for i in range(n):
        o = A(i)
        a = o.who()
        _flip_from(o, i, 5)     # the guard passes for i < 5, then fails
        out.append((a, o.who(), o.kind, type(o).__name__))
        _keep(o)
        out.append((o.who(), o.kind))
    return out


def hint_across_loop(n):
    # the type is forgotten at the end of one iteration and needed at the
    # start of the next, past the loop head's merge point
    o = A(0)
    out = []
    for i in range(n):
        out.append((o.kind, o.who()))
        _flip_from(o, i, 6)
    return out


class D(A):
    kind = 'D'

    def who(self):
        return 'D' + str(self.x)


def _cycle(o, i):
    if i >= 4:
        o.__class__ = (A, B, D)[i % 3]


def hint_many_classes(n):
    # once the guard fails, each later class goes to the promotion after it
    out = []
    o = A(0)
    for i in range(n):
        o.x = i
        a = o.who()
        _cycle(o, i)
        out.append((a, o.who(), o.kind, isinstance(o, A)))
    return out


def _flip_two(p, q, i):
    if i == 3:
        q.__class__ = B


def hint_two_values(n):
    # two values with hints; only one changes
    out = []
    for i in range(n):
        p = A(i)
        q = A(-i)
        p.who(), q.who()
        _flip_two(p, q, i)
        out.append((p.who(), q.who(), p.kind, q.kind))
    return out


def _use_g():
    return G.kind, G.who(), type(G).__name__, isinstance(G, B)


def global_instance_elsewhere(n):
    # the code using G is compiled and warm before another function, not
    # called from it, assigns the class
    out = []
    for i in range(n):
        for j in range(3):
            out.append(_use_g())
        _flip_global()
    if G.__class__ is B:
        _flip_global()
    return out


class C3:
    kind = 'C3'

    def __init__(self, x):
        self.x = x

    def who(self):
        return 'C' + str(self.x)


H = A(8)
_CYCLE = {A: B, B: C3, C3: A}


def _cycle_h():
    H.__class__ = _CYCLE[H.__class__]


def global_three_classes(n):
    # each class the object takes past the first adds a check to the chain
    out = []
    for i in range(n):
        out.append((H.kind, H.who()))
        _cycle_h()
    while H.__class__ is not A:
        _cycle_h()
    return out


def global_in_local(n):
    # the constant kept in a local: its checked type is forgotten after
    # the call, as a run-time value's is
    g = G
    out = []
    for i in range(n):
        out.append(g.kind)
        _flip_global()
        out.append((g.kind, g.who()))
    if G.__class__ is B:
        _flip_global()
    return out


class Holder:
    inst = A(9)


def _flip_held():
    o = Holder.inst
    o.__class__ = B if o.__class__ is A else A


def class_attribute(n):
    # an instance held by a class attribute: a constant of the load
    out = []
    for i in range(n):
        out.append((Holder.inst.kind, Holder.inst.who()))
        _flip_held()
    if Holder.inst.__class__ is B:
        _flip_held()
    return out


_DEFAULT = A(10)


def _with_default(o=_DEFAULT):
    return o.kind, o.who()


def _flip_default():
    _DEFAULT.__class__ = B if _DEFAULT.__class__ is A else A


def default_argument(n):
    out = []
    for i in range(n):
        out.append(_with_default())
        _flip_default()
    if _DEFAULT.__class__ is B:
        _flip_default()
    return out


T = (A(11), 'x')


def in_constant_tuple(n):
    # an item of a constant tuple: a constant inside a constant
    out = []
    for i in range(n):
        out.append((T[0].kind, T[0].who()))
        T[0].__class__ = B if T[0].__class__ is A else A
    if T[0].__class__ is B:
        T[0].__class__ = A
    return out


M = types.ModuleType('mconst')
M.answer = 'attr'


def module_constant(n):
    # a module global that is never rebound becomes a ModuleType subclass
    out = []
    for i in range(n):
        out.append((M.answer, type(M).__name__))
        if i == 1:
            _mod_class(M)
    M.__class__ = types.ModuleType
    return out


class _Kind(enum.IntEnum):
    FIRST = 0
    SECOND = 1
    THIRD = 2


_FIRST = _Kind.FIRST
_SECOND = _Kind.SECOND


class _Param:
    def __init__(self, kind, default):
        self.kind = kind
        self.default = default


def enum_constants(n):
    # inspect.Signature.__init__'s loop: constants of a heap type (IntEnum
    # members) in a local, compared, with respawned branches whose replay
    # crosses a block boundary (a respawn replay must forget the checked
    # types where the original's block began; test_types aborted)
    out = []
    for i in range(n):
        params = [_Param(_Kind(j % 3), None if j % 2 else _FIRST)
                  for j in range(i + 3)]
        top = _FIRST
        seen = False
        for p in params:
            kind = p.kind
            if kind < top:
                out.append('lower')
            elif kind > top:
                seen = False
                top = kind
            if kind in (_FIRST, _SECOND):
                if p.default is _FIRST:
                    seen = True
            out.append((int(top), seen))
    return out


def megamorphic_type(n, swap):
    # new classes on every call: the site of view's type goes megamorphic
    # before the assignment is first compiled, and type() must read the
    # type again after it
    a = type('A', (A,), {})
    b = type('B', (A,), {})
    out = []
    for k in range(n):
        cls = a if k % 2 else b
        if swap and k % 3 == 1:
            view = cls(k)
            view.__class__ = b if cls is a else a
            out.append((k, cls.__name__, type(view).__name__))
            continue
        out.append(cls(k).x)
    return out



# Within one instruction that calls Python code more than once, a call can
# change the class of an operand still to be used: the rest of the
# instruction must see the new class.
class Lt:
    def __lt__(self, other):
        return True


class Gt:
    def __gt__(self, other):
        return True

    def __radd__(self, other):
        return 'Gt.radd'

    def __rpow__(self, other):
        return 'Gt.rpow'


class Plain:
    pass


class Changer:
    def __init__(self, victim, to):
        self.victim, self.to = victim, to

    def change(self):
        if self.to is not None:
            self.victim.__class__ = self.to

    def __lt__(self, other):
        self.change()
        return False

    def __gt__(self, other):
        return False

    def __iadd__(self, other):
        self.change()
        return NotImplemented

    def __imul__(self, other):
        self.change()
        return NotImplemented

    def __ipow__(self, other):
        self.change()
        return NotImplemented


def smallest(b, a, c):
    return min(b, a, c)


def iadd(a, c):
    try:
        a += c
    except TypeError as e:
        return str(e)
    return a


def imul(a, c):
    try:
        a *= c
    except TypeError as e:
        return str(e)
    return a


def ipow(a, c):
    try:
        a **= c
    except TypeError as e:
        return str(e)
    return a


def changed_mid_instruction():
    out = []
    for to in [None] * 3 + [Lt, Gt, Plain, None]:
        c = Plain()
        out.append((type(smallest(Changer(c, None), Changer(c, to), c))
                    .__name__, type(c).__name__))
    for f in iadd, imul, ipow:
        for to in [None] * 3 + [Gt, None]:
            c = Plain()
            out.append((f.__name__, f(Changer(c, to), c), type(c).__name__))
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
    ('module_class', [(3,)], MISREAD),
    ('held_in_tuple', [(4,)], {}),
    ('by_finaliser', [(4,)], {}),
    ('global_instance', [(6,)], {}),
    ('global_rebound', [(4,)], {}),
    ('hint_kept', [(6,)], {}),
    ('hint_fails_later', [(10,)], {}),
    ('hint_across_loop', [(12,)], {}),
    ('hint_many_classes', [(12,)], {}),
    ('hint_two_values', [(6,)], {}),
    ('global_instance_elsewhere', [(4,)], {}),
    ('global_three_classes', [(10,)], {}),
    ('global_in_local', [(4,)], {}),
    ('class_attribute', [(6,)], {}),
    ('default_argument', [(6,)], {}),
    ('in_constant_tuple', [(6,)], {}),
    ('module_constant', [(4,)], MISREAD),
    ('enum_constants', [(6,)], {}),
    ('megamorphic_type', [(10, False)] * 5 + [(10, True)] * 6, {}),
    ('changed_mid_instruction', [()], {}),
]
