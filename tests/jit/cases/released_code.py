# Compiled code not entered for a few sweeps is released while its code
# object lives, and compiled afresh at the next call (codemanager.c, the
# sweep, run at full collections). Code that a running frame is in, or
# that code which stays calls directly, stays; code objects that only call
# each other go together. A released generator's frame resumes as before.
import gc
import sys


def sweeps():
    lejit = sys.modules.get('lejit')
    if lejit is None or not hasattr(lejit._core, '_stats'):
        return None
    return lejit._core._stats()['sweeps'][1]


def released_since(before):
    # True if code was released (or LeJIT isn't there to release any)
    after = sweeps()
    return before is None or after > before


def collect(n=8):
    for _ in range(n):
        gc.collect()


def add_up(n):
    t = 0
    for i in range(n):
        t += i * i
    return t


def even(n):
    return True if n == 0 else odd(n - 1)


def odd(n):
    return False if n == 0 else even(n - 1)


def leaf(x):
    return x * 3 + 1


def caller(n):
    t = 0
    for i in range(n):
        t += leaf(i)
    return t


def counter(n):
    for i in range(n):
        yield i * 2


SRC = '''
def add_up(n):
    t = 0
    for i in range(n):
        t += i * i
    return t
def even(n):
    return True if n == 0 else odd(n - 1)
def odd(n):
    return False if n == 0 else even(n - 1)
'''


def called_again_after_release():
    # fresh code objects, so that each call sees them released once
    ns = {}
    exec(SRC, ns)
    before = sweeps()
    a = [ns['add_up'](k) for k in range(5, 50, 5)]
    collect()
    b = [ns['add_up'](k) for k in range(5, 50, 5)]
    return a == b, b, released_since(before)


def cold_recursion_goes_together():
    ns = {}
    exec(SRC, ns)
    before = sweeps()
    a = [ns['even'](k) for k in range(30)]
    collect()
    b = [ns['odd'](k) for k in range(30)]
    return a, b, released_since(before)


def running_frame_stays():
    # this frame runs through every sweep; its loop goes on in its code
    t = 0
    for k in range(6):
        gc.collect()
        for i in range(20):
            t += i ^ k
    return t


def direct_callee_stays():
    # caller's code calls leaf's directly and runs between the sweeps
    t = 0
    for k in range(10):
        t += caller(k + 5)
        gc.collect()
    return t


def paused_generator_resumes():
    it = counter(10)
    first = [next(it), next(it)]
    collect()
    return first, list(it)


def generator_stepped_across_sweeps():
    # stepped from this frame's compiled code while the sweeps run
    t = []
    for x in counter(12):
        if x % 4 == 0:
            gc.collect()
        t.append(x)
    return t


def released_and_compiled_again_repeatedly():
    # after a few releases the code stays compiled
    out = []
    for _ in range(6):
        out.append(add_up(30) + caller(7) + even(9))
        collect(6)
    return out


FRESH = 'compiles fresh code objects on every call'
CASES = [
    ('called_again_after_release', [()],
     {'compiles_each_call': FRESH, 'interpreted': 'calls exec'}),
    ('cold_recursion_goes_together', [()],
     {'compiles_each_call': FRESH, 'interpreted': 'calls exec'}),
    ('running_frame_stays', [()], {}),
    ('direct_callee_stays', [()], {}),
    ('paused_generator_resumes', [()], {}),
    ('generator_stepped_across_sweeps', [()], {}),
    ('released_and_compiled_again_repeatedly', [()], {}),
]
