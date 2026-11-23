# async with, and awaits inside exception handlers, in a coroutine's body:
# the body runs compiled, but a handler that can await runs in ceval from
# its start (mergepoints.c:psyco_handler_awaits), as the coroutine keeps
# the exception it handles. Each case returns what CPython shows, and
# whether the bodies were compiled.
import asyncio, contextlib, dis, sys
from coro_bodies import compiled, drive, pause, work

# where a return, break or continue can leave a finally block with a
# pseudo-exception (BREAK_LOOP's bytecode), a body that also awaits in a
# handler stays interpreted (mergepoints.c:handler_yields_kept)
LEAVING_KEPT = 'BREAK_LOOP' not in dis.opmap

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


CASES = [
    ('basics', [(4,)], {}),
    ('errors', [(0,)], {}),
    ('with_asyncio', [(3,)], {}),
    ('handlers', [(0,)], {}),
    ('started', [(0,)], {}),
]
