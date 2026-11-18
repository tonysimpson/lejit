"""Lazy log-processing pipelines of generators abandoned early by islice, zip and break, relying on close() and finally blocks running at once and in order, with throw() resynchronisation, send() coroutines, return values through yield from, and a generator that ignores GeneratorExit: generator frames that a compiler keeps virtual or finalises late (PyPy, Differences between PyPy and CPython; LuaJIT NYI list: coroutines)."""
import itertools
import zlib

N = 370


class Resync(Exception):
    pass


class Tracked:
    def __init__(self, log, name):
        self.log, self.name = log, name

    def __enter__(self):
        self.log.append('open ' + self.name)
        return self

    def __exit__(self, et, ev, tb):
        self.log.append('exit %s %s' % (self.name, et.__name__ if et else 'ok'))
        return False


def source(log, name, lines):
    with Tracked(log, name):
        try:
            for ln in lines:
                yield ln
        finally:
            log.append('source %s finally' % name)


def parse(log, it):
    count = 0
    try:
        for ln in it:
            count += 1
            parts = ln.split(' ', 2)
            if len(parts) < 3:
                continue
            yield parts[0], parts[1], parts[2]
    except GeneratorExit:
        log.append('parse closed after %d' % count)
        raise
    finally:
        log.append('parse finally %d' % count)


def errors_only(log, it):
    try:
        for lvl, mod, msg in it:
            if lvl in ('ERROR', 'WARN'):
                yield '%s:%s' % (mod, msg)
    finally:
        log.append('errors_only finally')


def resync_parser(log):
    state = 'header'
    got = []
    while True:
        try:
            tok = yield state
            if tok is None:
                break
            if state == 'header' and tok.startswith('#'):
                state = 'body'
            elif state == 'body':
                if tok == 'END':
                    state = 'header'
                got.append(tok)
        except Resync as e:
            log.append('resync at %s (%s)' % (state, e))
            state = 'header'
    return got


def averager():
    total = 0
    count = 0
    avg = None
    while True:
        v = yield avg
        if v is None:
            return total, count
        total += v
        count += 1
        avg = total / count


def collect(sub):
    result = yield from sub
    return ('collected', result)


def stubborn(log):
    try:
        yield 1
        yield 2
    finally:
        log.append('stubborn finally')
        yield 'refused'


def make_lines(r, variant):
    levels = ['INFO', 'DEBUG', 'ERROR', 'WARN', 'INFO']
    out = []
    for k in range(600 + r):
        lvl = levels[(k * 7 + r) % 5]
        if variant == 1 and k % 11 == 3:
            out.append('garbage%d' % k)
        else:
            out.append('%s mod%d message %d' % (lvl, k % 6, k * r))
    return out if variant == 0 else tuple(out)


def run(r, variant):
    log = []
    lines = make_lines(r, variant)
    pipeline = errors_only(log, parse(log, source(log, 'A', lines)))
    first = list(itertools.islice(pipeline, 3 + r % 4))
    log.append('took %d' % len(first))
    del pipeline
    log.append('after drop')
    a = parse(log, source(log, 'B', lines))
    b = source(log, 'C', lines[:5 + r % 3])
    pairs = [(x[0], y[:4]) for x, y in zip(a, b)]
    log.append('zip done %d' % len(pairs))
    leftover = next(a, None)
    a = b = None
    log.append('zip dropped')
    for item in parse(log, source(log, 'D', lines)):
        if item[0] == 'ERROR':
            log.append('break on ' + item[2])
            break
    log.append('after break')
    full = 0
    for pass_no in range(4):
        full += sum(len(x) for x in errors_only(log, parse(log, source(log, 'E%d' % pass_no, lines))))
    log.append('full %d' % full)
    rp = resync_parser(log)
    states = [next(rp)]
    for k, tok in enumerate(['#h', 'x%d' % r, 'y', 'END', 'z', '#h2', 'q', 'w']):
        states.append(rp.send(tok))
        if k == 2 + variant * 3:
            states.append(rp.throw(Resync('k=%d' % k)))
    try:
        rp.send(None)
    except StopIteration as e:
        parsed = e.value
    avg = averager()
    next(avg)
    for v in range(1, 6 + r % 5):
        avg.send(v * (1.5 if variant else 1))
    try:
        avg.send(None)
    except StopIteration as e:
        averaged = e.value
    c = collect(averager())
    next(c)
    running = [c.send(v * r + (0.25 if variant else 0)) for v in range(4)]
    try:
        c.send(None)
    except StopIteration as e:
        collected = e.value
    st = stubborn(log)
    next(st)
    try:
        st.close()
    except RuntimeError as e:
        log.append('close: %s' % e)
    finished = st.gi_frame is None
    return first, pairs, leftover, states, parsed, averaged, running, collected, finished, log


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    return digest([run(r, variant) for r in range(n)], 2)
