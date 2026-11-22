from hdr import *
def case(name, attr, val, fn, base=object):
    C = type('C', (base,), {})
    o = C()
    hot(fn, o)
    show(name + ' before', fn, o)
    setattr(C, attr, val)
    show(name + ' after', fn, o)
case('len', '__len__', lambda s: 3, lambda o: len(o))
case('iter', '__iter__', lambda s: iter([1]), lambda o: [x for x in o])
case('call', '__call__', lambda s: 'called', lambda o: o())
case('getitem', '__getitem__', lambda s, k: 'gi', lambda o: o[0])
def fs(o):
    o[0] = 1
    return 'set'
case('setitem', '__setitem__', lambda s, k, v: None, fs)
def fd(o):
    del o[0]
    return 'del'
case('delitem', '__delitem__', lambda s, k: None, fd)
case('add', '__add__', lambda s, x: 'add', lambda o: o + 1)
case('sub', '__sub__', lambda s, x: 'sub', lambda o: o - 1)
case('mul-seq', '__rmul__', lambda s, x: 'rmul', lambda o: [1] * o)
case('neg', '__neg__', lambda s: 'neg', lambda o: -o)
case('invert', '__invert__', lambda s: 'inv', lambda o: ~o)
case('abs', '__abs__', lambda s: 'abs', lambda o: abs(o))
case('index', '__index__', lambda s: 1, lambda o: [5, 6][o])
case('int', '__int__', lambda s: 4, lambda o: int(o))
case('float', '__float__', lambda s: 4.5, lambda o: float(o))
case('next', '__next__', lambda s: 'nx', lambda o: next(o))
case('contains', '__contains__', lambda s, x: True, lambda o: 1 in o)
case('lt', '__lt__', lambda s, x: 'lt', lambda o: o < 1)
case('hash-none', '__hash__', lambda s: 5, lambda o: hash(o), base=type('B', (), {'__hash__': None}))
case('format', '__format__', lambda s, f: 'F', lambda o: format(o, 'x'))
case('iadd', '__iadd__', lambda s, x: 'iadd', lambda o: (lambda v: (v.__class__, v)[1])(o) and (o.__class__ and None) or (lambda: None)() or 'x')
def fia(o):
    o += 1
    return o
case('iadd2', '__iadd__', lambda s, x: 'iadd', fia)
case('pow', '__pow__', lambda s, x, m=None: 'pow', lambda o: o ** 2)
case('matmul', '__matmul__', lambda s, x: 'mm', lambda o: o @ 2)
case('divmod', '__divmod__', lambda s, x: 'dm', lambda o: divmod(o, 2))
case('round', '__round__', lambda s, n=None: 'rd', lambda o: round(o))
case('reversed', '__reversed__', lambda s: iter('r'), lambda o: list(reversed(o)))
def fw(o):
    with o:
        return 'with'
case('with-enter', '__enter__', lambda s: s, fw, base=type('B', (), {'__exit__': lambda s, *a: None}))
def fu(o):
    a, = o
    return a
case('unpack', '__iter__', lambda s: iter([7]), fu)
case('starcall', '__iter__', lambda s: iter([7]), lambda o: (lambda *a: a)(*o))
case('kwcall', 'keys', lambda s: ['k'], lambda o: (lambda **k: k)(**o), base=type('B', (), {'__getitem__': lambda s, k: 1}))
case('bool-len', '__len__', lambda s: 0, lambda o: 'T' if o else 'F')
case('sorted', '__iter__', lambda s: iter([2, 1]), lambda o: sorted(o))
case('sum', '__iter__', lambda s: iter([2, 1]), lambda o: sum(o))
case('min', '__iter__', lambda s: iter([2, 1]), lambda o: min(o))
case('tuple', '__iter__', lambda s: iter([2, 1]), lambda o: tuple(o))
case('join', '__iter__', lambda s: iter(['a']), lambda o: ''.join(o))
case('set', '__iter__', lambda s: iter([2]), lambda o: set(o))
case('dict-from', 'keys', lambda s: ['k'], lambda o: dict(o), base=type('B', (), {'__getitem__': lambda s, k: 1}))
case('str', '__str__', lambda s: 'S', lambda o: str(o) == 'S')
case('getattr', '__getattr__', lambda s, n: 'ga', lambda o: o.foo)
case('setattr-err', '__setattr__', lambda s, n, v: None, lambda o: (setattr(o, 'x', 1), getattr(o, 'x', 'none'))[1], base=type('B', (), {'__slots__': ()}))
case('isinstance-meta', '__class__', property(lambda s: int), lambda o: isinstance(o, int))
case('match-args', '__match_args__', ('a',), lambda o: (lambda: None)() or 'n/a')
