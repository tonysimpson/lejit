"""LeJIT starting in a process that can't start a thread: where the thread
state is a thread-local, finding its offset from the thread pointer
starts a thread, so that this fails gives the slower way of reading the
state, which must work too.

    python3.X tests/jit/no_threads.py
"""
import os, resource, sys, threading
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

# no new process or thread for this user from here on
resource.setrlimit(resource.RLIMIT_NPROC, (0, 0))
try:
    t = threading.Thread(target=lambda: None)
    t.start()
    t.join()
    print('no_threads.py: skipped, a thread starts anyway (root?)')
    sys.exit(0)
except RuntimeError:
    pass

import lejit
if not hasattr(lejit._core, '_fail_after'):
    sys.exit('lejit was not built for tests: build it with LEJIT_DEV_TRACE=1')
lejit.start()


def total(n):
    s = 0
    for i in range(n):
        s += i * i
    return s


got = [total(k) for k in range(20)][-1]
if got != sum(i * i for i in range(19)):
    sys.exit('no_threads.py: got %r' % (got,))
