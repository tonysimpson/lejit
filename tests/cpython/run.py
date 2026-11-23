"""Runs CPython's own test modules plain and with LeJIT; the counts, exit
status and names of the failing tests must match. Every test_* module of the
interpreter's test package runs except BANNED, and every test except SKIPPED.

    python3.X tests/cpython/run.py [-j N] [module ...]
"""
import fnmatch, json, os, re, resource, subprocess, sys, tempfile
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
    'sys.monitoring with a frame-evaluation hook installed '
    '(_testinternalcapi.set_eval_frame_record fails it the same way)':
        'test_monitoring',
}

# reason: tests skipped in both runs, by id as the report prints it (fnmatch
# patterns)
SKIPPED = {
    "sys.getrefcount counts a constant once for each compiled block that "
    'uses it, and the code compiled while a test counts holds more':
        'test.test_xml_etree.BugsTest.test_bug_xmltoolkit63 '
        'test.test_xml_etree_c.BugsTest.test_bug_xmltoolkit63',
    "CPython doesn't specialise Python calls, __getitem__ and properties "
    'while a frame-evaluation hook is installed (specialize.c, '
    'SPEC_FAIL_CALL_PEP_523)':
        'test.test_dis.DisTests.test_loop_quicken '
        'test.test_dis.DisWithFileTests.test_loop_quicken '
        'test.test_type_cache.TypeCacheWithSpecializationTests.'
        'test_binary_subscript_specialization_user_type '
        'test.test_type_cache.TypeCacheWithSpecializationTests.'
        'test_property_load_attr_specialization_user_type '
        'test.test_opcache.TestRacesDoNotCrash.test_binary_subscr_getitem '
        'test.test_opcache.TestRacesDoNotCrash.test_for_iter_gen '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_getattribute_overridden '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_property',
    "compiled code doesn't specialise the bytecode it runs, which ceval does "
    'as it runs it':
        'test.test_type_cache.TypeCacheWithSpecializationTests.'
        'test_load_method_specialization_user_type',
    'CPython counts each frame a frame-evaluation hook is given as a C call '
    'too, so a Python recursion 90,000 deep passes the C recursion limit '
    '(_testinternalcapi.set_eval_frame_record fails it the same way)':
        'test.test_call.TestRecursion.test_super_deep',
    'with a frame-evaluation hook installed, a recursion through '
    "functools' Python lru_cache passes the recursion limit "
    '(_testinternalcapi.set_eval_frame_record fails it the same way)':
        'test.test_functools.TestLRUPy.test_lru_recursion',
    "LeJIT replaces _warnings.warn and gc.get_referents with functions of "
    "its own, whose docstrings have no text signature":
        'test.test_inspect.*TestSignatureDefinitions.test_gc_module_has_signatures '
        'test.test_inspect.*TestSignatureDefinitions.test_warnings_module_has_signatures',
    'skipped when the machine converts fast enough':
        'test.test_int.IntStrDigitLimitsTests.test_denial_of_service_prevented_int_to_str '
        'test.test_int.IntStrDigitLimitsTests.test_denial_of_service_prevented_str_to_int '
        'test.test_int.IntSubclassStrDigitLimitsTests.test_denial_of_service_prevented_int_to_str '
        'test.test_int.IntSubclassStrDigitLimitsTests.test_denial_of_service_prevented_str_to_int',
    "code LeJIT compiles doesn't run in ceval, so ceval doesn't quicken it":
        'test.test_dis.DisTests.test_super_instructions '
        'test.test_dis.DisWithFileTests.test_super_instructions '
        'test.test_dis.DisTests.test_show_caches '
        'test.test_dis.DisWithFileTests.test_show_caches '
        'test.test_type_cache.TypeCacheWithSpecializationTests.'
        'test_load_attr_specialization_user_type '
        'test.test_type_cache.TypeCacheWithSpecializationTests.'
        'test_store_attr_specialization_user_type '
        'test.test_type_cache.TypeCacheWithSpecializationTests.'
        'test_class_load_attr_specialization_user_type '
        'test.test_type_cache.TypeCacheWithSpecializationTests.'
        'test_class_load_attr_specialization_static_type',
    "C code that reads the running frame itself (sys._current_frames, "
    "tracemalloc, a _warnings module imported after LeJIT started, type()'s "
    "and typing.TypeAliasType's __module__) sees the nearest frame ceval runs, not the compiled ones":
        'test.test_sys.SysModuleTest.test_current_frames '
        'test.test_sys.SysModuleTest.test_current_exceptions '
        'test.test_tracemalloc.TestCAPI.* '
        'test.test_tracemalloc.TestTracemallocEnabled.test_get_object_traceback '
        'test.test_tracemalloc.TestTracemallocEnabled.test_get_traced_memory '
        'test.test_tracemalloc.TestTracemallocEnabled.test_get_traces '
        'test.test_tracemalloc.TestTracemallocEnabled.test_get_traces_intern_traceback '
        'test.test_tracemalloc.TestTracemallocEnabled.test_new_reference '
        'test.test_tracemalloc.TestTracemallocEnabled.test_no_incomplete_frames '
        'test.test_tracemalloc.TestTracemallocEnabled.test_set_traceback_limit '
        'test.test_tracemalloc.TestTracemallocEnabled.test_snapshot '
        'test.test_warnings.CFilterTests.test_error_after_default '
        'test.test_warnings.CWarnTests.test_filename '
        'test.test_warnings.CWarnTests.test_stacklevel '
        'test.test_warnings.CWarnTests.test_stacklevel_import '
        'test.test_builtin.TestType.test_new_type '
        'test.test_builtin.TestType.test_type_name '
        'test.test_type_aliases.TypeAliasConstructorTest.test_basic '
        'test.test_type_aliases.TypeAliasConstructorTest.test_generic '
        'test.test_type_aliases.TypeAliasConstructorTest.test_not_generic '
        'test.test_type_aliases.TypeAliasConstructorTest.test_keywords',
    "a compiled frame's line, frame object or f_back isn't always what "
    'CPython gives':
        'test.test_frame.ClearTest.test_clear_executing '
        'test.test_frame.ReprTest.test_repr '
        'test.test_traceback.TestStack.test_walk_stack '
        'test.test_coroutines.OriginTrackingTest.test_origin_tracking '
        'test.test_coroutines.OriginTrackingTest.test_origin_tracking_warning '
        'test.test_coroutines.OriginTrackingTest.test_unawaited_warning_when_module_broken '
        'test.test_compile.TestSpecifics.test_lineno_after_implicit_return '
        'test.test_raise.TestTracebackType.test_attrs '
        'test.test_gc.GCTests.test_frame '
        'test.test_frame.TestIncompleteFrameAreInvisible.test_sneaky_frame_object_teardown '
        'test.test_capi.test_exceptions.Test_ErrSetAndRestore.test_err_writeunraisable',
    "an inlined frame's f_locals is empty once its call has returned":
        'test.test_frame.TestFrameLocals.test_constructor',
    "C code that reads a compiled frame's locals itself (PyEval_GetLocals, "
    'PyFrame_GetVar) misses those compiled code has set':
        'test.test_capi.test_eval.Tests.test_eval_getlocals '
        'test.test_frame.TestCAPI.test_getvar',
    "sys.settrace on 3.12 sees an async for loop's exception events "
    'differently':
        'test.test_sys_settrace.*.test_20_async_for_loop',
    'a failed task the test drops is freed only by the gc, so its '
    'exception handler runs late':
        'test.test_asyncio.test_futures2.PyFutureTests.test_task_exc_handler_correct_context',
    'tracing that starts in a running frame (pdb.set_trace)':
        'test.test_sys_settrace.*.test_09_settrace_and_raise '
        'test.test_pdb.test_* '
        'test.test_doctest.test_pdb_set_trace*',
    "compiled code's direct and inlined calls skip CPython's recursion "
    'depth count, so the limit trips somewhere else':
        'test.test_sys.SysModuleTest.test_recursionlimit_recovery',
    'compiled code holds what it specialised on':
        'test.test_weakref.ReferencesTestCase.test_callback_in_cycle_resurrection '
        'test.test_weakref.WeakMethodTestCase.test_callback_when_method_dead '
        'test.test_weakref.WeakMethodTestCase.test_method_dead '
        'test.test_gc.GCTests.test_newinstance '
        'test.test_gc.GCCallbackTests.test_collect_garbage '
        'test.test_capi.*CAPITest.*_decrefs_once',
    'sys.getrefcount counts differently':
        'test.test_sys.SysModuleTest.test_refcount '
        'test.test_generators.FinalizationTest.test_refcycle '
        'ctypes.test.test_memfunctions.MemFunctionsTest.test_string_at '
        'ctypes.test.test_python_api.PythonAPITestCase.test_PyLong_Long '
        'ctypes.test.test_refcounts.RefcountTestCase.test_1 '
        'ctypes.test.test_refcounts.RefcountTestCase.test_refcount '
        'ctypes.test.test_stringptr.StringPtrTestCase.test__POINTER_c_char '
        'test.test_ctypes.test_memfunctions.MemFunctionsTest.test_string_at '
        'test.test_ctypes.test_python_api.PythonAPITestCase.test_PyLong_Long '
        'test.test_ctypes.test_refcounts.RefcountTestCase.test_1 '
        'test.test_ctypes.test_refcounts.RefcountTestCase.test_refcount '
        'test.test_ctypes.test_stringptr.StringPtrTestCase.test__POINTER_c_char',
    'compiling allocates memory and objects the gc tracks, while the test '
    'counts them':
        'test.test_descr.ClassPropertiesAndMethods.test_slots '
        'test.test_gc.GCTests.test_get_objects_arguments '
        'test.test_sys.SysModuleTest.test_getallocatedblocks',
    'marshal of a code object with one reference':
        'test.test_importlib.test_abc.*_SourceLoaderBytecodeTests.test_code_bad_* '
        'test.test_importlib.test_abc.*_SourceLoaderBytecodeTests.test_no_bytecode',
    "compiled code makes Python calls without CPython's frame-evaluation "
    'hook, so a hook the test installs sees none of them':
        'test.test_capi.test_misc.Test_Pep523API.*',
    'a pool of processes started from threads, which hung here once in a '
    'child that runs without LeJIT':
        'test.test_importlib.test_threaded_import.ThreadedImportTests.'
        'test_multiprocessing_pool_circular_import',
    "SSL tests that race on OpenSSL 3, which the CI images' older CPythons predate":
        'test.test_poplib.TestPOP3_SSLClass.* '
        'test.test_poplib.TestPOP3_TLSClass.* '
        'test.test_poplib.TestPOP3Class.test_stls*',
    'binds a port find_unused_port chose, and skips itself when another '
    'process took it first':
        'test.test_smtplib.DebuggingServerTests.testSourceAddress',
    "start many threads, and fail with \"can't start new thread\" on a "
    'loaded machine, plain as well as with LeJIT':
        'test.test_queue.*SimpleQueueTest.test_many_threads*',
    'bind a port find_unused_port chose, which another process can take '
    'first':
        'test.test_socket.CreateServerFunctionalTest.*',
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


# Prints a JSON list of the failing tests, each its id and, for the first
# ten, the end of its traceback, then the counts. Tests whose ids match one of
# the patterns given are skipped.
CHILD = r'''
import sys, unittest, io, json, fnmatch
if sys.argv[1] == 'lejit':
    sys.path.insert(0, sys.argv[3]); import lejit; lejit.start()
mod = __import__('test.' + sys.argv[2], fromlist=['x'])
suite = unittest.defaultTestLoader.loadTestsFromModule(mod)

def skip(suite):
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            skip(test)
        elif any(fnmatch.fnmatchcase(test.id(), p) for p in sys.argv[4:]):
            setattr(test, test._testMethodName,
                    unittest.skip('skipped by run.py')(lambda: None))
skip(suite)
r = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
bad = r.failures + r.errors
print(json.dumps([[t.id()] + [l[:200] for l in tb.splitlines()[-10:]] * (i < 10)
                  for i, (t, tb) in enumerate(bad)]))
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
    """The run's counts and exit status, its failing tests (id: the end of
    the traceback), and the end of its stderr if it crashed."""
    with tempfile.TemporaryDirectory(prefix='le_cpython_tests_') as cwd, \
            tempfile.TemporaryFile() as errf:
        skip = [t for ts in SKIPPED.values() for t in ts.split()]
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
        return 'timeout', {}, stderr_tail(err)
    out = out.decode(errors='replace').strip().splitlines()
    counted = out[-1:] and out[-1].startswith('run=')
    failed = {}
    if counted and len(out) > 1:
        try:
            # a subtest's id may show an address
            failed = {re.sub('0x[0-9a-f]+', '0x...', t[0]): t[1:]
                      for t in json.loads(out[-2])}
        except (ValueError, TypeError, IndexError):
            pass
    crashed = [] if counted and not p.returncode else stderr_tail(err)
    return ('%s exit=%d' % (out[-1] if out else '(no output)', p.returncode),
            failed, crashed)


def check(name):
    plain, with_lejit = run('plain', name), run('lejit', name)
    if plain[0] == with_lejit[0] and set(plain[1]) == set(with_lejit[1]):
        return True, 'ok     %-24s %s' % (name, plain[0])
    report = ['FAIL   %-24s %s   with LeJIT: %s' % (name, plain[0], with_lejit[0])]
    for side, (_, failed, crashed), (_, other, _) in (
            ('plain', plain, with_lejit), ('with LeJIT', with_lejit, plain)):
        why = []
        for test in sorted(set(failed) - set(other)):
            why += [test] + ['    ' + l for l in failed[test]]
        why += crashed
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
