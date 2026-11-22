from hdr import *
class C(int): pass
class D(int):
    def __lt__(self, o): return 'D.lt'
    def __radd__(self, o): return 'D.radd'
    def __add__(self, o): return 'D.add'
flip = [False]
class A(int):
    def __lt__(self, o):
        if flip[0]: victim.__class__ = D
        return False
    def __add__(self, o):
        if flip[0]: victim.__class__ = D
        return int(self) + int(o)
victim = None
def f(a, b, c):
    return min(a, b, c)
def g(a, b, c):
    return sum((a, b, c))
def run(fn, on):
    global victim
    a, b, c = A(5), C(9), C(1)
    victim = c
    flip[0] = on
    try: r = fn(b, a, c)
    except Exception as e: r = repr(e)
    return r, type(c).__name__
for _ in range(300): run(f, False); run(g, False)
print('min', run(f, True))
print('sum', run(g, True))
