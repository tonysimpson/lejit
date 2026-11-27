# A generator body with `yield from` runs compiled where the interpreter
# has SEND: the loop sends each value into the generator it delegates to,
# directly where that is a generator whose body is compiled, and pauses
# with that generator under the value it yields, as ceval leaves the frame.
# Each case checks that what Python sees of both generators, their frames,
# their exceptions and their ends is CPython's.
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


def caller_names(f):
    out = []
    while f is not None and len(out) < 5:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def tb_entries(e):
    out = []
    tb = e.__traceback__
    while tb is not None:
        out.append((tb.tb_frame.f_code.co_name, tb.tb_lineno -
                    tb.tb_frame.f_code.co_firstlineno))
        tb = tb.tb_next
    return out


def counter(n):
    i = 0
    while i < n:
        yield work(i)
        i += 1
    return ('counted', n)


def delegates(it):
    r = yield from it
    yield ('returned', r)


def chain(n):
    yield work(-1)
    r = yield from counter(n)
    yield r
    yield from [n, n + 1]
    yield from range(n)


class Iterator:
    # an iterator that isn't a generator, whose end gives a value
    def __init__(self, n):
        self.i = 0
        self.n = n

    def __iter__(self):
        return self

    def __next__(self):
        if self.i == self.n:
            raise StopIteration(('iterator', self.n))
        self.i += 1
        return self.i


def delegation(n):
    out = [list(chain(n)), list(chain(0))]
    out.append(list(delegates(counter(n))))
    out.append(list(delegates(Iterator(n))))
    out.append(list(delegates(iter([n] * n))))
    out.append(list(delegates(x * 2 for x in range(n))))
    out.append(list(delegates(delegates(counter(n)))))
    g = chain(n)
    out.append([next(g) for _ in range(3)])
    out.append(sum(g))
    out.append(sorted(delegates({n: 1, 'k': 2})))
    return out, compiled(chain, delegates)


# send() into the outer generator reaches the one it delegates to
def adder(n):
    total = 0
    while True:
        x = yield total
        if x is None:
            return ('ended', total)
        total += x


class Sender:
    # a send method, which yield from calls for a value other than None
    def __init__(self):
        self.got = []

    def __iter__(self):
        return self

    def __next__(self):
        self.got.append('next')
        return len(self.got)

    def send(self, v):
        self.got.append(v)
        if v == 'stop':
            raise StopIteration('sent stop')
        return len(self.got)


def sends(n):
    out = []
    g = delegates(adder(n))
    out.append(g.send(None))
    for i in range(n):
        out.append(g.send(i))
    out.append(g.send(None))
    out.append(run(lambda: g.send(1)))
    s = Sender()
    g = delegates(s)
    out.append(next(g))
    out.append(g.send(n))
    out.append(next(g))
    out.append(g.send('stop'))
    out.append(s.got)
    g = delegates(iter([1, 2, 3]))
    out.append(next(g))
    out.append(run(lambda: g.send(n)))
    out.append(run(lambda: next(g)))
    return out, compiled(delegates, adder)


# gi_yieldfrom, gi_running and the frames while delegating
def inner_view(holder):
    for i in range(2):
        outer = holder[0]
        f = sys._getframe()
        yield (caller_names(f), outer.gi_running, outer.gi_yieldfrom is not None,
               caller_names(outer.gi_frame)[:1])


def outer_view(holder):
    yield from inner_view(holder)
    yield 'after', caller_names(sys._getframe())


def consumer(g):
    return [x for x in g]


def views(n):
    out = []
    h = [None]
    h[0] = g = outer_view(h)
    out.append(g.gi_yieldfrom)
    out.append(next(g))
    inner = g.gi_yieldfrom
    out.append((inner.gi_code.co_name, inner.gi_running,
                inner.gi_frame.f_lineno - inner.gi_frame.f_code.co_firstlineno,
                g.gi_frame.f_lineno - g.gi_frame.f_code.co_firstlineno))
    out.append(consumer(g))
    out.append((g.gi_yieldfrom, g.gi_frame, inner.gi_frame))
    h[0] = g = outer_view(h)
    out.append(consumer(g))
    return out, compiled(outer_view, inner_view)


# throw() into the outer generator goes down to the one it delegates to
def catcher(n):
    while True:
        try:
            yield work(n)
        except KeyError as e:
            yield ('caught', str(e))
        except ValueError as e:
            return ('returned', str(e))


def outer_catcher(n):
    r = yield from catcher(n)
    x = yield ('outer got', r)
    yield ('then', x)
    r = yield from catcher(n + 1)
    yield ('end', r)


def throws(n):
    out = []
    g = outer_catcher(n)
    out.append(next(g))
    out.append(g.throw(KeyError('k')))
    out.append(next(g))
    out.append(g.throw(ValueError('v')))
    out.append(g.send('sent'))
    out.append(next(g))
    out.append(g.throw(ValueError('w')))
    out.append(run(lambda: next(g)))
    g = outer_catcher(n)
    next(g)
    try:
        g.throw(TypeError('t'))
    except TypeError as e:
        out.append(tb_entries(e))
    out.append((g.gi_frame, run(lambda: next(g))))
    g = delegates(iter([1, 2]))
    next(g)
    out.append(run(lambda: g.throw(KeyError('no throw method'))))
    s = Sender()
    g = delegates(s)
    next(g)
    out.append(run(lambda: g.throw(KeyError('k'))))
    return out, compiled(outer_catcher)


# close() goes down too, and finally blocks run in CPython's order
LOG = []


def finishes(n):
    try:
        yield work(n)
        yield work(n + 1)
    finally:
        LOG.append(('inner finally', n))


def ignores_exit(n):
    # once: a second close, as it is freed, ends it
    try:
        yield work(n)
    except GeneratorExit:
        LOG.append('ignored')
    yield work(n)


def outer_finishes(inner):
    try:
        yield from inner
    finally:
        LOG.append('outer finally')


def plain_outer(inner):
    yield from inner


def closes(n):
    del LOG[:]
    out = []
    g = outer_finishes(finishes(n))
    out.append(next(g))
    g.close()
    out.append((LOG[:], g.gi_frame))
    del LOG[:]
    g = plain_outer(finishes(n))
    out.append(next(g))
    del g
    out.append(LOG[:])
    del LOG[:]
    g = plain_outer(ignores_exit(n))
    next(g)
    out.append(run(g.close))
    out.append(LOG[:])
    del LOG[:]
    out.append(list(plain_outer(finishes(n))))
    out.append(LOG[:])
    return out, compiled(plain_outer, outer_finishes, finishes)


# what goes wrong: PEP 479, exceptions out of the inner one, bad operands
def stops(n):
    yield work(n)
    raise StopIteration('inner')


def raises(n):
    yield work(n)
    work(1) // 0


def outer_stops(n):
    yield from range(n)
    raise StopIteration('outer')


async def a_coroutine():
    pass


def errors(n):
    out = []
    try:
        list(plain_outer(stops(n)))
    except RuntimeError as e:
        out.append((str(e), repr(e.__cause__), tb_entries(e)))
    try:
        list(outer_stops(n))
    except RuntimeError as e:
        out.append((str(e), repr(e.__cause__)))
    try:
        list(plain_outer(raises(n)))
    except ZeroDivisionError as e:
        out.append(tb_entries(e))
    out.append(run(lambda: list(plain_outer(n))))
    k = a_coroutine()
    out.append(run(lambda: list(plain_outer(k))))
    k.close()
    out.append(run(lambda: list(plain_outer(None))))
    return out, compiled(plain_outer, outer_stops)


# the generator delegating to one that is running, or to itself
def to_holder(holder):
    yield 1
    yield from holder[0]


def running(n):
    out = []
    h = [None]
    h[0] = g = to_holder(h)
    out.append(next(g))
    out.append(run(lambda: next(g)))
    out.append(run(lambda: next(g)))
    h[0] = counter(n)
    out.append(list(to_holder(h)))
    return out, compiled(to_holder)


# recursive delegation, as a tree's in-order walk
class Tree:
    def __init__(self, left, value, right):
        self.left = left
        self.value = value
        self.right = right

    def __iter__(self):
        if self.left:
            yield from self.left
        yield self.value
        if self.right:
            yield from self.right


def tree(lo, hi):
    if lo >= hi:
        return None
    mid = (lo + hi) // 2
    return Tree(tree(lo, mid), mid, tree(mid + 1, hi))


def trees(n):
    t = tree(0, n)
    total = 0
    for x in t:
        total += x
    return total, list(t)[:5], compiled(Tree.__iter__)


# tracing switched on inside the generator delegated to
EVENTS = []


def _tracer(frame, event, arg):
    if frame.f_code.co_name in ('traced_inner', 'traced_outer'):
        EVENTS.append((frame.f_code.co_name, event,
                       frame.f_lineno - frame.f_code.co_firstlineno))
    return _tracer


def traced_inner(n):
    yield work(1)
    sys.settrace(_tracer)
    yield work(2)


def traced_outer(n):
    yield from traced_inner(n)
    yield work(3)


def tracing(n):
    del EVENTS[:]
    try:
        out = list(traced_outer(n))
    finally:
        sys.settrace(None)
    out.append(EVENTS[:])
    del EVENTS[:]
    return out


# the exception being handled, seen from the generator delegated to
def exc_seen():
    yield repr(sys.exc_info()[1])
    yield repr(sys.exc_info()[1])


def exception_state(n):
    out = []
    try:
        raise ValueError('outer')
    except ValueError:
        for x in plain_outer(exc_seen()):
            out.append((x, repr(sys.exc_info()[1])))
    for x in plain_outer(exc_seen()):
        out.append((x, repr(sys.exc_info()[1])))
    return out


CASES = [
    ('delegation', [(3,), (0,)], {}),
    ('sends', [(3,)], {}),
    ('views', [(1,)], {}),
    ('throws', [(2,)], {}),
    ('closes', [(2,)], {}),
    ('errors', [(2,)], {}),
    ('running', [(2,)], {}),
    ('trees', [(20,), (1,)], {}),
    ('tracing', [(1,)], {}),
    ('exception_state', [(1,)], {}),
]
