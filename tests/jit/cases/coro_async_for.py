# async for in a coroutine's body, compiled: GET_AITER, GET_ANEXT, the
# await of __anext__() and END_ASYNC_FOR. Each case returns what CPython
# shows, and whether the bodies were compiled.
import asyncio, sys, traceback
from coro_bodies import compiled, drive, pause, work


def tb_lines(e):
    return [(f.name, f.lineno) for f in traceback.extract_tb(e.__traceback__)]


class Counter:
    # an async iterator: 'suspend' makes __anext__ suspend the coroutine
    # first; 'fail' makes it raise KeyError at that step instead of ending
    def __init__(self, n, suspend=False, fail=None):
        self.n, self.i, self.suspend, self.fail = n, 0, suspend, fail

    def __aiter__(self):
        return self

    async def __anext__(self):
        if self.suspend:
            await pause(('next', self.i))
        if self.i == self.fail:
            raise KeyError(self.i)
        if self.i >= self.n:
            raise StopAsyncIteration
        self.i += 1
        return self.i


async def total(n, suspend):
    t = 0
    async for v in Counter(n, suspend):
        t += work(v)
    return t


async def flow(n, suspend):
    # break, continue, else, return, and a nested loop
    out = []
    async for v in Counter(n, suspend):
        if v == 2:
            continue
        if v == 4:
            break
        async for w in Counter(v, suspend):
            out.append((v, w))
    else:
        out.append('else')
    async for v in Counter(n, suspend):
        if v == 3:
            return out, v
    return out, None


async def raising(n, suspend, fail, inside):
    try:
        async for v in Counter(n, suspend, fail):
            if v == inside:
                raise ValueError(v)
        return 'done'
    except (KeyError, ValueError) as e:
        return type(e).__name__, str(e), tb_lines(e), sys.exc_info()[0]


async def comprehension(n, suspend):
    return [v async for v in Counter(n, suspend)], \
        {v: w async for v in Counter(n, suspend)
         async for w in Counter(2, suspend)}


def basics(n):
    out = []
    for suspend in (False, True):
        out.append(drive(total(n, suspend)))
        out.append(drive(flow(n, suspend)))
        for fail, inside in ((None, None), (2, None), (None, 2)):
            out.append(drive(raising(n, suspend, fail, inside)))
        out.append(drive(comprehension(n, suspend)))
    return out, compiled(total, flow, raising)


class NoAiter:
    pass


class NoAnext:
    def __aiter__(self):
        return 5


class LosesAnext:
    def __aiter__(self):
        return self

    async def __anext__(self):
        del LosesAnext.__anext__
        return 1


class BadAnext:
    def __aiter__(self):
        return self

    def __anext__(self):
        return 7


class BadAwait:
    def __aiter__(self):
        return self

    def __anext__(self):
        return self

    def __await__(self):
        return 8


async def over(it):
    async for v in it:
        work(v)
    return 'end'


def errors(n):
    out = []
    for it in (NoAiter(), NoAnext(), LosesAnext(), BadAnext(), BadAwait()):
        c = over(it)
        try:
            c.send(None)
        except StopIteration as e:
            out.append(('return', e.value))
        except TypeError as e:
            out.append((str(e), type(e.__cause__).__name__,
                        str(e.__cause__), e.__context__ is e.__cause__,
                        tb_lines(e)))
        if not hasattr(LosesAnext, '__anext__'):
            LosesAnext.__anext__ = LosesAnext_anext
    return out


LosesAnext_anext = LosesAnext.__anext__


def interrupted(n):
    # thrown into, and closed, while suspended in __anext__'s await
    out = []
    for how in (ValueError, StopAsyncIteration, 'close'):
        c = total(n, True)
        out.append(c.send(None))
        try:
            if how == 'close':
                c.close()
                out.append('closed')
            else:
                out.append(c.throw(how))
        except StopIteration as e:
            out.append(('return', e.value))
        except BaseException as e:
            out.append(('exc', type(e).__name__, tb_lines(e)))
    return out


async def agen(n):
    for i in range(n):
        await asyncio.sleep(0)
        yield i


async def consumer(n):
    t = []
    async for v in agen(n):
        t.append(work(v))
    async for v in Counter(n, True):
        t.append(v)
    return t


async def cancelled(n):
    task = asyncio.ensure_future(consumer(1000))
    await asyncio.sleep(0)
    await asyncio.sleep(0)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        return 'cancelled'


def with_asyncio(n):
    r = asyncio.run(consumer(n))
    return r, asyncio.run(cancelled(n)), compiled(consumer)


TRACED = []


def tracer(frame, event, arg):
    if frame.f_code is traced.__code__:
        TRACED.append((event, frame.f_lineno))
    return tracer


async def traced(n):
    t = 0
    sys.settrace(tracer)
    async for v in Counter(n):
        t += v
    sys.settrace(None)
    return t


def tracing(n):
    out = [drive(traced(n)), TRACED[:]]
    del TRACED[:]
    return out


CASES = [
    ('basics', [(4,)], {}),
    ('errors', [(0,)], {}),
    ('interrupted', [(3,)], {}),
    ('with_asyncio', [(3,)],
     {'warmup': (60, "asyncio's paths go on compiling for some rounds"),
      'leaks_with_aslr': 'what plain CPython itself keeps after the rounds '
                         'varies with addresses, by tens of blocks'}),
    ('tracing', [(3,)], {}),
]
