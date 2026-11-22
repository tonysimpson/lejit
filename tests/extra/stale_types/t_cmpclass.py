from hdr import *
class C: pass
class D:
    def __gt__(self, o): return 'D.gt'
flip = [False]
class A:
    def __lt__(self, o):
        if flip[0]: o.__class__ = D
        return NotImplemented
def f(a, c):
    return a < c
def run(on):
    a, c = A(), C()
    flip[0] = on
    try: r = f(a, c)
    except Exception as e: r = '%s: %s' % (type(e).__name__, e)
    return r, type(c).__name__
for _ in range(300): run(False)
print('cmp', run(True))
