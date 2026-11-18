"""How often attribute load guards fail, per pyperformance
benchmark, or for a script that defines main():

    NI_CODESTATS=1 python3.7 setup.py build_ext --inplace
    python3.7 tools/loadguards.py richards deltablue ...
    python3.7 tools/loadguards.py path/to/script.py

Warms each benchmark (three iterations, then until compiling stops), then
counts over ten more. Columns: load guard sites compiled; guards failed
while warming and while warm, and of those how many found the class giving
what some site of (type, name) was compiled for (so a repair could re-arm
it) and how many were on a class that can't have a version tag (a
metaclass: CPython 3.7 clears Py_TPFLAGS_HAVE_VERSION_TAG) or whose
tp_getattro isn't generic (a load from a class whose metaclass is a
Python class, or from an instance of a class with __getattr__), where the
guard never passes; failure branches compiled (respawns), their bytes and
compile ms, and all respawns' compile ms for comparison. Each benchmark in its own process.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def child(target):
    sys.path.insert(0, ROOT)
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import ni
    if 'load_guards' not in ni._stats():
        sys.exit('build with NI_CODESTATS=1 (see the docstring)')
    if target.endswith('.py'):
        ns = {'__name__': 'loadguards_target'}
        with open(target) as f:
            exec(compile(f.read(), target, 'exec'), ns)
        run = ns['main']
    else:
        import pyperf37
        _, timed = pyperf37.load(os.path.join(
            ROOT, 'tools', 'pyperformance', 'bm_%s.py' % target))[0]
        run = lambda: timed(1)  # noqa: E731
    for _ in range(3):
        run()
    last = -1
    for _ in range(50):
        run()
        now = ni._stats()['code_bytes']
        if now == last:
            break
        last = now
    s0 = ni._stats()
    for _ in range(10):
        run()
    s1 = ni._stats()
    g0, g1 = s0['load_guards'], s1['load_guards']
    print('%-10s %6d %8d %8d %8d %8d %8d %8d %6d %8d %7.2f %7.2f' % (
        os.path.basename(target)[:10], g1[0], g0[1], g0[2],
        (g1[1] - g0[1]) // 10, (g1[2] - g0[2]) // 10,
        (g1[6] - g0[6]) // 10, (g1[7] - g0[7]) // 10, g1[3], g1[5], g1[4] * 1e-6,
        s1['respawn'][1] * 1e3))


def main():
    if sys.argv[1:2] == ['--child']:
        child(sys.argv[2])
        return
    print('%-10s %6s %8s %8s %8s %8s %8s %8s %6s %8s %7s %7s' % (
        '', 'sites', 'fail-c', 'same-c', 'fail/it', 'same/it', 'notag/it',
        'getat/it', 'resp', 'bytes', 'ms', 'all-ms'))
    for t in sys.argv[1:]:
        sys.stdout.flush()
        subprocess.call([sys.executable, __file__, '--child', t],
                        timeout=900)


main()
