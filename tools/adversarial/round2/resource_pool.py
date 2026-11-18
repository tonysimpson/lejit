"""Connection pool whose log depends on CPython destroying objects the moment their last reference goes: __del__ on leaked leases, weakref callbacks, weakref.finalize, WeakValueDictionary caches, and exception tracebacks keeping frames alive until the except clause ends: allocation removal or delayed reference counting changing finaliser timing (PyPy, Differences between PyPy and CPython: Garbage Collection; Bolz et al., Allocation Removal by Partial Evaluation in a Tracing JIT, PEPM 2011)."""
import gc
import weakref
import zlib

N = 650


class Connection:
    def __init__(self, pool, cid):
        self.pool = pool
        self.cid = cid
        self.queries = 0

    def execute(self, sql, arg):
        self.queries += 1
        return Cursor(self, '%s[%s]' % (sql, arg))


class Cursor:
    def __init__(self, conn, sql):
        self.conn = conn
        self.rows = [(len(sql) * k) % 13 for k in range(4)]
        weakref.finalize(self, conn.pool.log.append, 'cursor closed on c%d' % conn.cid)

    def fetch(self):
        return self.rows


class Lease:
    def __init__(self, pool, conn):
        self.pool = pool
        self.conn = conn
        self.released = False

    def __enter__(self):
        return self.conn

    def __exit__(self, et, ev, tb):
        self.release('exit' if et is None else 'exit after ' + et.__name__)
        return False

    def release(self, how):
        if not self.released:
            self.released = True
            self.pool.give_back(self.conn, how)

    def __del__(self):
        if not self.released:
            self.pool.log.append('leaked lease of c%d reclaimed' % self.conn.cid)
            self.release('gc')


class Statement:
    def __init__(self, sql):
        self.sql = sql


class Pool:
    def __init__(self, size):
        self.log = []
        self.free = [Connection(self, k) for k in range(size)]
        self.cache = weakref.WeakValueDictionary()
        self.watch = []
        self.pinned = []

    def acquire(self):
        if not self.free:
            raise RuntimeError('pool exhausted')
        conn = self.free.pop()
        self.log.append('acquire c%d' % conn.cid)
        return Lease(self, conn)

    def give_back(self, conn, how):
        self.log.append('release c%d by %s' % (conn.cid, how))
        self.free.append(conn)

    def prepare(self, sql):
        st = self.cache.get(sql)
        if st is None:
            st = Statement(sql)
            self.cache[sql] = st
            self.watch.append(weakref.ref(st, lambda r, s=sql: self.log.append('evict ' + s)))
            self.log.append('prepare ' + sql)
        return st


class QueryFailed(Exception):
    pass


def work(pool, k, variant):
    lease = pool.acquire()
    conn = lease.conn
    st = pool.prepare('select %d' % (k % 4))
    rows = conn.execute(st.sql, k).fetch()
    if k % 5 == 0:
        pool.pinned.append(st)
    if k % 3 == 0:
        lease.release('explicit')
    elif k % 3 == 1:
        pool.log.append('dropping lease of c%d' % conn.cid)
    else:
        keep = lease
        pool.log.append('kept lease of c%d' % keep.conn.cid)
        del keep
    pool.log.append('work %d done' % k)
    return sum(rows)


def failing(pool, k):
    lease = pool.acquire()
    st = pool.prepare('update %d' % (k % 2))
    lease.conn.execute(st.sql, k)
    raise QueryFailed('k=%d' % k)


def session(r, variant):
    pool = Pool(3)
    total = 0
    old = gc.isenabled()
    gc.disable()
    try:
        for k in range(12 + r % 5):
            total += work(pool, k + r, variant)
            if variant == 1:
                try:
                    failing(pool, k)
                except QueryFailed as e:
                    pool.log.append('caught %s, %d free' % (e, len(pool.free)))
                pool.log.append('after except, %d free' % len(pool.free))
                with pool.acquire() as conn:
                    pool.log.append('inside with c%d' % conn.cid)
                    if k % 4 == 1:
                        try:
                            with pool.acquire() as c2:
                                conn.execute('nested', c2.cid)
                                raise QueryFailed('nested')
                        except QueryFailed:
                            pool.log.append('nested failed')
        a, b = Statement('cycle a'), Statement('cycle b')
        a.other, b.other = b, a
        refs = [weakref.ref(a), weakref.ref(b)]
        del a, b
        alive_before = sum(1 for x in refs if x() is not None)
        found = gc.collect()
        alive_after = sum(1 for x in refs if x() is not None)
    finally:
        if old:
            gc.enable()
    pool.log.append('cycle alive %d then %d, collected %s' % (alive_before, alive_after, found > 0))
    return total, len(pool.free), sorted(c.queries for c in pool.free), pool.log


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    return digest([session(r, variant) for r in range(n)], 2)
