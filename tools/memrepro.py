"""Memory per code buffer (KNOWN_BUGS C61, FINDINGS.md "Memory per code
buffer"): compiles N distinct small functions, each with branches, a loop,
attribute and method calls and a call to another, called from C with ints,
floats, strings, None and instances, and reports the RSS growth per code
buffer with Ni's counters.

    python3.7 tools/memrepro.py DIR N     # the ni.so in DIR ('-': no Ni)

Use a release build. For where the memory goes, run it under heaptrack
with PYTHONMALLOC=malloc and read heaptrack_print's leaked allocations.
"""
import gc
import os
import sys

root, n = sys.argv[1], int(sys.argv[2])
if root != '-':
    sys.path.insert(0, os.path.abspath(root))
    import ni
else:
    ni = None


def rss():
    with open('/proc/self/statm') as f:
        return int(f.read().split()[1]) * os.sysconf('SC_PAGE_SIZE')


class P:
    def __init__(self, x, y):
        self.x = x
        self.y = y

    def norm(self):
        return self.x * self.x + self.y * self.y


TEMPLATE = '''
def f{i}(a, b, items):
    t = {i}
    if a > b:
        t += a - b
    elif a == b:
        t -= {k}
    else:
        t += b * {k}
    for it in items:
        if isinstance(it, P):
            t += it.norm() + it.x
        elif it is None:
            t -= 1
        else:
            t += len(str(it)) + {k}
    d = {{'k': t, 'n': {i}}}
    s = [a, b, t]
    s.append(d['k'] % 7)
    while t > {k} * 10:
        t //= 2
    return t + sum(s) + g{i}(a, t)

def g{i}(u, v):
    if u is None:
        return 0
    return (u + v) % {m}
'''


def main():
    ns = {'P': P}
    base = rss()
    s0 = ni._stats() if ni else None
    for i in range(n):
        exec(TEMPLATE.format(i=i, k=i % 13 + 1, m=i % 97 + 2), ns)
    pts = [P(1, 2), P(3.5, 1)]
    for i in range(n):
        f = ns['f%d' % i]
        # called from C (map), so that no call site specialises on f
        for args in ((3, 1, pts), (1, 1, [None, 'ab', 3]),
                     (2, 5, [1.5, P(0, 1)]), (2.5, 1, ())):
            sum(map(f, *([x] for x in args)))
    gc.collect()
    grow = rss() - base
    if ni is None:
        print('functions %d  RSS growth %.1f MB' % (n, grow / 1e6))
        return
    s = ni._stats()
    bufs = s['code_buffers'] - s0['code_buffers']
    code = s['code_bytes'] - s0['code_bytes']
    print('functions %d  buffers %d  code bytes %d (%.0f per buffer)' %
          (n, bufs, code, code / bufs))
    print('RSS growth %.1f MB, %.0f bytes per buffer' %
          (grow / 1e6, grow / bufs))
    print({k: s.get(k) for k in ('code_live', 'parked', 'respawn',
                                 'promotion', 'pause', 'call')})


main()
