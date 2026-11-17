# A trace or profile function must see every frame (KNOWN_BUGS C23): while
# one is set, eval_hook.c runs code interpreted, even code already compiled.
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


CASES = [
    ('traced', [(1,)], {'xfail': 'C23: compiled callers call or inline compiled code directly, past the eval hook'}),
    ('profiled', [(1,)], {'xfail': 'C23: compiled callers call or inline compiled code directly, past the eval hook'}),
    ('jump_out', [(1,)], {'xfail': 'C23: compiled callers call or inline compiled code directly, past the eval hook'}),
]
