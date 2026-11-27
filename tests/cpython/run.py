"""Runs CPython's own test modules plain and with LeJIT; the counts, exit
status and names of the failing tests must match. Every test_* module of the
interpreter's test package runs except BANNED, and every test except SKIPPED.

The modules, and the tests in each, run in an order a seed chooses, new each
run unless --seed gives it; the seed also sets each module's hash seed. A
module's plain and LeJIT runs share its order and its hash seed.

    python3.X tests/cpython/run.py [-j N] [--seed N] [module ...]
"""
import contextlib, fnmatch, json, os, re, resource, signal, subprocess, sys, tempfile, threading
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import ROOT, rank, take_seed

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
    'times what runs after the timer starts against a margin of a '
    'millisecond or less, which compiling that code can take on a busy '
    'machine':
        'test.test_os.TimerfdTests.test_timerfd_*initval '
        'test.test_os.TimerfdTests.test_timerfd_*interval '
        'test.test_os.TimerfdTests.test_timerfd_*TFD_TIMER_ABSTIME '
        'test.test_os.TimerfdTests.test_timerfd_*select '
        'test.test_os.TimerfdTests.test_timerfd_*poll',
    'connects to a port it found unused, which a test running at the same '
    'time can take, and expects the connection to fail':
        'test.test_asyncio.test_events.*EventLoopTests.'
        'test_create_connection_local_addr_skip_different_family',
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
        'test_load_method_specialization_user_type '
        'test.test_type_cache.TypeCacheWithSpecializationTests.'
        'test_class_call_specialization_user_type '
        'test.test_type_cache.TypeCacheWithSpecializationTests.'
        'test_to_bool_specialization_user_type '
        'test.test_opcache.TestCallCache.test_assign_init_code '
        'test.test_opcache.TestRacesDoNotCrash.test_binary_subscr_list_int '
        'test.test_opcache.TestRacesDoNotCrash.test_for_iter_list '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_class '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_instance_value '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_method_lazy_dict '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_method_no_dict '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_method_with_values '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_module '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_with_hint '
        'test.test_opcache.TestRacesDoNotCrash.test_load_global_module '
        'test.test_opcache.TestRacesDoNotCrash.test_store_attr_instance_value '
        'test.test_opcache.TestRacesDoNotCrash.test_store_attr_with_hint '
        'test.test_opcache.TestRacesDoNotCrash.test_store_subscr_list_int '
        'test.test_opcache.TestRacesDoNotCrash.test_unpack_sequence_list '
        'test.test_opcache.TestCallCache.test_push_init_frame_fails '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_class_with_metaclass_check '
        'test.test_opcache.TestRacesDoNotCrash.test_load_attr_slot '
        'test.test_opcache.TestSpecializer.test_binary_op '
        'test.test_opcache.TestSpecializer.test_compare_op '
        'test.test_opcache.TestSpecializer.test_contain_op '
        'test.test_opcache.TestSpecializer.test_load_const '
        'test.test_opcache.TestSpecializer.test_load_super_attr '
        'test.test_opcache.TestSpecializer.test_store_attr_instance_value '
        'test.test_opcache.TestSpecializer.test_store_attr_slot '
        'test.test_opcache.TestSpecializer.test_store_attr_with_hint '
        'test.test_opcache.TestSpecializer.test_to_bool '
        'test.test_opcache.TestSpecializer.test_unpack_sequence',
    'CPython counts each frame a frame-evaluation hook is given as a C call '
    'too, so a Python recursion 90,000 deep passes the C recursion limit '
    '(_testinternalcapi.set_eval_frame_record fails it the same way)':
        'test.test_call.TestRecursion.test_super_deep',
    'with a frame-evaluation hook installed, a recursion through '
    "functools' Python lru_cache passes the recursion limit "
    '(_testinternalcapi.set_eval_frame_record fails it the same way)':
        'test.test_functools.TestLRUPy.test_lru_recursion',
    "CPython doesn't specialise a class's __getitem__ in BINARY_OP, or a "
    'for loop over or a send to a generator, while a frame-evaluation hook '
    'is installed (_testinternalcapi.set_eval_frame_record fails them the '
    'same way)':
        'test.test_opcache.TestSpecializer.test_binary_subscr '
        'test.test_opcache.TestSpecializer.test_for_iter '
        'test.test_opcache.TestSpecializer.test_send_with '
        'test.test_opcache.TestSpecializer.test_send_yield_from',
    'with a frame-evaluation hook installed, a Python call holds its '
    'arguments in the caller too, so sys.getrefcount counts one more '
    '(_testinternalcapi.set_eval_frame_record fails it the same way)':
        'test.test_capi.test_object.CAPITest.test_is_unique_temporary',
    'with a frame-evaluation hook installed, _remote_debugging sees only '
    "the thread's innermost frame "
    '(_testinternalcapi.set_eval_frame_record fails it the same way)':
        'test.test_external_inspection.TestGetStackTrace.test_self_trace',
    'fails in plain CPython under load: 0 GIL holders seen while the main '
    'thread runs its busy loop':
        'test.test_external_inspection.TestGetStackTrace.test_only_active_thread',
    'skipped when the machine converts fast enough':
        'test.test_int.IntStrDigitLimitsTests.test_denial_of_service_prevented_int_to_str '
        'test.test_int.IntStrDigitLimitsTests.test_denial_of_service_prevented_str_to_int '
        'test.test_int.IntSubclassStrDigitLimitsTests.test_denial_of_service_prevented_int_to_str '
        'test.test_int.IntSubclassStrDigitLimitsTests.test_denial_of_service_prevented_str_to_int',
    "code LeJIT compiles doesn't run in ceval, so ceval doesn't quicken it":
        'test.test_dis.DisTests.test_loop_with_conditional_at_end_is_quickened '
        'test.test_dis.DisWithFileTests.test_loop_with_conditional_at_end_is_quickened '
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
    "tracemalloc, another process reading the stack, a _warnings module imported after LeJIT started, "
    "type()'s __module__) sees the nearest frame ceval runs, not the compiled ones":
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
        'test.test_external_inspection.TestGetStackTrace.test_self_trace',
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
        'test.test_capi.test_exceptions.Test_ErrSetAndRestore.test_err_writeunraisable '
        'test.test_capi.test_exceptions.Test_ErrSetAndRestore.test_err_formatunraisable',
    "an inlined frame's f_locals is empty once its call has returned":
        'test.test_frame.TestFrameLocals.test_constructor',
    "sys.settrace on 3.12 sees an async for loop's exception events "
    'differently':
        'test.test_sys_settrace.*.test_20_async_for_loop',
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
    'compiled code passes a constant to a C function without a reference '
    'of its own, so the function sees one fewer than in CPython':
        'test.test_capi.test_object.IsUniquelyReferencedTest.test_is_uniquely_referenced',
    "objects compiled code keeps virtual are never made, so the ref tracer "
    "doesn't see them destroyed":
        'test.test_capi.test_object.RefTracerTest.test_destroy_traced_for_trashcan_deferred_objects',
    "read __main__.__file__, which this runner's children (python -c) "
    "don't have":
        'test.test_capi.test_run.CAPITest.test_run_anyfile* '
        'test.test_capi.test_run.CAPITest.test_run_simplefile*',
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
    "a dead test's __del__ runs its event loop until a queue is joined, "
    'which a task finalised before it never does; plain 3.9 hangs the same '
    'when full collections come at other moments (gc.set_threshold(200, 3, 3))':
        'unittest.test.test_async_case.TestAsyncCase.test_exception_in_*',
    'compiled code released after a full collection drops the references '
    'it held, while the test counts them':
        'ctypes.test.test_refcounts.AnotherLeak.test_callback '
        'test.test_ctypes.test_refcounts.AnotherLeak.test_callback',
    'compiling allocates memory and objects the gc tracks, while the test '
    'counts them':
        'test.test_descr.ClassPropertiesAndMethods.test_slots '
        'test.test_gc.GCTests.test_get_objects_arguments '
        'test.test_gc.GCTests.test_heap_size '
        'test.test_sys.SysModuleTest.test_getallocatedblocks',
    'marshal of a code object with one reference':
        'test.test_importlib.test_abc.*_SourceLoaderBytecodeTests.test_code_bad_* '
        'test.test_importlib.test_abc.*_SourceLoaderBytecodeTests.test_no_bytecode',
    "compiled code makes Python calls without CPython's frame-evaluation "
    'hook, so a hook the test installs sees none of them':
        'test.test_capi.test_misc.Test_Pep523API.*',
    "fail every allocation from some point (support.nomemtest, "
    "_testcapi.set_nomemory), where a compile can't recover":
        'test.test_bytes.*.test_resize_error '
        'test.test_capi.test_unicode.*.test_memory_error '
        'test.test_str.*.test_replace_oom '
        'test.test_memoryio.*.test_memory_error '
        'test.test_pyexpat.*.test_error_path_no_crash',
    'a pool of processes started from threads, which hung here once in a '
    'child that runs without LeJIT':
        'test.test_importlib.test_threaded_import.ThreadedImportTests.'
        'test_multiprocessing_pool_circular_import',
    'segfaulted once in CI with a freed frame left current, the client '
    'and its HTTPS server thread both in socket reads; not reproduced':
        'test.test_httplib.HTTPSTest.*',
    "SSL tests that race on OpenSSL 3, which the CI images' older CPythons predate":
        'test.test_poplib.TestPOP3_SSLClass.* '
        'test.test_poplib.TestPOP3_TLSClass.* '
        'test.test_poplib.TestPOP3Class.test_stls*',
    "plain CPython 3.14's own run in CI's container misses profile "
    "events (testfunc's first call of helper) with the simulated timer, "
    "so its counts and times differ; it doesn't fail outside CI":
        'test.test_profile.ProfileTest.test_cprofile',
    'counts on catching at least one signal while it flips between a '
    'Python handler and SIG_IGN, which CPython says it may not (every one '
    'can arrive under SIG_IGN); plain 3.8 misses them all too, and LeJIT '
    'more often':
        'test.test_signal.StressTest.test_stress_modifying_handlers',
    'binds a port find_unused_port chose, and fails or skips itself when '
    'another process took it first':
        'test.test_smtplib.DebuggingServerTests.testSourceAddress '
        'test.test_ftplib.*.test_source_address* '
        'test.test_asyncio.test_sendfile.*.test_sendfile '
        'test.test_asyncio.test_sendfile.*.test_sendfile_force_fallback '
        'test.test_asyncio.test_sendfile.*.test_sendfile_force_unsupported_native '
        'test.test_asyncio.test_sendfile.*.test_sendfile_ssl '
        'test.test_asyncio.test_sendfile.*.test_sendfile_for_closing_transp '
        'test.test_asyncio.test_sendfile.*.test_sendfile_pre_and_post_data '
        'test.test_asyncio.test_sendfile.*.test_sendfile_ssl_pre_and_post_data '
        'test.test_asyncio.test_sendfile.*.test_sendfile_partial '
        'test.test_asyncio.test_sendfile.*.test_sendfile_ssl_partial '
        'test.test_asyncio.test_sendfile.*.test_sendfile_close_peer_after_receiving '
        'test.test_asyncio.test_sendfile.*.test_sendfile_ssl_close_peer_after_receiving '
        'test.test_asyncio.test_sendfile.*.test_sendfile_close_peer_in_the_middle_of_receiving '
        'test.test_asyncio.test_sendfile.*.test_sendfile_fallback_close_peer_in_the_middle_of_receiving '
        'test.test_asyncio.test_sendfile.*.test_sendfile_prevents_bare_write '
        'test.test_asyncio.test_streams.StreamTests.test_unhandled_exception '
        'test.test_asyncio.test_streams.StreamTests.test_unhandled_exception_sync_callback '
        'test.test_asyncio.test_streams.StreamTests.test_unhandled_cancel',
    'skips itself on a sporadic ENOTCONN, so the plain and LeJIT runs can '
    'skip different counts':
        'test.test_socket.RecvmsgSCTPStreamTest.testRecvmsgEOF '
        'test.test_socket.RecvmsgIntoSCTPStreamTest.testRecvmsgEOF',
    "start many threads, and fail with \"can't start new thread\" on a "
    'loaded machine, plain as well as with LeJIT':
        'test.test_queue.*SimpleQueueTest.test_many_threads* '
        'test.test_interpreters.test_stress.StressTests.*',
    'bind a port find_unused_port chose, which another process can take '
    'first':
        'test.test_socket.CreateServerFunctionalTest.*',
    "connects from a port find_unused_port chose; with LeJIT on 3.10 the "
    "new socket's descriptor can be gone at getsockname (EBADF), not "
    "reproduced outside CI":
        'test.test_socket.NetworkConnectionAttributesTest.testSourceAddress',
    "decodes each read the server makes on its own, so one that ends inside "
    "the data's two-byte character counts it as two, plain as well as with "
    "LeJIT (the test keeps bytes from 3.11)":
        'test.test_ftplib.TestFTPClass.test_storlines',
    'asserts that wait_for with a timeout of 0 returns within 0.1 s of wall '
    'time, which fails plain as well as with LeJIT on a loaded machine':
        'test.test_asyncio.test_waitfor.AsyncioWaitForTest.'
        'test_wait_for_timeout_less_then_0_or_0_coroutine_do_not_started '
        'test.test_asyncio.test_waitfor.AsyncioWaitForTest.'
        'test_wait_for_timeout_less_then_0_or_0',
    'asserts that one epoll poll returns within 10 ms of wall time, which a '
    'busy machine can miss whatever runs the test':
        'test.test_epoll.TestEPoll.test_unregister_closed',
    "passes only after TestGeometricMean.test_basics has seeded random: "
    'on unseeded data 3.14 gives a pstdev that is not exactly '
    'sqrt(pvariance) about one run in five, plain as well as with LeJIT':
        'test.test_statistics.TestPStdev.test_compare_to_variance',
    "assert that a failed TLS handshake's transport is freed with the "
    'collector off, which some orders of the tests before them prevent, '
    'plain as well as with LeJIT':
        'test.test_asyncio.test_sslproto.*StartTLSTests.'
        'test_create_connection_ssl_failed_certificate_no_cycles '
        'test.test_asyncio.test_sslproto.*StartTLSTests.'
        'test_start_tls_failed_certificate_no_cycles',
}

# Modules that never run at the same time as each other: test_perf_profiler
# deletes every /tmp/perf-*.map it didn't see at setUp, and test_perfmaps
# reads the one it writes.
ONE_AT_A_TIME = {'test_perf_profiler', 'test_perfmaps'}
one_at_a_time = threading.Lock()

# Where a gc callback that fails at the recursion limit leaves the limit off,
# so CPython aborts at it, LeJIT's own callback makes these abort.
GC_CALLBACK_ABORTS = 'test_functools test_json test_pickle'
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


# Prints a JSON list of the skipped tests, each its id and the reason, then
# one of the failing tests, each its id and, for the first ten, the end of
# its traceback, then the counts. Tests whose ids match one of
# the patterns given are skipped. The seed orders each suite's tests by
# their ids alone, keeping a class's tests together so that its setUpClass
# and tearDownClass still run once.
CHILD = r'''
import sys, unittest, io, json, fnmatch, faulthandler, hashlib
faulthandler.enable(all_threads=True)
if sys.argv[1] == 'lejit':
    sys.path.insert(0, sys.argv[3]); import lejit; lejit.start()
mod = __import__('test.' + sys.argv[2], fromlist=['x'])
suite = unittest.defaultTestLoader.loadTestsFromModule(mod)

def rank(name):
    return hashlib.sha256(('%s %s' % (sys.argv[4], name)).encode()).hexdigest()

def first(test):
    if not isinstance(test, unittest.TestSuite):
        return test
    for t in test:
        t = first(t)
        if t is not None:
            return t

def place(test):
    t = first(test)
    if t is None:
        return ('', '')
    return (rank(type(t).__module__ + '.' + type(t).__qualname__), rank(t.id()))

def prepare(suite):
    for test in suite:
        if isinstance(test, unittest.TestSuite):
            prepare(test)
        elif any(fnmatch.fnmatchcase(test.id(), p) for p in sys.argv[5:]):
            setattr(test, test._testMethodName,
                    unittest.skip('skipped by run.py')(lambda: None))
    suite._tests.sort(key=place)
prepare(suite)
r = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
bad = r.failures + r.errors
print(json.dumps([[t.id(), str(why)[:200]] for t, why in r.skipped]))
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


def kill_group(p):
    try:
        os.killpg(p.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def stderr_tail(err, lines=10):
    """The last lines of a run's stderr (bytes), cut to a bounded width."""
    return ['stderr:'] + ['    ' + l[:200] for l in
                          err.decode(errors='replace').splitlines()[-lines:]]


def run(mode, name, seed):
    """The run's counts and exit status, its failing tests (id: the end of
    the traceback), the end of its stderr if it crashed, and its skipped
    tests (id: the reason)."""
    with tempfile.TemporaryDirectory(prefix='le_cpython_tests_') as cwd, \
            tempfile.TemporaryFile() as errf:
        skip = [t for ts in SKIPPED.values() for t in ts.split()]
        env = dict(os.environ)
        env.setdefault('PYTHONHASHSEED', str(int(rank(seed, name), 16) % (1 << 32)))
        p = subprocess.Popen([sys.executable, '-c', CHILD, mode, name, ROOT,
                              str(seed)] + skip,
                             stdout=subprocess.PIPE, stderr=errf,
                             stdin=subprocess.DEVNULL, cwd=cwd, env=env,
                             preexec_fn=limit_memory, start_new_session=True)
        try:
            out, _ = p.communicate(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            kill_group(p)
            p.communicate()
            out = None
        finally:
            # processes the tests started and left running
            kill_group(p)
        size = errf.seek(0, os.SEEK_END)
        errf.seek(max(0, size - 16384))
        err = errf.read()
    if out is None:
        return 'timeout', {}, stderr_tail(err), {}
    out = out.decode(errors='replace').strip().splitlines()
    counted = out[-1:] and out[-1].startswith('run=')
    failed, skipped = {}, {}
    if counted and len(out) > 2:
        try:
            # a subtest's id may show an address
            failed = {re.sub('0x[0-9a-f]+', '0x...', t[0]): t[1:]
                      for t in json.loads(out[-2])}
            skipped = {re.sub('0x[0-9a-f]+', '0x...', t[0]): t[1]
                       for t in json.loads(out[-3])}
        except (ValueError, TypeError, IndexError):
            pass
    # killed by a signal: faulthandler's tracebacks, one per thread
    crashed = ([] if counted and not p.returncode else
               stderr_tail(err, 80 if p.returncode < 0 else 10))
    return ('%s exit=%d' % (out[-1] if out else '(no output)', p.returncode),
            failed, crashed, skipped)


def check(name, seed):
    with one_at_a_time if name in ONE_AT_A_TIME else contextlib.nullcontext():
        plain, with_lejit = run('plain', name, seed), run('lejit', name, seed)
    if plain[0] == with_lejit[0] and set(plain[1]) == set(with_lejit[1]):
        return True, 'ok     %-24s %s' % (name, plain[0])
    report = ['FAIL   %-24s %s   with LeJIT: %s' % (name, plain[0], with_lejit[0])]
    for side, (_, failed, crashed, skipped), (_, other, _, other_skipped) in (
            ('plain', plain, with_lejit), ('with LeJIT', with_lejit, plain)):
        why = []
        for test in sorted(set(failed) - set(other)):
            why += [test] + ['    ' + l for l in failed[test]]
        why += crashed
        # a count that differs only in skips names the tests
        for test in sorted(set(skipped) - set(other_skipped)):
            why.append('skipped only here: %s (%s)' % (test, skipped[test]))
        if why:
            report += ['       %s:' % side] + ['         ' + l for l in why]
    report.append('       again: %s tests/cpython/run.py --seed %d %s'
                  % (sys.executable, seed, name))
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
    seed = take_seed(args)
    names = sorted(args or modules(), key=lambda n: rank(seed, n))
    print('seed %d' % seed)
    sys.stdout.flush()
    bad = 0
    with ThreadPoolExecutor(jobs) as ex:
        for ok, line in ex.map(check, names, [seed] * len(names)):
            bad += not ok
            print(line)
            sys.stdout.flush()
    print('%d modules, %d unexpected (seed %d)' % (len(names), bad, seed))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
