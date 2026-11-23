# close(), throw(), gi_yieldfrom and cr_await on a generator or coroutine
# from inside its own running frame, called through a compiled function:
# CPython refuses, or gives None, without looking at what the frame runs
# (frames.c:gen_guard_init, where an eval hook could make it look).
import types

ME = []


def attempt(f):
    try:
        return ('ok', f())
    except Exception as e:
        return (type(e).__name__, str(e))


def poke(me, k):
    out = []
    for call in (lambda: me.throw(KeyError),
                 lambda: me.throw(KeyError('x'), None, None), me.close,
                 lambda: me.gi_yieldfrom if isinstance(me, types.GeneratorType)
                 else me.cr_await):
        out.append(attempt(call))
    return out, k


def gen(k):
    yield poke(ME[0], k)
    yield 'after'


async def coro(k):
    return poke(ME[0], k)


def running(n):
    out = []
    for k in range(n):
        g = gen(k)
        ME[:] = [g]
        out.append(list(g))
        c = coro(k)
        ME[:] = [c]
        try:
            c.send(None)
        except StopIteration as e:
            out.append(e.value)
    del ME[:]
    return out


CASES = [
    ('running', [(3,)], {}),
]
