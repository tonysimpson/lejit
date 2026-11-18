"""Context managers on the hot path: with statements, contextlib.contextmanager generators, ExitStack, suppress and locks. Variant 1 has other accounts, float balances and transfers."""
import contextlib
import threading


class Transaction:
    def __init__(self, db):
        self.db = db

    def __enter__(self):
        self.snapshot = dict(self.db)
        return self.db

    def __exit__(self, kind, value, tb):
        if kind is not None:
            self.db.clear()
            self.db.update(self.snapshot)
            return kind is KeyError
        return False


@contextlib.contextmanager
def tagged(log, tag):
    log.append('+' + tag)
    try:
        yield len(log)
    finally:
        log.append('-' + tag)


def transfer(db, src, dst, amount):
    with Transaction(db) as d:
        d[src] -= amount
        if d[src] < 0:
            raise KeyError(src)
        d[dst] += amount
        return True


def main(n, variant=0):
    if variant == 0:
        db = {'a%d' % k: 100 for k in range(10)}
        names, count, step, unit = 'a%d', 10, 3, 1
    else:
        db = {'acct-%d' % k: 60.5 for k in range(13)}
        names, count, step, unit = 'acct-%d', 13, 5, 0.75
    lock = threading.Lock()
    log = []
    ok = 0
    for i in range(n):
        with lock:
            if transfer(db, names % (i % count), names % ((i * step + 1) % count), i % 37 * unit):
                ok += 1
        with tagged(log, 'req') as depth:
            with contextlib.suppress(ZeroDivisionError):
                ok += 10 // (i % 4)
            ok += depth % 3
        with contextlib.ExitStack() as stack:
            for k in range(i % 3):
                stack.enter_context(tagged(log, 'r%d' % k))
        if len(log) > 1000:
            del log[:500]
    return ok, sorted(db.items()), len(log), log[-3:]


N = 20000

if __name__ == '__main__':
    print(main(N))
