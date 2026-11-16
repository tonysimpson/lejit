# Compiled code objects must be able to die (KNOWN_BUGS C14). Ni's
# snapshots held their code object, and the code object's co_extra held the
# snapshots, so a compiled code object was immortal. With globals that
# outlive it (here a shared dict), it now dies as in CPython. With globals
# of its own (exec into a new dict, namedtuple) Ni still keeps both alive.
import gc
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


CASES = [
    ('code_dies_with_shared_globals', [()], {'interpreted': 'calls exec (C15)'}),
]
