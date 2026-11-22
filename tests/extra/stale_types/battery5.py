from hdr import *
import collections.abc as cabc
# mutations inside a running compiled loop (via a call in the loop body)
def mk():
    class B1:
        def m(self): return 'B1'
        def __add__(self, o): return 'B1add'
        def __len__(self): return 1
        def __eq__(self, o): return 'B1eq'
        __hash__ = object.__hash__
        def __getitem__(self, i): return 'B1gi'
    class B2:
        def m(self): return 'B2'
        def __add__(self, o): return 'B2add'
        def __len__(self): return 2
        def __eq__(self, o): return 'B2eq'
        __hash__ = object.__hash__
        def __getitem__(self, i): return 'B2gi'
    class C(B1): pass
    class D(B2): pass
    return B1, B2, C, D
def loop(o, n, act):
    out = []
    for i in range(n):
        if i == n - 3: act(o)
        out.append((o.m(), o + 1, len(o), o == 0, o[0], type(o).__name__))
    return out[-4:]
def run(which):
    B1, B2, C, D = mk()
    o = C()
    acts = {
        'none': lambda o: None,
        'bases': lambda o: setattr(C, '__bases__', (B2,)),
        'class': lambda o: setattr(o, '__class__', D),
        'attr': lambda o: [setattr(C, k, getattr(B2, k)) for k in ('m', '__add__', '__len__', '__eq__', '__getitem__')],
        'baseattr': lambda o: [setattr(B1, k, getattr(B2, k)) for k in ('m', '__add__', '__len__', '__eq__', '__getitem__')],
        'del': lambda o: [delattr(B1, k) for k in ('__add__', '__len__', '__getitem__')],
    }
    return loop(o, 50, acts[which])
for w in ('none', 'bases', 'class', 'attr', 'baseattr', 'del'):
    for _ in range(30): run('none')
    try: print(w, run(w))
    except Exception as e: print(w, type(e).__name__, e)
# match inside loop with register mid-loop
def mloop(x, n, C):
    out = []
    for i in range(n):
        if i == n - 2: cabc.Sequence.register(C)
        match x:
            case [*_]: out.append('seq')
            case _: out.append('other')
    return out[-3:]
def mrun():
    class C:
        def __len__(self): return 0
        def __getitem__(self, i): raise IndexError
    return mloop(C(), 50, C)
for _ in range(50): mrun()
print('match-loop', mrun())
