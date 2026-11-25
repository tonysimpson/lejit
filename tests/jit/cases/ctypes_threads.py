# The GIL around direct calls of ctypes functions (Python/pyruntime.c:
# le_gil_release, le_gil_take): released for the call and taken back as
# CPython takes it, so other threads run while one is in C, threads calling
# C switch, signals and asynchronous exceptions arrive after a call, and
# daemon threads in C when the interpreter shuts down end as in CPython.
import ctypes
import signal
import sys
import threading
import time
from ctypes import c_long, c_uint

libc = ctypes.CDLL(None)
labs = libc.labs
labs.argtypes = (c_long,)
labs.restype = c_long
usleep = libc.usleep
usleep.argtypes = (c_uint,)


def _adder(n, out, k):
    t = 0
    for i in range(n):
        t += labs(-i) + k
    out[k] = t


def threads_calling_c():
    out = {}
    ts = [threading.Thread(target=_adder, args=(20000, out, k)) for k in (1, 2, 3)]
    for t in ts:
        t.start()
    for t in ts:
        t.join(60)
    return sorted(out.items()), [t.is_alive() for t in ts]


def _counter(stop, out):
    n = 0
    while not stop:
        n += 1
    out['count'] = n


def _sleeper(out):
    for _ in range(4):
        usleep(2000)
    out['slept'] = True


def python_runs_while_c_sleeps():
    stop = []
    res = {}
    c = threading.Thread(target=_counter, args=(stop, res))
    s = threading.Thread(target=_sleeper, args=(res,))
    c.start()
    s.start()
    s.join(60)
    stop.append(1)
    c.join(60)
    return res.get('slept'), res.get('count', 0) > 100


def main_sleeps_in_c():
    out = {}
    t = threading.Thread(target=_adder, args=(20000, out, 7))
    t.start()
    t0 = time.time()
    while t.is_alive() and time.time() - t0 < 60:
        usleep(1000)
    t.join()
    return sorted(out.items())


class Alarm(Exception):
    pass


def _on_alarm(sig, frame):
    raise Alarm()


def signal_during_calls():
    old = signal.signal(signal.SIGALRM, _on_alarm)
    signal.setitimer(signal.ITIMER_REAL, 0.02)
    i = 0
    try:
        while True:
            labs(-i)
            i += 1
    except Alarm:
        return 'alarm after calls: %s' % (i > 0)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


class Stop(Exception):
    pass


def _until_stopped(box):
    box.append('started')
    try:
        while True:
            labs(-3)
    except Stop:
        box.append('stopped')


def async_exception_after_call():
    box = []
    t = threading.Thread(target=_until_stopped, args=(box,))
    t.start()
    while not box:
        time.sleep(0.001)
    n = ctypes.pythonapi.PyThreadState_SetAsyncExc(ctypes.c_ulong(t.ident),
                                                   ctypes.py_object(Stop))
    t.join(60)
    return n, box, t.is_alive()


def _mixed(n, out, k):
    t = 0
    for i in range(n):
        t += labs(-i) * k
        if i % 7 == 0:
            usleep(0)
        t += sum(range(i % 5))
    out[k] = t


def stress_tiny_switch_interval():
    old = sys.getswitchinterval()
    sys.setswitchinterval(1e-5)
    try:
        out = {}
        ts = [threading.Thread(target=_mixed, args=(5000, out, k))
              for k in range(6)]
        for t in ts:
            t.start()
        for t in ts:
            t.join(120)
        return sorted(out.items()), [t.is_alive() for t in ts]
    finally:
        sys.setswitchinterval(old)


CHILD = r'''
import ctypes, sys, threading
from ctypes import c_long, c_uint
if sys.argv[1] == '1':
    import lejit
    lejit.start()
libc = ctypes.CDLL(None)
labs = libc.labs
labs.argtypes = (c_long,)
labs.restype = c_long
usleep = libc.usleep
usleep.argtypes = (c_uint,)


def calls():
    while True:
        labs(-1)


def sleeps(us):
    while True:
        usleep(us)


for target, args in ((calls, ()), (calls, ()), (sleeps, (0,)),
                     (sleeps, (200,)), (sleeps, (20000,))):
    threading.Thread(target=target, args=args, daemon=True).start()
usleep(20000)
print('ok', flush=True)
'''

_result = []


def daemon_threads_at_shutdown():
    if not _result:
        import os
        import subprocess
        env = dict(os.environ, PYTHONPATH=os.pathsep.join(sys.path))
        for _ in range(5):
            p = subprocess.run([sys.executable, '-c', CHILD,
                                '1' if 'lejit' in sys.modules else '0'],
                               env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, timeout=60)
            _result.append((p.returncode, p.stdout.decode().split(),
                            p.stderr.decode()))
    return _result


CASES = [
    ('threads_calling_c', [()], {}),
    ('python_runs_while_c_sleeps', [()], {}),
    ('main_sleeps_in_c', [()], {}),
    ('signal_during_calls', [()], {}),
    ('async_exception_after_call', [()], {}),
    ('stress_tiny_switch_interval', [()],
     {'leaks': 'at this switch interval the eval_breaker paths after the '
               'calls are first taken, and compiled, at rounds that depend on '
               'the threads\' timing'}),
    ('daemon_threads_at_shutdown', [()], {}),
]
