# Compiled code jumps between code regions with rel32, so a new region must
# be placed within 2 GB of all the others (platform.c). Here the program maps
# a little over 2 GB after the first region exists, where the kernel would
# put the next one by default, and then compiles more code than the first
# region holds. Run in a child, once: the mapping is never touched.

CHILD = r'''
import mmap, sys
if sys.argv[1] == '1':
    import lejit
    lejit.start()


def warm(n):
    return n + 1


warm(1)
block = mmap.mmap(-1, 2200 << 20)
N = 40000
src = '\n'.join('def f%d(x):\n    return x * %d + len(str(x))' % (i, i)
                for i in range(N))
ns = {}
exec(src, ns)
print(sum(ns['f%d' % i](i) for i in range(N)))
'''

_result = []


def far_mapping():
    if not _result:
        import os
        import subprocess
        import sys
        env = dict(os.environ, PYTHONPATH=os.pathsep.join(sys.path))
        p = subprocess.run([sys.executable, '-c', CHILD,
                            '1' if 'lejit' in sys.modules else '0'],
                           env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=300)
        _result.append((p.returncode, p.stdout.decode().split()))
    return _result[0]


CASES = [
    ('far_mapping', [()], {}),
]
