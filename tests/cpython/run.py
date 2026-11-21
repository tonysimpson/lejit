"""Runs CPython's own test modules plain and with Ni; the counts and exit
status must match. Every test_* module of the interpreter's test package
runs except BANNED.

    python3.X tests/cpython/run.py [-j N] [module ...]
"""
import fnmatch, os, resource, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import (LE_DIR, SANITIZE_LOGS, SANITIZE_SKIP, SLOWDOWN,
                     child_env, save_stderr)

TIMEOUT = 600 * SLOWDOWN        # seconds per run
MEMORY_LIMIT = 4 << 30          # bytes of address space per run

# reason: names (fnmatch patterns); a name no version has is fine
BANNED = {
    "can't run here, with or without Ni":
        'test_curses test_idle test_tcl test_tix test_tk test_ttk_* test_turtle '
        'test_*net test_nntplib test_ssl test_timeout test_codecmaps_* test_normalization '
        'test_win* test_msilib test_startfile test_osx_env test_kqueue test_devpoll test_ossaudiodev '
        'test_big* test_largefile test_zipfile64 test_multiprocessing_* test_concurrent_futures '
        'test_support test_script_helper test_regrtest test_crashers test_gdb test_check_c_globals',
    "compiled frames aren't kept up to date (line numbers, locals, stacks)":
        'test_sys test_frame test_traceback test_tracemalloc test_warnings test_logging '
        'test_coroutines test_asyncio test_compile test_raise test_exceptions test_builtin '
        'test_doctest test_capi',
    'compiled code holds what it specialised on':
        'test_weakref test_gc test_module test_descr',
    'sys.getrefcount counts differently':
        'test_generators test_ctypes test_memoryio test_socket',
    'tracing that starts in a running frame':
        'test_sys_settrace test_pdb',
    'marshal of a code object with one reference': 'test_importlib',
    'timing': 'test_email',
}

CHILD = r'''
import sys, unittest, io
if sys.argv[1] == 'lejit':
    sys.path.insert(0, sys.argv[3]); import lejit; lejit.start()
mod = __import__('test.' + sys.argv[2], fromlist=['x'])
suite = unittest.defaultTestLoader.loadTestsFromModule(mod)
r = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
print('run=%d fail=%d err=%d skip=%d' % (r.testsRun, len(r.failures),
                                         len(r.errors), len(r.skipped)))
sys.stdout.flush()
'''


def limit_memory():
    # a module that runs away under Ni gets MemoryError instead of taking
    # the machine
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))


def run(mode, name):
    tag = name + '.' + mode
    with tempfile.TemporaryDirectory(prefix='le_cpython_tests_') as cwd:
        p = subprocess.Popen([sys.executable, '-c', CHILD, mode, name, LE_DIR],
                             stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE if SANITIZE_LOGS else subprocess.DEVNULL,
                             stdin=subprocess.DEVNULL, cwd=cwd, env=child_env(tag),
                             preexec_fn=None if SANITIZE_LOGS else limit_memory)
        try:
            out, err = p.communicate(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            p.kill()
            p.communicate()
            return 'timeout'
    save_stderr(tag, p.pid, err)
    out = out.decode().strip().splitlines()
    return '%s exit=%d' % (out[-1] if out else '(no output)', p.returncode)


def check(name):
    plain, with_lejit = run('plain', name), run('lejit', name)
    if plain == with_lejit:
        return True, 'ok     %-24s %s' % (name, plain)
    return False, 'FAIL   %-24s %s   with Ni: %s' % (name, plain, with_lejit)


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
    return [n for n in names if not any(fnmatch.fnmatchcase(n, p) for p in banned)]


def main():
    args = sys.argv[1:]
    jobs = 6
    if '-j' in args:
        k = args.index('-j')
        jobs = int(args[k + 1])
        del args[k:k + 2]
    names = args or modules()
    if SANITIZE_LOGS:
        names = [n for n in names if n not in SANITIZE_SKIP]
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
