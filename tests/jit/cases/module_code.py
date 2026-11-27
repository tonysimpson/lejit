# Module code, run by exec with its globals as its locals mapping, as an
# import runs it: compiled, its names loaded and stored as globals
# (eval_hook.c:le_code_to_run). Each case runs one module's code in the same
# dict every call, emptied first, so that the compiled code is entered
# again (it is compiled for one globals dict). Class bodies, exec with
# locals of their own, and a dict subclass as globals stay interpreted.
import builtins
import sys
import types

run = exec   # called by another name: exec named in a case would keep the
             # case itself interpreted, which doesn't matter, but this way
             # every call goes through the same compiled caller

# a module for `from m import *`: one with __all__, one without
star_all = types.ModuleType('le_star_all')
star_all.__all__ = ['alpha', 'beta']
star_all.alpha, star_all.beta, star_all.gamma = 1, 2, 3
sys.modules['le_star_all'] = star_all
star_dict = types.ModuleType('le_star_dict')
star_dict.one, star_dict._hidden, star_dict.two = 1, 2, 3
sys.modules['le_star_dict'] = star_dict
star_bad = types.ModuleType('le_star_bad')
star_bad.__all__ = ['ok', 3]
star_bad.ok = 1
sys.modules['le_star_bad'] = star_bad
sys.modules['le_star_none'] = 7  # no __all__ and no __dict__

# another module that writes the module's dict while its loop runs
poker = types.ModuleType('le_poker')
exec('''
def poke(ns, i):
    ns['step'] = ns.get('step', 0) + i
    if i == 5:
        ns['late'] = 'defined by another module'
''', poker.__dict__)
sys.modules['le_poker'] = poker

G = {}


def module(name, src):
    """a case function running src as module code in G"""
    code = compile(src, '<%s>' % name, 'exec')

    def case():
        G.clear()
        G['__builtins__'] = builtins
        G['__name__'] = name
        try:
            run(code, G)
        except Exception as e:
            return 'raised %s: %s' % (type(e).__name__, e)
        return G.get('result')
    case.__name__ = name
    case.compiled_code = code
    globals()[name] = case


module('numeric_loop', '''
total = 0
for i in range(1000):
    total += i * i % 7
result = total, i
''')

module('string_loop', '''
words = ('alpha beta gamma delta ' * 20).split()
counts = {}
for w in words:
    u = w.upper()
    counts[u] = counts.get(u, 0) + len(u)
out = []
for w in words:
    if w.startswith('a'):
        out.append(w[::-1])
result = sorted(counts.items()), out[:3]
''')

module('functions_called_in_a_loop', '''
def f(x):
    return x * 2 + 1
class P:
    def __init__(self, x):
        self.x = x
    def get(self):
        return self.x + offset
offset = 10
acc = 0
for i in range(200):
    acc += f(i)
    acc += P(i).get()
    if i == 100:
        offset = -1
result = acc
''')

module('star_import_all', '''
from le_star_all import *
result = alpha + beta, 'gamma' in globals()
''')

module('star_import_dict', '''
from le_star_dict import *
result = one + two, '_hidden' in globals()
''')

module('star_import_math', '''
from math import *
total = 0.0
for i in range(50):
    total += sqrt(i) + floor(i / 3)
result = total
''')

module('star_import_not_str', '''
from le_star_bad import *
''')

module('star_import_no_dict', '''
from le_star_none import *
''')

module('annotations', '''
x: int = 1
y: 'str'
if x:
    z: bytes = b''
for i in range(3):
    x += i
result = x, sorted(k for k in globals() if 'annot' in k)
''')

module('annotations_kept', '''
__annotations__ = {'kept': 1}
z: float = 2.0
result = sorted(globals().get('__annotations__', {}))
''')

# an annotation is recorded in __conditional_annotations__ (3.14+), which
# the module may rebind to something not a set
module('annotations_rebound', '''
__conditional_annotations__ = {}
x: int
result = 'recorded'
''')

# an import the module names in __lazy_modules__ is lazy (3.15+): a proxy
# until the name is used
module('lazy_modules', '''
__lazy_modules__ = ['colorsys']
import colorsys
import sys
result = type(globals()['colorsys']).__name__, 'colorsys' in sys.modules
''')

module('del_of_a_global', '''
a = 1
b = 2
del a
for i in range(3):
    b += i
del i
result = 'a' in globals(), 'i' in globals(), b
''')

module('del_of_a_missing_name', '''
del never_bound
''')

module('missing_name', '''
x = 1
result = x + not_defined_anywhere
''')

module('comprehensions', '''
squares = [i * i % 7 for i in range(500)]
pairs = {k: v for k, v in zip('abc', range(3))}
total = sum(x for x in squares if x)
result = len(squares), pairs, total, sum(squares)
''')

module('dict_written_by_another_module', '''
import le_poker
step = 0
seen = []
for i in range(10):
    le_poker.poke(globals(), i)
    seen.append(step)
result = seen, late
''')

module('globals_writes', '''
g = globals()
for i in range(5):
    g['made_%d' % i] = i
    if i == 2:
        g['i'] = 100
total = made_0 + made_4
result = total, i
''')

module('global_written_by_a_function', '''
count = 0
def bump():
    global count
    count += 3
for i in range(10):
    bump()
    count += 1
result = count
''')

module('builtins_replaced_and_changed', '''
import builtins
__builtins__ = {'len': lambda o: -1}
first = len('abc')
real_len = builtins.len
builtins.len = lambda o: 42
try:
    second = len('abc')
finally:
    builtins.len = real_len
result = first, second, len('abcd')
''')

module('name_shadows_a_builtin', '''
seen = []
for i in range(4):
    seen.append(len('ab'))
    if i == 1:
        len = lambda o: 'mine'
del len
seen.append(len('abc'))
result = seen
''')

module('import_sees_its_locals', '''
import builtins
seen = []
real = builtins.__import__
def hook(name, globals=None, locals=None, fromlist=(), level=0):
    seen.append(locals is globals and locals is not None)
    return real(name, globals, locals, fromlist, level)
builtins.__import__ = hook
try:
    import sys
    from sys import maxsize
finally:
    builtins.__import__ = real
result = seen, maxsize > 0
''')

module('traceback_lines', '''
import sys
lines = []
for i in range(3):
    try:
        1 / (i - 1)
    except ZeroDivisionError:
        lines.append(sys.exc_info()[2].tb_lineno)
result = lines
''')

module('locals_named', '''
x = 1
result = sorted(k for k in locals() if not k.startswith('__'))
''')

# the same code run with locals of its own, as exec and class bodies do:
# its names are the locals mapping's
SEPARATE = compile('''
x = 5
for i in range(3):
    x += i
result = x, 'x' in globals()
''', '<separate>', 'exec')


def separate_locals():
    G.clear()
    G['__builtins__'] = builtins
    ns = {}
    run(SEPARATE, G, ns)
    return ns['result'], sorted(ns)


separate_locals.compiled_code = SEPARATE


class Recording(dict):
    """a globals dict that sees its stores"""
    def __setitem__(self, k, v):
        dict.__setitem__(self, k, v if k != 'x' else v * 10)


SUBCLASSED = compile('''
x = 1
result = x
''', '<subclassed>', 'exec')


def dict_subclass_globals():
    ns = Recording(__builtins__=builtins)
    run(SUBCLASSED, ns)
    return ns['result']


dict_subclass_globals.compiled_code = SUBCLASSED

SHARED = compile('''
y = 2
for i in range(4):
    y = y * 2
result = y
''', '<shared>', 'exec')


def same_code_both_ways():
    G.clear()
    G['__builtins__'] = builtins
    run(SHARED, G)
    first = G['result']
    ns = {}
    run(SHARED, G, ns)
    return first, ns['result'], G['result'], sorted(ns)


same_code_both_ways.compiled_code = SHARED

CASES = [
    ('numeric_loop', [()], {}),
    ('string_loop', [()], {}),
    ('functions_called_in_a_loop', [()], {}),
    ('star_import_all', [()], {}),
    ('star_import_dict', [()], {}),
    ('star_import_math', [()], {}),
    ('star_import_not_str', [()], {}),
    ('star_import_no_dict', [()], {}),
    ('annotations', [()], {}),
    ('annotations_kept', [()], {}),
    ('annotations_rebound', [()], {}),
    ('lazy_modules', [()], {}),
    ('del_of_a_global', [()], {}),
    ('del_of_a_missing_name', [()], {}),
    ('missing_name', [()], {}),
    ('comprehensions', [()], {}),
    ('dict_written_by_another_module', [()], {}),
    ('globals_writes', [()], {}),
    ('global_written_by_a_function', [()], {}),
    ('builtins_replaced_and_changed', [()], {}),
    ('name_shadows_a_builtin', [()], {}),
    ('import_sees_its_locals', [()], {}),
    ('traceback_lines', [()], {}),
    ('locals_named', [()],
     {'interpreted': 'names locals(), which inside a comprehension shows '
                     'the frame\'s own locals on 3.12+'}),
    ('separate_locals', [()],
     {'interpreted': 'its names are a locals mapping of their own'}),
    ('dict_subclass_globals', [()],
     {'interpreted': 'a dict subclass\'s __setitem__ sees the stores'}),
    ('same_code_both_ways', [()],
     {}),
]
