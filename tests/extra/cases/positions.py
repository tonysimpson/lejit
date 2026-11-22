# Positions in the bytecode where CPython sees them: LeJIT keeps them in bytes,
# CPython 3.10 counts instructions (jump arguments, f_lasti, b_handler).
# Jumps with a live local across a loop and a try in a loop (the merge
# points' units); a traceback's tb_lasti and tb_lineno, and a frame's
# f_lineno read from a callee (frames.c's conversions); a
# function compiled while tracing is off and run with it on (resuming in
# ceval at the instruction it reached); a generator suspended with items on
# its stack (f_stackdepth); a generator expression run fused.
import sys


def loop_live(n):
    t, k = 0, 7
    i = 0
    while i < n:
        if i % 3:
            t += k
        else:
            k += 1
        i += 1
    return t, k


def try_in_loop(n):
    out = []
    for i in range(n):
        try:
            out.append(10 // (i % 3))
        except ZeroDivisionError:
            out.append(-1)
        finally:
            out.append(i)
    return out


def _fail(x):
    return 1 // x


def tb_positions(x):
    try:
        a = 1
        b = _fail(x)
        return a + b
    except ZeroDivisionError:
        tb = sys.exc_info()[2]
        out = []
        while tb is not None:
            f = tb.tb_frame
            code = f.f_code
            # the instruction tb_lasti names is the call, on its line
            op = code.co_code[tb.tb_lasti]
            out.append((code.co_name, tb.tb_lineno - code.co_firstlineno, op))
            tb = tb.tb_next
        return out


def _caller_line():
    f = sys._getframe(1)
    return f.f_lineno - f.f_code.co_firstlineno


def _inner_line():
    # an emulated frame between: the one the record of its call places
    return _caller_line(), sys._getframe(0).f_back.f_code.co_name


def frame_place(n):
    out = []
    for i in range(n):
        out.append(_caller_line())
        if i:
            out.append(_inner_line())
    return out


def _tracer(frame, event, arg):
    return None


def traced_midway(n):
    t = 0
    for i in range(n):
        if i == 2:
            sys.settrace(_tracer)
        t += i
    sys.settrace(None)
    return t


def _gen(n):
    for i in range(n):
        x = [i, (yield i), i * 2]
        yield x


def suspended_stack(n):
    g = _gen(n)
    out = [next(g)]
    for k in range(n):
        out.append(g.send(k + 100))
        out.append(next(g, None))
    return out


def genexpr_sum(n):
    return sum(i * i for i in range(n)), list(x + 1 for x in range(n) if x % 2)


CASES = [
    ('loop_live', [(10,), (0,), (31,)], {}),
    ('try_in_loop', [(7,)], {}),
    ('tb_positions', [(1,), (0,)], {}),
    ('frame_place', [(3,)], {}),
    ('traced_midway', [(6,)], {}),
    ('suspended_stack', [(3,)], {}),
    ('genexpr_sum', [(10,), (0,)], {}),
]
