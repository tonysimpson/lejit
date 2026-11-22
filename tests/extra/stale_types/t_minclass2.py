from hdr import *
class C: pass
class D:
    def __lt__(self, o): return True
    def __add__(self, o): return 'D.add'
    def __radd__(self, o): return 'D.radd'
    def __len__(self): return 7
flip = [False]
victim = None
class A:
    def __lt__(self, o):
        if flip[0]: victim.__class__ = D
        return False
    def __gt__(self, o): return False
    def __add__(self, o):
        if flip[0]: victim.__class__ = D
        return 0
    def __radd__(self, o):
        if flip[0]: victim.__class__ = D
        return 0
def f(b, a, c):
    return min(b, a, c)
def g(b, a, c):
    return sum((b, a, c), A())
def h(b, a, c):
    return (a < b, c < a)
def run(fn, on):
    global victim
    a, b, c = A(), A(), C()
    victim = c
    flip[0] = on
    try: r = fn(b, a, c)
    except Exception as e: r = repr(e)
    return (r if not isinstance(r, (A, C, D)) else type(r).__name__), type(c).__name__
for _ in range(300): run(f, False); run(g, False); run(h, False)
print('min', run(f, True))
print('sum', run(g, True))
print('tuple-of-cmps', run(h, True))
