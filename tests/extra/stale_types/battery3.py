from hdr import *
def case(name, setup, mutate, fn):
    objs = setup()
    hot(fn, *objs)
    show(name + ' before', fn, *objs)
    mutate(objs)
    show(name + ' after', fn, *objs)
def sI():
    class I(int): pass
    return (I(2), [10, 20, 30], (10, 20, 30))
mI = lambda o: setattr(type(o[0]), '__index__', lambda s: 0)
case('intsub-index-list', sI, mI, lambda i, l, t: l[i])
case('intsub-index-tuple', sI, mI, lambda i, l, t: t[i])
case('intsub-index-str', sI, mI, lambda i, l, t: 'abc'[i])
case('intsub-index-range', sI, mI, lambda i, l, t: range(5)[i])
case('intsub-slice', sI, mI, lambda i, l, t: l[i:])
case('intsub-mul-seq', sI, mI, lambda i, l, t: len(l * i))
case('intsub-add', sI, lambda o: setattr(type(o[0]), '__add__', lambda s, x: 'add'), lambda i, l, t: i + 1)
case('intsub-radd', sI, lambda o: setattr(type(o[0]), '__radd__', lambda s, x: 'radd'), lambda i, l, t: 1 + i)
case('intsub-float-radd', sI, lambda o: setattr(type(o[0]), '__radd__', lambda s, x: 'radd'), lambda i, l, t: 1.5 + i)
case('intsub-eq', sI, lambda o: setattr(type(o[0]), '__eq__', lambda s, x: 'eq'), lambda i, l, t: i == 2)
case('intsub-bool', sI, lambda o: setattr(type(o[0]), '__bool__', lambda s: False), lambda i, l, t: 'T' if i else 'F')
case('intsub-range', sI, mI, lambda i, l, t: list(range(i)))
case('intsub-float', sI, lambda o: setattr(type(o[0]), '__float__', lambda s: 9.5), lambda i, l, t: float(i))
case('intsub-int', sI, lambda o: setattr(type(o[0]), '__int__', lambda s: 9), lambda i, l, t: int(i))
case('intsub-hash', sI, lambda o: setattr(type(o[0]), '__hash__', lambda s: 7), lambda i, l, t: hash(i))
case('intsub-dictkey', sI, lambda o: setattr(type(o[0]), '__hash__', lambda s: 7), lambda i, l, t: {2: 'two'}.get(i))
case('intsub-str-mul', sI, mI, lambda i, l, t: 'ab' * i)
case('intsub-fmt', sI, lambda o: setattr(type(o[0]), '__format__', lambda s, f: 'fmt'), lambda i, l, t: f'{i}')
case('intsub-repr', sI, lambda o: setattr(type(o[0]), '__repr__', lambda s: 'rep'), lambda i, l, t: '%r' % (i,))
case('intsub-pct-s', sI, lambda o: setattr(type(o[0]), '__str__', lambda s: 'S'), lambda i, l, t: '%s' % i)
case('intsub-pct-d', sI, mI, lambda i, l, t: '%d' % i)
case('intsub-lt', sI, lambda o: setattr(type(o[0]), '__lt__', lambda s, x: 'lt'), lambda i, l, t: i < 5)
case('intsub-min', sI, lambda o: setattr(type(o[0]), '__lt__', lambda s, x: True), lambda i, l, t: min(i, 1))
case('intsub-sum', sI, lambda o: setattr(type(o[0]), '__radd__', lambda s, x: 'radd'), lambda i, l, t: sum((i, i)))
case('intsub-neg', sI, lambda o: setattr(type(o[0]), '__neg__', lambda s: 'neg'), lambda i, l, t: -i)
case('intsub-abs', sI, lambda o: setattr(type(o[0]), '__abs__', lambda s: 'abs'), lambda i, l, t: abs(i))
case('intsub-round', sI, lambda o: setattr(type(o[0]), '__round__', lambda s, n=None: 'round'), lambda i, l, t: round(i))
def sF():
    class F(float): pass
    return (F(2.5),)
case('floatsub-add', sF, lambda o: setattr(type(o[0]), '__add__', lambda s, x: 'add'), lambda f: f + 1.0)
case('floatsub-radd', sF, lambda o: setattr(type(o[0]), '__radd__', lambda s, x: 'radd'), lambda f: 1.0 + f)
case('floatsub-int', sF, lambda o: setattr(type(o[0]), '__int__', lambda s: 42), lambda f: int(f))
case('floatsub-trunc', sF, lambda o: setattr(type(o[0]), '__trunc__', lambda s: 43), lambda f: int(f))
case('floatsub-bool', sF, lambda o: setattr(type(o[0]), '__bool__', lambda s: False), lambda f: 'T' if f else 'F')
def sS():
    class S(str): pass
    return (S('ab'),)
case('strsub-add', sS, lambda o: setattr(type(o[0]), '__add__', lambda s, x: 'add'), lambda s: s + 'x')
case('strsub-radd', sS, lambda o: setattr(type(o[0]), '__radd__', lambda s, x: 'radd'), lambda s: 'x' + s)
case('strsub-eq', sS, lambda o: setattr(type(o[0]), '__eq__', lambda s, x: 'eq'), lambda s: s == 'ab')
case('strsub-len', sS, lambda o: setattr(type(o[0]), '__len__', lambda s: 99), lambda s: len(s))
case('strsub-contains', sS, lambda o: setattr(type(o[0]), '__contains__', lambda s, x: 'yes'), lambda s: 'q' in s)
case('strsub-in-str', sS, lambda o: setattr(type(o[0]), '__eq__', lambda s, x: True), lambda s: s in 'xxabxx')
case('strsub-getitem', sS, lambda o: setattr(type(o[0]), '__getitem__', lambda s, i: 'gi'), lambda s: s[0])
case('strsub-iter', sS, lambda o: setattr(type(o[0]), '__iter__', lambda s: iter('zz')), lambda s: [c for c in s])
case('strsub-join', sS, lambda o: setattr(type(o[0]), 'join', lambda s, x: 'J'), lambda s: s.join(['1','2']))
case('strsub-split', sS, lambda o: setattr(type(o[0]), 'split', lambda s, *a: 'SP'), lambda s: s.split())
case('strsub-hash-dict', sS, lambda o: setattr(type(o[0]), '__hash__', lambda s: 1), lambda s: {'ab': 1}.get(s))
case('strsub-mod', sS, lambda o: setattr(type(o[0]), '__mod__', lambda s, x: 'MOD'), lambda s: s % ())
case('strsub-getattr-key', sS, lambda o: setattr(type(o[0]), '__eq__', lambda s, x: True), lambda s: getattr(type('Q', (), {'zz': 5}), s, 'nope'))
def sT():
    class T(tuple): pass
    return (T((1, 2)),)
case('tuplesub-getitem', sT, lambda o: setattr(type(o[0]), '__getitem__', lambda s, i: 'gi'), lambda t: t[0])
case('tuplesub-unpack', sT, lambda o: setattr(type(o[0]), '__iter__', lambda s: iter((7, 8))), lambda t: (lambda a, b: a + b)(*t))
def fu(t):
    a, b = t
    return a + b
case('tuplesub-unpack2', sT, lambda o: setattr(type(o[0]), '__iter__', lambda s: iter((7, 8))), fu)
case('tuplesub-len', sT, lambda o: setattr(type(o[0]), '__len__', lambda s: 9), lambda t: len(t))
case('tuplesub-add', sT, lambda o: setattr(type(o[0]), '__add__', lambda s, x: 'add'), lambda t: t + (3,))
case('tuplesub-eq', sT, lambda o: setattr(type(o[0]), '__eq__', lambda s, x: 'eq'), lambda t: t == (1, 2))
case('tuplesub-exc', sT, lambda o: None, lambda t: 'ok')
def sL():
    class L(list): pass
    return (L([1, 2]),)
case('listsub-setitem', sL, lambda o: setattr(type(o[0]), '__setitem__', lambda s, i, v: None), lambda l: (l.__setitem__(0, 5) if False else None, l.__getitem__(0))[1] if False else (lambda: (l.__setitem__(0, 1), l)[1])() and (l.__class__) and None)
def fset(l):
    l[0] = 9
    r = l[0]
    l[0] = 1
    return r
case('listsub-setitem2', sL, lambda o: setattr(type(o[0]), '__setitem__', lambda s, i, v: None), fset)
def fapp(l):
    l.append(3); return l.pop()
case('listsub-append', sL, lambda o: setattr(type(o[0]), 'append', lambda s, x: None), fapp)
case('listsub-add', sL, lambda o: setattr(type(o[0]), '__add__', lambda s, x: 'add'), lambda l: l + [3])
case('listsub-iadd', sL, lambda o: setattr(type(o[0]), '__iadd__', lambda s, x: 'iadd'), lambda l: (lambda x: (x.__iadd__([]) if False else None))(l) or 'x')
def fiadd(l):
    m = l
    m += []
    return m
case('listsub-iadd2', sL, lambda o: setattr(type(o[0]), '__iadd__', lambda s, x: 'iadd'), fiadd)
case('listsub-contains', sL, lambda o: setattr(type(o[0]), '__contains__', lambda s, x: 'yes'), lambda l: 7 in l)
case('listsub-sorted', sL, lambda o: setattr(type(o[0]), '__iter__', lambda s: iter([9, 8])), lambda l: sorted(l))
case('listsub-join', sL, lambda o: setattr(type(o[0]), '__iter__', lambda s: iter(['p', 'q'])), lambda l: ''.join(str(x) for x in l))
case('listsub-join2', sL, lambda o: setattr(type(o[0]), '__iter__', lambda s: iter(['p', 'q'])), lambda l: ','.join(l) if isinstance(l[0], str) else ','.join(map(str, l)))
case('listsub-sum', sL, lambda o: setattr(type(o[0]), '__iter__', lambda s: iter([100])), lambda l: sum(l))
case('listsub-tuple', sL, lambda o: setattr(type(o[0]), '__iter__', lambda s: iter([100])), lambda l: tuple(l))
case('listsub-star', sL, lambda o: setattr(type(o[0]), '__iter__', lambda s: iter([100, 200])), lambda l: (lambda *a: a)(*l))
case('listsub-bool', sL, lambda o: setattr(type(o[0]), '__len__', lambda s: 0), lambda l: 'T' if l else 'F')
def sD():
    class D(dict): pass
    return (D(a=1),)
case('dictsub-getitem', sD, lambda o: setattr(type(o[0]), '__getitem__', lambda s, k: 'gi'), lambda d: d['a'])
case('dictsub-missing', sD, lambda o: setattr(type(o[0]), '__missing__', lambda s, k: 'miss'), lambda d: d['zz'])
def fds(d):
    d['b'] = 2
    return d.get('b')
case('dictsub-setitem', sD, lambda o: setattr(type(o[0]), '__setitem__', lambda s, k, v: None), fds)
case('dictsub-contains', sD, lambda o: setattr(type(o[0]), '__contains__', lambda s, k: 'yes'), lambda d: 'zz' in d)
case('dictsub-len', sD, lambda o: setattr(type(o[0]), '__len__', lambda s: 42), lambda d: len(d))
case('dictsub-kwargs', sD, lambda o: setattr(type(o[0]), 'keys', lambda s: ['a']), lambda d: (lambda **k: sorted(k))(**d))
