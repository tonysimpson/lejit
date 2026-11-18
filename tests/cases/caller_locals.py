# A compiled frame's locals seen while it is in a call (KNOWN_BUGS C15):
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


# n is dead in the loop, so the compiled frame has dropped it
def dead_in_loop(n):
    out = {}
    for i in range(n):
        out[i] = sorted(peek())
    return out


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


class Obj:
    def __init__(self):
        self.a = 1
        self.b = 2


CASES = [
    ('kinds', "[(1, 2), (3, 4)]", {}),
    ('outer_of_two', "[(4,)]", {}),
    ('with_cell', "[(7,)]", {}),
    ('in_loop', "[(3,)]", {}),
    ('dead_in_loop', "[(3,)]", {'xfail': 'C15: a dead local is dropped'}),
    ('short_list', "[(3,)]",
     {'xfail': 'C15: a virtual list local is missing from f_locals'}),
    ('through_c', "[([3, 1, 2],)]", {}),
    ('repercent', "[(b'a\\xfcb\\xfc',)]", {}),
    ('after_caught', "[(Prop(),)]", {}),
    ('by_other_names', "[(3,)]", {}),
    ('with_args', "[(Obj(),)]", {}),
]
