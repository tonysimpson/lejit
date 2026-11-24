# A for loop or next() in compiled code steps a generator whose body is
# compiled by calling that body directly (psyfunc.c:psyco_gen_step), doing
# what gen_send_ex2 does around the run itself. The body runs on the
# generator's own frame without it being the thread's innermost: each case
# checks that what Python sees of the frame, its caller, the generator and
# the exception state is CPython's, and that anything else (tracing, a
# throw, a running or finished generator, ceval in between) goes on as in
# CPython.
import sys


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
        return type(e).__name__, str(e)


def counter(n):
    i = 0
    while i < n:
        yield work(i)
        i += 1


def for_sum(g):
    t = 0
    for x in g:
        t += x
    return t


def next_all(g):
    out = []
    while True:
        try:
            out.append(next(g))
        except StopIteration as e:
            out.append(('stop', e.value))
            return out


def returns(n):
    yield work(n)
    return n * 2


def plain(n):
    return [for_sum(counter(n)), for_sum(counter(0)), next_all(returns(n)),
            next_all(counter(n)), compiled(counter, returns)]


# the frame, seen from inside the body
def caller_names(f):
    out = []
    while f is not None and len(out) < 4:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def seen(holder):
    i = 0
    while i < 2:
        g = holder[0]
        f = sys._getframe()
        yield (caller_names(f), f is g.gi_frame, g.gi_running,
               f.f_lineno - f.f_code.co_firstlineno, f.f_lasti >= 0,
               caller_names(g.gi_frame),
               g.gi_frame.f_lineno - f.f_code.co_firstlineno,
               sorted(f.f_locals), f.f_globals is globals())
        i += 1


def seen_by(holder):
    out = []
    for x in holder[0]:
        out.append(x)
    return out


def seen_deeper(holder):
    return seen_by(holder)


def frames(n):
    h = [None]
    h[0] = seen(h)
    out = [seen_by(h)]
    h[0] = seen(h)
    out.append(seen_deeper(h))
    h[0] = seen(h)
    out.append(next(h[0]))
    return out, compiled(seen)


# a generator stepped by another generator's body, itself stepped directly
def inner(n):
    for i in range(n):
        yield caller_names(sys._getframe())


def outer(n):
    for x in inner(n):
        yield x, caller_names(sys._getframe())


def nested(n):
    out = []
    for x in outer(n):
        out.append(x)
    return out, compiled(inner, outer)


# a local Python writes while the body runs, through the frame
def writes_local(n):
    a = work(n)
    yield a
    sys._getframe().f_locals['a'] = 'written'
    yield a
    b = [n]
    yield b


def locals_written(n):
    return [x for x in writes_local(n)] + [list(writes_local(n))]


# an exception out of the body, and caught inside it
def raises_at(n):
    i = 0
    while True:
        if i == n:
            work(1) // 0
        yield i
        i += 1


def catches(n):
    for i in range(n):
        try:
            work(1) // 0
        except ZeroDivisionError as e:
            tb = e.__traceback__
            yield (tb.tb_frame is sys._getframe(), tb.tb_lineno -
                   sys._getframe().f_code.co_firstlineno)


def tb_entries(e):
    out = []
    tb = e.__traceback__
    while tb is not None:
        out.append((tb.tb_frame.f_code.co_name, tb.tb_lineno -
                    tb.tb_frame.f_code.co_firstlineno))
        tb = tb.tb_next
    return out


def tracebacks(n):
    out = []
    try:
        for x in raises_at(n):
            out.append(x)
    except ZeroDivisionError as e:
        out.append(tb_entries(e))
    g = raises_at(n)
    out.append(run(lambda: [next(g) for _ in range(n + 1)]))
    out.append(run(lambda: next(g)))
    out.append(list(catches(n)))
    return out, compiled(raises_at)


# the generator stepped while it runs
def steps_itself(holder):
    yield work(1)
    yield run(lambda: next(holder[0]))
    yield run(lambda: holder[0].send(None))
    yield run(lambda: holder[0].close())
    yield holder[0].gi_running


def running(n):
    h = [None]
    h[0] = steps_itself(h)
    out = [x for x in h[0]]
    out.append(h[0].gi_running)
    return out, compiled(steps_itself)


# send, throw and close between steps, from compiled code and from C
def takes(n):
    total = 0
    while True:
        try:
            x = yield total
        except KeyError:
            x = 100
        if x is None:
            x = 1
        total += x


def between(n):
    out = []
    g = takes(n)
    out.append(next(g))
    for i in range(n):
        out.append(next(g))
        out.append(g.send(i * 10))
        out.append(g.throw(KeyError))
    out.append(next(g))
    g.close()
    out.append(run(lambda: next(g)))
    g = counter(n)
    out.append(next(g))
    out.append(list(map(work, [next(g)])))
    out.append(for_sum(g))
    out.append(run(lambda: next(g)))
    out.append(for_sum(g))
    return out, compiled(takes)


# a generator ceval runs between direct steps, and one it finishes
def ceval_steps(n):
    out = []
    g = counter(n + 2)
    out.append(next(g))
    out.append(next(iter([g])).__next__())
    out.append(sum(g.__next__() for _ in range(1)))
    out.append(for_sum(g))
    g = counter(n)
    out.append(list(g))
    out.append(run(lambda: next(g)))
    return out


# tracing and profiling switched on while the body runs
EVENTS = []


def _tracer(frame, event, arg):
    if frame.f_code.co_name == 'traced_body':
        EVENTS.append((event, frame.f_lineno - frame.f_code.co_firstlineno))
    return _tracer


def _profiler(frame, event, arg):
    if frame.f_code.co_name == 'profiled_body':
        EVENTS.append(event)


def traced_body(n):
    yield work(1)
    sys.settrace(_tracer)
    yield work(2)
    yield work(3)


def profiled_body(n):
    yield work(1)
    sys.setprofile(_profiler)
    yield work(2)
    yield work(3)


def tracing(n):
    del EVENTS[:]
    try:
        out = [x for x in traced_body(n)]
    finally:
        sys.settrace(None)
    out.append(EVENTS[:])
    del EVENTS[:]
    try:
        out += [x for x in profiled_body(n)]
    finally:
        sys.setprofile(None)
    out.append(EVENTS[:])
    del EVENTS[:]
    g = counter(n)
    out.append(next(g))
    sys.settrace(_tracer)
    try:
        out.append(next(g))
    finally:
        sys.settrace(None)
    out.append(for_sum(g))
    return out


# the exception being handled, as the body sees it
def exc_seen():
    yield repr(sys.exc_info()[1])
    yield repr(sys.exc_info()[1])


def exc_handled():
    try:
        raise KeyError('inner')
    except KeyError:
        yield repr(sys.exc_info()[1])
    yield repr(sys.exc_info()[1])


def exception_state(n):
    out = []
    try:
        raise ValueError('outer')
    except ValueError:
        for x in exc_seen():
            out.append((x, repr(sys.exc_info()[1])))
    for x in exc_seen():
        out.append((x, repr(sys.exc_info()[1])))
    try:
        raise ValueError('outer')
    except ValueError:
        for x in exc_handled():
            out.append((x, repr(sys.exc_info()[1])))
    return out, compiled(exc_seen)


# recursion: a step that would overflow finishes the generator
def deep_for(g, depth):
    if depth == 0:
        for x in g:
            return x
    return deep_for(g, depth - 1)


def recursion_limit(n):
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


CASES = [
    ('plain', [(3,), (0,)], {}),
    ('frames', [(2,)], {}),
    ('nested', [(3,)], {}),
    ('locals_written', [(3,)], {}),
    ('tracebacks', [(2,)], {}),
    ('running', [(0,)], {}),
    ('between', [(3,)], {}),
    ('ceval_steps', [(3,)], {}),
    ('tracing', [(3,)], {}),
    ('exception_state', [(0,)], {}),
    ('recursion_limit', [(0,)], {}),
]
