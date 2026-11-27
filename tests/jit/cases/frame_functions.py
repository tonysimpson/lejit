# The frame object of a compiled call (a traceback's, the one
# sys._getframe or f_back gives, one kept after its call) holds the call's
# own function, as CPython's frame does: gc.get_referents finds it there,
# and no other function is made for the frame, so a function watcher
# (PyFunction_AddWatcher) is told of none. Where CPython has no function
# watchers, only the functions are compared.
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


def through_calls(n):
    try:
        _raise(n)
    except ValueError as e:
        tb = e.__traceback__
    out = []
    while tb is not None:
        out.append((tb.tb_frame.f_code.co_name, _functions(tb.tb_frame)))
        tb = tb.tb_next
    return out


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
    out = through_calls(n), seen_from_callee(3)
    if WATCHERS:
        _testcapi.clear_func_watcher(wid)
    return out, events


CASES = [
    ('through_calls', [(0,), (4,)], {}),
    ('seen_from_callee', [(3,)], {}),
    ('kept_after_return', [()], {}),
    ('watched', [(4,)], {}),
]
