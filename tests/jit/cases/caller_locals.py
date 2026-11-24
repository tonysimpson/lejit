# A compiled frame's locals seen while it is in a call:
# through sys._getframe(n).f_locals and f_back.f_locals from the callee,
# as pyparsing's autoname_elements and a Django test's mock read them,
# and through locals, vars, dir, eval and exec reached by another name.
import builtins
import sys


def peek(n=1):
    return sys._getframe(n).f_locals


def shown(d):
    return sorted((k, v) for k, v in d.items() if not callable(v))


def kinds(a, b):
    x = a + b
    s = 's%d' % a
    f = 1.5 * a
    t = (a, b)
    d = {a: b}
    r = shown(peek())
    return r, x, s, f, t, d


def outer_of_two(n):
    y = n * 3
    z = 'zz'
    return two_down(n), y, z


def two_down(k):
    w = k + 1
    return shown(peek(2)), w


def with_cell(v):
    c = v + 1

    def g():
        return c
    return shown(peek()), g()


def in_loop(n):
    out = {}
    for i in range(n):
        acc = i * 2
        out[i] = shown(peek())
    return out, acc, n


# n is never read in the loop, but CPython's frame keeps it
def dead_in_loop(n):
    out = {}
    for i in range(n):
        out[i] = sorted(peek())
    return out


# the same with values, and a float and a string read once before
def dead_values_in_loop(n):
    f = n * 0.5
    s = 's%d' % n
    t = f + len(s)
    out = {}
    for i in range(n):
        out[i] = shown(peek())
    return out, t


# b is not read after c is made
def after_last_use(a):
    b = a * 2
    c = b + 1
    return shown(peek()), c


# an argument the function never reads
def unused_arg(a, never):
    x = a + 1
    return shown(peek()), x


# a local deleted by del stays absent, and one rebound shows its new value
def deleted(a):
    t = a * 2
    u = t + 1
    del t
    r = sorted(peek())
    u = 'u'
    return r, shown(peek())


def raises_late(a):
    x = a * 3
    y = x + 1
    raise ValueError(y)


# a traceback's frame keeps a local that was not read again before the raise
def traceback_frame(a):
    try:
        raises_late(a)
    except ValueError as e:
        tb = e.__traceback__.tb_next
        return shown(tb.tb_frame.f_locals)


# a short list stays virtual: no object to show
def short_list(a):
    l = [a]
    return sorted(peek()), l


def mocked(path):
    return inspect_back()['path']


def inspect_back():
    return sys._getframe(1).f_back.f_locals


def repercent(path):
    parts = []
    while path:
        try:
            path.decode()
        except UnicodeDecodeError as e:
            parts.append(mocked(path[e.start:e.end]))
            path = path[e.end:]
        else:
            break
    return parts


# through C: the frame calling sorted is in another run of compiled code
def through_c(xs):
    seen = []
    m = len(xs) * 2
    w = 'w%d' % m

    def key(v):
        seen.append(shown(sys._getframe(1).f_locals))
        return -v
    return sorted(xs, key=key), seen[0], m, w


class Prop:
    @property
    def seen(self):
        return sorted(sys._getframe(1).f_locals)


def raiser():
    raise ValueError('x')


# after a call that raised, a property (not a call) reads the frame
def after_caught(p):
    q = 5
    try:
        raiser()
    except ValueError:
        pass
    return p.seen, q


_locals = locals
_vars = vars
_dir = dir
_eval = eval
_exec = exec


def by_other_names(x):
    y = x * 2
    out = [shown(_locals()), sorted(_vars()), sorted(_dir()),
           _eval('x + y'), builtins.eval('y - x')]
    ns = []
    _exec('ns.append(x * y)')
    out.append(ns)
    return out


def with_args(o):
    v = 1
    return sorted(_vars(o)), _eval('v + 1', {'v': 10})


# an operator's Python method and a class's __init__, which compiled code
# calls directly, see their caller's locals and its caller after it
def chain():
    f = sys._getframe(1)
    names = []
    while f is not None and len(names) < 3:
        names.append(f.f_code.co_name)
        f = f.f_back
    return shown(sys._getframe(2).f_locals), names


class Op:
    def __add__(self, other):
        return chain()

    def __radd__(self, other):
        return chain()

    def __neg__(self):
        return chain()


class Init:
    def __init__(self, k):
        self.seen = chain()


def operators(a):
    s = a * 2
    t = 'tt'
    return Op() + s, s + Op(), -Op(), Init(s).seen, t


def calls_operators(a):
    u = a + 1
    return operators(u), u


class Obj:
    def __init__(self):
        self.a = 1
        self.b = 2


class Walker:
    # small enough to be inlined into its caller; its call to step is not
    @classmethod
    def start(klass, n):
        return klass.step(n)

    @classmethod
    def step(klass, n):
        return shown(peek(3))


# while an inlined callee calls on, as traceback.StackSummary.extract
# with capture_locals reads them
def past_inlined(k, v):
    a = k + 1
    b = 'bb'
    return Walker.start(k), a, b


CASES = [
    ('kinds', "[(1, 2), (3, 4)]", {}),
    ('outer_of_two', "[(4,)]", {}),
    ('with_cell', "[(7,)]", {}),
    ('in_loop', "[(3,)]", {}),
    ('dead_in_loop', "[(3,)]", {}),
    ('dead_values_in_loop', "[(3,)]", {}),
    ('after_last_use', "[(4,)]", {}),
    ('unused_arg', "[(4, 'n')]", {}),
    ('deleted', "[(4,)]", {}),
    ('traceback_frame', "[(4,)]", {}),
    ('short_list', "[(3,)]",
     {'xfail': 'a virtual list local is missing from f_locals'}),
    ('through_c', "[([3, 1, 2],)]", {}),
    ('repercent', "[(b'a\\xfcb\\xfc',)]", {}),
    ('after_caught', "[(Prop(),)]", {}),
    ('by_other_names', "[(3,)]", {}),
    ('with_args', "[(Obj(),)]", {}),
    ('calls_operators', "[(3,)]", {}),
    ('past_inlined', "[(3, 4)]", {}),
]
