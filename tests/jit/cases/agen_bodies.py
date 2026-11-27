# An async generator's body runs compiled through the eval hook, as a
# coroutine's does; each yield wraps its value for asend() to tell it from
# what an await in the body yields. Each
# case drives async generators by hand (asend, athrow, aclose) or with
# asyncio and returns what CPython shows of them; compiled() says whether
# their bodies were compiled.
import asyncio, sys, traceback

from coro_bodies import HOOKED_TRACE, compiled, pause, work

def run(aw, sends=('s1', 's2', 's3')):
    # an awaitable run to its end: what it yields to the driver, then its
    # result or exception
    out = []
    sends = list(sends)
    try:
        v = aw.send(None)
        while True:
            out.append(v)
            v = aw.send(sends.pop(0) if sends else None)
    except StopIteration as e:
        out.append(('value', e.value))
    except BaseException as e:
        out.append(('exc', type(e).__name__, str(e), tb_lines(e),
                    type(e.__cause__).__name__,
                    type(e.__context__).__name__))
    return out


def tb_lines(e):
    return [(f.name, f.lineno) for f in traceback.extract_tb(e.__traceback__)]


async def counting(n, suspend):
    total = 0
    for i in range(n):
        if suspend:
            got = await pause(('await', i))
            total += work(i)
        sent = yield i * 10
        total += 1
        if sent is not None:
            yield ('sent', sent)
    yield ('total', total)


async def returns_early(k):
    yield 1
    if k:
        return
    yield 2


def drain(g, sends=()):
    out = []
    sends = list(sends)
    for _ in range(20):
        r = run(g.asend(sends.pop(0) if sends else None))
        out.append(r)
        if r[-1][0] == 'exc':
            break
    return out


def basics(n):
    out = []
    for suspend in (False, True):
        out.append(drain(counting(n, suspend)))
        out.append(drain(counting(n, suspend), ['x', None, 'y']))
    for k in (0, 1):
        out.append(drain(returns_early(k)))
    g = counting(2, True)
    out.append((g.ag_running, g.ag_frame.f_lineno, g.ag_await))
    out.append(run(g.__anext__()))
    out.append((g.ag_running, g.ag_frame.f_lineno))
    out.append(run(g.aclose()))
    out.append(g.ag_frame)
    return out, compiled(counting, returns_early)


async def raising(k):
    yield 'first'
    await pause('in between')
    if k == 0:
        raise StopIteration('si')
    if k == 1:
        raise StopAsyncIteration('sai')
    if k == 2:
        raise KeyError(k)
    yield 'second'


def errors(n):
    out = []
    for k in (0, 1, 2, 3):
        out.append(drain(raising(k)))
    return out, compiled(raising)


async def guarded(log):
    try:
        while True:
            try:
                v = yield 'ready'
                log.append(('got', v))
            except ValueError as e:
                log.append(('caught', str(e)))
                yield 'recovered'
    finally:
        log.append(('finally', sys.exc_info()[0]))


def thrown(n):
    out = []
    log = []
    g = guarded(log)
    out.append(run(g.asend(None)))
    out.append(run(g.athrow(ValueError('v'))))
    out.append(run(g.asend('a')))
    out.append(run(g.athrow(KeyError('k'))))
    out.append(run(g.asend(None)))
    g = guarded(log)
    out.append(run(g.asend(None)))
    out.append(run(g.aclose()))
    out.append(run(g.aclose()))
    out.append(log)
    return out, compiled(guarded)


HOOKED = []


def firstiter(g):
    HOOKED.append(('first', g.__name__))


def finalizer(g):
    HOOKED.append(('final', g.__name__))
    # what asyncio does: close it
    run(g.aclose())


def hooks(n):
    old = sys.get_asyncgen_hooks()
    sys.set_asyncgen_hooks(firstiter, finalizer)
    try:
        g = guarded([])
        r = run(g.asend(None))
        del g
        g = counting(3, True)
        r = (r, drain(g))
        del g
    finally:
        sys.set_asyncgen_hooks(*old)
    out = HOOKED[:]
    del HOOKED[:]
    return [r, out]


async def ticks(n):
    for i in range(n):
        await asyncio.sleep(0)
        yield i


async def consume(n):
    out = [v async for v in ticks(n)]
    g = ticks(n)
    async for v in g:
        if v == 1:
            break
    await g.aclose()
    return out


def with_asyncio(n):
    return asyncio.run(consume(n)), compiled(ticks)


TRACED = []


def tracer(frame, event, arg):
    if frame.f_code is counting.__code__:
        TRACED.append((event, frame.f_lineno))
    return tracer


def tracing(n):
    # tracing set while the generator is suspended: the rest is traced
    g = counting(2, True)
    out = [run(g.asend(None))]
    sys.settrace(tracer)
    try:
        out.append(drain(g))
    finally:
        sys.settrace(None)
    out.append(TRACED[:])
    del TRACED[:]
    return out


CASES = [
    ('basics', [(3,)], {}),
    ('errors', [(0,)], {}),
    ('thrown', [(0,)], {}),
    ('hooks', [(0,)], {}),
    ('with_asyncio', [(3,)],
     {'warmup': (60, "asyncio's paths go on compiling for some rounds"),
      'leaks_with_aslr': 'what plain CPython itself keeps after the rounds '
                         'varies with addresses, by tens of blocks'}),
    ('tracing', [(0,)], HOOKED_TRACE),
]
