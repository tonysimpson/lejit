# METH_FASTCALL builtins without a meta (KNOWN_BUGS F13): with no keywords
# and up to five arguments the C function is called directly with an array
# (pmethodobject.c:PsycoCFunction_CallFast); keywords, star arguments and
# more arguments take the other paths, which must give the same results.
import io
import math


class Obj:
    a = 1

    def __getattr__(self, name):
        if name == 'dyn':
            return 'dynamic'
        raise AttributeError(name)


def getattrs(o):
    return (getattr(o, 'a'), getattr(o, 'b', 'default'), hasattr(o, 'a'),
            hasattr(o, 'zz'))


def getattr_errors(o, name):
    out = []
    for i in range(5):
        try:
            if i == 0:
                out.append(getattr(o, name))
            elif i == 1:
                out.append(getattr(o))
            elif i == 2:
                out.append(getattr())
            elif i == 3:
                out.append(getattr(o, name, 1, 2))
            else:
                out.append(hasattr(o))
        except (TypeError, AttributeError) as e:
            out.append('%s: %s' % (type(e).__name__, e))
    return out


def set_del(o, v):
    setattr(o, 'x', v)
    r = o.x
    delattr(o, 'x')
    return r, hasattr(o, 'x')


def pops(n):
    # 0 and 1 arguments (bound METH_FASTCALL methods), and an IndexError
    xs = list(range(n))
    out = [xs.pop(), xs.pop(0)]
    xs.insert(1, 'i')
    out.append(xs)
    try:
        [].pop()
    except IndexError as e:
        out.append(str(e))
    return out


def pop_insert_loop(n):
    xs = list(range(10))
    t = 0
    for i in range(n):
        x = xs.pop(0)
        xs.insert(len(xs), x)
        t += x
    return t, xs


def gcds(n):
    t = 0
    for i in range(1, n):
        t += math.gcd(i, 12)
    return t


def nexts(xs):
    it = iter(xs)
    return next(it), next(it, 'end'), next(it, 'end')


def next_stop(xs):
    try:
        return next(iter(xs))
    except StopIteration:
        return 'stop'


def args_counts(s):
    # 2 to 6 arguments: compile() takes up to six, the last over FAST_MAX
    return (s.replace('a', 'b'), s.replace('a', 'b', 1),
            compile(s, 'f', 'eval').co_filename,
            compile(s, 'g', 'eval', 0).co_filename,
            compile(s, 'h', 'eval', 0, True).co_filename,
            compile(s, 'i', 'eval', 0, True, 1).co_filename)


def compile_error(s):
    try:
        return compile(s, 'f', 'eval', 0, True, 1)
    except SyntaxError as e:
        return str(e)


def fast_keywords_none(xs, x):
    # METH_FASTCALL | METH_KEYWORDS called without keywords
    return (sorted(xs), math.isclose(x, x + 1e-12), x.is_integer(),
            (5).to_bytes(2, 'big'))


def fast_keywords(xs, x):
    # with keywords: the residual PyCFunction_Call
    f = io.StringIO()
    print(xs, x, sep='|', end='!', file=f)
    return (sorted(xs, reverse=True), math.isclose(x, x + 0.5, abs_tol=1.0),
            (5).to_bytes(2, byteorder='little'), f.getvalue())


def fast_keyword_errors(xs):
    out = []
    for i in range(5):
        try:
            if i == 0:
                out.append(getattr(xs, 'append', default=1))
            elif i == 1:
                out.append(xs.pop(index=0))
            elif i == 2:
                out.append(sorted(xs, bogus=1))
            elif i == 3:
                out.append(math.gcd(1))
            else:
                out.append(math.gcd('a', 2))
        except TypeError as e:
            out.append(str(e))
    return out


def star_args(t):
    # the argument count isn't known: the tuple is passed whole
    return getattr(*t), math.gcd(*t[1:]) if len(t) == 3 else None


CASES = [
    ('getattrs', '[(Obj(),), (1,)]', {}),
    ('getattr_errors', "[(Obj(), 'dyn'), (Obj(), 'missing'), (1, 2), (1, 'real')]", {}),
    ('set_del', '[(Obj(), 5), (Obj(), None)]', {}),
    ('pops', [(5,), (2,)], {}),
    ('pop_insert_loop', [(0,), (25,)], {}),
    ('gcds', [(40,)], {}),
    ('nexts', [([1, 2],), ((1,),), ('ab',)], {}),
    ('next_stop', [([],), ([3],)], {}),
    ('args_counts', [('aa',), ('12',)], {}),
    ('compile_error', [('1 +',), ('(',)], {}),
    ('fast_keywords_none', [([3, 1, 2], 2.0), ([], 2.5)], {}),
    ('fast_keywords', [([3, 1, 2], 2.0), ([], 2.5)], {}),
    ('fast_keyword_errors', [([1, 2],)], {}),
    ('star_args', "[((Obj(), 'a'),), ((Obj(), 'b', 3),), ((1, 'real', 0),)]", {}),
]
