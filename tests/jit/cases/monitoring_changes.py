# Compiled code sees tracing, profiling and sys.monitoring switched on
# while it runs, however they are switched on: from another thread, from a
# callee, for one code object alone, and again after they were on and off;
# and a frame's locals written by a callee after that. Each loop below calls step() each round,
# and the change comes partway through.
import sys
import threading


def step(i):
    return i + 1


def thread_turns_on_monitoring(n):
    # the loop's own lines are seen too, once its frame goes on interpreted
    if not hasattr(sys, 'monitoring'):
        return None
    mon = sys.monitoring
    tool = mon.PROFILER_ID
    me = thread_turns_on_monitoring.__code__
    events = []
    mon.use_tool_id(tool, 'test')
    mon.register_callback(tool, mon.events.PY_START,
                          lambda code, off: events.append(code.co_name))
    mon.register_callback(
        tool, mon.events.LINE,
        lambda code, line: events.append(line - me.co_firstlineno)
        if code is me else None)

    def turn_on():
        mon.set_events(tool, mon.events.PY_START | mon.events.LINE)
    t = 0
    seen = 0
    try:
        for i in range(n):
            if i == n // 2:
                th = threading.Thread(target=turn_on)
                th.start()
                th.join()
                seen = len(events)
            t = step(t)
    finally:
        mon.set_events(tool, 0)
        mon.free_tool_id(tool)
    return t, [e for e in events[seen:] if e in ('step', 'turn_on') or
               isinstance(e, int)]


def thread_traces_all(n):
    if not hasattr(threading, 'settrace_all_threads'):
        return None
    events = []

    def tracer(frame, event, arg):
        if frame.f_code is step.__code__ and event == 'call':
            events.append(frame.f_locals['i'])
        return None
    th = threading.Thread(target=threading.settrace_all_threads,
                          args=(tracer,))
    t = 0
    try:
        for i in range(n):
            if i == n // 2:
                th.start()
                th.join()
            t = step(t)
    finally:
        threading.settrace_all_threads(None)
    return t, events


def turn_on_profile(prof):
    sys.setprofile(prof)


def callee_turns_on_profiling(n):
    calls = []

    def prof(frame, event, arg):
        if frame.f_code is step.__code__ and event == 'call':
            calls.append(frame.f_locals['i'])
    t = 0
    try:
        for i in range(n):
            if i == n // 2:
                turn_on_profile(prof)
            t = step(t)
    finally:
        sys.setprofile(None)
    return t, calls


def profiled_round(n, prof):
    t = 0
    for i in range(n):
        if i == n // 2:
            turn_on_profile(prof)
        t = step(t)
    return t


def on_off_on(n):
    # compiled before profiling is on and off, and on again while it runs
    calls = []

    def prof(frame, event, arg):
        if frame.f_code is step.__code__ and event == 'call':
            calls.append(frame.f_locals['i'])
    first = [profiled_round(n, None) for _ in range(3)]
    turn_on_profile(prof)
    turn_on_profile(None)
    try:
        t = profiled_round(n, prof)
    finally:
        sys.setprofile(None)
    return first[0], t, calls


def set_callers_x(v):
    sys._getframe(1).f_locals['x'] = v


def written(n, at):
    x = 0
    seen = []
    for i in range(n):
        if i == at:
            set_callers_x(100)
        seen.append(x)
        x = step(x)
    return seen


def written_after_tracing(n):
    # compiled before profiling is on and off, and run after
    first = [written(n, -1) for _ in range(3)]
    turn_on_profile(lambda *a: None)
    turn_on_profile(None)
    return first[0], written(n, n - 2)


def watch_lines(code, tool, events):
    mon = sys.monitoring
    mon.register_callback(
        tool, mon.events.LINE,
        lambda c, line: events.append(line - code.co_firstlineno)
        if c is code else None)
    mon.set_local_events(tool, code, mon.events.LINE)


def callee_sets_local_events(n):
    # events set on the running loop's code alone: its lines are seen from
    # the call on
    if not hasattr(sys, 'monitoring'):
        return None
    mon = sys.monitoring
    tool = mon.DEBUGGER_ID
    me = callee_sets_local_events.__code__
    events = []
    mon.use_tool_id(tool, 'test')
    t = 0
    try:
        for i in range(n):
            if i == n // 2:
                watch_lines(me, tool, events)
            t = step(t)
    finally:
        mon.set_local_events(tool, me, 0)
        mon.free_tool_id(tool)
    return t, events


def local_events_on_callee(n):
    # events set on a callee's code alone: its calls are seen from then
    # on, made from compiled code or from C (map)
    if not hasattr(sys, 'monitoring'):
        return None
    mon = sys.monitoring
    tool = mon.DEBUGGER_ID
    code = step.__code__
    starts = []
    mon.use_tool_id(tool, 'test')
    mon.register_callback(tool, mon.events.PY_START,
                          lambda c, off: starts.append(c.co_name))
    t = 0
    try:
        for i in range(n):
            if i == n // 2:
                mon.set_local_events(tool, code, mon.events.PY_START)
            t = step(t)
        mapped = list(map(step, range(3)))
    finally:
        mon.set_local_events(tool, code, 0)
        mon.free_tool_id(tool)
    return t, mapped, starts


CASES = [
    ('thread_turns_on_monitoring', "[(40,)]", {}),
    ('thread_traces_all', "[(40,)]", {}),
    ('callee_turns_on_profiling', "[(40,)]", {}),
    ('on_off_on', "[(20,)]", {}),
    ('written_after_tracing', "[(20,)]", {}),
    ('callee_sets_local_events', "[(20,)]", {}),
    ('local_events_on_callee', "[(20,)]", {}),
]
