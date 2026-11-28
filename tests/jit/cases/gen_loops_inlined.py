# A generator with one yield, stepped by a for loop (a generator
# expression's consumer, or a loop over a generator function's call),
# which LeJIT compiles into the loop. What Python sees must be CPython's:
# values, the return value dropped, exceptions and tracebacks raised in
# the body (PEP 479's RuntimeError included), frames asked for in it, and
# the order objects die in when the generator ends, stops early or raises.
import inspect
import sys

LOG = []


class Noted:
    def __init__(self, name):
        self.name = name

    def __del__(self):
        LOG.append(('del', self.name))


def tb_names(e):
    out = []
    tb = e.__traceback__
    while tb is not None:
        out.append(tb.tb_frame.f_code.co_name)
        tb = tb.tb_next
    return out


def run(f):
    try:
        return f()
    except Exception as e:
        return ('raised', type(e).__name__, str(e), tb_names(e)[1:],
                type(e.__cause__).__name__)


def add(xs, b):
    for x in xs:
        yield x + b


def evens(xs):
    for x in xs:
        if x % 2 == 0:
            yield x * 10
    return 'done'


def loops(n):
    xs = list(range(n))
    t = 0
    for y in add(xs, 3):
        t += y
    out = [t, [y for y in evens(xs)]]
    s = 0
    for y in (x * 2 for x in xs):
        s += y
    out.append(s)
    out.append(sum(1 for x in xs if x > 1))
    out.append([list(x * 2 for x in xs), set(x % 3 for x in xs),
                tuple(x + 1 for x in xs), any(x > 1 for x in xs),
                all(x < n for x in xs)])
    # nested: the inner loop's generator, inside the outer's body
    out.append([[y for y in add(range(i), i)] for i in range(n)])
    # a break: the generator is dropped paused
    y = None
    for y in add(xs, 1):
        if y == 2:
            break
    out.append(y)
    return out


def raiser(x):
    if x == 2:
        raise KeyError(x)
    return x


def stops(xs):
    for x in xs:
        if x == 2:
            raise StopIteration
        yield x


def raises(n):
    xs = list(range(n))
    out = [run(lambda: [y for y in add(map(raiser, xs), 0)]),
           run(lambda: list(raiser(x) for x in xs)),
           run(lambda: [y for y in add(xs, None)]),
           run(lambda: any(1 // (x - 1) for x in xs)),
           # PEP 479: StopIteration raised in the body
           run(lambda: [y for y in stops(xs)])]

    def handled():
        try:
            for y in add(xs, None):
                pass
        except TypeError as e:
            return tb_names(e)
    out.append(handled())
    return out


def where(x):
    f = sys._getframe(1)
    return (f.f_code.co_name, f.f_lineno - f.f_code.co_firstlineno,
            f.f_back.f_code.co_name, x)


def framed(xs):
    for x in xs:
        yield where(x)


def frames(n):
    return [[y for y in framed(range(n))], list(where(x) for x in range(n))]


def kept(xs):
    a = Noted('a')
    for x in xs:
        b = Noted('b%d' % x)
        yield x
    LOG.append(('end', a.name))


def finalisers(n):
    out = []
    # to the end
    LOG.clear()
    for y in kept([Noted('i0'), Noted('i1')][:n] and range(n)):
        pass
    out.append(list(LOG))
    # dropped paused at its yield
    LOG.clear()
    for y in kept(range(n)):
        if y == 1:
            break
    out.append(list(LOG))
    LOG.clear()
    out.append(any(w.name == 'w0' for w in [Noted('w0'), Noted('w1')]))
    out.append(list(LOG))
    # ended by an exception
    LOG.clear()
    out.append(run(lambda: [y for y in kept(map(raiser, range(n)))]))
    out.append(list(LOG))
    return out


# the consumer's own frame, traceback and handlers, while the generator is
# paused in its loop
def consumer_side(n):
    out = []
    for y in add(range(n), 1):
        f = sys._getframe()
        out.append((f.f_code.co_name, sorted(f.f_locals)))
        try:
            raiser(y)
        except KeyError as e:
            out.append(('caught', tb_names(e)))
    try:
        for y in add(range(n), 1):
            raiser(y)
    except KeyError as e:
        out.append(('outside', tb_names(e)))
    out.append(run(lambda: [raiser(y) for y in add(range(n), 0)]))
    return out


# a body whose values are of many types, past what its sites promote
KINDS = [type('Kind%d' % i, (int,), {}) for i in range(40)]


def many_kinds(n):
    out = []
    for i, k in enumerate(KINDS):
        t = 0
        for y in add([k(i)] * n, 1):
            t += y
        out.append(t)
    return out


def traced(n):
    seen = []

    def tracer(frame, event, arg):
        if frame.f_code.co_name == 'add':
            seen.append(event)
        return tracer
    out = [y for y in add(range(n), 1)]
    old = sys.gettrace()
    sys.settrace(tracer)
    try:
        out.append([y for y in add(range(n), 2)])
    finally:
        sys.settrace(old)
    return out, seen


# frames of the body asked for while it runs, through a helper, kept
# across its steps: the generator's own frame, as in CPython
FRAMES = []


def grab(x):
    FRAMES.append(sys._getframe(1))
    return x


def grabbing(xs):
    for x in xs:
        yield grab(x) + 1


def place(f):
    return f.f_lineno - f.f_code.co_firstlineno


def kept_across_steps(n):
    FRAMES.clear()
    out = []
    for y in grabbing(range(n)):
        f = FRAMES[-1]
        out.append((y, f is FRAMES[0], place(f), sorted(f.f_locals.items()),
                    f.f_back is None, f.f_code.co_name))
    f = FRAMES[0]
    out.append((len(FRAMES), all(g is f for g in FRAMES), f.f_back is None))
    FRAMES.clear()
    # a generator expression's
    ys = [y for y in (grab(x) * 2 for x in range(n))]
    out.append((ys, all(g is FRAMES[0] for g in FRAMES),
                FRAMES[0].f_code.co_name))
    FRAMES.clear()
    return out


def current(xs):
    for x in xs:
        yield inspect.currentframe(), x


def generator_seen(f):
    """the generator whose frame f is, by f_generator where frames have
    it, else among f's referrers"""
    if hasattr(f, 'f_generator'):
        g = f.f_generator
    else:
        import gc
        gens = [r for r in gc.get_referrers(f) if inspect.isgenerator(r)]
        g = gens[0] if len(gens) == 1 else None
    if g is None:
        return None
    return (g.gi_frame is f, g.gi_running, g.__name__, g.__qualname__,
            inspect.getgeneratorstate(g))


def running_seen(x):
    return generator_seen(sys._getframe(1)), x


def asks_running(xs):
    for x in xs:
        yield running_seen(x)


def via_inspect(n):
    out = []
    first = None
    for f, x in current(range(n)):
        if first is None:
            first = f
        out.append((x, f is first, f.f_code.co_name, place(f),
                     f.f_locals.get('x'), generator_seen(f)))
    out.append([y for y in asks_running(range(n))])
    return out


def holding(xs):
    a = Noted('a')
    for x in xs:
        b = Noted('b%d' % x)
        yield grab(x)


def keeps_alive(n):
    out = []
    LOG.clear()
    FRAMES.clear()
    for y in holding(range(n)):
        out.append((y, sorted(FRAMES[-1].f_locals), list(LOG)))
    out.append(list(LOG))
    del FRAMES[:]
    out.append(list(LOG))
    # dropped paused, its frame held
    LOG.clear()
    for y in holding(range(n)):
        if y == 1:
            break
    out.append(list(LOG))
    f = FRAMES[-1]
    FRAMES.clear()
    out.append((sorted(f.f_locals), f.f_back is None, list(LOG)))
    del f
    out.append(list(LOG))
    return out


def grab_raise(xs):
    for x in xs:
        yield 1 // (grab(x) - 1)


def frame_then_raise(n):
    FRAMES.clear()
    out = []
    try:
        for y in grab_raise(range(n)):
            out.append(y)
    except ZeroDivisionError as e:
        tb = e.__traceback__
        while tb.tb_next is not None:
            tb = tb.tb_next
        out.append((tb_names(e), tb.tb_frame is FRAMES[-1],
                    all(g is FRAMES[0] for g in FRAMES)))
    f = FRAMES[0]
    out.append((f.f_back is None, place(f)))
    FRAMES.clear()
    return out


# tracing started in the loop's body, as pdb.set_trace() starts it
def trace_in_body(n):
    seen = []

    def tracer(frame, event, arg):
        if frame.f_code.co_name in ('add', 'trace_in_body'):
            seen.append((frame.f_code.co_name, event, place(frame)))
        return tracer
    old = sys.gettrace()
    out = []
    try:
        for y in add(range(n), 1):
            if y == 2:
                sys.settrace(tracer)
                sys._getframe().f_trace = tracer
            out.append(y)
    finally:
        sys.settrace(old)
    return out, seen


CASES = [
    ('loops', [(0,), (1,), (4,)], {}),
    ('raises', [(4,)], {}),
    ('frames', [(2,)], {}),
    ('finalisers', [(2,)], {}),
    ('consumer_side', [(4,)], {}),
    ('many_kinds', [(3,)], {}),
    ('traced', [(3,)], {}),
    ('kept_across_steps', [(3,)], {}),
    ('via_inspect', [(3,)], {}),
    ('keeps_alive', [(3,)], {}),
    ('frame_then_raise', [(3,)], {}),
    ('trace_in_body', [(4,)], {}),
]
