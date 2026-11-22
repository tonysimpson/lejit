"""Runs CPython's own test modules plain and with LeJIT; the counts and exit
status must match. Every test_* module of the interpreter's test package
runs except BANNED, and every test except SKIPPED.

    python3.X tests/cpython/run.py [-j N] [module ...]
"""
import fnmatch, json, os, resource, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import ROOT

TIMEOUT = 600                   # seconds per run
MEMORY_LIMIT = 4 << 30          # bytes of address space per run

# reason: names (fnmatch patterns); a name no version has is fine
BANNED = {
    "can't run here, with or without LeJIT":
        'test_curses test_idle test_tcl test_tix test_tk test_tkinter test_ttk test_ttk_* test_turtle '
        'test_*net test_nntplib test_ssl test_timeout test_codecmaps_* test_normalization '
        'test_win* test_msilib test_startfile test_osx_env test_kqueue test_devpoll test_ossaudiodev '
        'test_big* test_largefile test_zipfile64 test_multiprocessing_* test_concurrent_futures '
        'test_support test_script_helper test_regrtest test_crashers test_gdb test_check_c_globals',
    "compiled frames aren't kept up to date (line numbers, locals, stacks)":
        'test_sys test_frame test_traceback test_tracemalloc test_warnings test_logging '
        'test_coroutines test_asyncio test_compile test_raise test_builtin '
        'test_doctest test_capi',
    'compiled code holds what it specialised on':
        'test_weakref test_gc test_module test_descr',
    'sys.getrefcount counts differently':
        'test_generators test_ctypes test_memoryio test_socket',
    'tracing that starts in a running frame':
        'test_sys_settrace test_pdb',
    'sys.monitoring with a frame-evaluation hook installed '
    '(_testinternalcapi.set_eval_frame_record fails it the same way)':
        'test_monitoring',
    'marshal of a code object with one reference': 'test_importlib',
    'timing': 'test_email',
    "SSL tests that race on OpenSSL 3, which the CI images' older CPythons predate":
        'test_poplib',
}

# reason: single tests (module.Class.method) skipped in both runs
SKIPPED = {
    "CPython doesn't specialise Python calls, __getitem__ and properties "
    'while a frame-evaluation hook is installed (specialize.c, '
    'SPEC_FAIL_CALL_PEP_523)':
        'test_dis.DisTests.test_loop_quicken '
        'test_dis.DisWithFileTests.test_loop_quicken '
        'test_type_cache.TypeCacheWithSpecializationTests.'
        'test_binary_subscript_specialization_user_type '
        'test_type_cache.TypeCacheWithSpecializationTests.'
        'test_property_load_attr_specialization_user_type',
    "compiled code doesn't specialise the bytecode it runs, which ceval does "
    'as it runs it':
        'test_type_cache.TypeCacheWithSpecializationTests.'
        'test_load_method_specialization_user_type',
    'CPython counts each frame a frame-evaluation hook is given as a C call '
    'too, so a Python recursion 90,000 deep passes the C recursion limit '
    '(_testinternalcapi.set_eval_frame_record fails it the same way)':
        'test_call.TestRecursion.test_super_deep',
    'skipped when the machine converts fast enough':
        'test_int.IntStrDigitLimitsTests.test_denial_of_service_prevented_int_to_str '
        'test_int.IntStrDigitLimitsTests.test_denial_of_service_prevented_str_to_int '
        'test_int.IntSubclassStrDigitLimitsTests.test_denial_of_service_prevented_int_to_str '
        'test_int.IntSubclassStrDigitLimitsTests.test_denial_of_service_prevented_str_to_int',
    "code LeJIT compiles doesn't run in ceval, so ceval doesn't quicken it":
        'test_dis.DisTests.test_super_instructions '
        'test_dis.DisWithFileTests.test_super_instructions '
        'test_dis.DisTests.test_show_caches '
        'test_dis.DisWithFileTests.test_show_caches '
        'test_type_cache.TypeCacheWithSpecializationTests.'
        'test_load_attr_specialization_user_type '
        'test_type_cache.TypeCacheWithSpecializationTests.'
        'test_store_attr_specialization_user_type',
    "a compiled call of a type calls its tp_new and tp_init, not its "
    'vectorcall, so their argument errors read differently':
        'test_super.TestSuper.test_super_argcount '
        'test_super.TestSuper.test_super_argtype',
    "a respawn into a with statement's handler reaches WITH_EXCEPT_START "
    "with a short stack (checked builds' assertion)":
        'test_unparse.UnparseTestCase.test_invalid_yield_from',
}

# Where a gc callback that fails at the recursion limit leaves the limit off,
# so CPython aborts at it, LeJIT's own callback makes these abort.
GC_CALLBACK_ABORTS = 'test_json'
GC_CALLBACK_PROBE = r'''
import gc, sys
gc.callbacks.append(lambda phase, info: None)
exec('def f(n, top):\n'
     '    if not top - n:\n'
     '        keep = (' + '{n}, ' * 800 + ')\n'
     '    f(n + 1, top)\n')
limit = sys.getrecursionlimit()
for top in range(limit - 10, limit + 10):
    try:
        f(0, top)
    except RecursionError:
        pass
'''


def gc_callback_aborts():
    return subprocess.run([sys.executable, '-c', GC_CALLBACK_PROBE],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                          timeout=TIMEOUT).returncode < 0


# Prints a JSON list of the first failing tests, each its id and the end of
# its traceback, then the counts.
CHILD = r'''
import sys, unittest, io, json
if sys.argv[1] == 'lejit':
    sys.path.insert(0, sys.argv[3]); import lejit; lejit.start()
mod = __import__('test.' + sys.argv[2], fromlist=['x'])
# a package's tests may be in its module of the same name (test_inspect)
try:
    sub = __import__('test.%s.%s' % (sys.argv[2], sys.argv[2]), fromlist=['x'])
except ImportError:
    sub = None
for name in sys.argv[4:]:
    cls, meth = name.split('.')
    cls = getattr(mod, cls, None) or getattr(sub, cls, None)
    if hasattr(cls, meth):
        setattr(cls, meth, unittest.skip('skipped by run.py')(getattr(cls, meth)))
suite = unittest.defaultTestLoader.loadTestsFromModule(mod)
r = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
bad = r.failures + r.errors
print(json.dumps([[t.id()] + [l[:200] for l in tb.splitlines()[-10:]]
                  for t, tb in bad[:10]] +
                 [['and %d more' % (len(bad) - 10)]] * (len(bad) > 10)))
print('run=%d fail=%d err=%d skip=%d' % (r.testsRun, len(r.failures),
                                         len(r.errors), len(r.skipped)))
sys.stdout.flush()
'''


def limit_memory():
    # a module that runs away under LeJIT gets MemoryError instead of taking
    # the machine
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))


def stderr_tail(err):
    """The last lines of a run's stderr (bytes), cut to a bounded width."""
    return ['stderr:'] + ['    ' + l[:200] for l in
                          err.decode(errors='replace').splitlines()[-10:]]


def run(mode, name):
    """The run's counts and exit status, and lines that say why tests failed."""
    with tempfile.TemporaryDirectory(prefix='le_cpython_tests_') as cwd, \
            tempfile.TemporaryFile() as errf:
        skip = [t.split('.', 1)[1] for ts in SKIPPED.values() for t in ts.split()
                if t.split('.', 1)[0] == name]
        p = subprocess.Popen([sys.executable, '-c', CHILD, mode, name, ROOT] + skip,
                             stdout=subprocess.PIPE, stderr=errf,
                             stdin=subprocess.DEVNULL, cwd=cwd,
                             preexec_fn=limit_memory)
        try:
            out, _ = p.communicate(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate()
            out = None
        size = errf.seek(0, os.SEEK_END)
        errf.seek(max(0, size - 4096))
        err = errf.read()
    if out is None:
        return 'timeout', stderr_tail(err)
    out = out.decode().strip().splitlines()
    counted = out[-1:] and out[-1].startswith('run=')
    why = []
    if counted and len(out) > 1:
        try:
            for test in json.loads(out[-2]):
                why += test[:1] + ['    ' + l for l in test[1:]]
        except (ValueError, TypeError):
            pass
    if not counted or p.returncode:
        why += stderr_tail(err)
    return '%s exit=%d' % (out[-1] if out else '(no output)', p.returncode), why


def check(name):
    (plain, plain_why), (with_lejit, lejit_why) = run('plain', name), run('lejit', name)
    if plain == with_lejit:
        return True, 'ok     %-24s %s' % (name, plain)
    report = ['FAIL   %-24s %s   with LeJIT: %s' % (name, plain, with_lejit)]
    for side, why in ('plain', plain_why), ('with LeJIT', lejit_why):
        if why:
            report += ['       %s:' % side] + ['         ' + l for l in why]
    return False, '\n'.join(report)


def modules():
    try:
        import test
    except ImportError:
        sys.exit("%s has no test package (CPython's tests)" % sys.executable)
    d = os.path.dirname(test.__file__)
    names = sorted({n[:-3] if n.endswith('.py') else n for n in os.listdir(d)
                    if n.startswith('test_') and
                    (n.endswith('.py') or os.path.isdir(os.path.join(d, n)))})
    banned = [p for ps in BANNED.values() for p in ps.split()]
    if gc_callback_aborts():
        banned += GC_CALLBACK_ABORTS.split()
    return [n for n in names if not any(fnmatch.fnmatchcase(n, p) for p in banned)]


def main():
    args = sys.argv[1:]
    jobs = 6
    if '-j' in args:
        k = args.index('-j')
        jobs = int(args[k + 1])
        del args[k:k + 2]
    names = args or modules()
    bad = 0
    with ThreadPoolExecutor(jobs) as ex:
        for ok, line in ex.map(check, names):
            bad += not ok
            print(line)
            sys.stdout.flush()
    print('%d modules, %d unexpected' % (len(names), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
