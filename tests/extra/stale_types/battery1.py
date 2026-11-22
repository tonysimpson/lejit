from hdr import *
import abc, collections.abc as cabc

def case(name, setup, mutate, fn):
    objs = setup()
    hot(fn, *objs)
    show(name + ' before', fn, *objs)
    mutate(objs)
    show(name + ' after', fn, *objs)

# 1 method replaced
def s1():
    class C:
        def m(self): return 1
    return (C(),)
def m1(o): type(o[0]).m = lambda self: 2
case('meth-replace', s1, m1, lambda o: o.m())
# 2 method deleted
def s2():
    class B:
        def m(self): return 'B'
    class C(B):
        def m(self): return 'C'
    return (C(),)
def m2(o): del type(o[0]).m
case('meth-del', s2, m2, lambda o: o.m())
# 3 base modified
def s3():
    class B:
        def m(self): return 'B'
    class C(B): pass
    return (C(),)
def m3(o): type(o[0]).__mro__[1].m = lambda self: 'B2'
case('base-mod', s3, m3, lambda o: o.m())
# 4 __bases__ assignment
def s4():
    class B1:
        def m(self): return 'B1'
        x = 1
    class B2:
        def m(self): return 'B2'
        x = 2
    class C(B1): pass
    return (C(), B2)
def m4(o): type(o[0]).__bases__ = (o[1],)
case('bases-assign meth', s4, m4, lambda o, b: o.m())
case('bases-assign attr', s4, m4, lambda o, b: o.x)
case('bases-assign isinst', s4, m4, lambda o, b: isinstance(o, b))
# 5 __getattr__ added
def s5():
    class C: pass
    return (C(),)
def m5(o): type(o[0]).__getattr__ = lambda self, n: 'ga:' + n
case('getattr-added', s5, m5, lambda o: o.missing)
# 6 __getattribute__ added
def s6():
    class C:
        x = 1
    return (C(),)
def m6(o): type(o[0]).__getattribute__ = lambda self, n: 'gab:' + n
case('getattribute-added', s6, m6, lambda o: o.x)
# 7 data descriptor (property) added over instance attr
def s7():
    class C:
        def __init__(self): self.x = 'inst'
    return (C(),)
def m7(o): type(o[0]).x = property(lambda self: 'prop')
case('property-added', s7, m7, lambda o: o.x)
# 8 heap descriptor type gains __get__
def s8():
    class D: pass
    class C: pass
    C.x = D()
    return (C(), D)
def m8(o): o[1].__get__ = lambda self, obj, t: 'got'
case('descr-gains-get', s8, m8, lambda o, d: o.x)
# 9 __len__
def s9():
    class C:
        def __len__(self): return 1
    return (C(),)
def m9(o): type(o[0]).__len__ = lambda self: 5
case('len', s9, m9, lambda o: len(o))
# 10 __bool__ deleted
def s10():
    class C:
        def __bool__(self): return False
    return (C(),)
def m10(o): del type(o[0]).__bool__
case('bool-del', s10, m10, lambda o: bool(o) and 'T' or 'F')
case('bool-del-if', s10, m10, lambda o: 'T' if o else 'F')
# 11 __eq__/__hash__
def s11():
    class C:
        def __eq__(self, other): return True
        __hash__ = object.__hash__
    return (C(), C())
def m11(o): type(o[0]).__eq__ = lambda s, x: 'neq'
case('eq', s11, m11, lambda a, b: a == b)
def m11b(o): type(o[0]).__hash__ = lambda s: 42
case('hash', s11, m11b, lambda a, b: (hash(a) == 42, hash(a) == object.__hash__(a)))
# 12 __iter__ added
def s12():
    class C:
        def __getitem__(self, i):
            if i < 2: return i
            raise IndexError
    return (C(),)
def m12(o): type(o[0]).__iter__ = lambda s: iter('xy')
case('iter-added', s12, m12, lambda o: [x for x in o])
# 13 __call__
def s13():
    class C:
        def __call__(self): return 1
    return (C(),)
def m13(o): type(o[0]).__call__ = lambda s: 2
case('call', s13, m13, lambda o: o())
# 14 __add__ / __radd__
def s14():
    class C:
        def __add__(self, o): return 'add'
    return (C(),)
def m14(o): type(o[0]).__add__ = lambda s, x: 'add2'
case('add', s14, m14, lambda o: o + 1)
def s14b():
    class C: pass
    return (C(),)
def m14b(o): type(o[0]).__radd__ = lambda s, x: 'radd'
case('radd-added', s14b, m14b, lambda o: 1 + o)
# 15 __init__/__new__ for class call
def s15():
    class C:
        def __init__(self, a): self.a = a
    return (C,)
def m15(o): o[0].__init__ = lambda self, a: setattr(self, 'a', a * 10)
case('init', s15, m15, lambda C: C(3).a)
def m15b(o): o[0].__new__ = staticmethod(lambda cls, a: 'new')
case('new', s15, m15b, lambda C: C(3))
# 16 metaclass __call__ added
def s16():
    class M(type): pass
    class C(metaclass=M):
        def __init__(self): pass
    return (C,)
def m16(o): type(o[0]).__call__ = lambda cls: 'metacall'
case('metacall', s16, m16, lambda C: C())
# 17 metaclass __instancecheck__
def s17():
    class M(type): pass
    class C(metaclass=M): pass
    return (C, 5)
def m17(o): type(o[0]).__instancecheck__ = lambda cls, x: True
case('instancecheck', s17, m17, lambda C, x: isinstance(x, C))
# 18 ABC register isinstance
def s18():
    class A(abc.ABC): pass
    class X: pass
    return (A, X())
def m18(o): o[0].register(type(o[1]))
case('abc-register', s18, m18, lambda A, x: isinstance(x, A))
# 19 __abstractmethods__
def s19():
    class C: pass
    return (C,)
def m19(o): o[0].__abstractmethods__ = frozenset({'f'})
case('abstract', s19, m19, lambda C: type(C()).__name__)
# 20 __class__ assignment
def s20():
    class A:
        def m(self): return 'A'
    class B:
        def m(self): return 'B'
    return (A(), B)
def m20(o): o[0].__class__ = o[1]
case('class-assign', s20, m20, lambda a, B: a.m())
# 21 class attr constant changed
def s21():
    class C: x = 1
    return (C(),)
def m21(o): type(o[0]).x = 2
case('cattr', s21, m21, lambda o: o.x)
# 22 __index__
def s22():
    class C:
        def __index__(self): return 0
    return (C(), [10, 20, 30])
def m22(o): type(o[0]).__index__ = lambda s: 2
case('index', s22, m22, lambda o, l: l[o])
# 23 __contains__
def s23():
    class C:
        def __contains__(self, x): return False
    return (C(),)
def m23(o): type(o[0]).__contains__ = lambda s, x: True
case('contains', s23, m23, lambda o: 1 in o)
# 24 __iadd__ added
def s24():
    class C:
        def __add__(self, o): return 'add'
    return (C(),)
def m24(o): type(o[0]).__iadd__ = lambda s, x: 'iadd'
def f24(o):
    o += 1
    return o
case('iadd-added', s24, m24, f24)
# 25 __getitem__ on list subclass
def s25():
    class L(list): pass
    return (L([1, 2, 3]),)
def m25(o): type(o[0]).__getitem__ = lambda s, i: 'gi'
case('listsub-getitem', s25, m25, lambda l: l[0])
case('listsub-getitem-neg', s25, m25, lambda l: l[-1])
def m25b(o): type(o[0]).__len__ = lambda s: 99
case('listsub-len', s25, m25b, lambda l: len(l))
def m25c(o): type(o[0]).__iter__ = lambda s: iter('ab')
case('listsub-iter', s25, m25c, lambda l: [x for x in l])
# 26 dict subclass get override
def s26():
    class D(dict): pass
    return (D(a=1),)
def m26(o): type(o[0]).get = lambda s, k, d=None: 'myget'
case('dictsub-get', s26, m26, lambda d: d.get('a'))
# 27 str subclass method override
def s27():
    class S(str): pass
    return (S(' x '),)
def m27(o): type(o[0]).strip = lambda s: 'mystrip'
case('strsub-strip', s27, m27, lambda s: s.strip())
# 28 __setattr__ added
def s28():
    class C: pass
    return (C(),)
def m28(o): type(o[0]).__setattr__ = lambda s, n, v: object.__setattr__(s, n, v * 2)
def f28(o):
    o.v = 1
    return o.v
case('setattr-added', s28, m28, f28)
# 29 __delattr__
def m29(o): type(o[0]).__delattr__ = lambda s, n: None
def f29(o):
    o.v = 1
    del o.v
    return getattr(o, 'v', 'gone')
case('delattr-added', s28, m29, f29)
# 30 __neg__
def s30():
    class C:
        def __neg__(self): return 1
    return (C(),)
def m30(o): type(o[0]).__neg__ = lambda s: 2
case('neg', s30, m30, lambda o: -o)
