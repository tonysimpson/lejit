# The GIL around direct calls of cffi functions (Modules/pcffi.c): released
# for the call and taken back, so other threads run while one is in C,
# threads calling C switch, callbacks into Python from several threads take
# the GIL themselves, and signals and asynchronous exceptions arrive after
# a call, as with cffi's own call.
import ctypes
import signal
import sys
import threading
import time

from cffi_lib import abi, api, api_ffi


def _adder(lib, n, out, k):
    t = 0
    for i in range(n):
        t += lib.add_int(i, k)
    out[k] = t


def threads_calling_c():
    out = {}
    ts = [threading.Thread(target=_adder, args=(lib, 20000, out, k))
          for k, lib in ((1, api), (2, abi), (3, api))]
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
        api.sleep_ms(2)
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
    t = threading.Thread(target=_adder, args=(api, 20000, out, 7))
    t.start()
    t0 = time.time()
    while t.is_alive() and time.time() - t0 < 60:
        abi.sleep_ms(1)
    t.join()
    return sorted(out.items())


def _plus_one(x):
    return x + 1


_plus_one_c = None
if api_ffi is not None:
    _plus_one_c = api_ffi.callback('int(int)')(_plus_one)


def _calls_back(n, out, k):
    cb = api_ffi.cast('void *', _plus_one_c)
    t = 0
    for i in range(n):
        t += api.call_cb(cb, i)
    out[k] = t


def callbacks_from_threads():
    out = {}
    ts = [threading.Thread(target=_calls_back, args=(3000, out, k))
          for k in range(4)]
    for t in ts:
        t.start()
    _calls_back(3000, out, 'main')
    for t in ts:
        t.join(60)
    return sorted(out.items(), key=repr), [t.is_alive() for t in ts]


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
            api.add_int(i, 1)
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
            abi.add_int(3, 4)
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
        t += api.add_int(i, k) * k
        if i % 7 == 0:
            abi.sleep_ms(0)
        t += sum(range(i % 5))
    out[k] = t


def stress_tiny_switch_interval():
    old = sys.getswitchinterval()
    sys.setswitchinterval(1e-5)
    try:
        out = {}
        ts = [threading.Thread(target=_mixed, args=(500, out, k))
              for k in range(6)]
        for t in ts:
            t.start()
        for t in ts:
            t.join(120)
        return sorted(out.items()), [t.is_alive() for t in ts]
    finally:
        sys.setswitchinterval(old)


TIMED = {'objects': 'runs until a timer or another thread stops it'}

CASES = [] if api is None else [
    ('threads_calling_c', [()], {}),
    ('python_runs_while_c_sleeps', [()], TIMED),
    ('main_sleeps_in_c', [()], {}),
    ('callbacks_from_threads', [()], {}),
    ('signal_during_calls', [()], TIMED),
    ('async_exception_after_call', [()], TIMED),
    ('stress_tiny_switch_interval', [()],
     dict(TIMED, leaks='at this switch interval the eval_breaker paths after '
                       'the calls are first taken, and compiled, at rounds that '
                       'depend on the threads\' timing')),
]
