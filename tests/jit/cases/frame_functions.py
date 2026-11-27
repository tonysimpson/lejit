# The frame object of a compiled call (a traceback's, the one
# sys._getframe or f_back gives, one kept after its call) holds the call's
# own function, as CPython's frame does: gc.get_referents finds it there,
# and no other function is made for the frame, so a function watcher
# (PyFunction_AddWatcher) is told of none. Where CPython has no function
# watchers, only the functions are compared. The function called may be
# known only at run time: passed in, or a closure made by the caller.
import gc
import sys
import types

try:
    import _testcapi
except ImportError:
    _testcapi = None

WATCHERS = hasattr(_testcapi, 'add_func_watcher')


def _functions(frame):
    return [o.__qualname__ for o in gc.get_referents(frame)
            if type(o) is types.FunctionType]


def _raise(n):
    if n == 0:
        raise ValueError(n)
    return _raise(n - 1)


def _walk(tb):
    out = []
    while tb is not None:
        out.append((tb.tb_frame.f_code.co_name, _functions(tb.tb_frame)))
        tb = tb.tb_next
    return out


def through_calls(n):
    try:
        _raise(n)
    except ValueError as e:
        return _walk(e.__traceback__)


def _call(f, n):
    return f(n)


def passed_in(n):
    try:
        _call(_raise, n)
    except ValueError as e:
        return _walk(e.__traceback__)


def _callers(depth):
    frames = []
    f = sys._getframe(1)
    while f is not None and len(frames) < depth:
        frames.append(f)
        f = f.f_back
    return frames


class Callers:
    def method(self, depth):
        return _callers(depth)


def _middle(depth):
    return Callers().method(depth)


def _maker():
    def inner(depth):
        return _callers(depth)
    return inner


# made once, so the call reads it at run time
_made = [_maker()]


def closure_seen(depth):
    frames = _made[0](depth)
    return [(fr.f_code.co_name, _functions(fr)) for fr in frames[:-1]]


def seen_from_callee(depth):
    # read once the calls have returned: a running frame's function is
    # its call's, which gc.get_referents doesn't show
    frames = _middle(depth)
    return [(f.f_code.co_name, _functions(f)) for f in frames[:-1]]


def _kept():
    return sys._getframe()


def kept_after_return():
    f = _kept()
    return f.f_code.co_name, _functions(f), f.f_back.f_code.co_name


def watched(n):
    events = []
    if WATCHERS:
        wid = _testcapi.add_func_watcher(
            lambda e, f, v: events.append(f.__qualname__))
    out = (through_calls(n), seen_from_callee(3), passed_in(n),
           closure_seen(3))
    if WATCHERS:
        _testcapi.clear_func_watcher(wid)
    return out, events


CASES = [
    ('through_calls', [(0,), (4,)], {}),
    ('seen_from_callee', [(3,)], {}),
    ('passed_in', [(0,), (4,)], {}),
    ('closure_seen', [(3,)], {}),
    ('kept_after_return', [()], {}),
    ('watched', [(4,)], {}),
]
