from hdr import *
import collections.abc as cabc
class C: pass
def f(x):
    match x:
        case [*_]: return 'sequence'
        case {}: return 'mapping'
        case _: return 'other'
c = C()
for _ in range(200): f(c)
cabc.Sequence.register(C)
print(f(c), f([1]))
