"""Cost of one attribute store, with or without Ni (doc/FINDINGS.md):

    python3.7 tools/store_micro.py ni|plain

Each loop's time minus an empty loop's, best of seven. Timings are noisy on a
shared machine; compare runs made together."""
import os
import sys
import time
if sys.argv[1] == 'ni':
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import ni  # noqa


class A:
    def __init__(self):
        self.a = 0
        self.b = 0


class C:
    pass


class V:
    def __init__(self, x, y, z):
        self.x = x
        self.y = y
        self.z = z


def base(o, n):
    i = 0
    while i < n:
        i = i + 1
    return o


def split_existing(o, n):
    i = 0
    while i < n:
        o.a = i
        o.b = i
        o.a = i
        o.b = i
        i = i + 1
    return o


def combined_existing(o, n):
    i = 0
    while i < n:
        o.a = i
        o.b = i
        o.a = i
        o.b = i
        i = i + 1
    return o


def construct(o, n):
    i = 0
    while i < n:
        c = C()
        c.x = i
        c.y = i
        c.z = i
        i = i + 1
    return o


def construct_base(o, n):
    i = 0
    while i < n:
        C()
        i = i + 1
    return o


def best(f, o, n):
    f(o, 1000)
    f(o, 1000)
    r = []
    for _ in range(7):
        t = time.perf_counter()
        f(o, n)
        r.append(time.perf_counter() - t)
    return min(r)


A()
A()
class D:
    pass


D(); D()
comb = D()
for k in range(20):
    setattr(comb, 'k%d' % k, k)   # many keys: combined after the class keys fill
comb.a = 0
comb.b = 0
comb.zz = 0
del comb.zz   # deleting from a split dict makes it combined
A(); V(1, 2, 3); V(1, 2, 3)
for _ in range(3):
    c = C(); c.x = 1; c.y = 2; c.z = 3
N = 2000000
b = best(base, A(), N)
print('split existing   %.2f ns/store' % ((best(split_existing, A(), N) - b) / (4 * N) * 1e9))
print('combined existing %.2f ns/store' % ((best(combined_existing, comb, N) - b) / (4 * N) * 1e9))
cb = best(construct_base, None, N)
print('C() then 3 stores, minus C(): %.2f ns per 3 stores (dict creation included)' % ((best(construct, None, N) - cb) / N * 1e9))
import ctypes
print('comb split?', bool(ctypes.c_void_p.from_address(id(comb.__dict__)+40).value), 'A split?', bool(ctypes.c_void_p.from_address(id(A().__dict__)+40).value))
c=C(); c.x=1; print('C split?', bool(ctypes.c_void_p.from_address(id(c.__dict__)+40).value))
