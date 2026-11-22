from hdr import *
import collections.abc as abc
class C:
    def __len__(self): return 0
    def __getitem__(self, i): raise IndexError
    def keys(self): return []
def f(x):
    match x:
        case {}: return 'mapping'
        case []: return 'sequence'
        case _: return 'other'
c = C()
hot(f, c)
show('before', f, c)
abc.Sequence.register(C)
show('after Sequence.register', f, c)
class D:
    def __len__(self): return 0
    def keys(self): return []
    def __getitem__(self, k): raise KeyError
d = D()
hot(f, d)
abc.Mapping.register(D)
show('after Mapping.register', f, d)
