# Runs one fuzz program under the CPython running it, plain or with LeJIT
# started for the whole process:  launch.py ROOT plain|lejit PROGRAM FAIL SHIFT
# With LeJIT it reports on stderr how many of the program's functions
# compiled. FAIL, SITE:N or -, makes the N-th allocation at SITE fail
# (lejit._core._fail_after, checked builds only; combine.py --fail).
# SHIFT first keeps that many floats alive, so the program's floats lie at
# other addresses (run.py:settle, for what a NaN's hash shows).
import runpy, sys, types
root, mode, prog, fail, shift = sys.argv[1:6]
_shift = [float(i) for i in range(int(shift))]
if mode == 'lejit':
    sys.path.insert(0, root)
    import lejit
    lejit.start()
    if fail != '-' and hasattr(lejit._core, '_fail_after'):
        site, n = fail.split(':')
        lejit._core._fail_after(site, int(n))
g = runpy.run_path(prog, run_name='__main__')
if mode == 'lejit':
    fs = [v for v in g.values() if isinstance(v, types.FunctionType)]
    n = sum(lejit._core._code_info(f.__code__) == 'compiled' for f in fs)
    sys.stderr.write('lejit-compiled %d/%d\n' % (n, len(fs)))
