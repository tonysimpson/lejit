from hdr import *
class C: pass
class D:
    def __radd__(self, o): return 'D.radd'
    def __rsub__(self, o): return 'D.rsub'
flip = [False]
class A:
    def __iadd__(self, o):
        if flip[0]: o.__class__ = D
        return NotImplemented
    def __isub__(self, o):
        if flip[0]: o.__class__ = D
        return NotImplemented
    def __sub__(self, o):
        return NotImplemented
def f(a, c):
    a += c
    return a
def g(a, c):
    a -= c
    return a
def run(fn, on):
    a, c = A(), C()
    flip[0] = on
    try: r = fn(a, c)
    except Exception as e: r = '%s: %s' % (type(e).__name__, e)
    return r, type(c).__name__
for _ in range(300): run(f, False); run(g, False)
print('iadd', run(f, True))
print('isub', run(g, True))
