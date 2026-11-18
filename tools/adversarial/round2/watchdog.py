"""Worker threads under a tiny switch interval that spin until another thread or a signal handler sets a flag, a SIGALRM handler that raises into a hot loop, and lock-protected shared totals: compiled loops that never poll for thread switches or pending signals (HotSpot C2 eliding safepoint polls in counted loops, addressed by loop strip mining in JDK 10; CPython's eval breaker)."""
import queue
import signal
import sys
import threading

N = 8


class Alarm(Exception):
    pass


def crunch(seed, count):
    acc = seed
    for k in range(count):
        acc = (acc * 1103515245 + 12345 + k) & 0x7fffffff
    return acc


def spin_until(flag):
    spins = 0
    while not flag[0]:
        spins += 1
    return spins > 0


def threaded_round(r, variant):
    shared = {'total': 0, 'parts': []}
    lock = threading.Lock()
    local = threading.local()
    go = [False]
    stop = [False]
    q = queue.Queue(maxsize=4)
    results = queue.Queue()

    def worker(wid):
        local.name = 'w%d' % wid
        spin_until(go)
        for chunk in range(6):
            v = crunch(wid * 1000 + chunk + r, 2000 + 500 * wid)
            with lock:
                shared['total'] += v % 1000
                shared['parts'].append((wid, chunk, v % 97))
        results.put((local.name, wid))

    def consumer():
        got = []
        while True:
            item = q.get()
            if item is None:
                break
            got.append(crunch(item, 300) % 1000)
        results.put(('consumer', sum(got), len(got)))

    def spinner():
        n = 0
        while not stop[0]:
            n = (n + 1) & 0xffff
        results.put(('spinner', 'stopped'))

    threads = [threading.Thread(target=worker, args=(w,)) for w in range(3 + variant)]
    threads.append(threading.Thread(target=consumer))
    threads.append(threading.Thread(target=spinner))
    for t in threads:
        t.start()
    go[0] = True
    for item in range(20 + r):
        q.put(item * (7 if variant else 3))
    q.put(None)
    for t in threads[:-1]:
        t.join()
    stop[0] = True
    threads[-1].join()
    got = []
    while not results.empty():
        got.append(results.get())
    return shared['total'], sorted(shared['parts']), sorted(got, key=repr)


def signal_round(r, variant):
    fired = [False]
    log = []

    def on_alarm(signum, frame):
        fired[0] = True
        if variant == 1 and len(log) % 2 == 0:
            log.append('handler raising')
            raise Alarm()
        log.append('handler flag')

    old = signal.signal(signal.SIGALRM, on_alarm)
    try:
        for attempt in range(2):
            fired[0] = False
            signal.setitimer(signal.ITIMER_REAL, 0.002)
            try:
                acc = 0
                try:
                    while not fired[0]:
                        acc = (acc + 3) & 0xffff
                finally:
                    log.append('loop left %d' % attempt)
                log.append('flag seen %d' % attempt)
            except Alarm:
                log.append('alarm caught %d' % attempt)
    finally:
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, old)
    return log


def main(n, variant=0):
    old = sys.getswitchinterval()
    sys.setswitchinterval(0.00005)
    try:
        out = []
        for r in range(n):
            out.append(threaded_round(r, variant))
            out.append(signal_round(r, variant))
        return out
    finally:
        sys.setswitchinterval(old)
