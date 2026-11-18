# Compiled code objects must be able to die (KNOWN_BUGS C14). The code
# object's co_extra holds its snapshots, so a snapshot holding the code
# object would make it immortal. Compiled code borrows its globals, so code
# with globals of its own (exec into a new dict, namedtuple) dies with them.
import collections
import ctypes
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
    # compiled code would keep it alive (namedtuple_code_dies checks the
    # same for the type-call meta)
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


# Another user of co_extra (PEP 523): CPython calls every registered free
# function for every slot, NULL included, so Ni's must accept NULL for a code
# object it never saw (CPython's test_code CoExtra segfaulted). The free
# function is C (PyMem_RawFree): a Python callback would run at exit too.
_api = ctypes.pythonapi
_api._PyEval_RequestCodeExtraIndex.argtypes = (ctypes.c_void_p,)
_api._PyEval_RequestCodeExtraIndex.restype = ctypes.c_ssize_t
_api._PyCode_SetExtra.argtypes = (ctypes.py_object, ctypes.c_ssize_t,
                                  ctypes.c_void_p)
_api._PyCode_SetExtra.restype = ctypes.c_int
_api.PyMem_RawMalloc.argtypes = (ctypes.c_size_t,)
_api.PyMem_RawMalloc.restype = ctypes.c_void_p
EXTRA_INDEX = _api._PyEval_RequestCodeExtraIndex(
    ctypes.cast(_api.PyMem_RawFree, ctypes.c_void_p))


def other_code_extra_user():
    c = compile('42', 'extra', 'eval')
    r = _api._PyCode_SetExtra(c, EXTRA_INDEX, _api.PyMem_RawMalloc(8))
    del c
    return r


LOOPS = '''
def f(n):
    t = 0
    i = 0
    while i < n:
        j = 0
        while j < n:
            t += i * j
            j += 1
        i += 1
    return t
'''


_exec = exec    # by another name, so that the caller is compiled (C15)
_warm = [200]


def recompiled_loops_free():
    # each compile of nested loops compares states at the loop heads; the
    # differences found for a block not taken must be freed (the leak
    # check, over 100 compiles). The first call compiles 200 times: what
    # Ni's own allocators hold grows a little before it levels off
    for _ in range(_warm.pop() if _warm else 1):
        ns = {}
        _exec(LOOPS, ns)
        # called from C: a call site of its own would specialise on each
        # new function until it went megamorphic, which looks like a leak
        r = sum(map(ns['f'], (5,)))
    return r


CASES = [
    ('code_dies_with_shared_globals', [()], {'interpreted': 'calls exec (C15)'}),
    ('code_dies_with_own_globals', [()], {'interpreted': 'calls exec (C15)'}),
    ('namedtuple_code_dies', [()], {}),
    ('isinstance_class_dies', [()], {}),
    ('isinstance_instance_class_dies', [()], {}),
    ('borrowed_globals_reused', [()], {}),
    ('other_code_extra_user', [()], {}),
    ('recompiled_loops_free', [()], {}),
]
