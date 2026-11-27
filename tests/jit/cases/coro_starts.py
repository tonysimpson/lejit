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
    if lejit is None or not lejit._started:
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


def drive(c, sends=(), collect=False):
    # the values c yields, then how it ended. Without 'collect', with the
    # collector off: a collection pending as a coroutine is sent into runs
    # that step in ceval (eval_hook.c:le_gen_to_run), which starts the
    # bodies it awaits through the eval hook, so started() would depend on
    # where collections fall
    out = []
    sends = list(sends)
    enabled = gc.isenabled()
    if not collect:
        gc.disable()
    try:
        v = c.send(None)
        while True:
            out.append(v)
            v = c.send(sends.pop(0) if sends else None)
    except StopIteration as e:
        out.append(('return', e.value))
    except Exception as e:
        out.append(('exc', type(e).__name__, str(e)))
    finally:
        if enabled:
            gc.enable()
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


def poke(inner):
    # what each way into a running coroutine raises
    for call in (lambda: inner.send(None), lambda: inner.throw(KeyError),
                 inner.close, inner.cr_frame.clear):
        try:
            call()
        except Exception as e:
            LOG.append((type(e).__name__, str(e)))


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
        poke(inner)
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
    elif how == 'raise':
        HELD.extend(running('lvl3'))
        raise KeyError('x')
    return work(k)


async def lvl2(k, how):
    return await lvl3(k, how) + 1


async def lvl1(k, how):
    return await lvl2(k, how) + 1


# as lvl1 to lvl3, but suspending: a coroutine resumed where its frame is
# inside it runs compiled from an entry of its own, which started()
# would see
async def sus3(k, how):
    HELD.extend(running('sus2') + running('sus3'))
    LOG.append(('resumed', await pause(work(k))))
    LOG.append(inspect.getcoroutinestate(HELD[-1]))
    return work(k)


async def sus2(k, how):
    return await sus3(k, how) + 1


async def sus1(k, how):
    return await sus2(k, how) + 1


def realised(n):
    # each way to find the running inner coroutines, then the same objects
    # once their bodies have returned, raised or suspended
    out = []
    hows = ('referrers', 'rename', 'suspend', 'raise', 'none')
    for how in ('objects',) * FRAME_OBJECTS + hows:
        top = sus1 if how == 'suspend' else lvl1
        out.append((how, drive(top(n, how), [7]), LOG[:]))
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
    out = [r, EXC[:], sys.exc_info()[0]]
    del EXC[:]
    return out, started(handling)


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
    return out, started(trh)


async def fg3(k):
    me = sys._getframe()
    # frames have no f_generator before 3.14
    c = getattr(me, 'f_generator', None)
    return (type(c).__name__, c is not None and c.cr_frame is me,
            c is not None and inspect.getcoroutinestate(c), work(k))


async def fg2(k):
    return await fg3(k)


def frame_generator(n):
    # a running body's frame gives its coroutine; run a few times, for
    # them to compile
    out = [drive(fg2(n)) for _ in range(20)]
    return out[0], all(o == out[0] for o in out), started(fg3)


async def th3(k):
    return work(await pause(k))


async def th2(k):
    return await th3(k) + 1


def thrown(n):
    # a coroutine its body made as it suspended, then thrown into, closed
    # or sent into from the coroutine awaiting it
    out = []
    for how in ('throw', 'close', 'send'):
        c = th2(n)
        out.append(c.send(None))
        inner = running('th3')
        try:
            if how == 'throw':
                c.throw(KeyError('t'))
            elif how == 'close':
                out.append(c.close())
            else:
                c.send(5)
        except BaseException as e:
            out.append((type(e).__name__, str(e)))
        out.append([(inspect.getcoroutinestate(i), i.cr_frame, i.cr_await,
                     i.cr_running) for i in inner])
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


async def gc3(k):
    # a collection as the coroutine of this running body is made, which
    # visits its frame's words
    a, b, c, d, e, f, g, h = [work(k)] * 8
    # objects the collector counts, for the coroutine's to start one
    held = [[] for _ in range(50)]
    old = gc.get_threshold()
    gc.set_threshold(1)
    objs = gc.get_objects()
    gc.set_threshold(*old)
    found = [o for o in objs if inspect.iscoroutine(o) and
             o.__qualname__ == 'gc3' and
             inspect.getcoroutinestate(o) != 'CORO_CLOSED']
    del objs
    return len(found), len(held), a + b + c + d + e + f + g + h


async def gc2(k):
    return await gc3(k)


def collected(n):
    out = [drive(gc2(n), collect=True) for _ in range(20)]
    return out[0], all(o == out[0] for o in out), started(gc3)


def now():
    # the exception handled here, as sys.exc_info() and sys.exception()
    # (3.11+) give it
    e = sys.exc_info()[1]
    f = getattr(sys, 'exception', None)
    return (type(e).__name__, f is None or f() is e)


class Swallow:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        EXC.append(('exit', a[0] and a[0].__name__, now()))
        return True


async def kinds(k):
    # except, finally and with, each handling an exception, with what each
    # sees and what is chained
    out = [now()]
    try:
        raise KeyError(k)
    except KeyError:
        out.append(now())
        try:
            raise ValueError(k)
        except ValueError as e:
            out.append((now(), type(e.__context__).__name__))
        out.append(now())
    out.append(now())
    try:
        work(k)
    finally:
        out.append(now())
    try:
        try:
            raise IndexError(k)
        finally:
            out.append(now())
    except IndexError as e:
        out.append((now(), e.__context__))
    with Swallow():
        raise TypeError(k)
    out.append(now())
    EXC.append(out)
    try:
        raise KeyError(k)
    except KeyError:
        try:
            raise ValueError(k)
        finally:
            out.append(now())


async def kinds_outer(k):
    try:
        return await kinds(k)
    except ValueError as e:
        return ('raised', now(), type(e.__context__).__name__)


def handler_kinds(n):
    out = []
    for _ in range(3):
        out.append(drive(kinds_outer(n)))
        try:
            raise IndexError
        except IndexError:
            out.append((drive(kinds_outer(n)), now()))
        out.append(EXC[:])
        del EXC[:]
    return out, now(), started(kinds)


async def seen(k):
    # what an await in a handler sees: its awaiter's exception
    return (now(), work(k))


async def nested(k, how):
    # a handler that handles an exception, around one that awaits: that one
    # goes on in ceval with the outer one's exception the coroutine's
    try:
        raise KeyError(k)
    except KeyError:
        if how == 'objects':
            found = running('nested')
            me = found[0]
            LOG.append((len(found), me.cr_frame is sys._getframe(),
                        inspect.getcoroutinestate(me), errors(me), now()))
        try:
            raise ValueError(k)
        except ValueError:
            r = await seen(k)
            v = await pause(k)
            LOG.append(now())
        LOG.append(now())
    LOG.append(now())
    return r, v


async def nested_mid(k, how):
    return await nested(k, how)


async def nested_top(k, how):
    # in its own handler, which runs in ceval as it awaits
    if how == 'handling':
        try:
            raise IndexError(k)
        except IndexError:
            r = await nested_mid(k, how)
            LOG.append(now())
            return r
    return await nested_mid(k, how)


def errors(c):
    # the exceptions gc finds a coroutine holds
    return [type(r).__name__ for r in gc.get_referents(c)
            if isinstance(r, BaseException)]


def handler_resumed(n):
    # suspended in a handler ceval runs, inside one compiled code ran: what
    # the coroutines hold and what their driver sees, at the suspension
    # and after
    out = []
    for how in ('plain', 'objects', 'handling'):
        c = nested_top(n, how)
        try:
            raise TypeError
        except TypeError:
            v = c.send(None)
            inner = running('nested')
            out.append((how, v, now(), [errors(i) for i in inner],
                        [inspect.getcoroutinestate(i) for i in inner],
                        errors(c)))
        try:
            c.send(5)
        except StopIteration as e:
            out.append(e.value)
        out.append((LOG[:], now(), [errors(i) for i in inner],
                    [i.cr_frame for i in inner]))
        del LOG[:]
    # nested resumes from an entry of its own once it has suspended, so
    # started() can't say how it began
    return out


async def held_inner(k):
    return await pause(work(k)) + 1


async def held_outer(k):
    return await held_inner(k) + 1


def suspended_referents(n):
    # a body started by its await that suspends becomes a coroutine as
    # CPython's: no frame object until cr_frame asks, and its own function.
    # Resumed, it runs from an entry of its own, so started() can't say.
    out = []
    enabled = gc.isenabled()
    gc.disable()
    try:
        for i in range(3):
            c = held_outer(n + i)
            out.append(c.send(None))
            inner = c.cr_await
            refs = gc.get_referents(inner)
            out.append((inner.__qualname__,
                        sorted(type(o).__name__ for o in refs),
                        any(o is held_inner for o in refs)))
            out.append(inner.cr_frame.f_code.co_name)
            try:
                c.send(5)
            except StopIteration as e:
                out.append(e.value)
    finally:
        if enabled:
            gc.enable()
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
    ('frame_generator', [(1,)], {}),
    ('thrown', [(1,)], {}),
    ('collected', [(1,)], {}),
    ('handler_kinds', [(1,)], {}),
    ('handler_resumed', [(1,)], {}),
    ('suspended_referents', [(1,)], {}),
]
