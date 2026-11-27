# A call from C runs with no real frame (eval_hook.c:le_run_frameless):
# under LeJIT, run() makes its call that way, through a checked build's
# test hook; plain, it calls f itself. Each case reads the frames as
# Python sees them from inside such a call and from what it calls: the
# frame's view is made only when asked (frames.c:keep_root).
import gc
import signal
import sys
import traceback
import warnings

def run(f, *args):
    lejit = sys.modules.get('lejit')
    frameless = lejit is not None and lejit._started
    # one line for both, as a warning's place shows it
    return lejit._core._run_frameless(f, *args) if frameless else f(*args)


def names(f):
    out = []
    while f is not None and f.f_code.co_name != 'main':
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def _here(a, b):
    f = sys._getframe()
    return names(f), f.f_locals['a'], f.f_locals['b'], f.f_lineno - f.f_code.co_firstlineno


def _middle(x):
    return _here(x, x + 1)


def _sum(n):
    t = 0
    for i in range(n):
        t += i
    return t


def chain(n):
    return [run(_here, i, -i) for i in range(n)] + [run(_middle, n)] + [run(_sum, n)]


def _nested(k):
    if k == 0:
        return names(sys._getframe())
    return run(_nested, k - 1)


def nested(k):
    return run(_nested, k)


def _raises(z):
    return 10 // z


def _catches(z):
    try:
        return _raises(z)
    except ZeroDivisionError:
        return [t.name for t in traceback.extract_tb(sys.exc_info()[2])]


def exceptions(n):
    out = []
    for i in range(n):
        try:
            run(_raises, 0)
        except ZeroDivisionError:
            out.append([t.name for t in traceback.extract_tb(sys.exc_info()[2])])
        out.append(run(_catches, 0))
    return out


def _own(a):
    f = sys._getframe()
    a += 1
    return f


def outlives(n):
    out = []
    for i in range(n):
        f = run(_own, i)
        # its locals once it has returned are a known difference
        out.append((f.f_code.co_name, f.f_back is not None and f.f_back.f_code.co_name))
    return out


def _same(a):
    return sys._getframe() is sys._getframe()


def identity(n):
    return [run(_same, i) for i in range(n)]


def _held(a):
    b = [a]
    return [r is sys._getframe() for r in gc.get_referrers(b)], len(b)


def referrers(n):
    return [run(_held, i) for i in range(n)]


def _traced(a):
    sys.settrace(_tracer)
    b = a * 2
    sys.settrace(None)
    return b + 1


def _tracer(frame, event, arg):
    _events.append((frame.f_code.co_name, event))
    return _tracer


_events = []


def tracing(n):
    del _events[:]
    out = [run(_traced, i) for i in range(n)]
    return out, sorted(set(_events))


def _deep(k):
    if k == 0:
        return len(names(sys._getframe()))
    return _deep(k - 1)


def deep(n):
    return [run(_deep, k) for k in range(n)]


def _warns(a):
    warnings.warn('w%d' % a, stacklevel=2)
    return a


def warns(n):
    with warnings.catch_warnings(record=True) as ws:
        warnings.simplefilter('always')
        out = [run(_warns, i) for i in range(n)]
    return out, [(str(w.message), w.lineno - warns.__code__.co_firstlineno) for w in ws]


class _Box:
    pass


def _spin(box):
    # the signal lands at one of the loop's eval_breaker tests
    while box.seen is None:
        box.n += 1
    return box.n > 0


def signals(n):
    out = []
    for i in range(n):
        box = _Box()
        box.seen = None
        box.n = 0

        def handler(signum, frame):
            box.seen = names(frame)
        old = signal.signal(signal.SIGALRM, handler)
        try:
            signal.setitimer(signal.ITIMER_REAL, 0.001)
            out.append((run(_spin, box), box.seen))
        finally:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, old)
    return out


def _current(a):
    import threading
    return names(sys._current_frames()[threading.get_ident()])


def current_frames(n):
    return [run(_current, i) for i in range(n)]


def _clears(a):
    try:
        sys._getframe().clear()
    except RuntimeError as e:
        return 'refused'
    return 'cleared'


def clears(n):
    return [run(_clears, i) for i in range(n)]


def _line_of_caller():
    f = sys._getframe(1)
    return f.f_code.co_name, f.f_lineno - f.f_code.co_firstlineno


def _lines(a):
    x = a + 1
    y = _line_of_caller()
    return y, x


def lines(n):
    return [run(_lines, i) for i in range(n)]


def _gen(k):
    for i in range(k):
        yield names(sys._getframe())


def _iterates(k):
    return list(_gen(k))


def generators(n):
    return [run(_iterates, i) for i in range(n)]


# tracing starts inside, so the frame goes on in ceval: its frame object,
# made then, keeps the locals ceval ends it with
def _traced_on(k):
    a = [k]
    try:
        if k < 0:
            raise ValueError
    except ValueError:
        pass
    sys.settrace(lambda *x: None)
    b = k + 1
    a = [a, b]
    sys.settrace(None)
    return sys._getframe(), a


def resumed_locals(n):
    out = []
    for i in range(n):
        f, a = run(_traced_on, i)
        del a
        gc.collect()
        out.append(f.f_locals['a'])
    return out


# subinterpreters run inside a frameless call from C with no frame out of
# it (a thread's start), on the same OS thread: they never see its frames,
# and what they make with their own allocator stays theirs
def _subinterpreters():
    for name in ('_interpreters', '_xxsubinterpreters'):
        try:
            return __import__(name)
        except ImportError:
            pass
    return None


def _sub(interps, out, done):
    run = getattr(interps, 'exec', None) or interps.run_string
    try:
        i = interps.create()
        out.append(run(i, 'import struct, sys; assert sys._getframe().f_back is None'))
        interps.destroy(i)
    finally:
        done.release()


def subinterpreter(n):
    import _thread
    interps = _subinterpreters()
    if interps is None:
        return None
    lejit = sys.modules.get('lejit')
    out = []
    for _ in range(n):
        done = _thread.allocate_lock()
        done.acquire()
        if lejit is not None and lejit._started:
            _thread.start_new_thread(lejit._core._run_frameless,
                                     (_sub, interps, out, done))
        else:
            _thread.start_new_thread(_sub, (interps, out, done))
        done.acquire()
    return out


CASES = [
    ('chain', [(5,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('nested', [(4,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('exceptions', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('outlives', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('identity', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('referrers', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('tracing', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('deep', [(30,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('warns', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('signals', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('current_frames', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('clears', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('lines', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('generators', [(4,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('resumed_locals', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
    ('subinterpreter', [(3,)], {'checked': 'needs lejit._core._run_frameless'}),
]
