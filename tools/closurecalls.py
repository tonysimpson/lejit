"""How often each pyperformance benchmark calls a closure (a function with
free variables) straight from Python code, per warm iteration, measured
under plain CPython with sys.setprofile (doc/FINDINGS.md).

A call counts as direct when the caller has no C function running (no
'c_call' without its 'c_return'), so map(), sorted(key=) and the like don't
count, and when the caller's instruction is a CALL_*, not an operator (a
slot such as __add__) or a class instantiation (__init__ or __new__ under
a CALL_*, which type_call makes): those are the calls a CALL_* in compiled
code makes itself. The other closure calls are printed apart. Also
counted: callees with cells of their own (psyfunc.c:fncall_init refuses
those too) and all direct calls, for scale.

    python3.7 tools/closurecalls.py              # every benchmark
    python3.7 tools/closurecalls.py -k richards  # names containing 'richards'
"""
import collections
import dis
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))


def child(script, iters):
    import pyperf37
    for name, timed in pyperf37.load(script):
        timed(2)                      # warm, as under Ni
        cdepth = collections.Counter()
        kinds = collections.Counter()
        closures = collections.Counter()
        others = collections.Counter()

        def prof(frame, event, arg):
            if event == 'call':
                if frame.f_lasti != -1:   # a generator resumed
                    return
                back = frame.f_back
                if back is None or cdepth[back]:
                    return
                co = frame.f_code
                op = dis.opname[back.f_code.co_code[back.f_lasti]]
                if (not op.startswith('CALL_') or
                        co.co_name in ('__init__', '__new__')):
                    if co.co_freevars:
                        others[op, co.co_name] += 1
                    return
                kinds['all'] += 1
                if co.co_freevars:
                    kinds['closure'] += 1
                    closures[co] += 1
                elif co.co_cellvars:
                    kinds['cells'] += 1
            elif event == 'c_call':
                cdepth[frame] += 1
            elif event in ('c_return', 'c_exception'):
                cdepth[frame] -= 1
        sys.setprofile(prof)
        timed(iters)
        sys.setprofile(None)
        print('%s: per iteration %d direct calls, %d to closures, '
              '%d to code with cells' % (
                  name, kinds['all'] // iters, kinds['closure'] // iters,
                  kinds['cells'] // iters))
        for co, n in closures.most_common(5):
            print('  %9d  %-28s %s:%d  free %s' % (
                n // iters, co.co_name[:28], os.path.basename(co.co_filename),
                co.co_firstlineno, ','.join(co.co_freevars)))
        for (op, fn), n in others.most_common(3):
            print('  %9d  not from a call: %s under %s' % (n // iters, fn, op))


def main():
    scripts = sorted(glob.glob(os.path.join(ROOT, 'tools', 'pyperformance',
                                            'bm_*.py')))
    if '-k' in sys.argv:
        k = sys.argv[sys.argv.index('-k') + 1]
        scripts = [s for s in scripts if k in os.path.basename(s)]
    for s in scripts:
        p = subprocess.run([sys.executable, __file__, '--child', s, '2'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=900)
        print(p.stdout.rstrip() or '%s: exit %d' % (s, p.returncode))


if __name__ == '__main__':
    if sys.argv[1:2] == ['--child']:
        child(sys.argv[2], int(sys.argv[3]))
    else:
        main()
