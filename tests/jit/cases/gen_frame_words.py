# A resumed generator body reads its frame's words where it uses them, and
# its frame keeps those it doesn't (vcompiler.c:PsycoObject_FromGenFrame):
# where the frame may hold NULL, what dies when, and what Python sees of
# the frame, at a resume through the hook (next, send) and from a compiled
# for loop (eval_hook.c:le_gen_iternext). Each case returns what CPython
# shows; compiled() says whether a body was compiled, where LeJIT compiles
# generator bodies.
import gc, inspect, sys


def work(x):
    # a call, so that a resume does some work
    return x


def compiled(*fns):
    lejit = sys.modules.get('lejit')
    # a release build can't say what compiled
    if lejit is None or not hasattr(lejit._core, '_code_info'):
        return True
    return all(lejit._core._code_info(f.__code__) == 'compiled'
               for f in fns)


LOG = []


class D:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        LOG.append(self.name)


def taken():
    out = LOG[:]
    del LOG[:]
    return out


def run(f):
    try:
        return f()
    except Exception as e:
        return 'exc %s: %s' % (type(e).__name__, e)


def by_next(g):
    out = []
    while True:
        try:
            out.append(next(g))
        except StopIteration as e:
            out.append(('stop', e.value))
            return out
        except Exception as e:
            out.append(('exc', type(e).__name__, str(e)))
            return out


def by_for(g):
    out = []
    try:
        for x in g:
            out.append(x)
    except Exception as e:
        out.append(('exc', type(e).__name__, str(e)))
    return out


def label(x):
    return x.name if isinstance(x, D) else x


def both(f, *args):
    # each way of resuming, twice, so that the second runs code compiled
    # by the first
    out = []
    for drive in (by_next, by_for, by_next, by_for):
        out.append(([label(x) for x in drive(f(*args))], taken()))
        gc.collect()
        out.append(taken())
    return out


# A store over a local the frame still holds drops its old value there,
# before what follows the store, as ceval does.
def stores(n):
    a = D('a1')
    b = D('b1')
    yield work(1)
    LOG.append('before a')
    a = D('a2')
    LOG.append('after a')
    yield work(2)
    for i in range(n):
        LOG.append('before b%d' % i)
        b = D('b%d' % (i + 2))
        LOG.append('after b%d' % i)
        yield work(i)
    yield work(a)
    LOG.append('end')


def deletes(n):
    a = D('a')
    b = D('b')
    yield work(1)
    LOG.append('before del')
    del a
    LOG.append('after del')
    yield work(2)
    try:
        del a
    except UnboundLocalError as e:
        LOG.append(str(e))
    yield work(b)


# On a return or a raise the frame frees its locals first to last, whether
# the run read them or not.
def ends_returning(n):
    a = D('a')
    b = D('b')
    c = D('c')
    yield work(1)
    LOG.append(label(c))
    yield work(2)
    LOG.append(label(b))
    return n


def ends_raising(n):
    a = D('a')
    b = D('b')
    c = D('c')
    yield work(1)
    LOG.append(label(c))
    1 / 0


def ends_stopping(n):
    a = D('a')
    b = D('b')
    yield work(1)
    LOG.append(label(b))
    raise StopIteration(n)


def store_and_end(n):
    fns = (stores, deletes, ends_returning, ends_raising, ends_stopping)
    return [(f.__name__, both(f, n)) for f in fns], compiled(*fns)


# A local bound in one generator's frame and unbound in another's at the
# same yield, and one whose frame holds NULL where the code reads it.
def maybe_bound(flag):
    if flag:
        x = D('x')
    yield work(1)
    try:
        LOG.append(label(x))
    except UnboundLocalError as e:
        LOG.append(str(e))
    yield work(2)
    x = 'rebound'
    yield work(x)


def bound_and_not(n):
    out = []
    for flag in (True, False, False, True, True, False):
        out.append((by_next(maybe_bound(flag)), taken()))
        out.append((by_for(maybe_bound(flag)), taken()))
    return out, compiled(maybe_bound)


# Paused with NULLs and methods on the stack (LOAD_METHOD's, the NULL under
# a function called). (A body that yields in a handler runs interpreted.)
class Box:
    def put(self, *args):
        return ('put',) + args


def pair(*args):
    return args


def stack_items(n):
    box = Box()
    for i in range(n):
        yield pair(i, (yield work(i)), i + 1)
        yield box.put(i, (yield work(-i)))


def stacks(n):
    out = []
    for k in range(2):
        g = stack_items(n)
        out.append([next(g), g.send('a'), next(g), g.send('b'), next(g),
                    g.send('c')])
        out.append(by_for(stack_items(n)))
    return out, compiled(stack_items)


# Cells and free variables are the frame's for the generator's life.
def cells(n):
    k = D('k')
    m = n

    def get():
        return label(k), m

    yield work(get())
    m += 1
    yield work(get())
    k = D('k2')
    yield work(get())
    yield work([label(v) for v in (k, k)])


def cell_runs(n):
    return both(cells, n), compiled(cells)


# What Python sees of the frame while the body runs: its locals, frame
# object and traceback entries, read or not by the run.
def seen_inside(n):
    a = D('a')
    b = n
    yield work(1)
    f = sys._getframe()
    yield work(sorted(f.f_locals))
    yield work(label(f.f_locals['a']))
    yield work((b, sorted(sys._getframe().f_locals)))
    try:
        c = b + 1
        1 / 0
    except ZeroDivisionError as e:
        tb = e.__traceback__
        seen = sorted(tb.tb_frame.f_locals), tb.tb_frame is f
    yield work(seen)
    del f, tb
    yield work(sorted(sys._getframe().f_locals))


def raised_out(n):
    a = D('ra')
    b = n
    yield work(1)
    c = [b]
    raise ValueError(c)


def tb_out(n):
    g = raised_out(n)
    next(g)
    try:
        next(g)
    except ValueError as e:
        tb = e.__traceback__
        while tb.tb_next is not None:
            tb = tb.tb_next
        fr = tb.tb_frame
        out = [fr.f_code.co_name, sorted(fr.f_locals),
               label(fr.f_locals['a']), fr.f_locals['c'],
               inspect.getgeneratorstate(g)]
        del e, tb, fr
    gc.collect()
    return out, taken()


def frames_inside(n):
    out = []
    for k in range(2):
        g = seen_inside(n)
        out.append((by_next(g), taken()))
        out.append((by_for(seen_inside(n)), taken()))
        out.append(tb_out(n))
    return out, compiled(seen_inside, raised_out)


# The paused frame changed by others between compiled resumes: ceval
# resuming it under tracing, f_locals written, throw() and close().
def changing(n):
    a = D('a')
    total = 0
    for i in range(n):
        x = yield work(total)
        total += x if x is not None else i
        LOG.append(('a', label(a)))
        a = D('a%d' % i)
    yield total


def _tracer(frame, event, arg):
    return None


def changed_while_paused(n):
    out = []
    g = changing(n + 3)
    out.append(next(g))
    out.append(g.send(10))
    sys.settrace(_tracer)
    try:
        out.append(g.send(20))
    finally:
        sys.settrace(None)
    out.append(g.send(30))
    g.gi_frame.f_locals['total'] = 'written'
    out.append(g.send(40))
    out.append(taken())
    out.append(run(lambda: g.throw(KeyError('k'))))
    out.append((taken(), run(lambda: next(g))))
    g = changing(n + 3)
    next(g)
    g.send(1)
    g.close()
    out.append((taken(), inspect.getgeneratorstate(g)))
    g = changing(n + 3)
    next(g)
    del g
    gc.collect()
    out.append(taken())
    return out, compiled(changing)


# PEP 479, recursion, a generator running itself, and more words than a
# frame used to be resumed compiled with.
def stops_late(n):
    yield work(n)
    raise StopIteration('inner')


def nested(n):
    if n > 0:
        for x in nested(n - 1):
            yield work(x + 1)
    yield work(0)


def self_resuming():
    g = yield work(0)
    yield work(run(lambda: next(g)))


def many_locals(n):
    v0 = v1 = v2 = v3 = v4 = v5 = v6 = v7 = v8 = v9 = n
    w0 = w1 = w2 = w3 = w4 = w5 = w6 = w7 = w8 = w9 = n + 1
    x0 = x1 = x2 = x3 = x4 = x5 = x6 = x7 = x8 = x9 = n + 2
    y0 = y1 = y2 = y3 = y4 = y5 = y6 = y7 = y8 = y9 = n + 3
    z0 = z1 = z2 = z3 = z4 = z5 = z6 = z7 = z8 = z9 = n + 4
    q0 = q1 = q2 = q3 = q4 = q5 = q6 = q7 = q8 = q9 = n + 5
    r0 = r1 = r2 = r3 = r4 = r5 = r6 = r7 = r8 = r9 = n + 6
    yield work(v0 + z9)
    q5 = 'q5'
    yield work((v1, w2, x3, y4, z5, q5, r9, len(sys._getframe().f_locals)))


def limits(n):
    out = [by_next(stops_late(n)), by_for(stops_late(n)), by_for(nested(n)),
           by_next(nested(n))]
    g = self_resuming()
    next(g)
    out.append((g.send(g), by_next(g)))
    out.append(by_next(many_locals(n)))
    out.append(by_for(many_locals(n)))
    old = sys.getrecursionlimit()
    sys.setrecursionlimit(60)
    try:
        out.append(by_for(nested(80))[-1][:2])
    finally:
        sys.setrecursionlimit(old)
    # many_locals runs compiled where the body reads the frame itself
    return out, compiled(stops_late, nested, self_resuming)


CASES = [
    ('store_and_end', [(3,)], {}),
    ('bound_and_not', [(0,)], {}),
    ('stacks', [(3,)], {}),
    ('cell_runs', [(3,)], {}),
    ('frames_inside', [(3,)], {}),
    ('changed_while_paused', [(2,)], {}),
    ('limits', [(4,)], {}),
]
