"""pyperformance's pure-Python benchmarks (tools/pyperformance/, vendored
from pyperformance 1.11.0, MIT) under CPython 3.7 with and without Ni.

pyperformance itself won't run them on 3.7 (they declare 3.8+) and needs
pyperf; here a stand-in 'pyperf' module calls each benchmark's function
directly. The loop count is calibrated on CPython (about 0.2 s), then each
mode runs one warm-up and the best of five, in a fresh process.

    python3.7 tools/pyperf37.py              # table
    python3.7 tools/pyperf37.py -k richards  # names containing 'richards'

Use a release build, as for tools/bench.py.
"""
import argparse
import glob
import os
import subprocess
import sys
import time
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BMDIR = os.path.join(ROOT, 'tools', 'pyperformance')
TARGET = 0.2


class Runner:
    """the part of pyperf.Runner the benchmarks use"""

    def __init__(self, *a, **kw):
        self.metadata = {}
        self.argparser = argparse.ArgumentParser()
        self.benches = []

    def parse_args(self, args=None):
        return self.argparser.parse_args([])

    def bench_func(self, name, func, *args, **kw):
        def timed(loops):
            t = time.perf_counter()
            for _ in range(loops):
                func(*args)
            return time.perf_counter() - t
        self.benches.append((name, timed))

    def bench_time_func(self, name, func, *args, **kw):
        self.benches.append((name, lambda loops: func(loops, *args)))

    def bench_async_func(self, name, func, *args, **kw):
        import asyncio

        def timed(loops):
            t = time.perf_counter()
            for _ in range(loops):
                asyncio.get_event_loop().run_until_complete(func(*args))
            return time.perf_counter() - t
        self.benches.append((name, timed))


def fake_pyperf():
    m = types.ModuleType('pyperf')
    m.Runner = Runner
    m.perf_counter = time.perf_counter
    m.python_implementation = lambda: 'cpython'
    sys.modules['pyperf'] = m
    return m


def load(script):
    """run the script as __main__ with the stand-in; its Runner's benches"""
    made = []
    orig = Runner.__init__

    def init(self, *a, **kw):
        orig(self, *a, **kw)
        made.append(self)
    Runner.__init__ = init
    fake_pyperf()
    sys.argv = [script]
    code = compile(open(script).read(), script, 'exec')
    exec(code, {'__name__': '__main__', '__file__': script})
    return [b for r in made for b in r.benches]


def child(mode, script, loops):
    if mode == 'ni':
        sys.path.insert(0, ROOT)
        import ni  # noqa: F401
    out = {}
    for name, timed in load(script):
        n = loops.get(name)
        if n is None:          # calibrate
            n = 1
            while timed(n) < TARGET / 4 and n < 1 << 20:
                n *= 2
            n = max(1, int(n * TARGET / max(timed(n), 1e-6)))
        timed(n)               # warm-up (and compilation)
        out[name] = (n, min(timed(n) for _ in range(5)))
    print('RESULT ' + repr(out))


def run_child(mode, script, loops):
    p = subprocess.run([sys.executable, __file__, '--child', mode, script,
                        repr(loops)], stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True)
    lines = [l for l in p.stdout.splitlines() if l.startswith('RESULT ')]
    if p.returncode or not lines:
        return None, (p.stderr.strip().splitlines() or ['?'])[-1]
    return eval(lines[-1][7:]), None


def main():
    scripts = sorted(glob.glob(os.path.join(BMDIR, 'bm_*.py')))
    if '-k' in sys.argv:
        k = sys.argv[sys.argv.index('-k') + 1]
        scripts = [s for s in scripts if k in os.path.basename(s)]
    print('%-26s %9s %9s %8s' % ('', 'CPython', 'Ni', 'speed-up'))
    ratios = []
    for s in scripts:
        plain, err = run_child('plain', s, {})
        if plain is None:
            print('%-26s CPython failed: %s' % (os.path.basename(s), err))
            continue
        ni_res, err = run_child('ni', s, {k: v[0] for k, v in plain.items()})
        for name, (n, tp) in sorted(plain.items()):
            if ni_res is None:
                print('%-26s %8.3fs   Ni failed: %s' % (name, tp, err[:60]))
                continue
            nn, tn = ni_res[name]
            if nn != n:
                print('%-26s loop counts differ: %d, %d' % (name, n, nn))
                continue
            ratios.append(tp / tn)
            print('%-26s %8.3fs %8.3fs %7.2fx %6d' % (name, tp, tn, tp / tn, n))
    if ratios:
        g = 1.0
        for r in ratios:
            g *= r
        print('%-26s %28.2fx  (%d benchmarks)' % ('geometric mean', g ** (1 / len(ratios)), len(ratios)))


if __name__ == '__main__':
    if sys.argv[1:2] == ['--child']:
        child(sys.argv[2], sys.argv[3], eval(sys.argv[4]))
    else:
        main()
