"""Settings the test runners share.

NI_MODULE_DIR: where `import ni` finds ni.so (default: the repo root).
NI_SANITIZE_LOGS: set by sanitizer runs; logs go there and runs get slower.
Run as a script, prints build_temp() for the running interpreter.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NI_DIR = os.environ.get('NI_MODULE_DIR') or ROOT
SANITIZE_LOGS = os.environ.get('NI_SANITIZE_LOGS')
SLOWDOWN = 4 if SANITIZE_LOGS else 1
# CPython test modules tests/cpython/run.py leaves out under the sanitizers
SANITIZE_SKIP = {
    'test_threading': 'with Ni, test_is_alive_after_fork\'s child can wait'
                      ' for good on a lock of ASan\'s allocator',
}


# setuptools' build_temp, relative to the repo root
BUILD_TEMP_CODE = ("from setuptools.dist import Distribution\n"
                   "b = Distribution().get_command_obj('build')\n"
                   "b.finalize_options(); print(b.build_temp)")


def build_temp(python=sys.executable):
    out = subprocess.check_output([python, '-c', BUILD_TEMP_CODE], cwd=ROOT)
    return os.path.join(ROOT, out.decode().strip())


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


if __name__ == '__main__':
    print(build_temp())
