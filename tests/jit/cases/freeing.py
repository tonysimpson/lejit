# Compiled code objects must be able to die. The code object's record
# holds its snapshots, so a snapshot holding the code object would make it
# immortal. Compiled code borrows its globals, so code with globals of its
# own (exec into a new dict, namedtuple) dies with them, and a cycle back
# to the code through what its code holds is found by the owner scan
# (codemanager.c).
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


CALLS = '''
def g(a):
    return a + 1
def f(a):
    t = 0
    for i in range(a):
        t += g(i)
    return t
'''


MANY = ''.join('def h%d(a):\n    return a + %d\n' % (i, i) for i in range(600))


def scan_soon():
    # the owner scan runs once enough records have been counted since the
    # last (codemanager.c), so make records until it has run
    import sys
    lejit = sys.modules.get('lejit')
    if lejit is None:
        return
    start = lejit._core._stats()['owner_scans'][0]
    for k in range(40):
        ns = {}
        exec(MANY, ns)
        for i in range(600):
            ns['h%d' % i](1)
        ns.clear()  # freed now: no cycle left for a later collection
        if lejit._core._stats()['owner_scans'][0] > start:
            return


def own_namespace_calls():
    # f's code holds g (a cached global), g holds the namespace, which
    # holds f: a cycle through f's code object, which CPython's GC can't see
    refs = []
    total = 0
    for k in range(5):
        ns = {}
        exec(CALLS, ns)
        refs.append(weakref.ref(ns['f'].__code__))
        total += ns['f'](10 + k)
    del ns
    scan_soon()
    gc.collect()
    return total, [r() is None for r in refs]


RECURSIVE = '''
def even(n):
    return True if n == 0 else odd(n - 1)
def odd(n):
    return False if n == 0 else even(n - 1)
'''


def own_namespace_recursion():
    # each code object holds the other, through code alone as well as
    # through the namespace
    refs = []
    out = []
    for k in range(5):
        ns = {}
        exec(RECURSIVE, ns)
        refs.append(weakref.ref(ns['even'].__code__))
        refs.append(weakref.ref(ns['odd'].__code__))
        out.append(ns['even'](20 + k))
    del ns
    scan_soon()
    gc.collect()
    return out, [r() is None for r in refs]


def freed_in_subinterpreter():
    # a cycle through compiled code, old by the time it is garbage, found
    # by a scan, then freed by a subinterpreter's collection, whose
    # co_extra free functions are junk on 3.7 (lejit.c)
    import _testcapi
    cycles = []
    for k in range(20):
        ns = {}
        exec(CALLS, ns)
        ns['f'](k)
        cycles.append(ns)
    refs = [weakref.ref(ns['f'].__code__) for ns in cycles]
    gc.collect()
    del cycles, ns
    keep = {}
    exec(MANY, keep)
    total = sum(keep['h%d' % i](1) for i in range(600))
    scan_soon()
    gc.collect(0)
    r = _testcapi.run_in_subinterp('import gc; gc.collect()')
    keep.clear()
    gc.collect()
    return total, r, [x() is None for x in refs]


def unscanned_freed_in_subinterpreter():
    # compiled code in plain cycles (the namespace and its function) that
    # become garbage with no scan since: the subinterpreter's collection
    # frees them (lejit.c: why LeJIT's records aren't in co_extra)
    import _testcapi
    gc.collect()
    gc.disable()
    try:
        refs = []
        for k in range(50):
            ns = {}
            _exec(SRC, ns)
            sum(map(ns['f'], (k,)))  # from C: no call site holds f
            refs.append(weakref.ref(ns['f'].__code__))
        del ns
        r = _testcapi.run_in_subinterp('import gc; gc.collect()')
    finally:
        gc.enable()
    if not _perturbed:
        _perturbed.append(_run_perturbed())
    return r, _perturbed[0], [x() is None for x in refs]


_perturbed = []


def _run_perturbed():
    # the same in a child whose fresh memory malloc fills with junk, so
    # that the subinterpreter's free functions aren't NULL by luck: only
    # once, not on every round of the leak check
    import os
    import subprocess
    import sys
    src = ('import sys\nif sys.argv[1] == "1":\n'
           '    import lejit\n    lejit.start()\n'
           'import freeing\nfreeing._perturbed.append(None)\n'
           'print(freeing.unscanned_freed_in_subinterpreter'
           '()[0::2] == (0, [True] * 50))\n')
    env = dict(os.environ, MALLOC_PERTURB_='165',
               PYTHONPATH=os.pathsep.join(sys.path))
    p = subprocess.run([sys.executable, '-c', src,
                        '1' if 'lejit' in sys.modules else '0'],
                       env=env, stdout=subprocess.PIPE, timeout=120)
    return p.returncode, p.stdout.decode().strip()


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


def _lengths(objs):
    return sum(len(o) for o in objs)


def genexpr_classes_die():
    # a type site in a fused generator expression past MEGAMORPHIC_MAX
    # classes stops its fusing, so a class made per call isn't kept by a
    # fused copy for each
    refs = []
    out = []
    for k in range(40):
        C = type('C%d' % k, (), {'__len__': lambda self, k=k: k})
        out.append(_lengths([C(), C()]))
        refs.append(weakref.ref(C))
    del C
    gc.collect()
    return out, sum(r() is not None for r in refs) <= 10


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
# function for every slot, NULL included, so LeJIT's must accept NULL for a code
# object it never saw (CPython's test_code CoExtra segfaulted). The free
# function is C (PyMem_RawFree): a Python callback would run at exit too.
_api = ctypes.pythonapi
# named PyUnstable_ where CPython has that API tier
_unstable = hasattr(_api, 'PyUnstable_Eval_RequestCodeExtraIndex')
_request_index = (_api.PyUnstable_Eval_RequestCodeExtraIndex if _unstable
                  else _api._PyEval_RequestCodeExtraIndex)
_set_extra = (_api.PyUnstable_Code_SetExtra if _unstable
              else _api._PyCode_SetExtra)
_request_index.argtypes = (ctypes.c_void_p,)
_request_index.restype = ctypes.c_ssize_t
_set_extra.argtypes = (ctypes.py_object, ctypes.c_ssize_t, ctypes.c_void_p)
_set_extra.restype = ctypes.c_int
_api.PyMem_RawMalloc.argtypes = (ctypes.c_size_t,)
_api.PyMem_RawMalloc.restype = ctypes.c_void_p
EXTRA_INDEX = _request_index(ctypes.cast(_api.PyMem_RawFree, ctypes.c_void_p))


def other_code_extra_user():
    c = compile('42', 'extra', 'eval')
    r = _set_extra(c, EXTRA_INDEX, _api.PyMem_RawMalloc(8))
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


_exec = exec    # by another name, so that the caller is compiled
_warm = [200]


def recompiled_loops_free():
    # each compile of nested loops compares states at the loop heads; the
    # differences found for a block not taken must be freed (the leak
    # check, over 100 compiles). The first call compiles 200 times: what
    # LeJIT's own allocators hold grows a little before it levels off
    for _ in range(_warm.pop() if _warm else 1):
        ns = {}
        _exec(LOOPS, ns)
        # called from C: a call site of its own would specialise on each
        # new function until it went megamorphic, which looks like a leak
        r = sum(map(ns['f'], (5,)))
    return r


NOTES = '''
def f(a, b):
    return sorted(a) + [repr(b)] + sorted(b, key=abs)
'''


def call_notes_reused():
    # a call that may run Python notes the frame's locals for f_locals
    # (codegen.c:psyco_note_call_site); the note's index must come back
    # when the code dies, or code made and dropped in a loop (a reload)
    # grows a table for good
    import sys
    lejit = sys.modules.get('lejit')

    def one():
        ns = {}
        _exec(NOTES, ns)
        r = list(map(ns['f'], ([3, 1],), ((2, -5),)))
        ns.clear()
        return r
    for _ in range(20):
        one()
    start = lejit._core._stats()['call_notes'][0] if lejit else 0
    for _ in range(200):
        r = one()
    # each round makes three notes; the caller's own new code a few more
    return r, (lejit._core._stats()['call_notes'][0] if lejit else 0) - start < 50


MAKES = '''
def f(a, b):
    def g(x, y=a, *, z=b) -> int:
        return x + y + z + a
    return g(1)
'''


def made_functions_free():
    # MAKE_FUNCTION's virtual function starts with 0 in its optional fields
    # (pfuncobject.c:PsycoFunction_NewWithQualName); the closure, defaults
    # and the rest replace those, which must be released, or each compile
    # of code making a function keeps a few vinfos for good (a reload)
    import sys
    lejit = sys.modules.get('lejit')

    def one():
        ns = {}
        _exec(MAKES, ns)
        r = sum(map(ns['f'], (2,), (3,)))
        ns.clear()
        return r
    for _ in range(20):
        one()
    vinfos = lambda: (lejit._core._stats().get('vinfos') or (0,))[0] if lejit else 0
    start = vinfos()
    for _ in range(300):
        r = one()
    # four a compile before the fix
    return r, vinfos() - start < 100


def _attr_callee(v):
    return v + 1


_ATTR_NS = types.SimpleNamespace(f=_attr_callee)


def promoted_reference_released():
    # ns.f(i) loads a new reference to the function, and the call promotes
    # it: a constant holds no run-time reference, so the promotion's code
    # must drop it (dispatcher.c:do_promotion_internal_body), or every call
    # keeps the function one more time (plugin_system's lambdas lived for
    # good)
    import sys
    before = sys.getrefcount(_attr_callee)
    t = 0
    for i in range(500):
        t += _ATTR_NS.f(i)
    return t, sys.getrefcount(_attr_callee) - before < 10


CASES = [
    ('code_dies_with_shared_globals', [()], {'interpreted': 'calls exec'}),
    ('code_dies_with_own_globals', [()], {'interpreted': 'calls exec'}),
    ('own_namespace_calls', [()], {'interpreted': 'calls exec'}),
    ('own_namespace_recursion', [()], {'interpreted': 'calls exec'}),
    ('freed_in_subinterpreter', [()], {'interpreted': 'calls exec'}),
    ('unscanned_freed_in_subinterpreter', [()], {}),
    ('namedtuple_code_dies', [()], {}),
    ('isinstance_class_dies', [()], {}),
    ('isinstance_instance_class_dies', [()], {}),
    ('genexpr_classes_die', [()], {}),
    ('borrowed_globals_reused', [()], {}),
    ('other_code_extra_user', [()], {}),
    ('recompiled_loops_free', [()], {}),
    ('call_notes_reused', [()], {}),
    ('made_functions_free', [()], {}),
    ('promoted_reference_released', [()], {}),
]
