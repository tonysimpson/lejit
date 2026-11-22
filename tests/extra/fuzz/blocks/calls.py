from . import Block

BLOCKS = [
    Block('inline_rebind', 'calls',
          'a small loop-free function inlined into its caller from the global it is '
          'read from',
          'global rebinding of the callee, in the middle of the caller\'s loop',
          '''
def P_h(a):
    return a + 1
def P_h2(a):
    return S_body
P_broken = False
def P_break():
    global P_broken
    P_broken = True
def P_flip():
    global P_h, P_h2
    P_h, P_h2 = P_h2, P_h
def P_run(r):
    s = []
    inner = None
    for i in range(4):
        s.append(P_h(i + r))
        if i == 1:
            inner = NEXT(r)
            if P_broken:
                P_flip()
    return s, inner
''', slots={'body': ['a * 10', "str(a)", 'a / 2', '(a, a)']}),

    Block('inline_raise', 'exceptions',
          'an inlined callee that raises for some values, caught in the caller',
          'exception through the site: the callee starts raising for other values, or an '
          'exception the handler does not catch',
          '''
P_m = S_m
def P_h(a):
    if a % P_m == 0:
        raise S_exc('h%d' % a)
    return a
def P_break():
    global P_m
    P_m = S_m2
def P_run(r):
    out = []
    for i in range(5):
        try:
            out.append(P_h(i + r))
        except S_catch as e:
            out.append(type(e).__name__ + str(e))
    return out, NEXT(r)
''', slots={'m': ['7', '1000'], 'm2': ['2', '1', '3'],
            'exc': ['ValueError', 'KeyError', 'ZeroDivisionError', 'StopIteration'],
            'catch': ['Exception', 'ValueError', '(KeyError, ArithmeticError)']}),

    Block('getframe', 'calls',
          "a callee that reads its caller's frame, which is compiled or inlined",
          'the depth read changes after the break, reaching frames of other blocks',
          '''
import sys
P_depth = 1
def P_h(a):
    f = sys._getframe(P_depth)
    return f.f_code.co_name.split('_')[-1], f.f_globals is globals(), a
def P_break():
    global P_depth
    P_depth = S_d
def P_run(r):
    x = P_h(r)
    inner = NEXT(r)
    return x, sys._getframe().f_code.co_name.split('_')[-1], inner
''', slots={'d': ['0', '2', '3']}),

    Block('kwonly_defaults', 'calls',
          'a call to a function with keyword-only defaults',
          '__kwdefaults__ replaced after the break',
          '''
def P_f(a, *, k=S_k, j=2):
    return a, k, j
def P_break():
    P_f.__kwdefaults__ = S_new
def P_run(r):
    try:
        x = P_f(r)
        y = P_f(r, k=1)
        z = P_f(a=r, j=r)
    except TypeError:
        x = y = z = 'TypeError'
    return x, y, z, NEXT(r)
''', slots={'k': ['1', "'k'"], 'new': ["{'k': 5, 'j': 3}", "{'j': 0}", 'None']}),

    Block('closure_cell', 'closures',
          "a closure's cell read in a compiled caller and callee",
          'the cell is rebound, by nonlocal from another closure or by cell_contents',
          '''
def P_make():
    n = S_n
    def get():
        return n
    def bump(d):
        nonlocal n
        n = n + d
    return get, bump
P_get, P_bump = P_make()
def P_break():
    S_how
def P_run(r):
    a = P_get()
    inner = NEXT(r)
    b = P_get()
    try:
        P_bump(1)
        P_bump(-1)
    except TypeError:
        pass
    return a, b, inner
''', slots={'n': ['0', '2**62', "'s'"],
            'how': ['P_bump(2**63)', 'P_bump(0.5)', "P_get.__closure__[0].cell_contents = 'cell'",
                    'P_get.__closure__[0].cell_contents = None']}),

    Block('closure_factory', 'closures',
          'closures and lambdas made per call with defaults and free variables',
          'type switch of the free variable after the break',
          '''
P_kk = 1
def P_break():
    global P_kk
    P_kk = S_kk
def P_run(r):
    k = r * P_kk
    def add(a):
        return a + k
    fs = [lambda a, i=i: a * i + k for i in range(3)]
    inner = NEXT(r)
    try:
        return add(1), [f(2) for f in fs], inner
    except TypeError:
        return 'TypeError', inner
''', slots={'kk': ['1.5', "'ab'", '2**64', '[0]']}),

    Block('recursion', 'calls',
          'a recursive compiled call, counted for the recursion limit',
          'size past a limit: the depth passes the recursion limit',
          '''
def P_f(n):
    return 0 if n <= 0 else 1 + P_f(n - 1)
P_depth = S_d
def P_break():
    global P_depth
    P_depth = S_d2
def P_run(r):
    try:
        v = P_f(P_depth)
    except RecursionError:
        v = 'RecursionError'
    return v, NEXT(r)
''', slots={'d': ['3', '50', '400'], 'd2': ['5000', '100000', '-1']}),

    Block('generator_protocol', 'generators',
          'a generator driven by a compiled caller: next, send, throw, close',
          'exception through the site: an exception thrown into the generator',
          '''
def P_gen(n):
    try:
        for i in range(n):
            got = yield i
            if got is not None:
                yield 'got%s' % got
    except S_exc:
        yield 'caught'
P_broken = False
def P_break():
    global P_broken
    P_broken = True
def P_run(r):
    g = P_gen(5)
    out = [next(g), next(g)]
    out.append(g.send(r))
    inner = NEXT(r)
    if P_broken:
        try:
            out.append(g.throw(S_thrown('t')))
        except Exception as e:
            out.append(type(e).__name__)
    g.close()
    out.append(list(P_gen(3)))
    return out, inner
''', slots={'exc': ['ValueError', 'Exception', 'KeyError'],
            'thrown': ['ValueError', 'KeyError', 'StopIteration', 'GeneratorExit']}),

    Block('genexpr_fuse', 'generators',
          'a generator expression consumed by sum, any or list, fused into the caller',
          'global rebinding of the consumer, or type switch of the iterable',
          '''
P_seq = S_seq
def P_break():
    S_how
def P_run(r):
    try:
        a = sum(x * 2 for x in P_seq)
        b = list(x for x in P_seq if x > r % 3)
        c = any(x == r for x in P_seq)
        d = sorted(x for x in P_seq)[:3]
    except TypeError as e:
        a = b = c = d = 'TypeError'
    return a, b, c, d, NEXT(r)
''', slots={'seq': ['[1, 2, 3]', '(0, 5, -1)', 'range(4)'],
            'how': ["globals()['sum'] = lambda g: 'shadow'", "globals()['P_seq'] = {3, 1, 2}",
                    "globals()['P_seq'] = [1.5, 2**64, True]", "globals()['P_seq'] = iter([1, 2])",
                    "globals()['P_seq'] = ['a', 1]"]}),
]
