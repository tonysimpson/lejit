# Calls of classes known when compiling (globals), with positional and
# keyword arguments, that LeJIT doesn't turn into object_new and a direct
# __init__ call: keywords, a custom __new__, a C base's __new__, an
# __init__ that isn't a plain function, f(*a) and f(**k). They go to the
# class's vectorcall where it has one, else to type_call with a tuple and
# a dict (ptypeobject.c:class_call), and must give what CPython gives:
# results, what __new__ and __init__ see, and CPython's errors.
import fractions


class Plain:
    def __init__(self, a, b=2, *, c=3):
        self.seen = (a, b, c)

    def __repr__(self):
        return 'Plain%r' % (self.seen,)


class Starred:
    def __init__(self, *args, **kw):
        # the dict is the callee's own: changing it changes nothing outside
        kw['added'] = len(kw)
        self.seen = (args, sorted(kw.items()))

    def __repr__(self):
        return 'Starred%r' % (self.seen,)


class New:
    def __new__(cls, a, b=0, **kw):
        self = super().__new__(cls)
        self.made = (a, b, sorted(kw.items()))
        return self

    def __repr__(self):
        return 'New%r' % (self.made,)


class NewInit(New):
    def __init__(self, a, b=0, **kw):
        self.inited = (a, b, sorted(kw.items()))

    def __repr__(self):
        return 'NewInit%r%r' % (self.made, self.inited)


class Other:
    pass


class NewOther:
    # __new__ returning another type's instance: no __init__
    def __new__(cls, x=0, *, y=0):
        return x + y

    def __init__(self, *args, **kw):
        raise AssertionError('not called')


class InitCallable:
    class Init:
        def __call__(self, obj, *args, **kw):
            obj.seen = (args, sorted(kw.items()))

    __init__ = Init()

    def __repr__(self):
        return 'InitCallable%r' % (self.seen,)


class List(list):
    def __init__(self, items=(), *, extra=None):
        super().__init__(items)
        self.extra = extra

    def __repr__(self):
        return 'List(%s, %r)' % (list.__repr__(self), self.extra)


class Dict(dict):
    pass


class Meta(type):
    def __call__(cls, *args, **kw):
        return ('meta', cls.__name__, args, sorted(kw.items()))


class WithMeta(metaclass=Meta):
    pass


def positional(n):
    out = []
    for i in range(n):
        out.append(repr(Plain(i)))
        out.append(repr(Plain(i, i + 1)))
        out.append(repr(Starred(i, 'x', None)))
        out.append(repr(New(i, 2)))
        out.append(repr(NewInit(i)))
        out.append(repr(NewOther(i)))
        out.append(repr(InitCallable(i, i)))
        out.append(repr(List([i, 1])))
        out.append(repr(Dict([(i, 1)])))
        out.append(repr(WithMeta(i)))
    return out


def keywords(n):
    out = []
    for i in range(n):
        out.append(repr(Plain(i, c=i)))
        out.append(repr(Plain(a=i, b=1, c=2)))
        out.append(repr(Starred(i, x=i, y=(i, [i]))))
        out.append(repr(New(i, b=i, z=1)))
        out.append(repr(NewInit(a=i, q=2)))
        out.append(repr(NewOther(i, y=1)))
        out.append(repr(InitCallable(i, k=i)))
        out.append(repr(List([i], extra=i)))
        out.append(repr(Dict(a=i, b=2)))
        out.append(repr(WithMeta(i, k=i)))
        out.append(repr(fractions.Fraction(i, 3)))
    return out


def starred(n):
    out = []
    args = (1, 2)
    kw = {'c': 5}
    empty = {}
    for i in range(n):
        out.append(repr(Plain(*args)))
        out.append(repr(Plain(*args, **kw)))
        out.append(repr(Plain(i, **kw)))
        out.append(repr(Plain(*args, **empty)))
        out.append(repr(Starred(*args, **kw)))
        out.append(repr(New(*args, **kw)))
        out.append(repr(Dict(**kw)))
    return out


# the values the instance keeps outlive the call
def kept(n):
    keep = []
    for i in range(n):
        keep.append(Starred(v=[i], w=(i, str(i))))
        keep.append(New(i, b=[i]))
    return [repr(k) for k in keep]


class InitReturns:
    def __init__(self, x=None):
        return x


class Abstract:
    def __init__(self, **kw):
        pass


Abstract.__abstractmethods__ = frozenset({'f'})


# CPython's errors: a keyword __init__ or __new__ doesn't take, a missing
# one, a class with neither taking arguments, __init__ returning a value,
# an abstract class
def errors(n):
    out = []
    calls = [
        lambda: Plain(1, d=2),
        lambda: Plain(b=2),
        lambda: Plain(1, 2, 3),
        lambda: Plain(1, a=2),
        lambda: New(b=1),
        lambda: NewInit(1, 2, 3),
        lambda: NewOther(1, z=2),
        lambda: Other(1),
        lambda: Other(x=1),
        lambda: List(extra=1, more=2),
        lambda: InitReturns(x=1),
        lambda: Abstract(k=1),
        lambda: fractions.Fraction(1, 0, _normalize=False) and 1 / 0,
        lambda: fractions.Fraction(1, x=2),
    ]
    for _ in range(n):
        for c in calls:
            try:
                out.append(repr(c()))
            except (TypeError, ZeroDivisionError) as e:
                out.append('%s: %s' % (type(e).__name__, e))
    return out


# fractions' own keyword call of its class, as mdp makes it
def fractions_calls(n):
    F = fractions.Fraction
    x = F(1, 3)
    for i in range(n):
        x = x + F(i, i + 1)
    return repr(x)


try:
    from _testcapi import HeapCTypeVectorcall
except ImportError:
    HeapCTypeVectorcall = None


# a class with a vectorcall of its own, which CPython calls instead of
# tp_new and tp_init (its value is 1 that way, 2 through tp_init)
def class_vectorcall(n):
    if HeapCTypeVectorcall is None:
        return None
    out = []
    for _ in range(n):
        out.append(HeapCTypeVectorcall().value)
        try:
            HeapCTypeVectorcall(1)
        except IndexError as e:
            out.append(str(e))
        try:
            HeapCTypeVectorcall(x=1)
        except IndexError as e:
            out.append(str(e))
    return out


CASES = [
    ('positional', [(3,)], {}),
    ('keywords', [(3,)], {}),
    ('starred', [(3,)], {}),
    ('kept', [(4,)], {}),
    ('errors', [(2,)], {}),
    ('fractions_calls', [(20,)], {}),
    ('class_vectorcall', [(3,)], {}),
]
