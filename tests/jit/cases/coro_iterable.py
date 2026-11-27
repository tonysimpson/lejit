# A types.coroutine generator's body runs compiled as a plain generator's
# does; it can also yield from a coroutine, which a plain generator can't.
# Each case drives such generators, awaited from coroutines or by hand, and
# returns what CPython shows of them; compiled() says whether their bodies
# were compiled.
import asyncio, dis, sys, types

from coro_bodies import compiled, drive, work


def compiled_yield_from(*fns):
    # without SEND a body with yield from runs interpreted
    return 'SEND' not in dis.opmap or compiled(*fns)


@types.coroutine
def steps(n):
    total = 0
    for i in range(n):
        got = yield ('step', i)
        total += work(i)
        if got is not None:
            total += 100
    return total


@types.coroutine
def nothing():
    return 'none'
    yield


async def native(n):
    return await steps(n) + 1


@types.coroutine
def onto_native(n):
    # yield from a coroutine, as only a types.coroutine generator may
    r = yield from native(n)
    r += yield from steps(1)
    return r


@types.coroutine
def onto_iterator(n):
    return (yield from iter(range(n)))


async def awaits(n):
    a = await steps(n)
    b = await nothing()
    c = await onto_native(n)
    d = await onto_iterator(n)
    return a, b, c, d


def basics(n):
    out = [drive(awaits(n)), drive(awaits(n), [None, 'x', None, 'y'])]
    g = steps(n)
    out.append((type(g).__name__, g.gi_running, g.gi_frame.f_lineno,
                g.gi_yieldfrom))
    out.append(g.send(None))
    out.append((g.gi_running, g.gi_frame.f_lineno))
    g.close()
    out.append(g.gi_frame)
    return out, compiled(steps, nothing), compiled_yield_from(onto_native,
                                                               onto_iterator)


@types.coroutine
def guarded(log):
    try:
        yield 'in'
        yield 'again'
    except KeyError as e:
        log.append(('caught', e.args))
        return 'handled'
    finally:
        log.append('finally')


async def await_guarded(log):
    try:
        return await guarded(log)
    except ValueError as e:
        log.append(('outer', e.args))
        return 'outer'


def thrown(n):
    out = []
    for exc in (KeyError(n), ValueError(n), GeneratorExit()):
        log = []
        c = await_guarded(log)
        out.append(c.send(None))
        try:
            out.append(c.throw(exc))
            out.append(c.send(None))
        except BaseException as e:
            out.append(('ended', type(e).__name__, getattr(e, 'value', None)))
        out.append(log)
        c.close()
    log = []
    g = guarded(log)
    g.send(None)
    g.close()
    out.append(log)
    return out, compiled(guarded)


@types.coroutine
def raising(k):
    yield k
    if k == 0:
        raise StopIteration('inner')
    if k == 1:
        raise KeyError('bad')
    return k


async def await_raising(k):
    return await raising(k)


def raised(n):
    out = []
    for k in range(3):
        c = await_raising(k)
        out.append(drive(c))
    return out, compiled(raising)


@types.coroutine
def onto_bad(o):
    return (yield from o)


def bad_operands(n):
    out = []
    for o in (n, None, native(0)):
        g = onto_bad(o)
        try:
            out.append(g.send(None))
        except BaseException as e:
            out.append((type(e).__name__, str(e)))
        g.close()
    return out, compiled_yield_from(onto_bad)


async def sleepy(n, log):
    for i in range(n):
        await asyncio.sleep(0)
        r = await steps_on_loop(i)
        log.append(r)
    return n


@types.coroutine
def steps_on_loop(i):
    # yields to the event loop as asyncio.sleep(0) does
    yield
    return work(i)


LOOP = asyncio.new_event_loop()


def with_asyncio(n):
    log = []
    r = LOOP.run_until_complete(asyncio.gather(sleepy(n, log),
                                               sleepy(n, log)))
    return r, log, compiled(steps_on_loop)


TRACED = []


def tracer(frame, event, arg):
    if frame.f_code.co_name in ('traced', 'work'):
        TRACED.append((event, frame.f_code.co_name, frame.f_lineno))
    return tracer


@types.coroutine
def traced(n):
    x = work(n)
    sys.settrace(tracer)
    v = yield x
    y = work(v)
    sys.settrace(None)
    return x + y


async def await_traced(n):
    return await traced(n)


def tracing(n):
    out = [drive(await_traced(n), [2]), TRACED[:]]
    del TRACED[:]
    # where the first run, traced from inside, leaves no entry compiled
    drive(await_traced(n), [2])
    del TRACED[:]
    return out, compiled(traced)


CASES = [
    ('basics', [(3,), (0,)], {}),
    ('thrown', [(3,)], {}),
    ('raised', [(3,)], {}),
    ('bad_operands', [(3,)], {}),
    ('with_asyncio', [(3,)],
     {'warmup': (300, "asyncio's paths go on compiling for some rounds"),
      'leaks_with_aslr': 'what plain CPython itself keeps after the rounds '
                         'varies with addresses, by tens of blocks'}),
    ('tracing', [(3,)], {}),
]
