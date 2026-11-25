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


def rechecks():
    """compiled code's exact tracing tests so far: 0 where it makes none,
    or where a release build can't say"""
    stats = getattr(sys.modules.get('lejit._core'), '_stats', None)
    return 0 if stats is None else stats().get('tracing_rechecks', 0)


def compiled(f):
    info = getattr(sys.modules.get('lejit._core'), '_code_info', None)
    return info is None or info(f.__code__) == 'compiled'


def cprofile_tool():
    import cProfile
    p = cProfile.Profile()
    return p.enable, p.disable


def monitoring_tool(local):
    # a sys.monitoring tool watching every code, or loop's and add's alone
    M = sys.monitoring
    tool = 3
    codes = (loop.__code__, add.__code__)

    def on():
        M.use_tool_id(tool, 'case')
        M.register_callback(tool, M.events.PY_START, lambda code, at: None)
        if local:
            for co in codes:
                M.set_local_events(tool, co, M.events.PY_START)
        else:
            M.set_events(tool, M.events.PY_START)

    def off():
        if local:
            for co in codes:
                M.set_local_events(tool, co, 0)
        else:
            M.set_events(tool, 0)
        M.register_callback(tool, M.events.PY_START, None)
        M.free_tool_id(tool)
    return on, off


TOOLS = {
    'settrace': lambda: (lambda: sys.settrace(lambda *a: None),
                         lambda: sys.settrace(None)),
    'setprofile': lambda: (lambda: sys.setprofile(lambda *a: None),
                           lambda: sys.setprofile(None)),
    'cprofile': cprofile_tool,
    'monitoring': lambda: monitoring_tool(False),
    'monitoring_local': lambda: monitoring_tool(True),
}


def after_a_tool(name, n):
    # once a tool has watched code and stopped, compiled calls into that
    # code make no exact tracing test: CPython keeps the code's monitoring
    # data, which the quick test must not take for a tool still watching
    if name.startswith('monitoring') and not hasattr(sys, 'monitoring'):
        return None
    on, off = TOOLS[name]()
    loop(n)
    on()
    try:
        loop(n)
    finally:
        off()
    loop(n)                           # the first test after brings it back
    start = rechecks()
    r = loop(n) + loop(n)
    return r, rechecks() - start, compiled(loop)


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
    ('after_a_tool', [(name, 200) for name in sorted(TOOLS)],
     {'clear_type_cache': "cProfile's enable and disable look its callbacks "
                          'up by names made afresh each time'}
     if hasattr(sys, 'monitoring') else {}),
]
