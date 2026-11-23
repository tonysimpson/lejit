# A generator's body runs compiled where it can, resumed at each yield
# through the eval hook (eval_hook.c:LeGen_Run): its locals, stack and
# blocks live in its frame between resumes, as ceval leaves them. Each case
# drives generators and returns what CPython shows of them; compiled()
# says whether a generator's body was compiled, where LeJIT compiles them.
# Only a body whose resumes do work is compiled (light_bodies).
import _thread, gc, inspect, itertools, sys


def work(x):
    # a call: a body runs compiled only where its resumes do work
    # (mergepoints.c:yields_do_work)
    return x


def compiled(*fns):
    lejit = sys.modules.get('lejit')
    if lejit is None:
        return True
    # where ceval pops and clears frames, generator bodies stay interpreted
    features = dict(lejit._core._build['features'])
    if (features.get('LE_HAS_INTERPRETER_FRAME') and
            features.get('LE_HAS_EVAL_FRAME_POPS')):
        return True
    return all(lejit._core._code_info(f.__code__) == 'compiled'
               for f in fns)


class D:
    log = []

    def __init__(self, name):
        self.name = name

    def __del__(self):
        D.log.append(self.name)


def drain(g):
    # list() would fuse some of these generators into itself
    out = []
    while True:
        try:
            out.append(next(g))
        except StopIteration:
            return out


def taken():
    out = D.log[:]
    del D.log[:]
    return out


def one():
    yield work(1)


def two(a):
    yield work(a)
    yield a + 1


def counter(n):
    i = 0
    while i < n:
        x = yield work(i)
        i += 1 if x is None else x


def held(a, b):
    yield a + (yield work(b))


def in_for(xs):
    for x in xs:
        yield work(x) * 2


def resuming(n):
    g = counter(10)
    sent = (next(g), g.send(3), g.send(2), next(g))
    h = held(n, 2)
    stack = (next(h), h.send(5))
    return (drain(one()), drain(two(n)), drain(counter(n)), sent, stack,
            drain(in_for(range(n))), drain(in_for('ab')),
            compiled(one, two, counter, held, in_for))


def late(n):
    yield 1
    b = n + 1
    yield b
    del b
    yield sorted(sys._getframe().f_locals)


def masks(n):
    a = 1
    if n & 1:
        b = 2
    if n & 2:
        c = 3
    if n & 4:
        d = 4
    yield sorted(sys._getframe().f_locals)
    yield n


def local_masks(n):
    out = [list(late(n))]
    # more kinds of NULL locals at one yield than it keeps entries for
    for k in range(8):
        out.append(list(masks(k)))
    return out, compiled(late, masks)


def handlers():
    try:
        D.log.append('try')
        yield 1
        yield 2
    except ValueError:
        D.log.append('caught')
    finally:
        D.log.append('finally')
    yield 3


class CM:
    def __enter__(self):
        D.log.append('enter')
        return self

    def __exit__(self, t, v, tb):
        D.log.append(('exit', t and t.__name__))
        return t is KeyError


def with_block():
    with CM():
        yield 1
        yield 2
    yield 3


def loop_block(n):
    for i in range(n):
        while True:
            yield work(i)
            break
    yield 'end'


def nested_handler():
    # a yield in an except body, after a try/finally inside it: runs
    # interpreted, as the handler's block can't live in compiled code
    try:
        yield 1
        raise KeyError
    except KeyError:
        try:
            D.log.append('inner')
        finally:
            D.log.append('inner finally')
        yield 2
    yield 3


def run(f):
    try:
        return f()
    except Exception as e:
        return 'exc %s: %s' % (type(e).__name__, e)


def blocks(n):
    out = [list(handlers()), taken(), list(with_block()), taken(),
           list(loop_block(n)), drain(nested_handler()), taken()]
    for f in (handlers, with_block, nested_handler):
        g = f()
        next(g)
        out.append((run(lambda: g.throw(ValueError)), run(lambda: next(g)),
                    taken()))
        g = f()
        next(g)
        out.append((run(lambda: g.throw(KeyError)), run(lambda: next(g)),
                    taken()))
        g = f()
        next(g)
        g.close()
        out.append((run(lambda: next(g)), taken()))
        g = f()
        next(g)
        next(g)
        out.append((run(lambda: next(g)), taken()))
    return out, compiled(handlers, with_block, loop_block)


def suspended_locals(a):
    b = D('b')
    yield 1
    c = [a, b]
    yield 2


def from_outside(n):
    g = suspended_locals(n)
    out = [inspect.getgeneratorstate(g)]
    next(g)
    f = g.gi_frame
    out += [inspect.getgeneratorstate(g), sorted(f.f_locals),
            sorted(inspect.getgeneratorlocals(g)), f.f_lineno,
            f.f_lasti > 0, len(gc.get_referents(f)) > 0]
    next(g)
    out += [sorted(inspect.getgeneratorlocals(g)), f.f_lineno, f.f_lasti]
    g2 = suspended_locals(n)
    next(g2)
    g2.gi_frame.clear()
    out += [taken(), inspect.getgeneratorstate(g2), run(lambda: next(g2))]
    del f, g
    gc.collect()
    out.append(taken())
    return out, compiled(suspended_locals)


def own_frame():
    f = sys._getframe()
    yield f
    yield sys._getframe().f_back.f_code.co_name
    seen = sys._getframe().f_locals
    yield sorted(seen)
    yield sorted(sys._getframe().f_locals)


def resumer_one(g):
    return next(g)


def resumer_two(g):
    return next(g)


def from_inside(n):
    g = own_frame()
    f = next(g)
    return (f is g.gi_frame, resumer_one(g), resumer_two(g), next(g),
            compiled(own_frame))


def returns(n):
    yield work(n)
    return n * 10


def raises(n):
    yield work(n)
    1 / 0


def stops(n):
    yield n
    raise StopIteration(n)


def tb_names(e):
    tb, out = e.__traceback__, []
    while tb is not None:
        out.append((tb.tb_frame.f_code.co_name, tb.tb_lineno))
        tb = tb.tb_next
    return out


def dies(n):
    a = D('a')
    b = D('b')
    yield 1
    c = D('c')
    yield 2


def cycle(n):
    me = [D('cycle')]
    yield me
    yield 2


def endings(n):
    out = []
    g = returns(n)
    next(g)
    try:
        next(g)
    except StopIteration as e:
        out.append(('stop', e.value))
    for f in (raises, stops):
        g = f(n)
        next(g)
        try:
            next(g)
        except Exception as e:
            out.append((type(e).__name__, str(e), repr(e.__cause__),
                        tb_names(e)[1:]))
        out.append(run(lambda: next(g)))
    out.append((list(dies(n)), taken()))
    g = dies(n)
    next(g)
    g.close()
    out.append(taken())
    g = dies(n)
    next(g)
    next(g)
    del g
    out.append(taken())
    g = cycle(n)
    me = next(g)
    me.append(g)
    del g, me
    gc.collect()
    out.append(taken())
    return out, compiled(returns, raises, stops, dies, cycle)


def with_cells(n):
    k = n

    def get():
        return k, n

    yield get()
    k += 1
    n = n * 2
    yield get()


def outer(n):
    m = n

    def inner():
        nonlocal m
        yield work(m)
        m += 1
        yield work(m)
    return inner


def genexpr_inside(n):
    for i in range(n):
        yield sum(x * i for x in range(n))


def cells(n):
    inner = outer(n)
    return (drain(with_cells(n)), drain(inner()), drain(inner()),
            drain(genexpr_inside(n)),
            compiled(with_cells, inner, genexpr_inside))


def any_type(xs):
    for x in xs:
        yield x + x


def many_types(n):
    vals = [1, 2.5, 'a', (1,), [2], 3j, b'x', True, 10 ** 30]
    return ([list(any_type(vals[:i])) for i in range(len(vals))],
            list(any_type(vals * n)))


def traced_gen(n):
    yield 1
    sys.settrace(_tracer)
    yield 2
    yield 3


EVENTS = []


def _tracer(frame, event, arg):
    if frame.f_code.co_name == 'traced_gen':
        EVENTS.append((event, frame.f_lineno - frame.f_code.co_firstlineno))
    return _tracer


def _profiler(frame, event, arg):
    if frame.f_code.co_name == 'two':
        EVENTS.append(event)


def tracing(n):
    del EVENTS[:]
    try:
        g = traced_gen(n)
        out = [next(g), next(g), next(g), run(lambda: next(g))]
    finally:
        sys.settrace(None)
    out.append(EVENTS[:])
    del EVENTS[:]
    g = two(n)
    out.append(next(g))
    sys.setprofile(_profiler)
    try:
        out.append(next(g))
    finally:
        sys.setprofile(None)
    out.append(run(lambda: next(g)))
    out.append(EVENTS[:])
    return out


def appending(xs):
    # paused with the list's append method (and LOAD_METHOD's NULL) on the
    # stack
    out = []
    for x in xs:
        out.append((yield work(x)))
    yield out


def resumed_by_ceval(n):
    # ceval resumes the paused frame while a profile function is set, and
    # unwinds it when thrown into: the stack it finds is what it left
    out = []
    g = appending(range(n))
    out.append(next(g))
    sys.setprofile(lambda *a: None)
    try:
        out += [g.send('a'), g.send('b')]
    finally:
        sys.setprofile(None)
    out.append(run(lambda: g.send('c')))
    g = appending(range(n))
    next(g)
    out.append(run(lambda: g.throw(KeyError('k'))))
    return out, compiled(appending)


def deep(g, depth):
    if depth == 0:
        return next(g)
    return deep(g, depth - 1)


def recursion_limit(n):
    # the first depth that fails, fails at the resume: the generator ends
    old = sys.getrecursionlimit()
    g = counter(1000)
    next(g)
    out = []
    try:
        sys.setrecursionlimit(80)
        for depth in range(50, 100):
            try:
                deep(g, depth)
            except RecursionError:
                out.append('RecursionError')
                break
    finally:
        sys.setrecursionlimit(old)
    out.append(run(lambda: next(g)))
    return out


def interrupted(n):
    # interrupt_main trips the signal, and the C loop resumes the generator
    # before any Python code tests for it: the handler runs at the resume
    out = []
    for skip in (0, 1):
        g = counter(5)
        for _ in range(skip):
            next(g)
        try:
            out.append(list(zip(itertools.starmap(_thread.interrupt_main,
                                                  [()] * 3), g)))
        except KeyboardInterrupt as e:
            out.append(('KeyboardInterrupt', [x[0] for x in tb_names(e)]))
        out.append((inspect.getgeneratorstate(g), run(lambda: next(g))))
    return out


def catches_interrupt():
    try:
        while True:
            yield work(1)
    except KeyboardInterrupt:
        seen = 'caught'
    yield seen


def interrupted_in_try(n):
    # the handler runs at the resume, inside the generator's try
    g = catches_interrupt()
    next(g)
    out = list(zip(itertools.starmap(_thread.interrupt_main, [()]), g))
    return out, run(lambda: next(g)), compiled(catches_interrupt)


def interpreted(*fns):
    lejit = sys.modules.get('lejit')
    if lejit is None:
        return True
    return all(lejit._core._code_info(f.__code__) != 'compiled'
               for f in fns)


def ones():
    while True:
        yield 1


def scenario_ids(diff):
    for k, v in diff.items():
        for _ in range(v):
            yield k


def counting(n):
    i = 0
    while i < n:
        yield i
        i += 1


def light_bodies(n):
    # resumes that do next to nothing cost less interpreted: these stay so
    g = ones()
    return ([next(g) for _ in range(n)], drain(scenario_ids({1: n, 2: 2})),
            drain(counting(n)), interpreted(ones, scenario_ids, counting))


def send_at_start(n):
    g = counter(n)
    return run(lambda: g.send(1)), next(g), g.send(None)


CASES = [
    ('resuming', [(3,), (0,)], {}),
    ('local_masks', [(3,)], {}),
    ('blocks', [(3,)], {}),
    ('from_outside', [(3,)], {}),
    ('from_inside', [(3,)], {}),
    ('endings', [(3,)], {}),
    ('cells', [(3,)], {}),
    ('many_types', [(2,)], {}),
    ('tracing', [(3,)], {}),
    ('recursion_limit', [(0,)], {}),
    ('interrupted', [(0,)], {}),
    ('interrupted_in_try', [(0,)], {}),
    ('light_bodies', [(3,)], {}),
    ('send_at_start', [(3,)], {}),
    ('resumed_by_ceval', [(3,)], {}),
]
