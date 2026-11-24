# A coroutine awaited as soon as it is made (await f(x)) has its body run
# as a call from the await, with no coroutine object (psyfunc.c:
# psyco_coro_start). The coroutine is made when something could see it
# (frames.c:psyco_coros_realise): the body suspends, gc.get_objects or
# gc.get_referrers looks, the function is renamed, tracing starts in it.
# Each case returns what CPython shows of the coroutines, at one, two and
# three levels of awaits.
import gc, inspect, sys, threading, types, weakref
from coro_bodies import HOOKED, HOOKED_TRACE


def work(x):
    return x


@types.coroutine
def pause(v=None):
    return (yield v)


def started(*fns):
    # whether the bodies of fns ran only as calls from their awaits, never
    # through the eval hook, where LeJIT starts them so
    lejit = sys.modules.get('lejit')
    if lejit is None or not lejit._started or not FRAME_OBJECTS:
        return True
    # a release build can't say what compiled
    if not hasattr(lejit._core, '_code_info'):
        return True
    return all(lejit._core._code_info(f.__code__) == 'no entry point'
               for f in fns)


def _frame_objects():
    # whether a frame is a frame object, where LeJIT starts coroutines so:
    # elsewhere 'objects' below would test only CPython, which crashes in
    # it (closing the awaited coroutine on 3.11, a later collection on
    # 3.12)
    try:
        import lejit._core
    except ImportError:
        return True
    return not dict(lejit._core._build['features']).get(
        'LE_HAS_INTERPRETER_FRAME')


FRAME_OBJECTS = _frame_objects()


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


def running(name):
    # the coroutines of 'name' gc finds that have not finished
    return [o for o in gc.get_objects()
            if inspect.iscoroutine(o) and o.__qualname__ == name and
            inspect.getcoroutinestate(o) != 'CORO_CLOSED']


async def fib(n):
    if n <= 1:
        return work(n)
    return await fib(n - 1) + await fib(n - 2)


async def raiser(k):
    work(k)
    if k == 0:
        raise KeyError(k)
    if k == 1:
        raise StopIteration(5)
    return k


async def catcher(k):
    try:
        return await raiser(k)
    except KeyError:
        return 'caught'


async def deep(n):
    if n == 0:
        work(n)
        raise ValueError('deep')
    return await deep(n - 1)


def raised(n):
    out = [drive(fib(n)), [drive(catcher(k)) for k in (0, 1, 2)]]
    for c in (deep(n), catcher(1)):
        try:
            c.send(None)
        except BaseException as e:
            tb, entries = e.__traceback__, []
            while tb is not None:
                entries.append((tb.tb_frame.f_code.co_name, tb.tb_lineno,
                                tb.tb_lasti))
                tb = tb.tb_next
            out.append((type(e).__name__, str(e), entries,
                        type(e.__cause__).__name__))
    return out, started(raiser)


LOG = []
HELD = []


async def lvl3(k, how):
    me = sys._getframe()
    if how == 'objects':
        cs = sorted(running('lvl1') + running('lvl2') + running('lvl3'),
                    key=lambda c: c.__qualname__)
        again = sorted(running('lvl1') + running('lvl2') + running('lvl3'),
                       key=lambda c: c.__qualname__)
        LOG.append([(c.__qualname__, inspect.getcoroutinestate(c),
                     c.cr_running, c.cr_await) for c in cs])
        LOG.append(all(a is b for a, b in zip(cs, again)))
        inner = cs[-1]
        LOG.append(inner.cr_frame is me)
        LOG.append(sorted(inspect.getcoroutinelocals(inner)))
        for call in (lambda: inner.send(None), lambda: inner.throw(KeyError),
                     inner.close, inner.cr_frame.clear):
            try:
                call()
            except Exception as e:
                LOG.append((type(e).__name__, str(e)))
        LOG.append(weakref.ref(inner)() is inner)
        HELD.append(inner)
    elif how == 'referrers':
        rs = [r for r in gc.get_referrers(me) if inspect.iscoroutine(r)]
        LOG.append([(r.__qualname__, r.cr_frame is me) for r in rs])
        HELD.extend(rs)
    elif how == 'rename':
        lvl3.__name__, lvl3.__qualname__ = 'renamed', 'renamed_q'
        cs = running('lvl3') + running('renamed_q')
        LOG.append([(c.__name__, c.__qualname__) for c in cs])
        lvl3.__name__ = lvl3.__qualname__ = 'lvl3'
    elif how == 'suspend':
        HELD.extend(running('lvl2') + running('lvl3'))
        LOG.append(('resumed', await pause(work(k))))
        LOG.append(inspect.getcoroutinestate(HELD[-1]))
    elif how == 'raise':
        HELD.extend(running('lvl3'))
        raise KeyError('x')
    return work(k)


async def lvl2(k, how):
    return await lvl3(k, how) + 1


async def lvl1(k, how):
    return await lvl2(k, how) + 1


def realised(n):
    # each way to find the running inner coroutines, then the same objects
    # once their bodies have returned, raised or suspended
    out = []
    hows = ('referrers', 'rename', 'suspend', 'raise', 'none')
    for how in ('objects',) * FRAME_OBJECTS + hows:
        out.append((how, drive(lvl1(n, how), [7]), LOG[:]))
        del LOG[:]
        for c in HELD:
            out.append((c.__qualname__, inspect.getcoroutinestate(c),
                        c.cr_frame, c.cr_running, c.cr_await))
            try:
                c.send(None)
            except Exception as e:
                out.append((type(e).__name__, str(e)))
        del HELD[:]
    return out, started(lvl2, lvl3)


async def waiting(started, go):
    started.set()
    go.wait()
    return work(1)


async def in_thread():
    return await waiting(*THREAD_EVENTS)


THREAD_EVENTS = []


def threads(n):
    # another thread's gc.get_objects finds a body running in this one
    started, go = threading.Event(), threading.Event()
    THREAD_EVENTS[:] = [started, go]
    res = []
    t = threading.Thread(target=lambda: res.append(drive(in_thread())))
    t.start()
    started.wait()
    found = sorted(o.__qualname__ for o in gc.get_objects()
                   if inspect.iscoroutine(o) and
                   inspect.getcoroutinestate(o) == 'CORO_RUNNING')
    go.set()
    t.join()
    return found, res


EVENTS = []


def tracer(frame, event, arg):
    if frame.f_code.co_name.startswith('tr'):
        EVENTS.append((event, frame.f_code.co_name, frame.f_lineno))
    return tracer


def profiler(frame, event, arg):
    if frame.f_code.co_name.startswith('tr'):
        EVENTS.append((event, frame.f_code.co_name))


async def tr3(k, how):
    x = work(k)
    if how == 'trace':
        sys.settrace(tracer)
    elif how == 'profile':
        sys.setprofile(profiler)
    y = work(x + 1)
    if k == 2:
        y = await pause(y)
    sys.settrace(None)
    sys.setprofile(None)
    return x + y


async def tr2(k, how):
    return await tr3(k, how) + 1


async def tr1(k, how):
    return await tr2(k, how) + 1


def tracing(n):
    # tracing or profiling started three levels down: the events, the
    # result and a suspension after it
    out = []
    for how in ('trace', 'profile'):
        for k in (1, 2):
            out.append((how, k, drive(tr1(k, how), [10]), EVENTS[:]))
            del EVENTS[:]
    return out, started(tr2, tr3)


EXC = []


async def handling(k):
    try:
        raise KeyError(k)
    except KeyError:
        me = running('handling')[0]
        EXC.append([type(r).__name__ for r in gc.get_referents(me)
                    if isinstance(r, BaseException)])
        EXC.append(sys.exc_info()[0])
    EXC.append(sys.exc_info()[0])
    return work(k)


async def handles(k):
    return await handling(k)


def handled(n):
    # the exception a running coroutine's body handles is its own
    try:
        raise IndexError
    except IndexError:
        r = drive(handles(n))
    out = [r, EXC[:]]
    del EXC[:]
    return out


async def trh(k):
    try:
        raise KeyError(k)
    except KeyError:
        sys.settrace(tracer)
        EXC.append(sys.exc_info()[0])
        work(k)
        EXC.append(sys.exc_info()[0])
    sys.settrace(None)
    EXC.append(sys.exc_info()[0])
    return work(k)


async def trhs(k):
    return await trh(k)


def traced_handler(n):
    # tracing started in a body's own handler: the handled exception stays
    # the body's, and its caller's is its own again after it
    try:
        raise IndexError
    except IndexError:
        r = drive(trhs(n))
        after = sys.exc_info()[0]
    out = [r, EXC[:], after, EVENTS[:]]
    del EXC[:]
    del EVENTS[:]
    return out


def origins(n):
    # no direct start where CPython records the origin
    old = sys.get_coroutine_origin_tracking_depth()
    sys.set_coroutine_origin_tracking_depth(2)
    try:
        # closed, as its never-awaited warning would show its origin's
        # line, which a compiled frame doesn't give
        lvl1(n, 'none').close()
        r = drive(lvl1(n, 'referrers'))
    finally:
        sys.set_coroutine_origin_tracking_depth(old)
    out = [r, LOG[:1], [c.cr_origin is None for c in HELD]]
    del LOG[:]
    del HELD[:]
    return out


CASES = [
    ('raised', [(3,)], HOOKED),
    ('realised', [(0,), (2,)], {}),
    ('threads', [(0,)], {}),
    ('tracing', [(0,)], HOOKED_TRACE),
    ('handled', [(1,)],
     {'warmup': (40, 'its paths go on compiling for some rounds'),
      'leaks': 'LeJIT keeps a fixed number of blocks more (up to about 90) '
               'that depends on addresses, even with randomisation off, and '
               'does not grow with the rounds'}),
    ('traced_handler', [(1,)], {}),
    ('origins', [(1,)], {}),
]
