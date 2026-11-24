# A running call's frame object is one object for the call's life, as in
# CPython: asked for twice, through f_back or a traceback, it is the same
# object, and what is stored on it (f_trace, extra f_locals keys) stays.
# The function a case names runs in its real frame: each calls the one
# tested, which has a loop, so it is called compiled, not inlined.
import gc, sys

from ref_tracer import count_made, made


def via(fn, n):
    t = 0
    for i in range(n):
        t += i
    return fn(n)


def me_twice(n):
    for i in range(n):
        pass
    a = sys._getframe()
    b = sys._getframe()
    return a is b


def inner_back(n):
    for i in range(n):
        pass
    return sys._getframe(1)


def caller_seen(n):                   # the caller's frame, from two callees
    for i in range(n):
        pass
    me = sys._getframe()
    return inner_back(n) is me, inner_back(n + 1) is me


def ask_back(n):
    for i in range(n):
        pass
    return sys._getframe().f_back, sys._getframe(1)


def back_same(n):
    for i in range(n):
        pass
    a, b = ask_back(n)
    return a is b, a is sys._getframe()


def held(n):
    for i in range(n):
        pass
    return sys._getframe()


def calls_distinct(n):                # each call's frame is its own
    frames = [held(n) for i in range(3)]
    return [f is g for f in frames for g in frames].count(True)


def recurse(n):
    for i in range(1):
        pass
    if n == 0:
        return [sys._getframe(k) for k in range(4)]
    return recurse(n - 1)


def depths(n):
    seen = recurse(n)
    again = recurse(n)
    return (len(set(map(id, seen))), [f.f_code.co_name for f in seen],
            any(f is g for f in seen for g in again))


def store_extra(n):
    for i in range(n):
        pass
    f = sys._getframe()
    f.f_locals['extra'] = n * 2
    x = n + 1
    return 'extra' in sys._getframe().f_locals, sys._getframe().f_locals['x']


def line_moves(n):
    for i in range(n):
        pass
    f = sys._getframe()
    first = f.f_lineno
    for i in range(n):
        pass
    return f.f_lineno - first


def raise_here(n):
    for i in range(n):
        pass
    me = sys._getframe()
    try:
        raise ValueError(n)
    except ValueError as e:
        return e.__traceback__.tb_frame is me


def trace_kept(n):
    for i in range(n):
        pass
    sys._getframe().f_trace = len
    return sys._getframe().f_trace is len


def trace_kept_caller(n):
    t = 0
    for i in range(n):
        t += i
    return trace_kept(n), t


LINES = []


def line_tracer(frame, event, arg):
    if frame.f_code is set_mid_call.__code__:
        LINES.append((event, frame.f_lineno - frame.f_code.co_firstlineno))
    return line_tracer


def attach(depth):
    for i in range(1):
        pass
    sys._getframe(depth).f_trace = line_tracer


def set_mid_call(n):                  # f_trace set by a callee, then tracing
    for i in range(n):
        pass
    attach(1)
    total = n
    sys.settrace(line_tracer)
    total += 1
    total += 2
    sys.settrace(None)
    return total


def mid_call(n):
    del LINES[:]
    t = set_mid_call(n)
    return list(LINES), t


def gen_body(n):
    for i in range(n):
        yield sys._getframe()


def generator_frame(n):
    g = gen_body(n)
    frames = list(g)
    g2 = gen_body(2)
    f0 = next(g2)
    same = f0 is g2.gi_frame
    f0.f_trace = len
    f1 = next(g2)
    return (len(set(map(id, frames))), same, f1 is f0, g2.gi_frame.f_trace is len)


import traceback


def deep(n, out):
    for i in range(1):
        pass
    if n == 0:
        out.append(sys._getframe())
        return [(f.name, f.lineno) for f in traceback.extract_stack()][-70:]
    return deep(n - 1, out)


def deep_walk(n):                     # many kept at once, then let go
    out = []
    a = deep(n, out)
    b = deep(n, out)
    return a == b, len(a), out[0] is out[1], out[0].f_back is not None


def raiser(n, seen):
    for i in range(n):
        pass
    seen.append(sys._getframe())
    raise KeyError(n)


def raise_through(n):                 # the kept call ends by an exception
    seen = []
    try:
        raiser(n, seen)
    except KeyError as e:
        tb = e.__traceback__
        return tb.tb_next.tb_frame is seen[0], tb.tb_frame is sys._getframe()


class Walker:
    def __del__(self):
        WALKED.append(sys._getframe(1).f_code.co_name)


WALKED = []


def holds_walker(n):
    for i in range(n):
        pass
    f = sys._getframe()
    f.f_locals['w'] = Walker()        # dies with the frame
    return n


def finaliser_walks(n):               # the frame let go runs Python
    del WALKED[:]
    r = holds_walker(n)
    gc.collect()                      # the frame and its locals are a cycle
    return r, list(WALKED)


def clear_running(n):                # a running frame can't be cleared
    for i in range(n):
        pass
    f = sys._getframe()
    f.f_trace = len
    try:
        f.clear()
        cleared = 'cleared'
    except RuntimeError as e:
        cleared = str(e)
    return cleared, f.f_trace is len


def run_clear_running(n): return via(clear_running, n)


def run_me_twice(n): return via(me_twice, n)
def run_caller_seen(n): return via(caller_seen, n)
def run_back_same(n): return via(back_same, n)
def run_calls_distinct(n): return via(calls_distinct, n)
def run_depths(n): return via(depths, n)
def run_store_extra(n): return via(store_extra, n)
def run_line_moves(n): return via(line_moves, n)
def run_raise_here(n): return via(raise_here, n)
def run_mid_call(n): return via(mid_call, n)
def run_generator_frame(n): return via(generator_frame, n)


def getframe_caller(n):
    t = 0
    for i in range(n):
        t += i
    return sys._getframe(1)


def raises(n):
    t = 0
    for i in range(n):
        t += i
    raise KeyError(t)


def reraises(n):
    t = 0
    for i in range(n):
        t += i
    try:
        raises(n)
    except KeyError:
        raise


def catches(n):
    t = 0
    for i in range(n):
        t += i
    try:
        reraises(n)
    except KeyError:
        return t


def walks_made(n):
    # tuples made per sys._getframe(1) from a compiled callee, and per
    # exception a compiled callee catches and raises again, whose traceback
    # frame is linked to its caller as the call ends (counted after the
    # first round, which compiles)
    out = []
    for i in range(n):
        count_made(tuple)
        for k in range(100):
            f = getframe_caller(2)
        getframe_count = made()
        count_made(tuple)
        for k in range(100):
            catches(2)
        catch_count = made()
        if i and getframe_count is not None:
            out.append((f.f_code.co_name, getframe_count // 100,
                        catch_count // 100))
    return out


class Index:
    def __index__(self):
        return 1


def getframe_arguments(n):
    # what sys._getframe takes and refuses, and its messages
    out = []
    for args, kwargs in [((), {}), ((0,), {}), ((-5,), {}), ((True,), {}),
                         ((Index(),), {}), (('1',), {}), ((1.0,), {}),
                         ((1, 2), {}), ((2 ** 70,), {}), ((), {'depth': 1}),
                         ((1000,), {})]:
        for i in range(n):
            try:
                out.append(sys._getframe(*args, **kwargs).f_code.co_name)
            except Exception as e:
                out.append('%s: %s' % (type(e).__name__, e))
    return out


CASES = [
    ('walks_made', [(3,)], {}),
    ('getframe_arguments', [(2,)], {}),
    ('run_me_twice', [(2,)], {}),
    ('run_caller_seen', [(2,)], {}),
    ('run_back_same', [(2,)], {}),
    ('run_calls_distinct', [(2,)], {}),
    ('run_depths', [(3,)], {}),
    ('run_store_extra', [(2,)], {}),
    ('run_line_moves', [(2,)], {}),
    ('run_raise_here', [(2,)], {}),
    ('trace_kept_caller', [(3,)], {}),
    ('run_mid_call', [(2,)], {}),
    ('run_generator_frame', [(3,)], {}),
    ('deep_walk', [(60,)], {}),
    ('raise_through', [(2,)], {}),
    ('finaliser_walks', [(2,)], {}),
    ('run_clear_running', [(2,)], {}),
]
