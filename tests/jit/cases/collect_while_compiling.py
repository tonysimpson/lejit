# A collection that starts during a compile, from an allocation the
# compiler makes, runs a finaliser that lets another thread take the GIL;
# that thread then compiles and runs code while the first compile is still
# open.
import gc
import threading
import time
from itertools import repeat


class Sleeps:
    # its finaliser lets the other thread run until it has compiled and run
    # one more function, or ended
    def __del__(self):
        n = len(self.out)
        while (len(self.out) == n and self.worker.is_alive() and
               threading.current_thread() is not self.worker):
            time.sleep(0.0001)


def _collects_inside_c_calls():
    # whether a collection can start at an allocation inside a C call, as
    # in a compile, rather than only between instructions
    seen = []
    def note(phase, info):
        seen.append(phase)
    old = gc.get_threshold()
    gc.callbacks.append(note)
    gc.set_threshold(1)
    try:
        made = list(zip(map(list, repeat((), 1000)), map(len, repeat(seen, 1000))))
    finally:
        gc.set_threshold(*old)
        gc.callbacks.remove(note)
    return made[-1][1] > made[0][1]


SOURCE = '''
def f%d(n):
    t = 0
    for i in range(n):
        t += i * %d
    return t
'''


def _fresh(k):
    ns = {}
    exec(SOURCE % (k, k), ns)
    return ns['f%d' % k]


def _compiles(stop, out):
    k = 1000
    while True:
        out.append(_fresh(k)(10))
        k += 1
        if stop:
            break


def finaliser_lets_a_thread_compile(n):
    stop, out = [], []
    worker = threading.Thread(target=_compiles, args=(stop, out))
    old = gc.get_threshold()
    worker.start()
    try:
        res = []
        for k in range(n):
            f = _fresh(k)
            gc.collect()
            s = Sleeps()
            s.out, s.worker = out, worker
            s.cycle = s
            del s
            # the next allocations start a collection, which finds the
            # cycle: the first call of f compiles it
            gc.set_threshold(1)
            res.append(f(10))
            gc.set_threshold(*old)
    finally:
        gc.set_threshold(*old)
        stop.append(True)
        worker.join(60)
    return res, out[:1], worker.is_alive()


CASES = [
    ('finaliser_lets_a_thread_compile', [(5,)],
     {'compiles_each_call': 'it makes new functions on every call',
      'objects': 'the other thread\'s allocations depend on timing',
      **({'xfail': "compile state is global: the other thread's compile stops "
                   "the process in codemanager.c:le_compile_thread_check"}
         if _collects_inside_c_calls() else {})}),
]
