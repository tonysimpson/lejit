"""LeJIT, a JIT compiler for CPython 3.7 to 3.15 on x86-64 Linux.

Importing lejit starts nothing; call start().
"""
import os as _os
import sys as _sys
import warnings as _warnings

__all__ = ['start', 'stop', 'check', 'build_features', 'runtime_features']

try:
    from . import _core
except ImportError as _e:
    # not built for this Python, or built but failing to load
    _core = None
    _missing = str(_e)

_started = False
_runtime = None
_no_headers = None


def _unsupported():
    import platform
    import sysconfig
    if platform.python_implementation() != 'CPython':
        return 'LeJIT runs only on CPython'
    if _sys.platform != 'linux' or platform.machine() != 'x86_64':
        return 'LeJIT runs only on x86-64 Linux'
    if sysconfig.get_config_var('Py_GIL_DISABLED'):
        return 'LeJIT runs only on CPython with the GIL'
    if _sys.version_info >= (3, 16):
        return 'LeJIT runs only on CPython 3.7 to 3.15'
    return _missing


def _pairs(d):
    return frozenset(list(d['features'].items()) +
                     list(d['messages'].items()) +
                     [('layout:' + k, v) for k, v in d['layouts'].items()])


def _shown(d, name):
    return repr(d[name]) if name in d else 'missing'


def build_features():
    """The features, messages and struct layouts LeJIT was built with, as a
    frozenset of (name, value) pairs."""
    return _pairs(_core._build) if _core else frozenset()


def runtime_features():
    """The same, detected on the running Python. Raises RuntimeWarning if
    the detectors fail."""
    global _runtime, _no_headers
    if _core is None:
        return frozenset()
    if _runtime is None:
        import json
        import subprocess
        # in a subprocess, so a crashing probe can't take this one down
        here = _os.path.dirname(__file__)
        r = subprocess.run(
            [_sys.executable, '-I', _os.path.join(here, '_features', 'check.py'),
             _os.path.dirname(here)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            universal_newlines=True)
        try:
            answers = json.loads(r.stdout)
        except ValueError:
            raise RuntimeWarning('lejit: the detectors failed: '
                                 + r.stderr.strip()[-300:])
        if 'error' in answers:
            raise RuntimeWarning('lejit: ' + answers['error'])
        _runtime = _pairs(answers)
        _no_headers = answers.get('no_headers')
    return _runtime


def check(error=False):
    """Compare build_features() with runtime_features() and return the
    differences. If there are any, warn, or raise if error is true."""
    try:
        diffs = build_features() ^ runtime_features()
    except RuntimeWarning as e:
        if error:
            raise
        _warnings.warn(str(e), RuntimeWarning, stacklevel=2)
        return frozenset()
    if diffs:
        built = dict(build_features())
        here = dict(runtime_features())
        names = sorted({n for n, _ in diffs})
        why = 'lejit: this Python differs from the one LeJIT was built for: ' + \
            '; '.join('%s is %s, built with %s' % (n, _shown(here, n), _shown(built, n))
                      for n in names[:3])
        if len(names) > 3:
            why += ' (and %d more)' % (len(names) - 3)
        if _no_headers:
            why += ('. Python.h is missing from %s: install the Python'
                    ' development package (python3-dev)' % _no_headers)
        if error:
            raise RuntimeWarning(why)
        _warnings.warn(why, RuntimeWarning, stacklevel=2)
    return diffs


_check = check   # start's own argument hides the name


def _env(name, allowed):
    value = _os.environ.get(name, '')
    if value not in allowed:
        raise ValueError('%s must be unset or one of: %s'
                         % (name, ', '.join(a for a in allowed if a)))
    return value


def start(check=False, error=False):
    """Start the JIT. LEJIT_START_CHECK=warning or error adds a check first,
    as check=True or error=True do. LEJIT_MODE=off makes this do nothing."""
    global _started
    if _core is None:
        _warnings.warn("lejit: %s, so start() does nothing" % _unsupported(),
                       RuntimeWarning, stacklevel=2)
        return
    mode = _env('LEJIT_MODE', ('', 'off'))
    when = _env('LEJIT_START_CHECK', ('', 'off', 'warning', 'error'))
    if mode == 'off' or _started:
        return
    if check or when in ('warning', 'error'):
        _check(error or when == 'error')
    _core._start()
    _started = True


def stop():
    """Stop the JIT. Code already running compiled finishes compiled."""
    global _started
    if _core and _started:
        _core._stop()
    _started = False
