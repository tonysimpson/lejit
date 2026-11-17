import functools, sys, time, traceback
import proto

def f(a, b):
    return a + b

def g(a, b):
    return a + b

proto.jit(f)

def hot(fn, n):
    t = 0
    for i in range(n):
        t = fn(t, i)      # CALL, specialised after warm-up
    return t

print('hot loop', hot(f, 100000) == hot(g, 100000), proto.counts())

class C(int):
    m = f
print('method', C(3).m(5), proto.counts())               # self + 1 arg
print('map/partial', list(map(f, [1, 2], [3, 4])), functools.partial(f, 10)(1), proto.counts())
print('star/kw', f(*(1, 2)), f(a=1, b=2), f(1, b=2), proto.counts())

try:
    f(1, 'x')
except TypeError as e:
    tb = traceback.extract_tb(e.__traceback__)
    print('traceback', [(x.name, x.lineno, x.line) for x in tb], str(e))

f.__code__ = (lambda a, b: a * b).__code__
print('after __code__ change', f(3, 4), proto.counts(), '(12 expected)')

calls = []
sys.setprofile(lambda fr, ev, arg: calls.append((ev, fr.f_code.co_name)) if ev == 'call' else None)
f(1, 2); g(1, 2)
sys.setprofile(None)
print('profile sees', [c for c in calls if c[1] in 'fg'])

for fn in (f, g):
    t = time.perf_counter(); hot(fn, 2000000); print(fn.__name__, '%.3fs' % (time.perf_counter() - t))
print('refcount check', sys.getrefcount(object()))
import gc; gc.collect()
before = sys.getallocatedblocks()
for _ in range(5): hot(f, 100000)
print('allocated blocks growth', sys.getallocatedblocks() - before)
