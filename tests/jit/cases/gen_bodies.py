# A generator's body runs compiled where it can, resumed at each yield
# through the eval hook (eval_hook.c:LeGen_Run): its locals, stack and
# blocks live in its frame between resumes, as ceval leaves them. Each case
# drives generators and returns what CPython shows of them; compiled()
# says whether a generator's body was compiled, where LeJIT compiles them.
import _thread, gc, inspect, itertools, subprocess, sys, types


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


def frame_words(xs):
    # paused with immortal objects and NULLs (an unbound local, the NULL
    # under a called function) in its locals and on its stack, which ceval
    # reads by their words once it resumes the frame
    off, nothing = False, None
    out = []
    for x in xs:
        out.append((off, work((yield work(x)))))
        if off:
            out.append('off')
        if nothing is not None:
            out.append('nothing')
        try:
            out.append(late)
        except UnboundLocalError:
            out.append('unbound')
    late = 1
    yield out, late


def words_resumed_by_ceval(n):
    out = []
    g = frame_words(range(n))
    out.append(next(g))
    sys.setprofile(lambda *a: None)
    try:
        out += [g.send(1), g.send(2)]
    finally:
        sys.setprofile(None)
    out.append(run(lambda: g.send(3)))
    out.append(run(lambda: next(g)))
    return out, compiled(frame_words)


def exc_seen():
    # the exception being handled, as the body sees it at each resume
    yield repr(sys.exc_info()[1])
    try:
        raise KeyError('inner')
    except KeyError:
        pass
    yield repr(sys.exc_info()[1])


def exception_state(n):
    # a pause gives the caller back its own exception state, which the body
    # sees again once its own handler has ended
    out = []
    try:
        raise ValueError('outer')
    except ValueError:
        g = exc_seen()
        out += [next(g), repr(sys.exc_info()[1]), next(g),
                repr(sys.exc_info()[1])]
    out += [run(lambda: next(g)), repr(sys.exc_info()[1])]
    g = exc_seen()
    out += [next(g), next(g), run(lambda: next(g))]
    return out, compiled(exc_seen)


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
    # resumes that do next to nothing, compiled as any other body
    g = ones()
    return ([next(g) for _ in range(n)], drain(scenario_ids({1: n, 2: 2})),
            drain(counting(n)), compiled(ones, scenario_ids, counting))


def send_at_start(n):
    g = counter(n)
    return run(lambda: g.send(1)), next(g), g.send(None)


# Compiled code's for loop resumes a generator itself where it can
# (eval_hook.c:le_gen_iternext), without gen_send_ex2 and the hook: what
# CPython shows of each way a resume ends must be the same.
def for_each(g):
    out = []
    for x in g:
        out.append(x)
    return out


def for_first(g, n):
    out = []
    for x in g:
        out.append(x)
        if len(out) == n:
            break
    return out


def for_catching(g):
    out = []
    try:
        for x in g:
            out.append(x)
    except Exception as e:
        out.append('%s: %s %r %r' % (type(e).__name__, e, e.__cause__,
                                     e.__context__))
    return out


def gives_back(n):
    for i in range(n):
        yield work(i)
    return D('returned')


def raises_late(n):
    yield work(n)
    raise KeyError(n)


def stops_inside(n):
    yield work(n)
    raise StopIteration(n)


def dropping(n):
    a = D('a')
    yield work(1)
    b = D('b')
    yield work(2)
    yield a, b


def finally_dropped(n):
    try:
        yield work(1)
        yield work(2)
    finally:
        D.log.append('finally')


def frames_seen(n):
    yield sys._getframe().f_back.f_code.co_name
    yield sys._getframe() is sys._getframe()
    f = sys._getframe()
    yield f.f_lineno - f.f_code.co_firstlineno


def written_while_paused(n):
    a = n
    yield work(a)
    yield work(a)


def running_state():
    g = yield work(0)
    yield work(g.gi_running)
    yield run(lambda: next(g))


def inner_gen(n):
    for i in range(n):
        yield work(i)


def outer_gen(n):
    for x in inner_gen(n):
        yield work(x) * 10


def for_loops(n):
    out = [for_each(counter(n)), for_each(gives_back(n)), taken(),
           for_catching(raises_late(n)), for_catching(stops_inside(n)),
           for_each(outer_gen(n))]
    g = counter(n)
    out.append((next(g), for_first(g, 1), next(g), for_each(g),
                run(lambda: next(g))))
    out.append((for_first(dropping(n), 2), taken()))
    out.append((for_first(finally_dropped(n), 1), taken()))
    g = finally_dropped(n)
    out.append((for_first(g, 1), taken(), inspect.getgeneratorstate(g)))
    del g
    out.append(taken())
    out.append(for_each(frames_seen(n)))
    g = written_while_paused(n)
    out.append(for_first(g, 1))
    locals_seen = g.gi_frame.f_locals
    locals_seen['a'] = 'written'
    out.append((sorted(locals_seen), for_each(g)))
    g = running_state()
    next(g)
    out.append((g.send(g), for_each(g)))
    out.append(compiled(counter, gives_back, raises_late, stops_inside,
                        dropping, finally_dropped, frames_seen,
                        written_while_paused, outer_gen))
    return out


def deep_for(g, depth):
    if depth == 0:
        for x in g:
            return x
    return deep_for(g, depth - 1)


def for_recursion_limit(n):
    old = sys.getrecursionlimit()
    g = counter(1000)
    next(g)
    out = []
    try:
        sys.setrecursionlimit(80)
        for depth in range(50, 100):
            try:
                deep_for(g, depth)
            except RecursionError:
                out.append('RecursionError')
                break
    finally:
        sys.setrecursionlimit(old)
    out.append(run(lambda: next(g)))
    return out


def for_traced(n):
    del EVENTS[:]
    try:
        out = for_each(traced_gen(n))
    finally:
        sys.settrace(None)
    out.append(EVENTS[:])
    del EVENTS[:]
    return out


def for_exception_state(n):
    out = []
    try:
        raise ValueError('outer')
    except ValueError:
        out.append(for_each(exc_seen()))
        out.append(repr(sys.exc_info()[1]))
    out.append(for_each(exc_seen()))
    return out



# A static type CPython readies only when an attribute is first looked up
# on it (3.11's line_iterator), iterated by a body compiled before and
# after that.
def line_starts(code):
    last = None
    for start, end, line in code.co_lines():
        if line is not None and line != last:
            last = line
            yield start, line


def readied_late(n):
    # resumed, but not to its end, before the type is readied, and to its
    # end only after
    out = []
    for f in (counting, ones, light_bodies):
        g = line_starts(f.__code__)
        out.append([next(g) != () for _ in range(2)])
    for f in (counting, ones):
        list(f.__code__.co_lines())
    for f in (light_bodies, counting, readied_late):
        out.append(dict(line_starts(f.__code__)) != {})
    return out, compiled(line_starts)


# a generator expression made over an object that isn't an iterator,
# as its .0: CPython's FOR_ITER raises TypeError where it tests the slot,
# and crashes where it doesn't, so a child tells which
NOT_ITERATOR_PROBE = (
    'import types\n'
    'f = types.FunctionType((x for x in ()).gi_code, {})\n'
    'try:\n    list(f(42))\nexcept TypeError:\n    pass\n')
FOR_ITER_TESTS_SLOT = subprocess.run(
    [sys.executable, '-c', NOT_ITERATOR_PROBE],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def genexpr_over(n):
    return (x * 2 for x in range(n))


def genexpr_not_iterator(n):
    if not FOR_ITER_TESTS_SLOT:
        return None
    f = types.FunctionType(genexpr_over(0).gi_code, {})
    out = []
    for obj in (range(n), [1, 2], 42, iter(range(n))):
        try:
            out.append(list(f(obj)))
        except TypeError as e:
            out.append(str(e))
    return out


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
    ('readied_late', [(0,)], {}),
    ('send_at_start', [(3,)], {}),
    ('resumed_by_ceval', [(3,)], {}),
    ('words_resumed_by_ceval', [(3,)], {}),
    ('exception_state', [(3,)], {}),
    ('for_loops', [(3,), (0,)], {}),
    ('for_recursion_limit', [(0,)], {}),
    ('for_traced', [(3,)], {}),
    ('for_exception_state', [(3,)], {}),
    ('genexpr_not_iterator', [(3,)], {}),
]
