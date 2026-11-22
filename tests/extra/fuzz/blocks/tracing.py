from . import Block

BLOCKS = [
    Block('settrace_midcall', 'tracing',
          'a frame running compiled code when tracing starts goes on interpreted after '
          'its next call',
          'tracing switched on mid-call, by sys.settrace, and off again',
          '''
import sys
P_events = []
def P_tracer(frame, event, arg):
    if event == 'call':
        P_events.append(frame.f_code.co_name.split('_')[-1])
        del P_events[:-12]
    return None
def P_h(a):
    return a + 1
P_broken = False
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    del P_events[:]
    x = P_h(r)
    if P_broken:
        sys.settrace(P_tracer)
    try:
        y = P_h(r)
        inner = NEXT(r)
        z = P_h(r + 1)
    finally:
        if P_broken:
            sys.settrace(None)
    return x, y, z, list(P_events), inner
'''),

    Block('setprofile_midcall', 'tracing',
          'profiling switched on mid-call: C calls must show as c_call events',
          'tracing switched on mid-call, by sys.setprofile, and off again',
          '''
import sys
P_events = []
def P_prof(frame, event, arg):
    if event in ('call', 'return'):
        P_events.append(event + ':' + frame.f_code.co_name.split('_')[-1])
    elif event == 'c_call':
        P_events.append('c:' + arg.__name__)
    del P_events[:-16]
def P_h(a):
    return len(str(a))
P_broken = False
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    del P_events[:]
    if P_broken:
        sys.setprofile(P_prof)
    try:
        x = P_h(r)
        inner = NEXT(r)
        y = S_ccall
    finally:
        if P_broken:
            sys.setprofile(None)
    return x, y, list(P_events), inner
''', slots={'ccall': ['len([r, r])', 'max(r, 3)', 'abs(-r)', 'str(r)']}),
]
