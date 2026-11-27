# Runs one test case in a fresh interpreter. Kept at module level on purpose:
# LeJIT doesn't compile module-level code, so the harness itself isn't compiled.
import sys, importlib
sys.path.insert(0, sys.argv[1])          # repo root (for lejit)
sys.path.insert(0, sys.argv[2])          # cases directory
mod = importlib.import_module(sys.argv[3])
fn = getattr(mod, sys.argv[4])
# list of argument tuples: a Python expression evaluated in the case module
calls = eval(sys.argv[5], dict(vars(mod), nan=float('nan'), inf=float('inf')))
# The objects check counts, in the same rounds as the leak check, the
# object allocator's allocations (testing.c:le_allocs_begin, through ctypes
# so the plain run needn't import lejit; not for a release build, which
# has none)
# and, by type, the objects the gc tracks that are alive after them against
# those alive before them, and whether the case called lejit._core's test
# functions meanwhile. Both start after the warm-up, so what compiling
# makes and keeps isn't counted.
import gc
allocs_begin = allocs_end = allocs_asked = None
counting = sys.argv[10] == '1'
if counting:
    import ctypes, importlib.machinery, os
    core = os.path.join(sys.argv[1], 'lejit',
                        '_core' + importlib.machinery.EXTENSION_SUFFIXES[0])
    try:
        core = ctypes.PyDLL(core)
        allocs_begin, allocs_end = core.le_allocs_begin, core.le_allocs_end
        allocs_asked = core.le_allocs_asked
        allocs_end.restype = allocs_asked.restype = ctypes.c_long
    except (OSError, AttributeError):
        allocs_begin = allocs_end = allocs_asked = None
# '0' plain, '1' a checked build of LeJIT, 'release' a release build, which
# can't say whether it compiled the function
use_lejit = sys.argv[6] != '0'
checked = sys.argv[6] == '1'
if use_lejit:
    import lejit
    if checked and not hasattr(lejit._core, '_stats'):
        sys.exit('lejit was not built for tests: build it with LEJIT_DEV_TRACE=1')
    lejit.start()
# run with locals apart from its globals, so LeJIT never compiles it; it
# names no object it counts, as loading a name bound to a lazy import's
# object (3.15) imports its module
COUNT_ALIVE = compile('''
alive = {}
for t in map(type, gc.get_objects()):
    name = t.__qualname__ if t.__module__ == 'builtins' else '%s.%s' % (t.__module__, t.__qualname__)
    alive[name] = alive.get(name, 0) + 1
del t, name
''', 'count_alive', 'exec')
counted = {}
out = []
for args in calls:
    try:
        r = fn(*args)
        out.append('ok ' + repr(r))
    except Exception as e:
        out.append('exc %s: %s' % (type(e).__name__, e))
if checked:
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
# Where emptying it also drops the types' version tags (before 3.11),
# compiled code that checks a tag compiles again on its next run, so the
# calls run once more after the cache is emptied, before the count starts.
clear_type_cache = sys._clear_type_cache if sys.argv[8] == '1' else (lambda: None)
for _ in range(int(sys.argv[7])):
    for args in calls:
        try:
            fn(*args)
        except Exception:
            pass
# In a checked build, if anything compiled during the rounds, they are run
# again (up to three times) and the last run counts: a branch first taken
# late (a thread switch or a signal landing at a check not reached before)
# compiles then and keeps its code, which is not a leak. A leak grows in
# every run. A case that compiles new code on every call always compiles
# during the rounds, so its rounds are run once.
max_repeats = 0 if sys.argv[9] == '1' else 3
repeats = 0
while True:
    clear_type_cache()
    gc.collect()
    if sys.argv[8] == '1':
        for args in calls:
            try:
                fn(*args)
            except Exception:
                pass
        gc.collect()
    buffers = lejit._core._stats()['code_buffers'] if checked else 0
    if counting:
        exec(COUNT_ALIVE, {'gc': gc}, counted)
        alive_before = counted['alive']
    before = sys.getallocatedblocks()
    gc.disable()
    if allocs_begin:
        allocs_begin()
    for _ in range(100):
        for args in calls:
            try:
                fn(*args)
            except Exception:
                pass
    allocs = allocs_end() if allocs_end else -1
    asked = allocs_asked() if allocs_asked else 0
    gc.enable()
    clear_type_cache()
    gc.collect()
    leak = sys.getallocatedblocks() - before
    if not checked or lejit._core._stats()['code_buffers'] == buffers or repeats == max_repeats:
        break
    repeats += 1
out.append('leak: %d %d' % (leak, repeats))
left = []
if counting:
    exec(COUNT_ALIVE, {'gc': gc}, counted)
    alive = counted['alive']
    left = sorted((name, alive.get(name, 0) - alive_before.get(name, 0))
                  for name in set(alive) | set(alive_before)
                  if alive.get(name, 0) != alive_before.get(name, 0))
out.append('objects: %d %d %r' % (allocs, asked, left))
for line in out:
    print(line)
