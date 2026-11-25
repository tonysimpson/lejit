# A generator only the compiled code that made it steps (a for loop over a
# generator function's call, next() or send() of one held on the stack) is
# a virtual one: no generator object exists, its paused state is a record
# (Python/genrecord.c). Each case checks that what Python sees is CPython's:
# every way a step ends, the order objects die in when it is dropped
# paused or ends, frames and tracebacks from inside its body, and every way
# it is made real (escapes) followed by each use of the real one.
import gc, sys

LOG = []


class Noted:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        LOG.append(('del', self.name))


def work(x):
    return x


def compiled(*fns):
    lejit = sys.modules.get('lejit')
    # a release build can't say what compiled
    if lejit is None or not hasattr(lejit._core, '_code_info'):
        return True
    return all(lejit._core._code_info(f.__code__) == 'compiled'
               for f in fns)


def run(f):
    try:
        return f()
    except Exception as e:
        return ('raised', type(e).__name__, str(e), repr(e.__cause__),
                repr(e.__context__))


def tb_names(e):
    out = []
    tb = e.__traceback__
    while tb is not None:
        out.append((tb.tb_frame.f_code.co_name,
                    tb.tb_lineno - tb.tb_frame.f_code.co_firstlineno))
        tb = tb.tb_next
    return out


def two(n):
    i = 0
    while i < n:
        yield work(i)
        yield -i
        i += 1
    return 'done'


def with_args(a, b, c=3, *, d=4):
    yield work(a + b + c + d)
    yield c


def closure_gen(n):
    k = Noted('cell')

    def inner():
        return k.name
    for i in range(n):
        yield work(inner())
        yield i


# each consumer of a virtual generator
def loops(n):
    out = []
    for x in two(n):
        out.append(x)
    for x in with_args(1, 2):
        out.append(x)
    for x in with_args(1, 2, 5):
        out.append(x)
    LOG.clear()
    for x in closure_gen(n):
        out.append(x)
    out.append(list(LOG))
    return out, compiled(loops, two)


def nexts(n):
    out = [next(two(n + 1))]
    try:
        next(two(0))
    except StopIteration as e:
        out.append(('stop', e.value))
    return out


def replies(n):
    x = yield work(n)
    while x is not None:
        x = yield x * 2
    return 'end'


def sends(n):
    out = []
    out.append(run(lambda: replies(n).send(1)))
    out.append(replies(n).send(None))
    return out


def inner_gen(n):
    for i in range(n):
        yield work(i)
    return n


def delegating(n):
    r = yield from inner_gen(n)
    yield r
    r = yield from two(n)
    yield r


def yield_from(n):
    out = []
    for x in delegating(n):
        out.append(x)
    return out, compiled(delegating, inner_gen)


# how a run ends: StopIteration's value, an exception, PEP 479
def raiser(n):
    yield work(1)
    raise ValueError(n)


def stopper(n):
    yield work(n)
    raise StopIteration('inside')


def ends(n):
    out = []
    try:
        for x in raiser(n):
            out.append(x)
    except ValueError as e:
        out.append(('caught', str(e), tb_names(e)))
    try:
        for x in stopper(n):
            out.append(x)
    except RuntimeError as e:
        out.append((str(e), repr(e.__cause__), repr(e.__context__),
                    tb_names(e)))
    return out


# dropped paused, and ended: what dies, in CPython's order, and finally
def finally_gen(n):
    a = Noted('a')
    try:
        for i in range(n):
            yield work(i)
    finally:
        LOG.append('finally')
    b = Noted('b')
    yield b.name


def locals_gen(n):
    a = Noted('x1')
    b = Noted('x2')
    for i in range(n):
        yield work(i)
    c = Noted('x3')


def drops(n):
    out = []
    LOG.clear()
    for x in finally_gen(n):
        if x == 1:
            break
    out.append(list(LOG))
    LOG.clear()
    for x in locals_gen(n):
        if x == 1:
            break
    out.append(list(LOG))
    LOG.clear()
    for x in locals_gen(n):
        pass
    out.append(list(LOG))
    LOG.clear()
    for x in finally_gen(n):
        pass
    out.append(list(LOG))
    LOG.clear()
    try:
        for x in locals_gen(n):
            work(1) // 0
    except ZeroDivisionError:
        out.append(list(LOG))
    LOG.clear()
    next(locals_gen(n))
    out.append(list(LOG))
    LOG.clear()
    return out


# frames and tracebacks from inside the body
def caller_names(f):
    out = []
    while f is not None and len(out) < 4:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def frame_gen(n):
    f = sys._getframe()
    for i in range(n):
        g = sys._getframe()
        yield work((f is g, caller_names(g), sorted(g.f_locals),
                    g.f_lineno - g.f_code.co_firstlineno))


def frame_loop(n):
    out = []
    for x in frame_gen(n):
        out.append(x)
    return out


def catching_gen(n):
    for i in range(n):
        try:
            work(1) // 0
        except ZeroDivisionError as e:
            r = (e.__traceback__.tb_frame is sys._getframe(), tb_names(e))
        yield r


def frames(n):
    out = [frame_loop(n)]
    for x in catching_gen(n):
        out.append(x)
    return out, compiled(frame_gen, catching_gen)


KEPT = []


def keeps_frame(n):
    q = Noted('q')
    KEPT.append(sys._getframe())
    for i in range(n):
        yield work(i)


def kept_frames(n):
    out = []
    LOG.clear()
    for x in keeps_frame(n):
        f = KEPT[-1]
        out.append((x, f.f_lineno - f.f_code.co_firstlineno, sorted(f.f_locals),
                    f.f_back is None))
    f = KEPT.pop()
    out.append((sorted(f.f_locals), f.f_back is sys._getframe(), list(LOG)))
    del f
    out.append(list(LOG))
    LOG.clear()
    for x in keeps_frame(n):
        if x == 1:
            break
    f = KEPT.pop()
    out.append((sorted(f.f_locals), list(LOG)))
    del f
    out.append(list(LOG))
    return out


def clears_frame(n):
    yield work(1)
    KEPT.append(sys._getframe())
    yield 2
    yield 3


def frame_clear(n):
    out = []
    for x in clears_frame(n):
        out.append(x)
        if x == 2:
            out.append(run(lambda: KEPT.pop().clear()))
    return out


# each escape, then each use of the real generator
HELD = []


def escapes_held(n):
    out = []
    for x in two(n):
        out.append(x)
    g = two(n)
    out.append(next(g))
    HELD.append(g)
    out.append(next(HELD[-1]))
    out.append(type(HELD.pop()).__name__)
    return out


def stored(n):
    out = []
    it = iter(two(n))
    for x in it:
        out.append(x)
        if x == 1:
            HELD.append(it)
            break
    g = HELD.pop()
    out.append((g.gi_running, g.gi_frame.f_lineno - two.__code__.co_firstlineno,
                sorted(g.gi_frame.f_locals), g.gi_code is two.__code__))
    out.append(list(g))
    out.append(run(lambda: next(g)))
    return out


def tracing_inside(n):
    yield work(1)
    sys.settrace(lambda *a: None)
    yield 2
    sys.settrace(None)
    yield sys._getframe().f_back.f_code.co_name


def traces(n):
    out = []
    for x in tracing_inside(n):
        out.append(x)
    sys.settrace(None)
    for x in two(n):
        out.append(x)
        if x == 1:
            sys.settrace(lambda *a: None)
    sys.settrace(None)
    return out


def recurse_gen(n):
    if n > 0:
        for x in recurse_gen(n - 1):
            yield x
    yield work(n)


def recursion(n):
    return [x for x in [0]] + list_of(recurse_gen(n))


def list_of(g):
    out = []
    for x in g:
        out.append(x)
    return out


def deep(n):
    out = []
    for x in recurse_gen(n):
        out.append(x)
    return out


CASES = [
    ('loops', [(3,), (0,)], {}),
    ('nexts', [(2,)], {}),
    ('sends', [(2,)], {}),
    ('yield_from', [(3,)], {}),
    ('ends', [(2,)], {}),
    ('drops', [(3,)], {}),
    ('frames', [(2,)], {}),
    ('kept_frames', [(3,)], {}),
    ('frame_clear', [(0,)], {}),
    ('escapes_held', [(3,)], {}),
    ('stored', [(3,)], {}),
    ('traces', [(3,)], {}),
    ('recursion', [(4,)], {}),
    ('deep', [(30,)], {}),
]
