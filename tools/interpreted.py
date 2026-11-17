"""Which functions still run interpreted under Ni in each pyperformance
benchmark (tools/pyperformance), the most-called first, with ni._code_info's
reason. sys.setprofile sees only interpreted calls (PORT312.md, Tracing),
so every 'call' event it gets is one Ni didn't compile.

    python3.7 tools/interpreted.py              # every benchmark
    python3.7 tools/interpreted.py -k richards  # names containing 'richards'

Run each benchmark in its own process: the output is per benchmark script.
"""
import collections
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))


def child(script):
    sys.path.insert(0, ROOT)
    import ni
    import pyperf37
    ni._set_trace_fallback(False)     # else everything runs interpreted
    for name, timed in pyperf37.load(script):
        timed(2)                      # compile
        counts = collections.Counter()

        def prof(frame, event, arg):
            if event == 'call':
                counts[frame.f_code] += 1
        sys.setprofile(prof)
        timed(2)
        sys.setprofile(None)
        total = sum(counts.values())
        print('%s: %d interpreted calls' % (name, total))
        for co, n in counts.most_common(6):
            print('  %7d  %-28s %s:%d  %s' % (
                n, co.co_name[:28], os.path.basename(co.co_filename),
                co.co_firstlineno, ni._code_info(co)))


def main():
    scripts = sorted(glob.glob(os.path.join(ROOT, 'tools', 'pyperformance',
                                            'bm_*.py')))
    if '-k' in sys.argv:
        k = sys.argv[sys.argv.index('-k') + 1]
        scripts = [s for s in scripts if k in os.path.basename(s)]
    for s in scripts:
        p = subprocess.run([sys.executable, __file__, '--child', s],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=600)
        print(p.stdout.rstrip() or '%s: exit %d' % (s, p.returncode))


if __name__ == '__main__':
    if sys.argv[1:2] == ['--child']:
        child(sys.argv[2])
    else:
        main()
