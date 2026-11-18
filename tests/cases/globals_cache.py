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


CASES = [
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
]
