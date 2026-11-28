"""Compiled code running out of memory while it records a traceback entry
for an exception not yet raised (lejit._core._fail_after('traceback', n)):
the MemoryError replaces the exception, as it does for one already raised.

Freeing compiled code with no memory to list what it frees ('freelist') or
to move a group's references to its owner ('pins'): that code memory is
never used again and those references are kept for good, and nothing fails.

    python3.X tests/jit/out_of_memory.py
"""
import gc, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
import lejit
if not hasattr(lejit._core, '_fail_after'):
    sys.exit('lejit was not built for tests: build it with LEJIT_DEV_TRACE=1')
lejit.start()


def item(i):
    return [1, 2][i]


def catch(i):
    try:
        return item(i)
    except IndexError:
        return 'IndexError'
    except MemoryError:
        return 'MemoryError'


for k in range(50):
    catch(k % 3)
lejit._core._fail_after('traceback', 1)
got = catch(2)
left = lejit._core._fail_after('traceback', 0)
after = catch(2)
if left != 0 or (got, after) != ('MemoryError', 'IndexError'):
    sys.exit('out of memory in a traceback entry: got %r then %r (site %s)'
             % (got, after, 'fired' if left == 0 else 'not reached'))


SRC = 'def f(a):\n    t = 0\n    for i in range(a):\n        t += i + %d\n    return t\n'


def compile_some(n):
    spaces = []
    for k in range(n):
        ns = {}
        exec(SRC % k, ns)
        ns['f'](10)
        spaces.append(ns)
    return spaces


# every other one, so that what is freed lies between live code
spaces = compile_some(40)
lejit._core._fail_after('freelist', 1)
for ns in spaces[::2]:
    ns.clear()
gc.collect()
left = lejit._core._fail_after('freelist', 0)
if left != 0:
    sys.exit('freeing with no room in the free list: site not reached')
if [ns['f'](10) for ns in spaces[1::2]] != [45 + 10 * k for k in range(1, 40, 2)]:
    sys.exit('freeing with no room in the free list: wrong results after')
if [ns['f'](10) for ns in compile_some(5)] != [45 + 10 * k for k in range(5)]:
    sys.exit('freeing with no room in the free list: wrong results compiling after')


def use(c):
    t = 0
    for i in range(20):
        t += c().m()
    return t


def make(k):
    class C:
        def m(self):
            return k
    return C


# each class gets a group, freed when the class dies
classes = [make(k) for k in range(10)]
results = [use(c) for c in classes]
kept = lejit._core._stats()['code_live'][1]
lejit._core._fail_after('pins', 1)
del classes
gc.collect()
left = lejit._core._fail_after('pins', 0)
if left != 0 or lejit._core._stats()['code_live'][1] != kept + 1:
    sys.exit('freeing a group with no room for its references: %s'
             % ('site not reached' if left else 'not kept for good'))
if [use(make(k)) for k in range(10)] != results:
    sys.exit('freeing a group with no room for its references: wrong results after')
