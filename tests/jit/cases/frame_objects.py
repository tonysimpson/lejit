# Compiled code makes a frame object only when something asks for one
# (pyruntime.c:le_pyframe_new, mechanisms/frames.h:le_frame_object, counted
# in lejit._core._stats()['frame_objects']). Each case runs work that asks
# for none and gives its result and whether any was made while it ran.
import asyncio, sys, types

CHECKED = {'checked': 'reads lejit._core._stats'}


def _interpreter_frames():
    # where a frame is a frame object, as before 3.11, CPython's coroutine
    # is one too: only the counts of what CPython makes none of are checked
    try:
        import lejit._core
    except ImportError:
        return False
    return bool(dict(lejit._core._build['features']).get(
        'LE_HAS_INTERPRETER_FRAME'))


INTERPRETER_FRAMES = _interpreter_frames()


def _made():
    lejit = sys.modules.get('lejit')
    if lejit is None or not lejit._started:
        return 0
    return lejit._core._stats()['frame_objects']


def none_made(work, *args):
    before = _made()
    result = work(*args)
    return result, _made() - before


def _add(a, b):
    return a + b


def _sum_to(n):
    total = 0
    for i in range(n):
        total = _add(total, i)
    return total


def calls(n):
    return none_made(_sum_to, n)


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
    return none_made(_generators, n)


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
    # finishing one raises StopIteration, whose traceback CPython makes a
    # frame object for too: only the pauses are counted
    (coros, paused), made = none_made(_pause_all, n)
    out = []
    for c in coros:
        try:
            c.send(10)
        except StopIteration as e:
            out.append(e.value)
    return paused, out, made if INTERPRETER_FRAMES else None


async def _wait(fut):
    return await fut


async def _request(loop, futs):
    fut = loop.create_future()
    futs.append(fut)
    return await _wait(fut) * 2


async def _gather(n):
    loop = asyncio.get_running_loop()
    futs = []
    tasks = asyncio.gather(*[_request(loop, futs) for i in range(n)])
    await asyncio.sleep(0)
    for i, f in enumerate(futs):
        f.set_result(i)
    return await tasks


def gather(n):
    # making an event loop catches a KeyError (asyncio's debug-mode test
    # reads os.environ), whose traceback CPython makes frame objects for:
    # only the run is counted
    loop = asyncio.new_event_loop()
    try:
        result, made = none_made(loop.run_until_complete, _gather(n))
    finally:
        loop.close()
    return result, made if INTERPRETER_FRAMES else None


def _fails(i):
    if i % 3 == 0:
        raise ValueError(i)
    return i


def _catches(n):
    out = []
    for i in range(n):
        try:
            out.append(_fails(i))
        except ValueError:
            out.append(-1)
    return out


def exceptions(n):
    # CPython makes a frame object for each raising call and one for the
    # catching frame, which it keeps
    out, made = none_made(_catches, n)
    return out, made <= out.count(-1) + 1


CASES = [
    ('calls', [(50,)], CHECKED),
    ('generators', [(20,)], CHECKED),
    ('coroutines_pausing', [(10,)], CHECKED),
    ('gather', [(20,)], CHECKED),
    ('exceptions', [(20,)],
     dict(CHECKED, xfail='a catching frame gets a frame object again at each'
                         ' catch, with its callers\' (frames.c:catcher_caller)')),
]
