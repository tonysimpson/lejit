"""Settings the test runners share with tools/sanitize.sh.

NI_MODULE_DIR: the directory `import ni` finds ni.so in (default the repo
root, where check.sh builds it in place).
NI_SANITIZE_LOGS: a directory; set by tools/sanitize.sh. Each child's
ASan reports go to a log there named after the test, and so does its
stderr if it holds a UBSan report (gcc's libubsan, preloaded beside
libasan, ignores log_path); runs get SLOWDOWN times longer, and there is no
address-space limit (ASan reserves terabytes for its shadow memory; the
script limits resident memory).
"""
import os, re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NI_DIR = os.environ.get('NI_MODULE_DIR') or ROOT
SANITIZE_LOGS = os.environ.get('NI_SANITIZE_LOGS')
SLOWDOWN = 4 if SANITIZE_LOGS else 1
# CPython test modules tests/cpython_tests.py leaves out under the sanitizers
SANITIZE_SKIP = {
    'test_threading': 'with Ni, test_is_alive_after_fork\'s child can wait'
                      ' for good on a lock of ASan\'s allocator (BUILD.md)',
}


def _base(tag):
    return os.path.join(SANITIZE_LOGS, re.sub(r'[^\w.-]', '_', tag))


def child_env(tag):
    """The environment for a child run named 'tag', or None to inherit."""
    if not SANITIZE_LOGS:
        return None
    env = dict(os.environ)
    opts = env.get('ASAN_OPTIONS', '')
    env['ASAN_OPTIONS'] = (opts + ':' if opts else '') + 'log_path=%s.asan' % _base(tag)
    return env


def save_stderr(tag, pid, data):
    """Keep a child's stderr (bytes) if it holds a UBSan report."""
    if SANITIZE_LOGS and data and b'runtime error:' in data:
        with open('%s.ubsan.%d' % (_base(tag), pid), 'wb') as f:
            f.write(data)
