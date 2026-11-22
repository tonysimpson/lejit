# Cached LOAD_GLOBAL (pycompiler.c:load_global_cached): a name found in
# globals is guarded by its entry in the dict's keys holding the same name
# and value, a name found in builtins by the probe of globals showing it
# absent and its entry in builtins (else by the version tags), and a name
# rebound is read from its entry, with C on a miss. Each case changes the
# dicts in a way a guard or load must catch, or must survive.
import builtins
import sys
import types

TICK = 0
A = 7
B = 7          # the same object as A, at another entry
SHADOW = 'g'


def tick():
    global TICK
    TICK = TICK + 1


def loop_with_writes(n):
    # TICK is rebound on every pass; A, B and len stay cached
    t = 0
    for i in range(n):
        tick()
        t = t + A + B + len('ab') + TICK
    return t


def read_a():
    return A


def read_b():
    return B


def moved_entry():
    # A deleted and set again: the same object at a later entry
    global A
    out = [read_a()]
    v = A
    del A
    A = v
    out.append(read_a())
    A = 8
    out.append(read_a())
    A = 7
    out.append(read_a())
    return out


def resized_keys():
    # enough new names to replace the keys with larger ones
    g = globals()
    out = [read_a(), read_b()]
    for i in range(200):
        g['_filler%d' % i] = i
    out += [read_a(), read_b()]
    for i in range(200):
        del g['_filler%d' % i]
    out += [read_a(), read_b()]
    return out


def swapped_same_value():
    # after A goes, another name holding the same object may sit at A's
    # old entry: the key must be compared too
    global A
    out = [read_a(), read_b()]
    del A
    try:
        out.append(read_a())
    except NameError as e:
        out.append(str(e))
    out.append(read_b())
    A = 7
    out.append(read_a())
    return out


def use_len(s):
    return len(s)


def use_shadow():
    return SHADOW


def global_shadows_builtin():
    # a global len appears, then goes, then comes back
    g = globals()
    out = [use_len('abc')]
    g['len'] = lambda s: -1
    out.append(use_len('abc'))
    g['len'] = lambda s: -2
    out.append(use_len('abc'))
    del g['len']
    out.append(use_len('abc'))
    return out


def builtin_behind_deleted_global():
    # SHADOW is a global; once deleted, the builtin of that name is found
    global SHADOW
    builtins.SHADOW = 'b'
    try:
        out = [use_shadow()]
        del SHADOW
        out.append(use_shadow())
        SHADOW = 'g2'
        out.append(use_shadow())
        SHADOW = 'g'
    finally:
        del builtins.SHADOW
    return out


SRC = '''
def f(x):
    return x + K + len('ab')
'''


def fresh_code():
    return compile(SRC, 'src', 'exec').co_consts[0]


def replaced_module_dicts():
    # one code object, globals dicts that die and are replaced; a new dict
    # may reuse the old one's address and entry layout
    code = fresh_code()
    out = []
    for k in range(40):
        d = {'K': k % 3, '__builtins__': builtins}
        f = types.FunctionType(code, d)
        out.append(f(0))
        d['K'] = 100
        out.append(f(0))
        del f, d
    return out


def cleared_and_refilled():
    # the dict is cleared (small keys again) and refilled in another order
    code = fresh_code()
    d = {'__builtins__': builtins}
    for i in range(100):
        d['n%d' % i] = i
    d['K'] = 1
    f = types.FunctionType(code, d)
    out = [f(0), f(1)]
    d.clear()
    d['K'] = 2
    d['__builtins__'] = builtins
    out.append(f(0))
    d.clear()
    d['__builtins__'] = builtins
    try:
        f(0)
    except NameError as e:
        out.append(str(e))
    d['K'] = 3
    out.append(f(0))
    return out


SRC_KEY = '''
def f(x):
    return x + KEY
'''


def equal_key_not_identical():
    # a key equal to the name but another str object
    code = compile(SRC_KEY, 'src', 'exec').co_consts[0]
    d = {}
    d[''.join(['KE', 'Y'])] = 5
    f = types.FunctionType(code, d)
    out = [f(0), f(1)]
    del d['KEY']
    d['KEY'] = 6
    out.append(f(0))
    del d['KEY']
    d[''.join(['K', 'EY'])] = 7
    out.append(f(0))
    return out


SRC_NOBUILTINS = '''
def f(x):
    return x + K
'''


class Holder:
    pass


def split_dict_globals():
    # an instance __dict__ (a split table) as globals
    code = compile(SRC_NOBUILTINS, 'src', 'exec').co_consts[0]
    h1 = Holder()
    h1.K = 1
    h2 = Holder()
    h2.K = 2
    f = types.FunctionType(code, h2.__dict__)
    out = [f(0)]
    h2.K = 3
    out.append(f(0))
    del h2.K
    try:
        f(0)
    except NameError as e:
        out.append(str(e))
    h2.K = 4
    out.append(f(0))
    return out


def read_tick():
    return TICK


def rebound_then_moved():
    # TICK rebound often enough to be loaded from its entry; then deleted,
    # shadowed by a builtin, set again at a later entry, and the keys grown
    global TICK
    g = globals()
    out = []
    for i in range(30):
        TICK = i
        out.append(read_tick())
    del TICK
    try:
        out.append(read_tick())
    except NameError as e:
        out.append(str(e))
    builtins.TICK = 'b'
    try:
        out.append(read_tick())
        TICK = 'g'
        out.append(read_tick())
    finally:
        del builtins.TICK
    for i in range(200):
        g['_grow%d' % i] = i
    for i in range(5):
        TICK = i * 10
        out.append(read_tick())
    for i in range(200):
        del g['_grow%d' % i]
    out.append(read_tick())
    TICK = 0
    return out


def shadowed_mid_loop(n):
    # len read while TICK is written; a global len appears part way, then
    # goes; other names come and go meanwhile
    g = globals()
    out = []
    for i in range(n):
        tick()
        if i == n // 2:
            g['len'] = lambda s: -1
        if i == n // 2 + 3:
            del g['len']
        g['_extra%d' % (i % 7)] = i
        out.append(use_len('abc'))
    for i in range(7):
        g.pop('_extra%d' % i, None)
    return out


def builtin_replaced():
    # the builtin itself is rebound, then restored
    out = [use_len('abc')]
    real = builtins.len
    builtins.len = lambda s: 99
    try:
        out.append(use_len('abc'))
    finally:
        builtins.len = real
    out.append(use_len('abc'))
    return out


SRC_BIG = """
def f(x):
    return len(x) + K
"""
# a code object per size: code is compiled for the globals of its first call
BIG_CODE = {n: compile(SRC_BIG, 'src', 'exec').co_consts[0]
            for n in (10, 300, 50000)}


def wide_indices(names):
    # globals whose keys have 2- or 4-byte indices; len stays a builtin,
    # then a global len shadows it, then goes
    code = BIG_CODE[names]
    d = {'n%d' % i: i for i in range(names)}
    d['K'] = 1
    d['__builtins__'] = builtins
    f = types.FunctionType(code, d)
    out = [f('ab'), f('abc')]
    d['n0'] = -1
    out.append(f('ab'))
    d['len'] = lambda s: 100
    out.append(f('ab'))
    del d['len']
    out.append(f('ab'))
    d['K'] = 2
    out.append(f('ab'))
    return out


# one size a case: together they leave blocks behind in a checked build,
# with or without these guards
def wide_indices_1():
    return wide_indices(10)


def wide_indices_2():
    return wide_indices(300)


def wide_indices_4():
    return wide_indices(50000)


SRC_SWAP = """
def h(a):
    return a + 1
def h2(a):
    return a * 10
def flip():
    global h, h2
    if SWAP:
        h, h2 = h2, h
def run(r):
    s = []
    for i in range(4):
        s.append(h(i + r))
        if i == 1:
            flip()
    return s
def calls(n):
    return [run(k) for k in range(n)]
"""


def _swap_module(swap):
    d = {'__builtins__': builtins, 'SWAP': swap}
    exec(SRC_SWAP, d)   # exec: this helper is not compiled
    return d


def _code_buffers():
    lejit = sys.modules.get('lejit')
    return lejit._core._stats()['code_buffers'] if lejit is not None else 0


def _resized_growth(swap, out):
    # code buffers made by calls after one more name resizes the keys
    d = _swap_module(swap)
    out.append(sum(map(sum, d['calls'](50))))
    size = sys.getsizeof(d)
    k = 0
    while sys.getsizeof(d) == size:
        d['_x%d' % k] = k
        k += 1
    before = _code_buffers()
    out.append(sum(map(sum, d['calls'](50))))
    return _code_buffers() - before


_SWAPPED = []


def swapped_then_resized():
    # two globals swapped every call, then the keys resized: the guards
    # whose failure already respawned are re-armed like the others, so the
    # calls after the resize compile no more than with nothing swapped (a
    # varying version taken for good compiled 7 buffers more). Twice, the
    # second time with these functions' own code compiled; once a process,
    # the later calls only returning it, as each round's code lives until
    # an owner scan.
    if not _SWAPPED:
        out = []
        for round in range(2):
            extra = _resized_growth(True, out) - _resized_growth(False, out)
        _SWAPPED.append((out, extra < 3))
    return _SWAPPED[0]


SRC_BUILTIN = """
def total(n):
    t = 0
    for i in range(n):
        t += len('ab')
    return t
def calls(n):
    return [total(k) for k in range(n)]
"""


def _global_fails():
    lejit = sys.modules.get('lejit')
    return lejit._core._stats()['global_fails'] if lejit is not None else 0


def _resized_builtin_fails(out):
    # failed guards in calls after one more name resizes the keys
    d = {'__builtins__': builtins}
    exec(SRC_BUILTIN, d)   # exec: this helper is not compiled
    out.append(sum(d['calls'](20)))
    size = sys.getsizeof(d)
    k = 0
    while sys.getsizeof(d) == size:
        d['_x%d' % k] = k
        k += 1
    out.append(sum(d['calls'](20)))
    before = _global_fails()
    out.append(sum(d['calls'](100)))
    return _global_fails() - before


def _resized_builtin_ok():
    out = []
    fails = 0
    for round in range(2):
        fails = fails + _resized_builtin_fails(out)
    return out, fails < 5


# hash seeds whose keys after the resize give range or len a probe of five
# slots or more: in 84 and 92 it comes back to a slot, in 1315 and 1399 the
# five are different, more than an absent guard shows
_LONG_PROBE_SEEDS = (84, 92, 1315, 1399)


def _resized_builtin_seeded():
    import os
    import subprocess
    src = ('import sys\nif sys.argv[1] == "1":\n    import lejit\n    lejit.start()\n'
           'import globals_entry\nprint(globals_entry._resized_builtin_ok())\n')
    out = []
    for seed in _LONG_PROBE_SEEDS:
        env = dict(os.environ, PYTHONHASHSEED=str(seed),
                   PYTHONPATH=os.pathsep.join(sys.path))
        p = subprocess.run([sys.executable, '-c', src,
                            '1' if 'lejit' in sys.modules else '0'],
                           env=env, stdout=subprocess.PIPE, timeout=120)
        out.append((seed, p.returncode, p.stdout.decode().strip()))
    return out


_BUILTIN_RESIZED = []


def builtin_after_resize():
    # range and len are builtins, guarded by their absence from the
    # module's globals; one more name resizes the keys, the guards fail
    # once and are shown again for the new keys, and the calls after it
    # don't fail them again (5051 times here when they stayed failing).
    # Twice, the second time with these functions' own code compiled; once
    # a process, as in swapped_then_resized. The same in children under
    # seeds that give a long probe after the resize, which this run's seed
    # gives only now and then.
    if not _BUILTIN_RESIZED:
        _BUILTIN_RESIZED.append((_resized_builtin_ok(),
                                 _resized_builtin_seeded()))
    return _BUILTIN_RESIZED[0]


CASES = [
    ('rebound_then_moved', [()], {}),
    ('shadowed_mid_loop', [(20,)], {}),
    ('builtin_replaced', [()], {}),
    ('wide_indices_1', [()], {}),
    ('wide_indices_2', [()], {}),
    ('wide_indices_4', [()], {}),
    ('loop_with_writes', [(50,), (7,)], {}),
    ('moved_entry', [()], {}),
    ('resized_keys', [()], {}),
    ('swapped_same_value', [()], {}),
    ('global_shadows_builtin', [()], {}),
    ('builtin_behind_deleted_global', [()], {}),
    ('replaced_module_dicts', [()], {}),
    ('cleared_and_refilled', [()], {}),
    ('equal_key_not_identical', [()], {}),
    ('split_dict_globals', [()], {}),
    ('swapped_then_resized', [()], {}),
    ('builtin_after_resize', [()], {}),
]
