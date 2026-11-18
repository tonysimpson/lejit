"""Debugger that attaches to a running loop like pdb.set_trace: reads callers' f_locals and f_lineno, sets f_trace on a live frame, records line events and writes variables back through f_locals, and reads locals of finished frames from tracebacks: optimised frames that must be materialised on demand (Hoelzle, Chambers and Ungar, Debugging Optimized Code with Dynamic Deoptimization, PLDI 1992)."""
import sys
import zlib

N = 3000

WATCH = ('i', 'total', 'best', 'item', 'scale')


def snapshot(frame):
    loc = frame.f_locals
    return tuple((k, loc[k]) for k in WATCH if k in loc)


class Debugger:
    def __init__(self, target, stop_after, patch_at, patch):
        self.target = target
        self.events = []
        self.stop_after = stop_after
        self.patch_at = patch_at
        self.patch = patch
        self.frame = None

    def attach(self):
        frame = sys._getframe(1)
        self.frame = frame
        self.events.append(('attach', frame.f_code.co_name,
                            frame.f_lineno - frame.f_code.co_firstlineno, snapshot(frame)))
        frame.f_trace = self.local_trace
        sys.settrace(self.global_trace)

    def global_trace(self, frame, event, arg):
        if frame.f_code.co_name == self.target:
            return self.local_trace
        return None

    def local_trace(self, frame, event, arg):
        if frame is not self.frame:
            return self.local_trace
        rel = frame.f_lineno - frame.f_code.co_firstlineno
        self.events.append((event, rel, snapshot(frame)))
        if len(self.events) == self.patch_at:
            name, value = self.patch
            frame.f_locals[name] = value
            self.events.append(('patched', name, value))
        if len(self.events) >= self.stop_after:
            sys.settrace(None)
            frame.f_trace = None
            return None
        return self.local_trace


def peek_caller():
    f = sys._getframe(1)
    chain = []
    g = f
    while g is not None and len(chain) < 3:
        chain.append(g.f_code.co_name)
        g = g.f_back
    return f.f_lineno - f.f_code.co_firstlineno, snapshot(f), tuple(chain)


def score(item, scale):
    return (item * 37 + scale) % 101


def scan(items, scale, dbg, attach_at):
    total = 0
    best = None
    peeks = []
    for i, item in enumerate(items):
        s = score(item, scale)
        total += s
        if best is None or s > best:
            best = s
        if i % 50 == 7:
            peeks.append(peek_caller())
        if i == attach_at:
            dbg.attach()
        scale = scale + 1
    return total, best, peeks


def stale_locals(k):
    snap = locals()
    x = k * 2
    before = 'x' in snap
    again = locals()
    after = snap.get('x')
    x = x + 1
    return before, after, again is snap, snap['x'], locals()['x']


def failing(depth, payload):
    secret = payload * 3
    if depth == 0:
        raise KeyError(secret)
    return failing(depth - 1, payload + 1)


def post_mortem(k):
    try:
        failing(3 + k % 4, k)
    except KeyError as e:
        tb = e.__traceback__
        found = []
        while tb is not None:
            loc = tb.tb_frame.f_locals
            if 'secret' in loc:
                found.append((loc['depth'], loc['secret']))
            tb = tb.tb_next
        return found


def generator_locals(k):
    def gen(m):
        acc = 0
        for j in range(m):
            acc += j
            yield j
    g = gen(k)
    next(g)
    next(g)
    first = g.gi_frame.f_locals.get('acc')
    for _ in g:
        pass
    return first, g.gi_frame is None


def session(r, variant):
    items = [(r * 13 + j * 7) % 1000 for j in range(300 + r % 40)]
    if variant == 0:
        dbg = Debugger('scan', 14, 6, ('total', 0))
        result = scan(items, 3, dbg, 120 + r % 30)
    else:
        items = [x / 4 for x in items]
        dbg = Debugger('scan', 22, 9, ('scale', 10 ** 20))
        result = scan(items, 3, dbg, 40 + r % 70)
    sys.settrace(None)
    return (result, dbg.events, stale_locals(r), post_mortem(r), generator_locals(5 + r % 3))


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    old = sys.gettrace()
    try:
        return digest([session(r, variant) for r in range(n)], 2)
    finally:
        sys.settrace(old)
