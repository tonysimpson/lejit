# ceval's eval_breaker, tested by compiled code at loop heads, after calls
# and on entry from the interpreter: other threads get the
# GIL, signal handlers run and async exceptions arrive while a compiled loop
# spins. Without the test these loops never end (the harness's timeout).
import _thread, ctypes, functools, itertools, operator, os, signal, sys, \
    threading, time, zlib


class Box:
    pass


class Alarm(Exception):
    pass


class AsyncStop(Exception):
    pass


def _raise_alarm(signum, frame):
    raise Alarm()


FLAG = False


def _set_flag(box):
    global FLAG
    time.sleep(0.001)
    box.flag = True
    FLAG = True


# another thread sets an attribute while this one spins
def thread_sets_attribute():
    box = Box()
    box.flag = False
    t = threading.Thread(target=_set_flag, args=(box,))
    t.start()
    n = 0
    while not box.flag:
        n += 1
    t.join()
    return 'done'


# the same through a global
def thread_sets_global():
    global FLAG
    FLAG = False
    t = threading.Thread(target=_set_flag, args=(Box(),))
    t.start()
    n = 0
    while not FLAG:
        n += 1
    t.join()
    return 'done'


# the loop head is reached only by a conditional backward jump
def thread_conditional_back_edge():
    box = Box()
    box.flag = False
    t = threading.Thread(target=_set_flag, args=(box,))
    t.start()
    n = 0
    while True:
        if not box.flag:
            continue
        break
    t.join()
    return 'done'


# a handler that raises, for a signal sent before the loop: the exception
# is caught by the try that was running, not outside it
def signal_before_loop():
    old = signal.signal(signal.SIGUSR1, _raise_alarm)
    try:
        n = 0
        try:
            os.kill(os.getpid(), signal.SIGUSR1)
            while True:
                n += 1
        except Alarm:
            return 'caught'
    finally:
        signal.signal(signal.SIGUSR1, old)


# a timer signal arriving while the loop spins
def timer_signal_in_loop():
    old = signal.signal(signal.SIGALRM, _raise_alarm)
    try:
        n = 0
        try:
            signal.setitimer(signal.ITIMER_REAL, 0.001)
            while True:
                n += 1
        except Alarm:
            return 'caught'
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


# Ctrl-C: SIGINT with the default handler raises KeyboardInterrupt
def keyboard_interrupt():
    old = signal.signal(signal.SIGINT, signal.default_int_handler)
    try:
        n = 0
        try:
            os.kill(os.getpid(), signal.SIGINT)
            while True:
                n += 1
        except KeyboardInterrupt:
            return 'interrupted'
    finally:
        signal.signal(signal.SIGINT, old)


def keyboard_interrupt_from_timer():
    old = signal.signal(signal.SIGALRM, signal.default_int_handler)
    try:
        n = 0
        try:
            signal.setitimer(signal.ITIMER_REAL, 0.001)
            while True:
                n += 1
        except KeyboardInterrupt:
            return 'interrupted'
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


def _bump(box):
    box.n += 1
    return 1


# after a call: the handler's exception arrives before the next call's
# body runs (inlined or called), as in ceval
def signal_at_entry():
    old = signal.signal(signal.SIGUSR1, _raise_alarm)
    box = Box()
    box.n = 0
    try:
        try:
            os.kill(os.getpid(), signal.SIGUSR1)
            _bump(box)
        except Alarm:
            return ('caught', box.n)
        return ('not caught', box.n)
    finally:
        signal.signal(signal.SIGUSR1, old)


def _one(x):
    return 1


# a loop in C calling a compiled function with no loop and no call: the
# test on entry from the interpreter (LeCode_Run) is the only one
def signal_in_c_loop():
    old = signal.signal(signal.SIGALRM, signal.default_int_handler)
    try:
        try:
            signal.setitimer(signal.ITIMER_REAL, 0.001)
            sum(map(_one, itertools.repeat(0)))
        except KeyboardInterrupt:
            return 'interrupted'
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


class _InterruptingManager:
    # a C call that marks SIGINT as arrived and handles nothing itself
    __enter__ = staticmethod(functools.partial(_thread.interrupt_main))

    def __init__(self, box):
        self.box = box

    def __exit__(self, *exc):
        self.box.exited = exc[0].__name__ if exc[0] else 'no exception'


def _loop_in_with(box):
    try:
        with _InterruptingManager(box):
            # a loop, tested at its backward jump as in ceval (an inlined
            # call is tested at no entry)
            while box.n < 1000:
                box.n += 1
    except KeyboardInterrupt:
        return 'interrupted'
    return 'not interrupted'


# an interrupt during __enter__: where ceval tests no eval_breaker after
# its call, the KeyboardInterrupt is raised inside the with block, and
# __exit__ sees it; elsewhere it is raised as the call ends, before the
# block, and __exit__ isn't called. Raised at the loop's backward jump, it
# is caught where ceval's exception table catches it: where ceval tests
# after jumping, the jump itself may be in no handler's range; where it
# tests before, the exception leaves the frame from such a jump
def interrupt_in_enter():
    old = signal.signal(signal.SIGINT, signal.default_int_handler)
    box = Box()
    box.n = 0
    box.exited = 'not called'
    try:
        try:
            return (_loop_in_with(box), box.exited)
        except KeyboardInterrupt:
            return ('left the frame', box.exited)
    finally:
        signal.signal(signal.SIGINT, old)


_DATA = bytes(64 << 20)


def _crc_then_count(box):
    zlib.crc32(_DATA)
    box.after += 1
    return 0


# a signal that arrives during a long C call (zlib.crc32 of 64 MB, which
# releases the GIL and doesn't look at signals) in a function with no loop
# called from a C loop: the test after that call raises before the next
# line, as ceval's does; without it the exception would come only on the
# next entry, after box.after was counted
def signal_in_c_call_from_c_loop():
    old = signal.signal(signal.SIGALRM, _raise_alarm)
    box = Box()
    box.after = 0
    try:
        try:
            signal.setitimer(signal.ITIMER_REAL, 0.002)
            sum(map(_crc_then_count, [box] * 4))
        except Alarm:
            return ('caught', box.after)
        return ('not caught', box.after)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


def _inc(box):
    box.n += 1


# Ctrl-C in a loop whose body is only a call that is inlined: no test after
# the call (it makes no C call), the loop head's test stops it
def keyboard_interrupt_inlined_body():
    old = signal.signal(signal.SIGALRM, signal.default_int_handler)
    box = Box()
    box.n = 0
    try:
        try:
            signal.setitimer(signal.ITIMER_REAL, 0.001)
            while True:
                _inc(box)
        except KeyboardInterrupt:
            return 'interrupted'
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


_set_async_exc = ctypes.pythonapi.PyThreadState_SetAsyncExc
_set_async_exc.argtypes = [ctypes.c_ulong, ctypes.py_object]


def _fire(tid, box):
    # only once the target is inside its try: with a fixed delay the
    # exception can arrive before it on a loaded machine
    while not box.ready:
        time.sleep(0.0005)
    _set_async_exc(tid, AsyncStop)


# PyThreadState_SetAsyncExc from another thread, while this one spins
def async_exc_from_thread():
    box = Box()
    box.ready = False
    t = threading.Thread(target=_fire, args=(threading.get_ident(), box))
    t.start()
    n = 0
    try:
        box.ready = True
        while True:
            n += 1
    except AsyncStop:
        r = 'stopped'
    t.join()
    return r


# ... and on this thread itself: it arrives at the loop head
def async_exc_on_self():
    n = 0
    try:
        _set_async_exc(threading.get_ident(), AsyncStop)
        while True:
            n += 1
    except AsyncStop:
        return 'stopped'


def _count(box):
    while not box.stop:
        box.a += 1


# two threads in compiled loops, switching at a short interval: each makes
# progress only if the other gives up the GIL
def switch_interval():
    old = sys.getswitchinterval()
    sys.setswitchinterval(0.0001)
    try:
        box = Box()
        box.stop = False
        box.a = 0
        t = threading.Thread(target=_count, args=(box,))
        t.start()
        while box.a < 1000:
            pass
        box.stop = True
        t.join()
        return 'both ran'
    finally:
        sys.setswitchinterval(old)


_add_pending_call = ctypes.pythonapi.Py_AddPendingCall
_add_pending_call.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
_PENDING_CALL = ctypes.CFUNCTYPE(ctypes.c_int, ctypes.c_void_p)
_lines_seen = []


@_PENDING_CALL
def _note_lines(arg):
    f = sys._getframe(1)
    for _ in range(2):
        _lines_seen.append((f.f_code.co_name, f.f_lineno))
        f = f.f_back
    return 0


_queue_note_lines = functools.partial(
    _add_pending_call, ctypes.cast(_note_lines, ctypes.c_void_p), None)


def _entered():
    return 'entered'


# a pending call that runs on entry to a compiled function called from C,
# before its code starts, reads that frame's line and its caller's
def pending_call_reads_lines_at_entry():
    del _lines_seen[:]
    r = list(map(operator.methodcaller('__call__'),
                 [_queue_note_lines, _entered]))
    return r[1], _lines_seen[:]


def _tree(n):
    if n:
        _tree(n - 1)
        _tree(n - 1)


def _raise_from_handler(n):
    try:
        raise ValueError
    except ValueError:
        if n:
            try:
                _raise_from_handler(n - 1)
            except ValueError:
                _raise_from_handler(n - 1)
        raise


def _interrupted(f, *args, after=0.002):
    old = signal.signal(signal.SIGALRM, signal.default_int_handler)
    try:
        try:
            signal.setitimer(signal.ITIMER_REAL, after)
            f(*args)
        except KeyboardInterrupt:
            return 'interrupted'
        return 'not interrupted'
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


# a signal handler runs on entry to a function called directly from
# compiled code, as ceval runs it on entering a frame: here nothing else
# tests, since no call returns to a loop
def signal_in_direct_recursion():
    return _interrupted(_tree, 60)


# the same where every call raises, from a handler. The signal lands at the
# entry check of each of the two calls only now and then, as rarely as one
# round in a thousand, and the first time it does compiles that check's
# branch: the case warms up for thousands of rounds, so its timer is short
def signal_in_recursion_from_handler():
    return _interrupted(_raise_from_handler, 60, after=0.0002)


# a signal handler is given the frame the signal landed in, the compiled
# loop's own, linked to its callers, as CPython gives it
def _spin_until_seen(box):
    while box.seen is None:
        box.n += 1
    return box.n > 0


def _note_frame(box):
    def handler(signum, frame):
        box.seen = [frame.f_code.co_name, frame.f_back.f_code.co_name]
    return handler


def handler_frame():
    box = Box()
    box.seen = None
    box.n = 0
    old = signal.signal(signal.SIGALRM, _note_frame(box))
    try:
        signal.setitimer(signal.ITIMER_REAL, 0.001)
        spun = _spin_until_seen(box)
        return box.seen, spun
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)


CASES = [
    ('thread_sets_attribute', [()], {}),
    ('thread_sets_global', [()], {}),
    ('thread_conditional_back_edge', [()], {}),
    ('signal_before_loop', [()], {}),
    ('timer_signal_in_loop', [()], {}),
    ('keyboard_interrupt', [()], {}),
    ('keyboard_interrupt_from_timer', [()], {}),
    ('signal_at_entry', [()], {}),
    ('interrupt_in_enter', [()], {}),
    ('signal_in_c_loop', [()], {}),
    ('signal_in_c_call_from_c_loop', [()], {}),
    ('keyboard_interrupt_inlined_body', [()], {}),
    ('async_exc_from_thread', [()], {}),
    ('async_exc_on_self', [()], {}),
    ('switch_interval', [()], {}),
    ('handler_frame', [()], {}),
    ('pending_call_reads_lines_at_entry', [()], {}),
    ('signal_in_direct_recursion', [()], {}),
    ('signal_in_recursion_from_handler', [()],
     {'warmup': (5000, 'a call\'s entry check compiles its branch the first '
                       'time the signal lands there')}),
]
