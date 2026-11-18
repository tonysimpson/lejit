"""Event loop of generator and async-def tasks with virtual-time timers, futures, queues, cancellation by throw() and finally blocks that yield: suspended frames that escape into the scheduler (LuaJIT NYI list: coroutines; PyPy, Differences between PyPy and CPython: generators)."""
import heapq
from collections import deque

N = 600


class Cancelled(Exception):
    pass


class Timeout(Exception):
    pass


class Future:
    __slots__ = ('done', 'result', 'error', 'callbacks')

    def __init__(self):
        self.done = False
        self.result = None
        self.error = None
        self.callbacks = []

    def set(self, value=None, error=None):
        if self.done:
            return
        self.done, self.result, self.error = True, value, error
        cbs, self.callbacks = self.callbacks, []
        for cb in cbs:
            cb(self)

    def __await__(self):
        return (yield self)

    def __iter__(self):
        return (yield self)


class Sleep:
    __slots__ = ('delay',)

    def __init__(self, delay):
        self.delay = delay

    def __await__(self):
        return (yield self)


class Task(Future):
    __slots__ = ('coro', 'name', 'loop', 'pending_cancel', 'waiting')

    def __init__(self, loop, coro, name):
        super().__init__()
        self.coro, self.name, self.loop = coro, name, loop
        self.pending_cancel = False
        self.waiting = None

    def cancel(self):
        if not self.done:
            self.pending_cancel = True
            self.loop.ready.append((self, None, None))

    def step(self, value, error):
        if self.done:
            return
        if self.pending_cancel:
            self.pending_cancel = False
            error = Cancelled(self.name)
        try:
            if error is not None:
                req = self.coro.throw(error)
            else:
                req = self.coro.send(value)
        except StopIteration as e:
            self.set(e.value)
            return
        except Exception as e:
            self.loop.log.append('%s died %s(%s)' % (self.name, type(e).__name__, e))
            self.set(None, e)
            return
        if isinstance(req, Sleep):
            self.loop.call_later(req.delay, self, None)
        elif isinstance(req, Future):
            if req.done:
                self.loop.ready.append((self, req.result, req.error))
            else:
                req.callbacks.append(lambda f: self.loop.ready.append((self, f.result, f.error)))
        elif req is None:
            self.loop.ready.append((self, None, None))
        else:
            self.loop.ready.append((self, None, TypeError('bad yield %r' % type(req).__name__)))


class Loop:
    def __init__(self):
        self.now = 0
        self.seq = 0
        self.timers = []
        self.ready = deque()
        self.log = []

    def call_later(self, delay, task, value):
        self.seq += 1
        heapq.heappush(self.timers, (self.now + delay, self.seq, task, value))

    def spawn(self, coro, name):
        t = Task(self, coro, name)
        self.ready.append((t, None, None))
        return t

    def run(self):
        steps = 0
        while self.ready or self.timers:
            if not self.ready:
                when, _, task, value = heapq.heappop(self.timers)
                self.now = when
                self.ready.append((task, value, None))
            task, value, error = self.ready.popleft()
            task.step(value, error)
            steps += 1
        return steps


class Queue:
    def __init__(self, cap):
        self.items = deque()
        self.getters = deque()
        self.putters = deque()
        self.cap = cap

    def put(self, item):
        while len(self.items) >= self.cap:
            f = Future()
            self.putters.append(f)
            yield from f
        self.items.append(item)
        if self.getters:
            self.getters.popleft().set()

    def get(self):
        while not self.items:
            f = Future()
            self.getters.append(f)
            yield from f
        item = self.items.popleft()
        if self.putters:
            self.putters.popleft().set()
        return item


def wait_for(loop, task, timeout):
    timer = Future()

    def fire():
        try:
            yield Sleep(timeout)
        except Cancelled:
            return 'stopped'
        timer.set(None, Timeout(task.name))
    watchdog = loop.spawn(fire(), 'watchdog-' + task.name)
    task.callbacks.append(lambda f: timer.set(f.result, f.error))
    try:
        return (yield from timer)
    except Timeout:
        task.cancel()
        raise
    finally:
        watchdog.cancel()


def producer(loop, q, name, count, gap):
    try:
        for i in range(count):
            yield Sleep(gap + i % 3)
            yield from q.put((name, i))
        loop.log.append('%s finished at %d' % (name, loop.now))
    finally:
        loop.log.append('%s cleanup at %d' % (name, loop.now))


def handle(loop, item, cost):
    name, i = item
    total = 0
    for k in range(cost):
        total += (i * 31 + k) % 17
        if k % 8 == 7:
            yield Sleep(1)
    return total


def worker(loop, q, wid, results, cost):
    handled = 0
    try:
        while True:
            item = yield from q.get()
            sub = loop.spawn(handle(loop, item, cost + item[1] % 5 * 6), 'h%d-%s' % (wid, item))
            try:
                r = yield from wait_for(loop, sub, 3 + (item[1] + wid) % 5)
                results.append((item, r))
            except Timeout:
                loop.log.append('w%d timeout %s at %d' % (wid, item, loop.now))
            handled += 1
    except Cancelled:
        loop.log.append('w%d cancelled after %d' % (wid, handled))
        try:
            yield Sleep(2)
        finally:
            loop.log.append('w%d drained at %d' % (wid, loop.now))
        return handled


async def async_worker(loop, q, wid, results, cost):
    handled = 0
    try:
        while True:
            item = await q_get_future(loop, q)
            await Sleep(item[1] % 4)
            r = 0
            for k in range(cost):
                r += (item[1] ^ k) & 15
            results.append((item, r))
            handled += 1
    except Cancelled:
        loop.log.append('aw%d cancelled after %d' % (wid, handled))
        return handled


class Wrapper:
    def __init__(self, gen):
        self.gen = gen

    def __await__(self):
        return (yield from self.gen)


def q_get_future(loop, q):
    f = Future()

    def getter():
        item = yield from q.get()
        f.set(item)
    loop.spawn(getter(), 'getter')
    return f


def supervisor(loop, workers, producers):
    while not all(p.done for p in producers):
        yield Sleep(5)
    yield Sleep(40)
    for w in workers:
        w.cancel()
    counts = []
    for w in workers:
        counts.append((yield from w))
    loop.log.append('supervisor counts %s' % counts)
    return counts


def run_once(i, variant):
    loop = Loop()
    q = Queue(3 + i % 3)
    results = []
    producers = [loop.spawn(producer(loop, q, 'p%d' % p, 12 + p * 3 + i % 4, 1 + (p + i) % 3), 'p%d' % p)
                 for p in range(3)]
    workers = [loop.spawn(worker(loop, q, w, results, 10 + 4 * w), 'w%d' % w) for w in range(3)]
    if variant == 1:
        workers.append(loop.spawn(async_worker(loop, q, 9, results, 12), 'aw9'))
        workers.append(loop.spawn(Wrapper(worker(loop, q, 7, results, 30)).__await__(), 'w7'))
    sup = loop.spawn(supervisor(loop, workers, producers), 'sup')
    steps = loop.run()
    return sup.result, sorted(results), loop.log, loop.now, steps


def main(n, variant=0):
    runs = [run_once(i, variant) for i in range(n)]
    summary = [(r[0], len(r[1]), sum(x[1] for x in r[1]), len(r[2]), r[3], r[4]) for r in runs]
    return summary, runs[-1]
