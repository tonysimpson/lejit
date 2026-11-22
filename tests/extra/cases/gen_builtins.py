# Builtins whose metas are generated from Python/bltinmodule.c:
# abs, len, hash, id, divmod, pow, isinstance, issubclass, getattr,
# setattr. Each with subclasses, errors and their messages, keyword
# arguments (CPython's own refusal), wrong counts (the C function's
# message), and shadowed builtins (no meta).


def msg(f, *a, **k):
    try:
        return ('ok', f(*a, **k))
    except Exception as e:
        return (type(e).__name__, str(e))


class MyList(list):
    def __len__(self):
        return 3


class HugeLen:
    def __len__(self):
        return 2 ** 70


class NegLen:
    def __len__(self):
        return -5


class StrLen:
    def __len__(self):
        return 'x'


def lens(a):
    return [len(a), len(MyList([1])), msg(len, HugeLen()), msg(len, NegLen()),
            msg(len, StrLen()), msg(len, 5), msg(len)]


class MyInt(int):
    def __hash__(self):
        return 2 ** 80

    def __abs__(self):
        return 'myabs'

    def __divmod__(self, o):
        return 'mydivmod'

    def __rdivmod__(self, o):
        return 'myrdivmod'

    def __pow__(self, o, m=None):
        return ('mypow', o, m)


class Unhashable:
    __hash__ = None


class BadHash:
    def __hash__(self):
        return 'h'


def hashes(a):
    return [hash(a) == hash(a), hash(MyInt(1)), msg(hash, Unhashable()),
            msg(hash, []), msg(hash, BadHash()), hash(-1), hash(True)]


def abss(a):
    return [abs(a), abs(MyInt(-3)), msg(abs, 'x'), abs(True), abs(-0.0)]


def ids(a):
    b = [a]
    return [id(a) == id(a), id(b) == id(b[0]), id(b[0]) == id(a),
            msg(id), msg(id, 1, 2)]


def divmods(a, b):
    return [divmod(a, b), divmod(MyInt(7), 2), divmod(7, MyInt(2)),
            msg(divmod, a, 0), msg(divmod, 'a', b), msg(divmod, a),
            msg(divmod, a, b, 1), msg(divmod, x=a, y=b)]


def pows(a, b):
    return [pow(a, b), pow(a, b, 5), pow(a, b, None), pow(MyInt(2), b),
            pow(MyInt(2), b, 7), msg(pow, a, -1, 5), msg(pow, 2.0, b, 3),
            msg(pow, a), msg(pow, a, b, 1, 2), msg(pow, x=a, y=b)]


class Meta(type):
    def __instancecheck__(cls, inst):
        return inst == 'magic'

    def __subclasscheck__(cls, sub):
        return sub is int


class Magic(metaclass=Meta):
    pass


class Base:
    pass


class Derived(Base):
    pass


class TupleSub(tuple):
    pass


def isinstances(a):
    return [isinstance(a, int), isinstance(a, (str, int)), isinstance(True, int),
            isinstance('magic', Magic), isinstance(a, Magic), isinstance(Derived(), Base),
            isinstance(a, TupleSub((str, int))), msg(isinstance, a, 1),
            msg(isinstance, a, (int, 1)), msg(isinstance, a),
            msg(isinstance, a, int, str), msg(isinstance, obj=a, class_or_tuple=int)]


def issubclasses(t):
    return [issubclass(t, int), issubclass(bool, t), issubclass(t, (str, t)),
            issubclass(Derived, Base), issubclass(Base, Derived), issubclass(int, Magic),
            issubclass(str, Magic), issubclass(t, TupleSub((str, t))),
            msg(issubclass, 1, int), msg(issubclass, t, 1), msg(issubclass, t, (int, 1)),
            msg(issubclass, t), msg(issubclass, t, t, t),
            msg(issubclass, cls=t, class_or_tuple=t)]


class WithGetattr:
    x = 1

    def __getattr__(self, name):
        if name == 'missing':
            raise AttributeError(name)
        return 'dyn ' + name


class Prop:
    @property
    def p(self):
        raise AttributeError('inner')

    @property
    def q(self):
        raise ValueError('not attribute')


class Slots:
    __slots__ = ('a',)


class StrSub(str):
    pass


def getattrs(o, name):
    w = WithGetattr()
    return [getattr(o, name), getattr(o, 'real'), getattr(o, 'nope', 'dflt'),
            getattr(w, 'x'), getattr(w, 'y'), getattr(w, 'missing', 'm'),
            msg(getattr, w, 'missing'), getattr(Prop(), 'p', 'pd'),
            msg(getattr, Prop(), 'q', 'qd'), msg(getattr, Slots(), 'a'),
            getattr(Slots(), 'a', None), getattr(o, StrSub('real')),
            msg(getattr, o, 1), msg(getattr, o, 1, 2), msg(getattr, o),
            msg(getattr, o, 'real', 1, 2), msg(getattr, o, name='real'),
            getattr(int, '__name__')]


class Rec:
    def __setattr__(self, name, value):
        object.__setattr__(self, name, ('set', value))


class Frozen:
    __slots__ = ()


def setattrs(v, name):
    class C:
        pass
    c = C()
    r = Rec()
    out = [setattr(c, name, v), c.attr, setattr(c, 'b', v + 1), c.b,
           setattr(r, 'z', v), r.z, setattr(C, 'k', v), C.k,
           setattr(c, StrSub('s'), 3), c.s]
    out += [msg(setattr, Frozen(), 'a', 1), msg(setattr, 1, 'real', 2),
            msg(setattr, c, 1, 2), msg(setattr, c, 'a'), msg(setattr, c, 'a', 1, 2),
            msg(setattr, obj=c, name='a', value=1)]
    return out


def attr_loop(n):
    class P:
        pass
    p = P()
    t = 0
    for i in range(n):
        setattr(p, 'v', i)
        t += getattr(p, 'v') + getattr(p, 'w', 1)
    return t, hasattr(p, 'v')


def count_loop(n):
    t = 0
    for i in range(n):
        t += len((i, i)) + hash(i) + abs(-i) + divmod(i, 3)[0] + pow(i, 2) \
            + isinstance(i, int) + issubclass(type(i), int)
    return t


def star_args(t, d):
    # unknown counts and keywords: the C function's own result or message
    return [msg(getattr, *t), msg(setattr, *t), msg(pow, *t), msg(divmod, *t),
            msg(isinstance, *t), msg(issubclass, *t), msg(getattr, 1, 'real', **d)]


def shadow_local(x):
    len = lambda o: 'local len'
    abs = str
    return len(x), abs(x)


# a function whose globals shadow the builtin: no meta
_ns = {}
exec('def shadow_global(x):\n    return len(x), abs(x)\n',
     {'len': lambda o: 'module len', 'abs': repr}, _ns)
shadow_global = _ns['shadow_global']


def call_builtins_module(x):
    import builtins
    return builtins.len(x), builtins.getattr(x, 'real'), builtins.isinstance(x, int)


def rebind_builtin(x):
    import builtins
    old = builtins.hash
    try:
        r1 = hash(x)
        builtins.hash = lambda o: 'rebound'
        r2 = hash(x)
    finally:
        builtins.hash = old
    return r1, r2, hash(x)


CASES = [
    ('lens', [([1, 2],), ((),), ('abc',)], {}),
    ('hashes', [(1,), ('s',), ((1, 2),)], {}),
    ('abss', [(-3,), (2.5,), (-(2 ** 70),)], {}),
    ('ids', [(1,), ('s',)], {}),
    ('divmods', [(7, 2), (-7, 2), (7.5, 2.0), (2 ** 70, 3)], {}),
    ('pows', [(2, 3), (3, 4), (-2, 3)], {}),
    ('isinstances', [(1,), ('s',), (True,), (2.5,), (None,)], {}),
    ('issubclasses', "[(int,), (bool,), (str,), (object,)]", {}),
    ('getattrs', [(1, 'real'), (2.5, 'imag'), (True, 'real')], {}),
    ('setattrs', [(1, 'attr'), (5, 'attr')], {}),
    ('attr_loop', [(0,), (20,)], {}),
    ('count_loop', [(0,), (30,)], {}),
    ('star_args', "[((), {}), ((1,), {}), ((1, 'real'), {}), ((1, 'real', 2), {'x': 1}), ((int, int, int), {})]", {}),
    ('shadow_local', [(1,), ('ab',)], {}),
    ('shadow_global', [([1],), ('ab',)], {}),
    ('call_builtins_module', [(1,), (True,)], {}),
    ('rebind_builtin', [(1,), (-7,)], {}),
]
