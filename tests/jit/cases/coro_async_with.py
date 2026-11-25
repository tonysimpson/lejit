# async with, and awaits inside exception handlers, in a coroutine's body:
# the body runs compiled, and so does a handler that can await where
# handlers are the exception table's; where they have blocks it runs in
# ceval from its start (mergepoints.c:psyco_handler_awaits), as the
# coroutine keeps the exception it handles. Each case returns what CPython
# shows, and whether the bodies were compiled.
import asyncio, contextlib, dis, gc, sys, traceback
from coro_bodies import compiled, drive, pause, work

# where a return, break or continue can leave a finally block with a
# pseudo-exception (BREAK_LOOP's bytecode), a body that also awaits in a
# handler stays interpreted (mergepoints.c:handler_yields_kept)
LEAVING_KEPT = 'BREAK_LOOP' not in dis.opmap
# where handlers are the exception table's, a handler that awaits runs
# compiled too
HANDLERS_COMPILED = hasattr(compile('0', '', 'eval'), 'co_exceptiontable')

LOG = []


class CM:
    # an async context manager: 'suspend' makes its methods suspend the
    # coroutine; 'swallow' makes __aexit__ silence the exception
    def __init__(self, name, suspend=False, swallow=False):
        self.name, self.suspend, self.swallow = name, suspend, swallow

    async def __aenter__(self):
        if self.suspend:
            await pause(('enter', self.name))
        LOG.append(('enter', self.name, sys.exc_info()[0]))
        return self.name

    async def __aexit__(self, t, v, tb):
        if self.suspend:
            await pause(('exit', self.name))
        LOG.append(('exit', self.name, t, str(v) if v else v,
                    sys.exc_info()[0]))
        return self.swallow


async def plain(k, suspend):
    async with CM('a', suspend) as x:
        y = work(x) + str(k)
    return y


async def nested(k, suspend):
    async with CM('a', suspend) as x, CM('b', suspend) as y:
        async with CM('c', suspend):
            r = work(x + y)
    return r, k


async def raising(k, suspend, swallow):
    try:
        async with CM('a', suspend, swallow) as x:
            work(x)
            if k:
                raise KeyError(k)
            r = 'body'
        return 'after', r if not k else None
    except KeyError as e:
        return 'caught', str(e), sys.exc_info()[0]


async def leaving(n, suspend):
    out = []
    for i in range(n):
        async with CM(str(i), suspend):
            if i == 1:
                continue
            if i == 3:
                break
            out.append(work(i))
    async with CM('r', suspend):
        return out


def run_all(n):
    out = []
    for suspend in (False, True):
        out.append(drive(plain(n, suspend)))
        out.append(drive(nested(n, suspend)))
        for k in (0, 1):
            for swallow in (False, True):
                out.append(drive(raising(k, suspend, swallow)))
        out.append(drive(leaving(n, suspend)))
        out.append(LOG[:])
        del LOG[:]
    return out, compiled(plain, nested,
                         *((raising, leaving) if LEAVING_KEPT else ()))


def basics(n):
    return run_all(n)


class NoEnter:
    async def __aexit__(self, *a):
        return False


class NoExit:
    async def __aenter__(self):
        return self


class BadEnter:
    def __aenter__(self):
        return 5

    async def __aexit__(self, *a):
        return False


class BadExit:
    async def __aenter__(self):
        return self

    def __aexit__(self, *a):
        return 5


async def using(cm):
    async with cm:
        return work(1)


def errors(n):
    return [drive(using(cm)) for cm in (NoEnter(), NoExit(), BadEnter(),
                                        BadExit(), 5)]


@contextlib.asynccontextmanager
async def managed(name, log):
    log.append(('in', name))
    try:
        await asyncio.sleep(0)
        yield name
    except ValueError as e:
        log.append(('handled', name, str(e)))
    finally:
        await asyncio.sleep(0)
        log.append(('out', name))


async def job(k, log):
    async with managed('m%d' % k, log) as name:
        await asyncio.sleep(0)
        if k % 2:
            raise ValueError(k)
        log.append(work(name))
    return k


async def jobs(n, log):
    return await asyncio.gather(*[job(k, log) for k in range(n)])


def with_asyncio(n):
    log = []
    r = asyncio.run(jobs(n, log))
    return r, log, compiled(job)


EXC = []


async def handler_awaits(k, suspend):
    try:
        try:
            work(k)
            raise KeyError(k)
        except KeyError:
            EXC.append(sys.exc_info()[0])
            if suspend:
                await pause('in handler')
            else:
                await CM('x').__aenter__()
            EXC.append(sys.exc_info()[0])
            if k == 2:
                raise ValueError('again')
        EXC.append(sys.exc_info()[0])
        r = 'done'
    finally:
        EXC.append(('finally', sys.exc_info()[0]))
    return r


def handlers(n):
    # an await in an except body: the coroutine keeps its handled exception
    # while it is suspended, and the driver's is its own
    out = []
    for k in (1, 2):
        for suspend in (False, True):
            try:
                raise IndexError
            except IndexError:
                c = handler_awaits(k, suspend)
                seen = []
                try:
                    v = c.send(None)
                    seen.append((v, sys.exc_info()[0]))
                    v = c.send(None)
                except StopIteration as e:
                    seen.append(('return', e.value))
                except ValueError as e:
                    seen.append(('exc', str(e), sys.exc_info()[0]))
                seen.append(sys.exc_info()[0])
            out.append((k, suspend, seen, EXC[:]))
            del EXC[:]
    del LOG[:]
    return out, compiled(handler_awaits)


async def awaits_in(k):
    return await handler_awaits(k, True)


def started(n):
    # the same body started by its caller's await
    out = []
    for k in (1, 2):
        out.append(drive(awaits_in(k)))
        out.append(EXC[:])
        del EXC[:]
    return out


def tb_lines(e):
    return [(f.name, f.lineno) for f in traceback.extract_tb(e.__traceback__)]


class Exits:
    # __aexit__ suspends, then lets the exception go on or silences it
    def __init__(self, swallow=False):
        self.swallow = swallow

    async def __aenter__(self):
        return self

    async def __aexit__(self, t, v, tb):
        await pause(('exit', t and t.__name__, sys.exc_info()[0]))
        return self.swallow


async def exit_reraises(k, swallow):
    # RERAISE after the exit suspended, from the place first raised
    async with Exits(swallow):
        work(k)
        if k == 1:
            raise KeyError(k)
        if k == 2:
            {}[k]
    return 'ok'


async def finally_awaits(k):
    try:
        if k == 1:
            raise ValueError(k)
        if k == 2:
            return 'returned'
    finally:
        v = await pause(('finally', sys.exc_info()[0]))
        EXC.append(v)
    return 'end'


async def except_awaits(k):
    try:
        raise IndexError(k)
    except IndexError as e:
        v = await pause(('except', str(e), sys.exc_info()[0]))
        if k == 1:
            raise
        if k == 2:
            raise TypeError('new') from e
        if k == 3:
            try:
                raise KeyError('inner')
            except KeyError:
                v = await pause(('inner', sys.exc_info()[0]))
            v = (v, await pause(('outer again', sys.exc_info()[0])))
        return v, sys.exc_info()[0]


def outcome(c, sends=('s1', 's2', 's3', 's4')):
    # what a coroutine yields, with the driver's exception state at each,
    # then how it ended: an exception with its traceback and context
    out = []
    sends = list(sends)
    try:
        v = c.send(None)
        while True:
            out.append((v, sys.exc_info()[0]))
            v = c.send(sends.pop(0) if sends else None)
    except StopIteration as e:
        out.append(('return', e.value))
    except Exception as e:
        out.append(('exc', type(e).__name__, str(e), tb_lines(e),
                    type(e.__context__).__name__, type(e.__cause__).__name__))
    return out


def interrupted(c, how):
    # throw into, or close, a coroutine suspended in a handler
    out = [c.send(None)]
    try:
        if how == 'close':
            c.close()
            out.append(('closed', c.cr_frame))
        else:
            out.append(c.throw(how))
            out.append(c.send(None))
    except StopIteration as e:
        out.append(('return', e.value))
    except BaseException as e:
        out.append(('exc', type(e).__name__, tb_lines(e),
                    type(e.__context__).__name__))
    return out


def suspended_in_handlers(n):
    # suspended inside except, finally and async with's exit, resumed,
    # raising again and replaced; the driver's exception state is its own
    out = []
    for k in (0, 1, 2):
        for swallow in (False, True):
            out.append(outcome(exit_reraises(k, swallow)))
    for k in (0, 1, 2):
        out.append(outcome(finally_awaits(k)))
    for k in (0, 1, 2, 3):
        out.append(outcome(except_awaits(k)))
    try:
        raise OSError('driver')
    except OSError:
        out.append(outcome(except_awaits(0)))
        out.append(sys.exc_info()[0])
    for f in (except_awaits, finally_awaits):
        out.append(interrupted(f(1), ZeroDivisionError))
        out.append(interrupted(f(1), 'close'))
    c = except_awaits(0)
    c.send(None)
    # gc sees the exception the suspended coroutine handles
    out.append(sum(isinstance(o, IndexError) for o in gc.get_referents(c)))
    out.append((c.cr_frame.f_lineno, c.cr_await is not None))
    c.close()
    out.append(EXC[:])
    del EXC[:]
    return out, compiled(*((exit_reraises, finally_awaits, except_awaits)
                           if HANDLERS_COMPILED else ()))


TRACED = []


def tracer(frame, event, arg):
    if frame.f_code is except_awaits.__code__:
        TRACED.append((event, frame.f_lineno))
    return tracer


def traced_in_handler(n):
    # tracing set while the coroutine is suspended in its handler: the
    # resume and the rest of the handler are traced (with no await after
    # it, which under any frame-evaluation hook CPython traces otherwise)
    out = []
    if not HANDLERS_COMPILED:
        # 3.9 gives the resume's first 'exception' event another line
        return out
    for k in (1, 2):
        c = except_awaits(k)
        out.append(c.send(None)[0])
        sys.settrace(tracer)
        try:
            out.append(outcome(c))
        finally:
            sys.settrace(None)
        out.append(TRACED[:])
        del TRACED[:]
    return out


def quiet(frame, event, arg):
    return None


def handler_lasti(n):
    # the place a handler raises again from, given by ceval and resumed
    # compiled (run into the handler with tracing on), and given by
    # compiled code and resumed in ceval (tracing set while suspended)
    out = []
    for traced_first in (True, False):
        for c in (exit_reraises(1, False), exit_reraises(2, False),
                  finally_awaits(1)):
            sys.settrace(quiet if traced_first else None)
            try:
                out.append(c.send(None))
            finally:
                sys.settrace(None if traced_first else quiet)
            try:
                out.append(outcome(c, ()))
            finally:
                sys.settrace(None)
    del EXC[:]
    return out


# What the gc sees holding a local of a running coroutine or generator.
# Each looks twice: calling the gc itself, and in a call to Python code
# (holders). ceval shows the gc a running frame's locals, except where it
# hides them while the frame calls C (running_frame_hidden): there it
# shows them during a call to Python code, and compiled code doesn't.
async def holds_exception(k):
    exc = None
    try:
        async with Exits():
            raise KeyError(k)
    except KeyError as e:
        exc = e
    refs = gc.get_referrers(exc)
    return holders(exc), names(refs)


def holders(o):
    if HIDDEN_IN_C_CALLS:
        return None
    return names(gc.get_referrers(o))


def names(refs):
    return sorted(type(r).__name__ for r in refs)


class Held:
    pass


def gen_holds(k):
    # before and after the generator pauses
    held = Held()
    for i in range(k):
        work(i)
    refs = gc.get_referrers(held)
    yield holders(held), names(refs)
    refs = gc.get_referrers(held)
    yield holders(held), names(refs)


async def resumed_holds(k):
    # after the coroutine has paused
    held = Held()
    await pause(k)
    for i in range(k):
        work(i)
    refs = gc.get_referrers(held)
    return holders(held), names(refs)


def running_frame_hidden():
    # whether ceval hides a running frame's locals from the gc while the
    # frame calls C
    def gen():
        x = Held()
        yield gc.get_referrers(x)
    return not next(gen())


HIDDEN_IN_C_CALLS = running_frame_hidden()


def referrers(n):
    out = [outcome(holds_exception(k)) for k in range(3)]
    out.append([list(gen_holds(k)) for k in range(3)])
    out.append([outcome(resumed_holds(k)) for k in range(3)])
    ok = compiled(holds_exception) if HANDLERS_COMPILED else True
    # (after compiled(), which reads whether lejit is imported)
    return out, ok


CASES = [
    ('basics', [(4,)], {}),
    ('errors', [(0,)], {}),
    ('with_asyncio', [(3,)],
     {'warmup': (60, "asyncio's paths go on compiling for some rounds"),
      'clear_type_cache': "asyncio's task_wakeup looks up the result of "
                          "gather()'s future by a name made afresh each time"}),
    ('handlers', [(0,)], {}),
    ('started', [(0,)], {}),
    ('suspended_in_handlers', [(0,)], {}),
    ('traced_in_handler', [(0,)], {}),
    ('handler_lasti', [(0,)], {}),
    ('referrers', [(0,)], {}),
]
