from hdr import *
flip = [False]
def mk():
    class C: pass
    class A:
        def __lt__(self, o):
            if flip[0]: C.__gt__ = lambda s, x: 'C.gt'
            return NotImplemented
        def __iadd__(self, o):
            if flip[0]: C.__radd__ = lambda s, x: 'C.radd'
            return NotImplemented
        def __eq__(self, o):
            if flip[0]: C.__eq__ = lambda s, x: 'C.eq'
            return NotImplemented
        __hash__ = None
    return A, C
A, C = mk()
def f(a, c): return a < c
def g(a, c):
    a += c
    return a
def h(a, c): return a == c
def run(fn, on):
    global A, C
    A, C = mk() if on else (A, C)
    return fn
def go(fn, on):
    a, c = A(), C()
    flip[0] = on
    try: r = fn(a, c)
    except Exception as e: r = '%s: %s' % (type(e).__name__, e)
    return r
for _ in range(300): go(f, False); go(g, False); go(h, False)
print('cmp', go(f, True))
print('iadd', go(g, True))
print('eq', go(h, True))
