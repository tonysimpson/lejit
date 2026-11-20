# A trace or profile function must see every frame:
# while one is set, eval_hook.c runs code interpreted, even code already
# compiled, and a compiled call or inlined frame tests it at entry and makes
# the real call instead (psyfunc.c:tracing_on). The filters below keep the
# case function's own frame out (tracing_midframe.py tests that frame).
import sys


def body(out):
    out.append(1)
    for i in range(3):
        out.append(i)
    return len(out)


def traced(n):
    body([])                          # compiled before tracing starts
    events = []

    def tracer(frame, event, arg):
        if frame.f_code is body.__code__:
            events.append((event, frame.f_lineno - body.__code__.co_firstlineno))
        return tracer
    sys.settrace(tracer)
    try:
        r = body([])
    finally:
        sys.settrace(None)
    return r, events, body([])


def profiled(n):
    body([])
    calls = []

    def prof(frame, event, arg):
        if event == 'call':
            calls.append(frame.f_code.co_name)
    sys.setprofile(prof)
    try:
        body([])
    finally:
        sys.setprofile(None)
    return calls


def jump_out(n):
    # the reproduction of the 20 GB run: a tracer jumps out of an endless loop
    def f(out):
        out.append(1)
        while True:
            out.append(3)
        out.append(4)
    first = f.__code__.co_firstlineno

    def tracer(frame, event, arg):
        if frame.f_code is f.__code__ and event == 'line' \
                and frame.f_lineno == first + 3:
            frame.f_lineno = first + 4
        return tracer
    out = []
    sys.settrace(tracer)
    try:
        f(out)
    finally:
        sys.settrace(None)
    return out



def add(a, b):                        # inlined into a compiled caller
    return a + b


def fib(n):                           # a compiled recursive call
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


def watch(codes):
    """a tracer recording (event, name, line) for frames of 'codes'"""
    events = []

    def tracer(frame, event, arg):
        co = frame.f_code
        if co in codes:
            events.append((event, co.co_name,
                           frame.f_lineno - co.co_firstlineno))
        return tracer
    return events, tracer


def inlined(n):
    t = 0
    for i in range(5):
        t = add(t, i)                 # compiled (and inlined) before tracing
    events, tracer = watch({add.__code__})
    sys.settrace(tracer)
    try:
        for i in range(n):
            t = add(t, i)
    finally:
        sys.settrace(None)
    return t, events


def recursive(n):
    fib(n)
    events, tracer = watch({fib.__code__})
    sys.settrace(tracer)
    try:
        r = fib(n)
    finally:
        sys.settrace(None)
    return r, len(events), events[:6], fib(n)


def start(tracer):                    # tracing set by a compiled callee
    sys.settrace(tracer)


def set_in_callee(n):
    body([])
    events, tracer = watch({body.__code__})
    start(tracer)
    try:
        r = body([])
    finally:
        sys.settrace(None)
    return r, events


def c_calls(n):
    body([])
    calls = []

    def prof(frame, event, arg):
        if frame.f_code is body.__code__:
            calls.append((event, getattr(arg, '__name__', None)))
    sys.setprofile(prof)
    try:
        body([])
    finally:
        sys.setprofile(None)
    return calls


def genexpr(n):
    # a generator expression a builtin consumes is fused into the caller
    # (psyfunc.c:psyco_call_genexpr): traced, it must be a real generator
    total = sum(x * x for x in range(n))
    events = []

    def tracer(frame, event, arg):
        if frame.f_code.co_name == '<genexpr>':
            events.append(event)
        return tracer
    sys.settrace(tracer)
    try:
        total += sum(x * x for x in range(n))
        m = max(x for x in range(n))
        lst = list(x for x in range(n))
    finally:
        sys.settrace(None)
    return total, m, lst, events


def loop(n):
    t = 0
    for i in range(n):
        t += add(i, 1)
    return t


def timed(n):
    import time
    t0 = time.perf_counter()
    loop(n)
    return time.perf_counter() - t0


def turned_off(n):
    # compiled speed comes back once tracing stops: the traced path is only
    # the rare branch of each call site
    timed(n)
    before = min(timed(n) for _ in range(3))
    sys.settrace(lambda *a: None)
    try:
        loop(n)
    finally:
        sys.settrace(None)
    after = min(timed(n) for _ in range(3))
    return after < 3 * before + 0.002


CASES = [
    ('traced', [(1,)], {}),
    ('profiled', [(1,)], {}),
    ('jump_out', [(1,)], {}),
    ('inlined', [(3,)], {}),
    ('recursive', [(5,)], {}),
    ('set_in_callee', [(1,)], {}),
    ('c_calls', [(1,)], {}),
    ('genexpr', [(4,)], {}),
    ('turned_off', [(20000,)], {}),
]
