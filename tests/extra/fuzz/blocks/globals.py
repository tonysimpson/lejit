from . import Block

BLOCKS = [
    Block('global_const', 'folding',
          'a global read as a constant and folded into arithmetic',
          'global rebinding to another value or type',
          '''
P_K = S_k
def P_break():
    global P_K
    P_K = S_k2
def P_run(r):
    try:
        a = P_K * 2
        b = P_K + P_K
        c = (P_K, r)[0] == S_k
    except TypeError:
        a = b = c = 'TypeError'
    return a, b, c, NEXT(r)
''', slots={'k': ['3', '2**62', '1.5', "'ab'", '-2**63'],
            'k2': ['4', '2**63', 'None', "'cd'", '(1,)', '-0.0']}),

    Block('builtin_shadow', 'globals',
          'builtins read through the globals and folded',
          'global rebinding: a module global shadows a builtin, then is deleted again',
          '''
def P_break():
    g = globals()
    if 'abs' in g:
        del g['abs']
    else:
        g['abs'] = S_new
def P_run(r):
    try:
        a = abs(-r - 1)
        b = abs(S_c)
    except Exception as e:
        a = b = type(e).__name__
    return a, b, NEXT(r)
''', slots={'new': ['lambda x: "shadow"', 'len', 'str'], 'c': ['-3', '-2**63', '-1.5']}),

    Block('builtins_patch', 'globals',
          'a builtin called with constant arguments, folded at compile time',
          'global rebinding in the builtins module itself',
          '''
import builtins
def P_break():
    builtins.hex = S_new
def P_run(r):
    try:
        return hex(255), hex(r), oct(8), NEXT(r)
    except Exception as e:
        return type(e).__name__
''', slots={'new': ['lambda x: "patched%d" % x', 'oct', 'str']}),

    Block('global_delete', 'globals',
          'a global known at compile time',
          'the global is deleted (NameError), then made again',
          '''
P_G = S_v
def P_break():
    global P_G
    del P_G
def P_run(r):
    try:
        g = P_G
    except NameError:
        g = 'NameError'
        globals()['P_G'] = S_v2
    return g, NEXT(r)
''', slots={'v': ['1', "'s'", '(1, 2)', 'len'], 'v2': ['2', '2.5', "b'x'"]}),

    Block('module_attr', 'globals',
          'a module attribute read and called',
          'method replacement on the module',
          '''
import math
P_saved = math.sqrt
def P_break():
    math.sqrt = S_new
def P_run(r):
    try:
        a = math.sqrt(16)
        b = math.sqrt(r)
    except Exception as e:
        a = b = type(e).__name__
    c = math.pi > 3
    return a, b, c, NEXT(r)
''', slots={'new': ['lambda x: -1', 'math.floor', 'abs']}),

    Block('mutable_default', 'folding',
          "a function's defaults read at the call site",
          'mutable default change, then __defaults__ replaced',
          '''
def P_f(a, acc=[], k=S_k):
    acc.append(a)
    del acc[:-3]
    return len(acc), k
P_n = 0
def P_break():
    P_f.__defaults__ = ([9], S_k2)
def P_run(r):
    x = P_f(r)
    y = P_f(r, [])
    P_f.__defaults__[0].append(r)
    return x, y, NEXT(r)
''', slots={'k': ['1', "'k'", 'None'], 'k2': ['2', '2.5', '()']}),

    Block('code_swap', 'folding',
          'a call to a known function compiled or inlined from its code',
          'the callee\'s __code__ is replaced',
          '''
def P_f(a):
    return a + 1
def P_g(a):
    return S_body
def P_break():
    P_f.__code__ = P_g.__code__
def P_run(r):
    s = 0
    for i in range(3):
        s = s + P_f(r + i)
    return s, NEXT(r)
''', slots={'body': ['a * 3', 'a - 0.5', '-a', 'a // 2']}),
]
