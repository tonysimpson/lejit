# Tracing or profiling that starts while a frame runs compiled code: the
# frame, and its compiled callers when the call returns to them, go on
# interpreted from the next instruction, with the events CPython gives a
# frame already running (no 'call'; 'line' only where f_trace was set).
import sys


def recorder():
    events = []

    def prof(frame, event, arg):
        if event.startswith('c_'):
            name = getattr(arg, '__name__', '?')
        else:
            name = frame.f_code.co_name
        if name not in ('setprofile', 'settrace'):
            events.append((event, name))
    return events, prof


def mid_profile(n):
    events, prof = recorder()
    sys.setprofile(prof)
    x = len('abc') + abs(-n)
    sys.setprofile(None)
    return events, x


def mid_trace(n):
    lines = []
    me = sys._getframe()
    first = me.f_code.co_firstlineno

    def tracer(frame, event, arg):
        if frame is me:
            lines.append((event, frame.f_lineno - first))
        return tracer
    me.f_trace = tracer
    sys.settrace(tracer)
    a = n + 1
    b = a * 2
    sys.settrace(None)
    return lines, b


def start(p):                         # inlined into its caller
    sys.setprofile(p)


def mid_inlined(n):
    events, prof = recorder()
    start(prof)
    y = len('ab') + n
    sys.setprofile(None)
    return events, y


def start_loop(p, n):                 # a loop: called, not inlined
    t = 0
    for i in range(n):
        t += i
    sys.setprofile(p)
    for i in range(n):
        t += abs(-i)
    return t


def mid_callee(n):
    events, prof = recorder()
    t = start_loop(prof, n)
    t += len('xyz')
    sys.setprofile(None)
    return events, t


def start_raise(p, n):
    for i in range(n):
        pass
    sys.setprofile(p)
    raise ValueError(n)


def mid_raise(n):
    events, prof = recorder()
    try:
        start_raise(prof, n)
    except ValueError as e:
        tb = sys.exc_info()[2]
        names = []
        while tb is not None:
            names.append(tb.tb_frame.f_code.co_name)
            tb = tb.tb_next
        r = (e.args, names)
    sys.setprofile(None)
    return events, r


def frames_in_callee(p, n):
    for i in range(n):
        pass
    sys.setprofile(p)
    names = [sys._getframe(i).f_code.co_name for i in range(3)]
    return names


def mid_frames(n):
    events, prof = recorder()
    names = frames_in_callee(prof, n)
    sys.setprofile(None)
    return names


def mid_handler(n):
    events, prof = recorder()
    try:
        raise KeyError(n)
    except KeyError:
        sys.setprofile(prof)
        info = sys.exc_info()[0].__name__
        try:
            raise
        except KeyError as e:
            again = e.args
    after = sys.exc_info()
    sys.setprofile(None)
    return events, info, again, after


class Ctx:
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def mid_with(n):
    events, prof = recorder()
    with Ctx() as c:
        sys.setprofile(prof)
        k = abs(-n)
    sys.setprofile(None)
    return events, k


def mid_loop(n):
    events, prof = recorder()
    t = 0
    for i in range(n):
        if i == 2:
            sys.setprofile(prof)
        t += abs(-i)
    sys.setprofile(None)
    return events, t


LINES = []


def line_tracer(frame, event, arg):
    if frame.f_code is trace_callee.__code__:
        LINES.append((event, frame.f_lineno - frame.f_code.co_firstlineno))
    return line_tracer


def trace_callee(n):                  # no cells: called compiled
    for i in range(n):                # a loop: not inlined
        pass
    sys._getframe().f_trace = line_tracer
    sys.settrace(line_tracer)
    a = n + 1
    sys.settrace(None)
    return a


def mid_trace_callee(n):
    del LINES[:]
    a = trace_callee(n)
    return list(LINES), a


SEEN = []


def locals_tracer(frame, event, arg):
    loc = frame.f_locals
    SEEN.append((event, loc.get('i'), loc.get('total')))
    return locals_tracer


def attach_locals_tracer():
    sys._getframe(1).f_trace = locals_tracer
    sys.settrace(lambda *a: None)


def trace_reads_locals(n):            # ceval runs it on: f_locals is ceval's
    del SEEN[:]
    total = 0
    for i in range(n):
        total += i
        if i == 1:
            attach_locals_tracer()
    sys.settrace(None)
    return total, list(SEEN)


CASES = [
    ('mid_profile', [(1,), (2,)], {}),
    ('mid_trace', [(1,), (2,)], {}),
    ('mid_inlined', [(1,)], {}),
    ('mid_callee', [(3,)], {}),
    ('mid_raise', [(2,)], {}),
    ('mid_frames', [(2,)], {}),
    ('mid_handler', [(5,)], {}),
    ('mid_with', [(4,)], {}),
    ('mid_loop', [(5,)], {}),
    ('trace_reads_locals', [(4,)], {}),
    ('mid_trace_callee', [(2,)],
     {'xfail': 'C29: f_trace set on a compiled callee\'s emulated frame is lost'}),
]
