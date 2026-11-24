# A code region lies wherever the kernel puts it, and a jump to code out of
# rel32 reach goes through a far jump (codemanager.c:psyco_code_far_jump).
# Here the program maps a little over 2 GB after the first region exists,
# where the kernel puts the next one, and then compiles more code than the
# first region holds, so code in the second jumps to the first through far
# jumps. A promotion site's chain made in the first region then grows into
# the second (dispatcher.c:promotion_need_chain), once the function's other
# sites have filled its stub pool's chunk in the first. Run in a child,
# once: the mapping is never touched.

CHILD = r'''
import mmap, sys
if sys.argv[1] == '1':
    import lejit
    lejit.start()


def warm(n):
    return n + 1


classes = [type('C%d' % i, (), {'v': i}) for i in range(5)]
objs = [c() for c in classes]


def values(i, b, c, d, e, f, g):
    return objs[i].v + b.v + c.v + d.v + e.v + f.v + g.v


warm(1)
first = objs[0]
print(values(0, first, first, first, first, first, first))
print(values(1, first, first, first, first, first, first))
block = mmap.mmap(-1, 2200 << 20)
N = 40000
src = '\n'.join('def f%d(x):\n    return x * %d + len(str(x))' % (i, i)
                for i in range(N))
ns = {}
exec(src, ns)
print(sum(ns['f%d' % i](i) for i in range(N)))
for o in objs[1:]:
    print(values(0, o, o, o, o, o, o))
print(values(2, first, first, first, first, first, first))
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
