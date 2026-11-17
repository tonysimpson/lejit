# Compiled code objects must be able to die (KNOWN_BUGS C14). Ni's
# snapshots held their code object, and the code object's co_extra held the
# snapshots, so a compiled code object was immortal. Compiled code also
# held its globals, so code with globals of its own (exec into a new dict,
# namedtuple) stayed alive with them; the globals are now borrowed.
import collections
import gc
import types
import weakref

SHARED = {}
SRC = '''
def f(a):
    t = 0
    for i in range(a):
        t += i
    return t
'''


def code_dies_with_shared_globals():
    refs = []
    total = 0
    for k in range(5):
        exec(SRC, SHARED)
        refs.append(weakref.ref(SHARED['f'].__code__))
        total += SHARED['f'](10 + k)
    del SHARED['f']
    gc.collect()
    return total, [r() is None for r in refs]


def code_dies_with_own_globals():
    refs = []
    total = 0
    for k in range(5):
        ns = {}
        exec(SRC, ns)
        refs.append(weakref.ref(ns['f'].__code__))
        total += ns['f'](10 + k)
    del ns
    gc.collect()
    return total, [r() is None for r in refs]


def namedtuple_code_dies():
    refs = []
    for k in range(5):
        P = collections.namedtuple('P', 'x y')
        p = P(k, 2)
        refs.append(weakref.ref(P.__new__.__code__))
    del P, p
    gc.collect()
    return [r() is None for r in refs]


def isinstance_class_dies():
    # isinstance's meta must not make a constant of a heap class: the
    # compiled code would keep it alive (the type-call meta had the same
    # problem with namedtuple_code_dies)
    refs = []
    out = []
    for k in range(4):
        C = type('C%d' % k, (), {})
        out.append(isinstance(5, C))
        refs.append(weakref.ref(C))
    del C
    gc.collect()
    return out, [r() is None for r in refs]


def isinstance_instance_class_dies():
    # nor of the instance's type: a new class each iteration kept every
    # one alive and compiled a new path for each
    refs = []
    out = []
    for k in range(20):
        C = type('C%d' % k, (), {})
        c = C()
        out.append(isinstance(c, C))
        refs.append(weakref.ref(C))
    del C, c
    gc.collect()
    return out, [r() is None for r in refs]


GSRC = compile('''
def g(a):
    t = 0
    for i in range(a):
        t += i * SCALE
    return t
''', 'gsrc', 'exec').co_consts[0]


def borrowed_globals_reused():
    # one code object, compiled for a dict that then dies; new dicts often
    # get the dead one's address, and the old code must use their contents
    out = []
    for k in range(30):
        d = {'SCALE': k, '__builtins__': __builtins__}
        out.append(types.FunctionType(GSRC, d)(4))
        del d
    return out


CASES = [
    ('code_dies_with_shared_globals', [()], {'interpreted': 'calls exec (C15)'}),
    ('code_dies_with_own_globals', [()], {'interpreted': 'calls exec (C15)'}),
    ('namedtuple_code_dies', [()], {'leaks': 'C14 (2): state referenced from machine code'}),
    ('isinstance_class_dies', [()], {'leaks': 'C14 (2): state referenced from machine code'}),
    ('isinstance_instance_class_dies', [()], {'leaks': 'C14 (2): state referenced from machine code'}),
    ('borrowed_globals_reused', [()], {'leaks': 'C14 (2): a snapshot per new dict'}),
]
