# A collection that would start during a compile, from an allocation the
# compiler makes, waits until the compile ends (codemanager.h:le_gc_off):
# a finaliser it ran could let another thread take the GIL, and that
# thread compile and run code while the first compile is still open.
import gc
import threading
import time


class Sleeps:
    # its finaliser lets another thread take the GIL
    def __del__(self):
        time.sleep(0)


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
    ('finaliser_lets_a_thread_compile', [(20,)],
     {'compiles_each_call': 'it makes new functions on every call',
      'objects': 'the other thread\'s allocations depend on timing'}),
]
