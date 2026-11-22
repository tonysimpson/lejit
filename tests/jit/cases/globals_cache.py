# Cached LOAD_GLOBAL (pycompiler.c:load_global_cached): a global or
# builtin is a compile-time constant behind a guard on the dicts' version
# tags. Every way the cached value can go stale must give CPython's result.
import builtins
import gc
import types
import weakref

G = 10


def helper(x):
    return x + 1


def other_helper(x):
    return x * 100


def read_g():
    return G


def call_helper(x):
    return helper(x)


def use_len(s):
    return len(s)


def rebind_between_calls():
    # the same compiled code, called after the global changes
    global G, helper
    out = [read_g(), call_helper(1)]
    G = 20
    helper = other_helper
    out += [read_g(), call_helper(1)]
    G = 10
    helper = _helper
    out += [read_g(), call_helper(1)]
    return out


def rebind_in_loop(n):
    # the global changes while a loop that reads it is running
    global G
    t = 0
    for i in range(n):
        t = t + G
        if i == n // 2:
            G = 1000
    G = 10
    return t


def bump():
    global G
    G = G + 1


def rebind_by_callee(n):
    # a called function changes the global the loop reads
    global G
    t = 0
    for i in range(n):
        t = t + G
        bump()
    G = 10
    return t


COUNTER = 0


def unrelated_writes(n):
    # writes to another global change globals' tag, not the value read
    global COUNTER
    t = 0
    for i in range(n):
        COUNTER = COUNTER + 1
        t = t + G + len('abc')
    COUNTER = 0
    return t


def shadow_builtin():
    # a global named like a builtin appears, then goes away
    g = globals()
    out = [use_len('abcd')]
    g['len'] = lambda s: -1
    out.append(use_len('abcd'))
    del g['len']
    out.append(use_len('abcd'))
    return out


def change_builtins_module():
    # the builtin itself is replaced
    saved = builtins.len
    out = [use_len('ab')]
    builtins.len = lambda s: 42
    try:
        out.append(use_len('ab'))
    finally:
        builtins.len = saved
    out.append(use_len('ab'))
    return out


def deleted_global():
    global G
    out = [read_g()]
    del G
    try:
        out.append(read_g())
    except NameError as e:
        out.append(str(e))
    G = 10
    out.append(read_g())
    return out


SRC = '''
def f(x):
    return x + K + len('ab')
'''


def exec_namespaces():
    # one code object, several globals dicts, one without __builtins__
    code = compile(SRC, 'src', 'exec').co_consts[0]
    out = []
    for k in range(6):
        d = {'K': k}
        if k % 2:
            d['__builtins__'] = {'len': lambda s: 100}
        f = types.FunctionType(code, d)
        out.append((f(1), f(2)))
        d['K'] = -k
        out.append(f(3))
    return out


def reused_addresses():
    # globals dicts that die; new ones often reuse the address
    code = compile(SRC, 'src', 'exec').co_consts[0]
    out = []
    for k in range(30):
        d = {'K': k, '__builtins__': builtins}
        out.append(types.FunctionType(code, d)(0))
        del d
    return out


class Value:
    pass


V = Value()


def read_v():
    return V


def cached_value_after_rebind():
    # the constant compiled into read_v must stay valid after V is rebound
    # (the cache owns it); compare with CPython by identity of the result
    global V
    first = read_v()
    V = Value()
    second = read_v()
    ok = first is not second and second is V
    return ok, type(first).__name__


def global_class_isinstance(xs):
    # isinstance against a global class and builtin types, the shapes case
    out = []
    for x in xs:
        out.append((isinstance(x, int), isinstance(x, Value),
                    isinstance(x, (str, Value))))
    return out


def megamorphic_global(n):
    # a global that changes every iteration
    global G
    t = 0
    for i in range(n):
        G = i
        t = t + read_g()
    G = 10
    return t


_helper = helper


def small():
    return 1


def bigger():
    t = 0
    for i in range(3):
        t = t + i
    return t


F = small
H = 1


def swap():
    global F, H
    F = bigger if F is small else small
    H = H + 1


def replay_after_rebind():
    # H's guard fires after swap() rebinds F too: the respawn replays the
    # block through F's cached site, whose global has changed since
    global F, H
    out = []
    for k in range(4):
        a = F()
        swap()
        b = H
        out.append((a, b))
    F = small
    H = 1
    return out


_helper = helper

R = None


def raising_closure(b, x):
    return [x for w in b]   # closure code: a real call, which can raise


MODE = 'normal'


def store_in_untaken_branch(b):
    # STORE_GLOBAL marks R as changing; the error path after the call is
    # compiled by a respawn, which must see the same mark (signal -11
    # without the fix: changing_globals wasn't in snapshots)
    global R
    if MODE == 'never':
        R = 1
    return raising_closure(b, R)


def store_then_respawn():
    out = [store_in_untaken_branch(b'ab')]
    try:
        store_in_untaken_branch(5)
    except TypeError as e:
        out.append(str(e))
    out.append(store_in_untaken_branch(b'ab'))
    return out



def missing_global_name(n):
    # the NameError of a missing global, and of a deleted one, carries the
    # name where CPython's does (3.10)
    out = []
    for i in range(n):
        try:
            no_such_global_xyz
        except NameError as e:
            out.append((str(e), getattr(e, 'name', None)))
        try:
            global no_such_global_abc
            del no_such_global_abc
        except NameError as e:
            out.append((str(e), getattr(e, 'name', None)))
    return out


# without calls, so compiled wherever LOAD_GLOBAL is


def read_in_loop(n):
    # a global (entry guard) and a builtin (absent guard) in a loop
    t = 0
    i = 0
    while i < n:
        t = t + G + (len is not None) + i
        i = i + 1
    return t


def rebind_in_loop_inline(n):
    # the global changes while the loop that reads it runs
    global G
    t = 0
    i = 0
    while i < n:
        t = t + G
        if i == n // 2:
            G = 1000
        i = i + 1
    G = 10
    return t


def grow_globals(n):
    # new names grow the module's keys, which moves G's entry, then go
    global G1, G2, G3, G4, G5, G6, G7, G8, G9, G10, G11, G12
    t = G
    if n:
        G1 = G2 = G3 = G4 = G5 = G6 = G7 = G8 = G9 = G10 = G11 = G12 = n
        t = t + G + G12
        del G1, G2, G3, G4, G5, G6, G7, G8, G9, G10, G11, G12
    return t + G


_len = len


def shadow_builtin_inline():
    # a global named like a builtin appears, then goes away
    global len
    a = len is _len
    len = 5
    b = len
    del len
    return a + 2 * (len is _len) + 4 * b


class _MissingNamespace(dict):
    def __missing__(self, key):
        if key == 'FROM_MISSING':
            return 7
        raise KeyError(key)


class _GetitemNamespace(dict):
    def __getitem__(self, key):
        if key == 'FROM_GETITEM':
            return 5
        if key == 'RAISES':
            raise ZeroDivisionError(key)
        return dict.__getitem__(self, key)


def _with_globals(source, namespace):
    return types.FunctionType(
        compile(source, 'src', 'exec').co_consts[0], namespace)


_LOOP = ('def {0}(n):\n'
         '    t = 0\n'
         '    i = 0\n'
         '    while i < n:\n'
         '        t = t + {1}\n'
         '        i = i + 1\n'
         '    return t\n')

# globals that are a dict subclass: CPython looks a name up with their
# __getitem__, which may call __missing__, then in the builtins likewise
missing_globals = _with_globals(
    _LOOP.format('missing_globals', 'FROM_MISSING'), _MissingNamespace())
getitem_globals = _with_globals(
    _LOOP.format('getitem_globals', 'FROM_GETITEM'), _GetitemNamespace())
# __getitem__ raises KeyError: the builtins give the name
builtin_through_getitem = _with_globals(
    _LOOP.format('builtin_through_getitem', '(len is not None)'),
    _GetitemNamespace())
# builtins that are a dict subclass
builtins_subclass = _with_globals(
    _LOOP.format('builtins_subclass', 'FROM_MISSING'),
    {'__builtins__': _MissingNamespace()})
# NameError where neither has the name; __getitem__'s own error as is
unknown_in_both = _with_globals(
    'def unknown_in_both():\n    return NOWHERE\n', _GetitemNamespace())
getitem_raises = _with_globals(
    'def getitem_raises():\n    return RAISES\n', _GetitemNamespace())

# STORE_NAME and LOAD_NAME in a frame with no f_locals: CPython raises
# SystemError; such code isn't compiled
_names = compile('x = 1\ny = x\n', 'src', 'exec')
if hasattr(_names, 'replace'):
    _names = _names.replace(co_flags=_names.co_flags | 0x3)
else:
    _names = types.CodeType(
        _names.co_argcount, _names.co_kwonlyargcount, _names.co_nlocals,
        _names.co_stacksize, _names.co_flags | 0x3, _names.co_code,
        _names.co_consts, _names.co_names, _names.co_varnames,
        _names.co_filename, _names.co_name, _names.co_firstlineno,
        _names.co_lnotab, _names.co_freevars, _names.co_cellvars)
name_opcodes = types.FunctionType(_names, {})


CASES = [
    ('read_in_loop', [(5,), (20,)], {}),
    ('rebind_in_loop_inline', [(10,), (11,)], {}),
    ('grow_globals', [(0,), (3,), (0,)], {}),
    ('shadow_builtin_inline', [()], {}),
    ('rebind_between_calls', [()], {}),
    ('rebind_in_loop', [(10,), (11,)], {}),
    ('rebind_by_callee', [(20,)], {}),
    ('unrelated_writes', [(50,)], {}),
    ('shadow_builtin', [()], {}),
    ('change_builtins_module', [()], {}),
    ('deleted_global', [()], {}),
    ('exec_namespaces', [()], {}),
    ('reused_addresses', [()], {}),
    ('cached_value_after_rebind', [()], {}),
    ('global_class_isinstance', '[([1, Value(), "s", 2.5, True],)]', {}),
    ('megamorphic_global', [(30,)], {}),
    ('replay_after_rebind', [()], {}),
    ('store_then_respawn', [()], {}),
    ('missing_global_name', [(3,), (4,)], {}),
    ('missing_globals', [(3,), (4,)], {}),
    ('getitem_globals', [(3,), (4,)], {}),
    ('builtin_through_getitem', [(3,), (4,)], {}),
    ('builtins_subclass', [(3,), (4,)], {}),
    ('unknown_in_both', [()], {}),
    ('getitem_raises', [()], {}),
    ('name_opcodes', [()], {'interpreted': 'STORE_NAME and LOAD_NAME'}),
]
