# Runs one test case in a fresh interpreter. Kept at module level on purpose:
# LeJIT doesn't compile module-level code, so the harness itself isn't compiled.
import sys, importlib
sys.path.insert(0, sys.argv[1])          # repo root (for lejit)
sys.path.insert(0, sys.argv[2])          # cases directory
mod = importlib.import_module(sys.argv[3])
fn = getattr(mod, sys.argv[4])
# list of argument tuples: a Python expression evaluated in the case module
calls = eval(sys.argv[5], dict(vars(mod), nan=float('nan'), inf=float('inf')))
use_lejit = sys.argv[6] == '1'
if use_lejit:
    import lejit
    if not hasattr(lejit._core, '_stats'):
        sys.exit('lejit was not built for tests: build it with LEJIT_DEV_TRACE=1')
    lejit.start()
out = []
for args in calls:
    try:
        r = fn(*args)
        out.append('ok ' + repr(r))
    except Exception as e:
        out.append('exc %s: %s' % (type(e).__name__, e))
if use_lejit:
    info = lejit._core._code_info(fn.__code__)
    out.append('compiled: yes' if info == 'compiled' else
               'compiled: no (%s)' % info)
# leak check: after warm-up, run the calls 100 more times and report how
# many more memory blocks are allocated than before. The warm-up is a few
# rounds (9 unless the case says), not one: compiling can go on for a few
# calls (a call site that sees a new function each time specialises until
# it goes megamorphic), and that bounded growth is
# tests/jit/bounds.py's business. The collector is off for the 100 rounds
# and runs at their end: where it is scheduled through eval_breaker, a
# collection starting in compiled code takes that test's rare path, which
# is compiled when first taken, so which of them compile during the rounds
# would depend on when collections happen.
# The type cache keeps the attribute names it was last asked for, by slot,
# so a name made afresh for each lookup (PyObject_CallMethod's) stays alive
# until another lookup takes its slot: an amount that differs from process
# to process. A case that makes such names can ask for the cache emptied.
import gc
clear_type_cache = sys._clear_type_cache if sys.argv[8] == '1' else (lambda: None)
for _ in range(int(sys.argv[7])):
    for args in calls:
        try:
            fn(*args)
        except Exception:
            pass
clear_type_cache()
gc.collect()
before = sys.getallocatedblocks()
gc.disable()
for _ in range(100):
    for args in calls:
        try:
            fn(*args)
        except Exception:
            pass
gc.enable()
clear_type_cache()
gc.collect()
out.append('leak: %d' % (sys.getallocatedblocks() - before))
for line in out:
    print(line)
