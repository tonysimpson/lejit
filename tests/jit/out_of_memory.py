"""Compiled code running out of memory while it records a traceback entry
for an exception not yet raised (lejit._core._fail_after('traceback', n)):
the MemoryError replaces the exception, as it does for one already raised.

    python3.X tests/jit/out_of_memory.py
"""
import os, sys
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
