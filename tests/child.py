# Runs one test case in a fresh interpreter. Kept at module level on purpose:
# Ni doesn't compile module-level code, so the harness itself isn't compiled.
import sys, importlib
sys.path.insert(0, sys.argv[1])          # repo root (for ni)
sys.path.insert(0, sys.argv[2])          # cases directory
mod = importlib.import_module(sys.argv[3])
fn = getattr(mod, sys.argv[4])
# list of argument tuples: a Python expression evaluated in the case module
calls = eval(sys.argv[5], dict(vars(mod), nan=float('nan'), inf=float('inf')))
use_ni = sys.argv[6] == '1'
if use_ni:
    import ni
out = []
for args in calls:
    try:
        r = fn(*args)
        out.append('ok ' + repr(r))
    except Exception as e:
        out.append('exc %s: %s' % (type(e).__name__, e))
if use_ni:
    code = fn.__code__
    if code not in ni.co_to_mp:
        out.append('compiled: no (never seen by the hook)')
    elif ni.co_to_mp[code] is None:
        out.append('compiled: no (unsupported opcode or flags)')
    elif ni.co_to_entry_point.get(code) is None:
        out.append('compiled: no (no entry point)')
    else:
        out.append('compiled: yes')
# leak check: after warm-up, run the calls 100 more times and report how
# many more memory blocks are allocated than before
import gc
gc.collect()
before = sys.getallocatedblocks()
for _ in range(100):
    for args in calls:
        try:
            fn(*args)
        except Exception:
            pass
gc.collect()
out.append('leak: %d' % (sys.getallocatedblocks() - before))
for line in out:
    print(line)
