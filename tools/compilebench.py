"""Compile cost against pay-off, on pyperformance's pure-Python benchmarks
(tools/pyperformance/, run as in tools/pyperf37.py), each in a fresh process:

  first   the first iteration: on Ni this includes compiling
  warm    an iteration once everything is compiled (best of five runs)
  compile time Ni spent compiling over the whole run, from ni._stats(),
          and the code it emitted
  even    iterations before Ni is ahead of CPython overall:
          compile time / (CPython warm - Ni warm)

    python3.7 tools/compilebench.py              # table
    python3.7 tools/compilebench.py -k richards  # names containing 'richards'
    python3.7 tools/compilebench.py -v           # compile time by entry kind

Use a release build, as for tools/bench.py.
"""
import glob
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import pyperf37  # noqa: E402

KINDS = ('analyse', 'call', 'pause', 'respawn', 'promotion')


def stats():
    import ni
    return ni._stats()


def child(mode, script):
    if mode == 'ni':
        sys.path.insert(0, ROOT)
        import ni  # noqa: F401
    benches = pyperf37.load(script)
    out = {}
    for name, timed in benches:
        s0 = stats() if mode == 'ni' else None
        first = timed(1)
        n = 1
        while timed(n) < pyperf37.TARGET / 4 and n < 1 << 20:
            n *= 2
        warm = min(timed(n) for _ in range(5)) / n
        r = {'first': first, 'warm': warm}
        if mode == 'ni':
            s1 = stats()
            r['bytes'] = s1['code_bytes'] - s0['code_bytes']
            r['kinds'] = {k: (s1[k][0] - s0[k][0], s1[k][1] - s0[k][1])
                          for k in KINDS}
        out[name] = r
    print('RESULT ' + repr(out))


def run_child(mode, script):
    p = subprocess.run([sys.executable, __file__, '--child', mode, script],
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       universal_newlines=True)
    lines = [l for l in p.stdout.splitlines() if l.startswith('RESULT ')]
    if p.returncode or not lines:
        return None, (p.stderr.strip().splitlines() or ['?'])[-1]
    return eval(lines[-1][7:]), None


def ms(t):
    return '%8.2f' % (t * 1e3)


def main():
    scripts = sorted(glob.glob(os.path.join(pyperf37.BMDIR, 'bm_*.py')))
    if '-k' in sys.argv:
        k = sys.argv[sys.argv.index('-k') + 1]
        scripts = [s for s in scripts if k in os.path.basename(s)]
    verbose = '-v' in sys.argv
    print('%-24s %17s  %17s  %8s %7s %8s %6s' % (
        '', 'CPython (ms)', 'Ni (ms)', 'compile', 'code', 'compile', 'even'))
    print('%-24s %8s %8s  %8s %8s  %8s %7s %8s %6s' % (
        '', 'first', 'warm', 'first', 'warm', '(ms)', '(KB)', 'share', '(its)'))
    tot_compile = tot_bytes = 0
    for s in scripts:
        plain, err = run_child('plain', s)
        if plain is None:
            print('%-24s CPython failed: %s' % (os.path.basename(s), err))
            continue
        res, err = run_child('ni', s)
        for name, p in sorted(plain.items()):
            if res is None or name not in res:
                print('%-24s Ni failed: %s' % (name, (err or '?')[:60]))
                continue
            r = res[name]
            ct = sum(v[1] for v in r['kinds'].values())
            tot_compile += ct
            tot_bytes += r['bytes']
            gain = p['warm'] - r['warm']
            even = '%6d' % max(1, -(-ct // gain)) if gain > 0 else '     -'
            share = ct / r['first'] if r['first'] else 0
            print('%-24s %s %s  %s %s  %s %7.1f %7.0f%% %s' % (
                name, ms(p['first']), ms(p['warm']), ms(r['first']),
                ms(r['warm']), ms(ct), r['bytes'] / 1024, 100 * share, even))
            if verbose:
                print('    ' + ', '.join('%s %d in %.2f ms' % (k, c, t * 1e3)
                                         for k, (c, t) in r['kinds'].items() if c))
    if tot_bytes:
        print('total compile %.1f ms for %.0f KB: %.0f KB/ms' % (
            tot_compile * 1e3, tot_bytes / 1024,
            tot_bytes / 1024 / max(tot_compile * 1e3, 1e-9)))


if __name__ == '__main__':
    if sys.argv[1:2] == ['--child']:
        child(sys.argv[2], sys.argv[3])
    else:
        main()
