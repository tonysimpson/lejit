# Compiled code makes a frame object only where CPython would have one:
# each case counts the frame objects alive while its work holds them, the
# same way plain and under LeJIT, and under LeJIT also that it made no
# more than those (frames.c:le_pyframe_new, mechanisms/frames.h:
# le_frame_object, counted in lejit._core._stats()['frame_objects']), so
# none was made and dropped on the side.
import asyncio, gc, sys, types

CHECKED = {'checked': 'reads lejit._core._stats'}


def _made():
    lejit = sys.modules.get('lejit')
    if lejit is None or not lejit._started:
        return 0
    return lejit._core._stats()['frame_objects']


def _alive():
    return sum(type(o) is types.FrameType for o in gc.get_objects())


class Count:
    # frame objects alive and made since it started; 'held' is taken while
    # the work holds what it made
    def __init__(self):
        self.alive = _alive()
        self.made = _made()

    def held(self):
        self.alive_then = _alive() - self.alive

    def result(self):
        return self.alive_then, _made() - self.made <= self.alive_then


def counted(work, *args):
    count = Count()
    result = work(*args)
    count.held()
    return result, count.result()


def _add(a, b):
    return a + b


def _sum_to(n):
    total = 0
    for i in range(n):
        total = _add(total, i)
    return total


def calls(n):
    return counted(_sum_to, n)


def _squares(n):
    for i in range(n):
        yield i * i


def _echo():
    total = 0
    while True:
        total += yield total


def _generators(n):
    g = _echo()
    next(g)
    sent = [g.send(i) for i in range(n)]
    return sum(_squares(n)), list(_squares(n)), sent


def generators(n):
    return counted(_generators, n)


@types.coroutine
def _pause(v):
    return (yield v)


async def _inner(k):
    return await _pause(k) + 1


async def _outer(k):
    return await _inner(k) + 1


def _pause_all(n):
    coros = [_outer(k) for k in range(n)]
    return coros, [c.send(None) for c in coros]


def coroutines_pausing(n):
    # counted while paused: finishing raises StopIteration, a traceback
    (coros, paused), count = counted(_pause_all, n)
    out = []
    for c in coros:
        try:
            c.send(10)
        except StopIteration as e:
            out.append(e.value)
    return paused, out, count


async def _wait(fut):
    return await fut


async def _request(loop, futs):
    fut = loop.create_future()
    futs.append(fut)
    return await _wait(fut) * 2


async def _gather(n, count):
    loop = asyncio.get_running_loop()
    futs = []
    tasks = asyncio.gather(*[_request(loop, futs) for i in range(n)])
    await asyncio.sleep(0)
    count.held()
    for i, f in enumerate(futs):
        f.set_result(i)
    return await tasks


def gather(n):
    # making an event loop catches a KeyError (asyncio's debug-mode test
    # reads os.environ), a traceback: only the run is counted, while
    # every request waits
    loop = asyncio.new_event_loop()
    try:
        count = Count()
        result = loop.run_until_complete(_gather(n, count))
    finally:
        loop.close()
    return result, count.result()


CASES = [
    ('calls', [(50,)], CHECKED),
    ('generators', [(20,)], CHECKED),
    ('coroutines_pausing', [(10,)], CHECKED),
    ('gather', [(20,)],
     dict(CHECKED,
          warmup=(300, "asyncio's paths go on compiling for some rounds"),
          clear_type_cache="asyncio's task_wakeup looks up the result of "
                           "gather()'s future by a name made afresh each time")),
]
