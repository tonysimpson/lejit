"""Run some of CPython's own unit tests with and without Ni and compare.

Each module runs in a fresh interpreter, once plain and once with Ni
imported first. The test counts (run, failures, errors, skips) and the exit
status must match. Known differences are listed in KNOWN with their
KNOWN_BUGS ids; one that starts matching is reported, so it can be removed.
Every one of the 416 modules is in MODULES (the default set), EXTENDED
(added by --all) or EXCLUDED with a reason. Runs go 3 at a time (-j), each
with a timeout and a memory limit, in a temporary directory.

    python3.7 tests/cpython_tests.py [-j N] [--all | module ...]
"""
import os, resource, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TIMEOUT = 600                   # seconds per run
MEMORY_LIMIT = 4 << 30          # bytes of address space per run

# the default set (tools/check.sh): fast modules, mostly pure Python
MODULES = ['test_bisect', 'test_heapq', 'test_textwrap', 'test_fractions',
           'test_string', 'test_operator', 'test_itertools', 'test_enum',
           'test_csv', 'test_base64', 'test_functools', 'test_collections',
           'test_json', 'test_statistics', 'test_list', 'test_dict',
           'test_set', 'test_tuple', 'test_math', 'test_random',
           'test_difflib', 'test_pprint', 'test_shlex', 'test_copy',
           'test_deque', 'test_sort', 'test_contextlib', 'test_abc',
           'test_dataclasses', 'test_struct', 'test_re', 'test_ordered_dict',
           'test_generators', 'test_scope', 'test_class', 'test_exceptions',
           'test_with', 'test_keywordonlyarg', 'test_grammar', 'test_types',
           'test_bool', 'test_int', 'test_float', 'test_long', 'test_complex',
           'test_builtin', 'test_richcmp', 'test_userdict', 'test_userlist',
           'test_userstring', 'test_super', 'test_descr', 'test_coroutines',
           'test_asyncgen', 'test_fstring', 'test___all__', 'test___future__',
           'test__locale', 'test__opcode', 'test__osx_support',
           'test_abstract_numbers', 'test_aifc', 'test_argparse', 'test_array',
           'test_asdl_parser', 'test_ast', 'test_atexit', 'test_audioop',
           'test_augassign', 'test_baseexception', 'test_binascii',
           'test_binhex', 'test_binop', 'test_bufio', 'test_bytes', 'test_bz2',
           'test_calendar', 'test_call', 'test_cgi', 'test_cgitb',
           'test_charmapcodec', 'test_clinic', 'test_cmath', 'test_cmd',
           'test_code', 'test_code_module', 'test_codeccallbacks',
           'test_codecencodings_cn', 'test_codecencodings_hk',
           'test_codecencodings_iso2022', 'test_codecencodings_jp',
           'test_codecencodings_kr', 'test_codecencodings_tw', 'test_codecs',
           'test_codeop', 'test_colorsys', 'test_compare', 'test_compile',
           'test_configparser', 'test_contains', 'test_context',
           'test_contextlib_async', 'test_copyreg', 'test_ctypes',
           'test_datetime', 'test_dbm', 'test_dbm_dumb', 'test_dbm_gnu',
           'test_dbm_ndbm', 'test_decimal', 'test_decorators',
           'test_defaultdict', 'test_descrtut', 'test_dict_version',
           'test_dictcomps', 'test_dictviews', 'test_dis', 'test_doctest',
           'test_doctest2', 'test_dummy_thread', 'test_dynamic',
           'test_dynamicclassattribute', 'test_enumerate', 'test_eof',
           'test_errno', 'test_exception_hierarchy',
           'test_exception_variations', 'test_extcall', 'test_fcntl',
           'test_file', 'test_file_eintr', 'test_filecmp', 'test_fileinput',
           'test_fileio', 'test_finalization', 'test_flufl', 'test_fnmatch',
           'test_format', 'test_frame', 'test_frozen', 'test_funcattrs',
           'test_future', 'test_future3', 'test_future4', 'test_future5',
           'test_gc', 'test_generator_stop', 'test_genericclass',
           'test_genericpath', 'test_genexps', 'test_getargs2', 'test_getopt',
           'test_gettext', 'test_glob', 'test_global', 'test_grp', 'test_gzip',
           'test_hash', 'test_hashlib', 'test_hmac', 'test_html',
           'test_htmlparser', 'test_http_cookiejar', 'test_http_cookies',
           'test_imghdr', 'test_imp', 'test_index', 'test_inspect',
           'test_int_literal', 'test_ipaddress', 'test_isinstance',
           'test_iter', 'test_iterlen', 'test_keyword', 'test_linecache',
           'test_listcomps', 'test_locale', 'test_longexp', 'test_macpath',
           'test_mailcap', 'test_marshal', 'test_memoryio', 'test_memoryview',
           'test_metaclass', 'test_mimetypes', 'test_minidom', 'test_mmap',
           'test_module', 'test_modulefinder', 'test_multibytecodec',
           'test_netrc', 'test_nis', 'test_ntpath', 'test_numeric_tower',
           'test_opcodes', 'test_optparse', 'test_parser', 'test_pathlib',
           'test_peepholer', 'test_pickletools', 'test_pkg', 'test_pkgimport',
           'test_pkgutil', 'test_plistlib', 'test_posix', 'test_posixpath',
           'test_pow', 'test_print', 'test_property', 'test_pstats',
           'test_pulldom', 'test_pwd', 'test_py_compile', 'test_pyclbr',
           'test_pyexpat', 'test_quopri', 'test_raise', 'test_range',
           'test_reprlib', 'test_resource', 'test_rlcompleter',
           'test_robotparser', 'test_sax', 'test_sched', 'test_secrets',
           'test_setcomps', 'test_shelve', 'test_slice', 'test_sndhdr',
           'test_source_encoding', 'test_spwd', 'test_sqlite', 'test_stat',
           'test_strftime', 'test_string_literals', 'test_stringprep',
           'test_strptime', 'test_strtod', 'test_structmembers',
           'test_structseq', 'test_subclassinit', 'test_sunau', 'test_sundry',
           'test_symbol', 'test_symtable', 'test_syntax', 'test_syslog',
           'test_traceback', 'test_tracemalloc', 'test_typechecks',
           'test_typing', 'test_ucn', 'test_unary', 'test_unicode',
           'test_unicode_file', 'test_unicode_file_functions',
           'test_unicode_identifiers', 'test_unicodedata', 'test_unittest',
           'test_univnewlines', 'test_sys_settrace', 'test_sys_setprofile',
           'test_bdb', 'test_profile',
           'test_unpack', 'test_unpack_ex', 'test_urllib', 'test_urllib2',
           'test_urllib_response', 'test_urlparse', 'test_utf8source',
           'test_uu', 'test_warnings', 'test_wave', 'test_weakset',
           'test_wsgiref', 'test_xdrlib', 'test_xml_dom_minicompat',
           'test_xml_etree', 'test_xml_etree_c', 'test_xxtestfuzz',
           'test_yield_from', 'test_zipapp', 'test_zipimport', 'test_zlib']

# --all adds these: slower, or using subprocesses, sockets, threads or
# timing, so more exposed to a loaded machine
EXTENDED = ['test_pdb', 'test_cprofile', 'test_trace', 'test_asynchat', 'test_asyncio', 'test_asyncore', 'test_buffer',
            'test_c_locale_coercion', 'test_cmd_line', 'test_cmd_line_script',
            'test_compileall', 'test_crypt', 'test_distutils',
            'test_docxmlrpc', 'test_dtrace', 'test_dummy_threading',
            'test_eintr', 'test_email', 'test_embed', 'test_ensurepip',
            'test_epoll', 'test_faulthandler', 'test_fork1', 'test_ftplib',
            'test_getpass', 'test_httplib', 'test_httpservers', 'test_imaplib',
            'test_import', 'test_importlib', 'test_io', 'test_ioctl',
            'test_lib2to3',
            'test_logging', 'test_lzma', 'test_mailbox', 'test_openpty',
            'test_os', 'test_pickle', 'test_pipes', 'test_platform',
            'test_poll', 'test_popen', 'test_poplib', 'test_pty', 'test_pydoc',
            'test_queue', 'test_readline', 'test_repl', 'test_runpy',
            'test_select', 'test_selectors', 'test_shutil', 'test_signal',
            'test_site',
            'test_smtpd', 'test_smtplib', 'test_socket', 'test_socketserver',
            'test_subprocess', 'test_sysconfig', 'test_tarfile',
            'test_telnetlib', 'test_tempfile', 'test_thread',
            'test_threaded_import', 'test_threadedtempfile',
            'test_threading', 'test_threading_local', 'test_threadsignals',
            'test_time',
            'test_timeit', 'test_tokenize', 'test_tools',
            'test_urllib2_localnet', 'test_utf8_mode', 'test_uuid',
            'test_venv', 'test_wait3', 'test_wait4', 'test_weakref',
            'test_webbrowser', 'test_xmlrpc', 'test_zipfile',
            'test_zipimport_support']

# module -> reason it is expected to differ, or (the expected result with
# Ni, reason) when only that difference is accepted
KNOWN = {
    'test_re': 'C11: warnings stacklevel skips compiled callees',
    'test_exceptions': ('run=53 fail=3 err=0 skip=1 exit=0',
                        'C18: tracebacks don\'t keep compiled locals alive; '
                        'C11: f_back of a traceback frame'),
    'test_builtin': ('run=92 fail=1 err=0 skip=3 exit=0',
                     'C11: type() in a compiled callee takes __module__ from '
                     'the caller\'s globals'),
    'test_generators': ('run=16 fail=1 err=0 skip=0 exit=0',
                        'C19: getrefcount misses borrowed arguments'),
    'test_descr': ('run=138 fail=1 err=0 skip=2 exit=0',
                   'C20: compiling allocates a GC object test_slots counts'),
    'test_coroutines': ('run=94 fail=3 err=0 skip=0 exit=0',
                        'C11: a compiled frame\'s f_lineno is its def line '
                        '(also in coroutine origins and warning locations)'),
    'test_cgitb': ('run=6 fail=0 err=1 skip=0 exit=0',
                   'C11: a compiled frame\'s f_locals is empty (cgitb.html)'),
    'test_ctypes': ('run=485 fail=6 err=0 skip=89 exit=0',
                    'C19: getrefcount around calls into C'),
    'test_email': ('run=1631 fail=0 err=1 skip=1 exit=0',
                   'environment: test_make_msgid_collisions makes ids for '
                   '3 s; faster under Ni, it reaches the memory limit'),
    'test_frame': ('run=10 fail=2 err=1 skip=0 exit=0',
                   'C11: f_locals, f_lineno and f_back of compiled frames'),
    'test_gc': ('run=39 fail=2 err=0 skip=0 exit=0',
                'C11: locals not in the frame, so no cycle; C14: compiled '
                'code keeps classes alive'),
    'test_importlib': ('run=1267 fail=6 err=0 skip=17 exit=0',
                       'C9: marshal writes no FLAG_REF for a code object '
                       'with refcount 1'),
    'test_inspect': ('run=228 fail=2 err=0 skip=0 exit=0',
                     'C11: f_locals and line numbers of compiled frames'),
    'test_logging': ('run=183 fail=1 err=0 skip=1 exit=0',
                     'C11: line numbers in stack_info'),
    'test_module': ('run=30 fail=1 err=0 skip=0 exit=0',
                    'C14: compiled code keeps a class, so its module dict, '
                    'alive'),
    'test_raise': ('run=35 fail=1 err=0 skip=0 exit=0',
                   'C11: tb_frame is an emulated frame'),
    'test_traceback': ('run=63 fail=6 err=0 skip=0 exit=0',
                       'C11: frames, locals and line numbers in stacks'),
    'test_tracemalloc': ('run=43 fail=8 err=0 skip=0 exit=0',
                         'C11: allocation tracebacks miss compiled frames; '
                         'compiling allocates'),
    'test_warnings': ('run=121 fail=5 err=0 skip=0 exit=0',
                      'C11: warnings stacklevel skips compiled callees'),
    'test_socket': 'C19: getrefcount around calls into C '
                   '(testRefCountGetNameInfo); counts vary with the '
                   'environment, so no expected result',
    'test_unittest': ('run=798 fail=3 err=0 skip=3 exit=0',
                      'C11: warnings line numbers; F52: __defaults__ '
                      'changed after compiling'),
    'test_asyncio': ('run=2055 fail=18 err=0 skip=35 exit=0',
                     'C11: line numbers and stacks in source tracebacks'),
    'test_timeit': 'C30: crashes in test_timeit_few_iters after the '
                   'other TestTimeit tests (since e26c2a3)',
    'test_shutil': 'C31: segfaults after test_tarfile_vs_tar',
    'test_sys_settrace': ('run=136 fail=6 err=0 skip=0 exit=0',
                          'C29: f_trace set on a frame already running '
                          'compiled gets no events (settrace_and_return, '
                          'settrace_and_raise)'),
    'test_pdb': ('run=39 fail=15 err=0 skip=0 exit=0',
                 'C29: set_trace() in a compiled frame stops only in '
                 'its callees, not on its own next line; C11: line '
                 'numbers of compiled frames'),
    'test_weakref': ('run=124 fail=3 err=0 skip=0 exit=0',
                     'C14: compiled code keeps objects alive (classes, '
                     'bound methods)'),
}

# not run, and why (doc/BUILD.md, Tests)
EXCLUDED = {
    'needs a GUI or a terminal': ['test_tk', 'test_ttk_guionly',
        'test_ttk_textonly', 'test_tix', 'test_tcl', 'test_idle',
        'test_turtle', 'test_curses'],
    'needs the network or downloads test data': ['test_urllib2net',
        'test_urllibnet', 'test_smtpnet', 'test_xmlrpc_net', 'test_nntplib',
        'test_timeout', 'test_ssl', 'test_codecmaps_cn', 'test_codecmaps_hk',
        'test_codecmaps_jp', 'test_codecmaps_kr', 'test_codecmaps_tw',
        'test_normalization'],
    'another platform': ['test_winreg', 'test_winsound', 'test_winconsoleio',
        'test_msilib', 'test_startfile', 'test_osx_env', 'test_kqueue',
        'test_devpoll', 'test_ossaudiodev'],
    'very slow or needs resources (big memory, many processes)': [
        'test_bigmem', 'test_bigaddrspace', 'test_largefile',
        'test_zipfile64', 'test_multiprocessing_fork',
        'test_multiprocessing_forkserver', 'test_multiprocessing_spawn',
        'test_multiprocessing_main_handling', 'test_concurrent_futures'],
    'test infrastructure, empty, or needs gdb (D28)': ['test_support',
        'test_script_helper', 'test_regrtest', 'test_crashers', 'test_gdb'],
    'C11: test_current_frames fails on a line number and leaves its '
    'thread waiting, so the run never exits': ['test_sys'],
    'C25: return_null_without_error segfaults (an assertion in the checked '
    'build)': ['test_capi'],
}

CHILD = r'''
import sys, unittest, io
if sys.argv[1] == 'ni':
    sys.path.insert(0, sys.argv[3]); import ni
mod = __import__('test.' + sys.argv[2], fromlist=['x'])
suite = unittest.defaultTestLoader.loadTestsFromModule(mod)
r = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
print('run=%d fail=%d err=%d skip=%d' % (r.testsRun, len(r.failures),
                                         len(r.errors), len(r.skipped)))
sys.stdout.flush()
'''


def limit_memory():
    # a module that runs away under Ni (KNOWN_BUGS C23 reached 20 GB) gets
    # MemoryError instead of taking the machine
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))


def run(mode, name):
    with tempfile.TemporaryDirectory(prefix='ni_cpython_tests_') as cwd:
        try:
            p = subprocess.run([sys.executable, '-c', CHILD, mode, name, ROOT],
                               stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL,
                               stdin=subprocess.DEVNULL, cwd=cwd,
                               timeout=TIMEOUT, preexec_fn=limit_memory)
        except subprocess.TimeoutExpired:
            return 'timeout'
    out = p.stdout.decode().strip().splitlines()
    return '%s exit=%d' % (out[-1] if out else '(no output)', p.returncode)


def check(name):
    plain, with_ni = run('plain', name), run('ni', name)
    same = plain == with_ni
    known = KNOWN.get(name)
    if isinstance(known, tuple):
        expected, known = known
        if same:
            known = None    # the accepted difference may come and go
        elif with_ni != expected:
            known = None    # a difference other than the accepted one
    if same and not known:
        status = 'ok'
    elif not same and known:
        status = 'known'
    elif same:
        status = 'FIXED?'
    else:
        status = 'FAIL'
    line = '%-6s %-24s %s' % (status, name, plain)
    if not same:
        line += '   with Ni: ' + with_ni
    if known:
        line += '  [%s]' % known
    return status, line


def main():
    args = sys.argv[1:]
    jobs = 3
    if '-j' in args:
        k = args.index('-j')
        jobs = int(args[k + 1])
        del args[k:k + 2]
    every = '--all' in args
    args = [a for a in args if a != '--all']
    names = args or (MODULES + EXTENDED if every else MODULES)
    bad = 0
    with ThreadPoolExecutor(jobs) as ex:
        for status, line in ex.map(check, names):
            if status in ('FAIL', 'FIXED?'):
                bad += 1
            print(line)
            sys.stdout.flush()
    print('%d modules, %d unexpected' % (len(names), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
