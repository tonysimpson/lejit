# Compiled code still runs while the interpreter shuts down: here a
# finaliser freed with its thread state, after sys.modules is cleared,
# promotes on a new class at a megamorphic call site, which asks
# codemanager.c:looks_short_lived for the class's module. Run in a child,
# once: the child must print both lines and exit 0, as plain CPython does.

CHILD = r'''
import ctypes, os, sys, threading
if sys.argv[1] == '1':
    import lejit
    lejit.start()


def kind(o):
    return o.tag()


def new_class(make=type):
    return make('K', (), {'tag': lambda self: 1, '__module__': 'm'})


for _ in range(40):
    kind(new_class()())


class Late:
    # module globals and builtins are gone by then: keep what it needs as
    # defaults
    def __del__(self, kind=kind, new_class=new_class, w=os.write):
        kind(new_class()())
        w(1, b'finaliser done\n')


local = threading.local()
local.late = Late()
# a reference nobody drops keeps the local past module teardown; its
# per-thread dict, and Late, is freed when the thread state is cleared
ctypes.pythonapi.Py_IncRef(ctypes.py_object(local))
print('ok', flush=True)
'''

_result = []


def promotes_after_modules_cleared():
    if not _result:
        import os
        import subprocess
        import sys
        env = dict(os.environ, PYTHONPATH=os.pathsep.join(sys.path))
        p = subprocess.run([sys.executable, '-c', CHILD,
                            '1' if 'lejit' in sys.modules else '0'],
                           env=env, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL, timeout=120)
        _result.append((p.returncode, p.stdout.decode().split()))
    return _result[0]


CASES = [
    ('promotes_after_modules_cleared', [()], {}),
]
