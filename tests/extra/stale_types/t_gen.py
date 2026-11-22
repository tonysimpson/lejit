from hdr import *
class A:
    def m(self): return 'A'
    x = 'Ax'
class B:
    def m(self): return 'B'
    x = 'Bx'
def gen(o):
    r = o.m()
    while True:
        yield r, o.m(), o.x, type(o).__name__
def run(swap):
    o = A()
    g = gen(o)
    out = [next(g)]
    if swap: o.__class__ = B
    out.append(next(g))
    return out
for i in range(300): run(False)
print(run(True))
# same, local object made inside generator, swapped via shared list
def gen2(box):
    o = A()
    box.append(o)
    for i in range(3):
        yield o.m(), o.x
def run2(swap):
    box = []
    g = gen2(box)
    out = [next(g)]
    if swap: box[0].__class__ = B
    out.append(next(g))
    return out
for i in range(300): run2(False)
print(run2(True))
# class mutation between resumes
def gen3(o):
    while True:
        yield o.m(), o.x
def run3(mut):
    class C:
        def m(self): return 'C1'
        x = 1
    o = C(); g = gen3(o)
    out = [next(g)]
    if mut:
        C.m = lambda s: 'C2'; C.x = 2
    out.append(next(g))
    return out
for i in range(300): run3(False)
print(run3(True))
