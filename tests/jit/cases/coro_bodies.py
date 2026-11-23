# A coroutine's body runs compiled from its start, through the eval hook
# (eval_hook.c:LeGen_Run), until an await suspends it: the frame is then
# paused as ceval's YIELD_FROM leaves it, and ceval runs the rest. Each case
# drives coroutines, by send() or with asyncio, and returns what CPython
# shows of them; compiled() says whether their bodies were compiled, where
# LeJIT compiles them.
import asyncio, gc, inspect, sys, types, warnings


def work(x):
    # a call: a body runs compiled only where it does work
    # (mergepoints.c:yields_do_work)
    return x


def compiled(*fns):
    lejit = sys.modules.get('lejit')
    if lejit is None or dict(lejit._core._build['features']).get(
            'LE_HAS_INTERPRETER_FRAME'):
        return True
    return all(lejit._core._code_info(f.__code__) == 'compiled'
               for f in fns)


class D:
    log = []

    def __init__(self, name):
        self.name = name

    def __del__(self):
        D.log.append(self.name)


def taken():
    out = D.log[:]
    del D.log[:]
    return out


@types.coroutine
def pause(v=None):
    # suspends whatever awaits it once, giving v to the driver
    return (yield v)


class Awaitable:
    # an awaitable that is not a coroutine: __await__ gives an iterator
    def __init__(self, n):
        self.n = n

    def __await__(self):
        for i in range(self.n):
            yield ('step', i)
        return 'done'


def drive(c, sends=()):
    # the values c yields, then how it ended
    out = []
    sends = list(sends)
    try:
        v = c.send(None)
        while True:
            out.append(v)
            v = c.send(sends.pop(0) if sends else None)
    except StopIteration as e:
        out.append(('return', e.value))
    except Exception as e:
        out.append(('exc', type(e).__name__, str(e)))
    return out


def outcome(f):
    try:
        return ('ok', f())
    except StopIteration as e:
        return ('stop', e.value)
    except Exception as e:
        return ('exc', type(e).__name__, str(e))


async def fib(n):
    if n <= 1:
        return work(n)
    return await fib(n - 1) + await fib(n - 2)


async def adder(a):
    x = work(a)
    y = await pause(x)
    z = work(y)
    return x + (y or 0) + (z or 0)


async def chain(n):
    if n == 0:
        return await adder(1)
    return work(n) + await chain(n - 1)


async def many_awaits(n):
    t = 0
    for i in range(n):
        t += await fib(work(i))
    t += await Awaitable(2)  == 'done'
    return t


async def appending(n):
    # paused with the list's append method (and LOAD_METHOD's NULL) on the
    # stack under what it awaits
    out = []
    for i in range(n):
        out.append(await pause(work(i)))
    return out


def basics(n):
    return (drive(fib(n)), drive(adder(n), [5]), drive(chain(n), [7]),
            drive(many_awaits(n)), drive(appending(n), 'abc'),
            compiled(fib, adder, chain, many_awaits, appending))


async def watched(log):
    me = log['coro']
    log['state'] = inspect.getcoroutinestate(me)
    log['running'] = me.cr_running
    log['await'] = me.cr_await
    log['frame'] = me.cr_frame is sys._getframe()
    a = work(1)
    v = await pause(a)
    log['sent'] = v
    return a + v


def states(n):
    log = {}
    c = watched(log)
    log['coro'] = c
    out = [inspect.getcoroutinestate(c), c.cr_frame.f_lasti]
    out.append(c.send(None))
    f = c.cr_frame
    out += [inspect.getcoroutinestate(c), c.cr_running,
            type(c.cr_await).__name__, f.f_lasti, f.f_lineno,
            sorted(f.f_locals), f.f_back]
    out.append(outcome(lambda: c.send(n)))
    out += [inspect.getcoroutinestate(c), c.cr_frame, c.cr_await]
    del log['coro']
    out.append(sorted(log.items()))
    out.append(outcome(lambda: c.send(None)))
    return out, compiled(watched)


async def guarded(k):
    d = D('local')
    try:
        D.log.append('try')
        v = await pause(k)
        D.log.append(('after', v))
        return v
    except ValueError:
        D.log.append('caught')
        return 'caught'
    finally:
        D.log.append('finally')


def thrown(n):
    out = []
    for how in ('send', 'throw', 'throw_other', 'close', 'drop'):
        c = guarded(n)
        out.append(c.send(None))
        if how == 'send':
            out.append(outcome(lambda: c.send(5)))
        elif how == 'throw':
            out.append(outcome(lambda: c.throw(ValueError)))
        elif how == 'throw_other':
            out.append(outcome(lambda: c.throw(KeyError('k'))))
        elif how == 'close':
            out.append(outcome(c.close))
        else:
            del c
            gc.collect()
        out.append(taken())
    return out, compiled(guarded)


async def raiser(k):
    work(k)
    if k == 0:
        raise KeyError('k')
    if k == 1:
        raise StopIteration(5)
    return k


async def catcher(k):
    try:
        return await raiser(k)
    except KeyError as e:
        return 'caught %r' % (e,)


async def deep(n):
    if n == 0:
        work(n)
        raise ValueError('deep')
    return await deep(n - 1)


def raised(n):
    out = []
    for k in (0, 1, 2):
        out.append(drive(catcher(k)))
    try:
        deep(n).send(None)
    except ValueError as e:
        tb = e.__traceback__
        entries = []
        while tb is not None:
            entries.append((tb.tb_frame.f_code.co_name, tb.tb_lineno,
                            tb.tb_lasti))
            tb = tb.tb_next
        out.append(entries)
    try:
        catcher(1).send(None)
    except RuntimeError as e:
        out.append((str(e), type(e.__cause__).__name__,
                    type(e.__context__).__name__))
    return out, compiled(raiser, catcher, deep)


async def await_it(o):
    work(o)
    return await o


async def awaiting_twice():
    inner = adder(1)
    first = await_it(inner)
    first.send(None)
    try:
        return await inner
    finally:
        first.close()


def await_errors(n):
    async def done():
        return n

    finished = done()
    drive(finished)
    out = [drive(await_it(n)), drive(await_it(Awaitable(n))),
           drive(await_it(types.SimpleNamespace()))]

    class GivesCoroutine:
        def __await__(self):
            c = done()
            c.close()
            return c

    class GivesNonIterator:
        def __await__(self):
            return 5

    out += [drive(await_it(GivesCoroutine())),
            drive(await_it(GivesNonIterator())),
            drive(awaiting_twice()), drive(await_it(finished))]
    return out, compiled(await_it, awaiting_twice)


async def sleepy(n, log):
    try:
        for i in range(n):
            await asyncio.sleep(0)
            log.append(work(i))
    except asyncio.CancelledError:
        log.append('cancelled')
        raise
    return n


async def tasks(n):
    log = []
    t = asyncio.ensure_future(sleepy(10, log))
    await asyncio.sleep(0)
    await asyncio.sleep(0)
    t.cancel()
    try:
        await t
    except asyncio.CancelledError:
        log.append('saw cancel')
    fut = LOOP.create_future()
    LOOP.call_soon(fut.set_result, 'fut')
    log.append(await fut)
    r = await asyncio.gather(sleepy(n, log), sleepy(n, log))
    return log, r


LOOP = asyncio.new_event_loop()


def with_asyncio(n):
    return LOOP.run_until_complete(tasks(n)), compiled(sleepy, tasks)


def origins(n):
    out = []
    old = sys.get_coroutine_origin_tracking_depth()
    sys.set_coroutine_origin_tracking_depth(1)
    try:
        c = adder(n)
        out.append([o[2] for o in c.cr_origin])
        out.append(drive(c, [1]))
    finally:
        sys.set_coroutine_origin_tracking_depth(old)
    with warnings.catch_warnings(record=True) as w:
        warnings.simplefilter('always')
        c = adder(n)
        del c
        gc.collect()
    out.append([str(x.message) for x in w])
    return out, compiled(adder)


TRACED = []


def tracer(frame, event, arg):
    if frame.f_code.co_name in ('traced', 'work'):
        TRACED.append((event, frame.f_code.co_name, frame.f_lineno))
    return tracer


async def traced(n):
    x = work(n)
    sys.settrace(tracer)
    y = work(x + 1)
    v = await pause(y)
    sys.settrace(None)
    return x + y + (v or 0)


def tracing(n):
    out = [drive(traced(n), [2]), TRACED[:]]
    del TRACED[:]
    return out, compiled(traced)


async def down(n):
    work(n)
    return await down(n + 1)


def recursion_limit(n):
    old = sys.getrecursionlimit()
    sys.setrecursionlimit(200)
    try:
        r = drive(down(0))
    finally:
        sys.setrecursionlimit(old)
    return [x[:2] for x in r], compiled(down)


async def exc_seen():
    return sys.exc_info()[0]


async def exc_states():
    out = [work(sys.exc_info()[0])]
    try:
        raise KeyError
    except KeyError:
        out.append(sys.exc_info()[0])
    out.append(sys.exc_info()[0])
    out.append(await exc_seen())
    return out


def exc_info(n):
    out = [drive(exc_states())]
    try:
        raise ValueError
    except ValueError:
        out.append(drive(exc_states()))
    return out, compiled(exc_states)


def _deep_lasti():
    try:
        deep(2).send(None)
    except ValueError as e:
        tb, out = e.__traceback__, []
        while tb is not None:
            out.append(tb.tb_lasti)
            tb = tb.tb_next
        return out


def _hook_changes_awaits():
    # whether CPython sends into an awaited coroutine another way while any
    # frame-evaluation hook is installed, LeJIT's or not: the traceback of
    # an exception through the await, and a tracer's events, then differ
    try:
        import _testinternalcapi
        record = _testinternalcapi.set_eval_frame_record
    except (ImportError, AttributeError):
        return False
    plain = _deep_lasti()
    record([])
    try:
        hooked = _deep_lasti()
    finally:
        _testinternalcapi.set_eval_frame_default()
    return plain != hooked


HOOKED = ({'xfail': 'CPython sends into an awaited coroutine another way '
           'while a frame-evaluation hook is installed'}
          if _hook_changes_awaits() else {})


# with_asyncio's leak check is unreliable: under load, LeJIT sometimes
# keeps tens of blocks more than plain CPython (seen on 3.10 and 3.11)
ASYNCIO_LEAKS = {'leaks': 'intermittent extra blocks under LeJIT under load, '
                          'seen on 3.10 and 3.11, KNOWN_BUGS'}


CASES = [
    ('basics', [(3,), (0,)], {}),
    ('states', [(3,)], {}),
    ('thrown', [(3,)], {}),
    ('raised', [(3,)], HOOKED),
    ('await_errors', [(2,)], {}),
    ('with_asyncio', [(3,)],
     dict({'warmup': (40, "asyncio's paths go on compiling for some rounds")},
          **ASYNCIO_LEAKS)),
    ('origins', [(3,)], {}),
    ('tracing', [(3,)], HOOKED),
    ('recursion_limit', [(0,)], {}),
    ('exc_info', [(0,)], {}),
]
