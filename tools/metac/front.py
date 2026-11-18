"""The generator's front end (doc/GENERATOR_DESIGN.md "Front end"): a CPython
.c file preprocessed as CPython's own build compiles it, then parsed by the
vendored pycparser.

    t = target(python)          # the interpreter Ni is built for
    text = preprocess(t, 'Objects/abstract.c')
    ast = parse(t, 'Objects/abstract.c')

The target interpreter supplies its configuration through sysconfig: its
pyconfig.h, and the flags its Makefile compiled the core, built-in modules
and shared modules with. Of those flags only -D, -U, -std and the -I paths
relative to the build directory are kept; an absolute -I is the install's
prefix, whose headers must not shadow the source's. The build directory is
the source tree; pyconfig.h's directory is searched after it (-idirafter),
so only what the source lacks comes from there.

One departure from CPython's flags: -DUSE_COMPUTED_GOTOS=0 (D95).
"""
import collections
import json
import os
import re
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'third_party', 'pycparser'))
import pycparser  # noqa: E402
from pycparser import c_parser  # noqa: E402

PYCPARSER_VERSION = '3.11'
if pycparser.__version__ != PYCPARSER_VERSION:
    raise ImportError('front.py needs the vendored pycparser %s, got %s from %s'
                      % (PYCPARSER_VERSION, pycparser.__version__,
                         pycparser.__file__))

PRELUDE = os.path.join(HERE, 'shim', 'prelude.h')
VOCAB = os.path.join(HERE, 'shim', 'vocab.h')
DEPARTURES = ['-DUSE_COMPUTED_GOTOS=0']

Target = collections.namedtuple('Target', 'src version pyconfig flags builtins')


class FrontError(Exception):
    pass


_CONFIG = '''
import json, sys, sysconfig
v = sysconfig.get_config_var
print(json.dumps({"version": sys.version.split()[0],
                  "pyconfig": sysconfig.get_config_h_filename(),
                  "core": v("PY_CORE_CFLAGS"),
                  "builtin": v("PY_BUILTIN_MODULE_CFLAGS"),
                  "module": v("PY_STDMODULE_CFLAGS"),
                  "builtins": list(sys.builtin_module_names)}))
'''


def target(python, src=None):
    """The Target for interpreter `python`; its source is `src`, else
    cpython_src.find_source's."""
    import cpython_src
    try:
        r = subprocess.run([python, '-I', '-c', _CONFIG], stdout=subprocess.PIPE,
                           universal_newlines=True, timeout=60)
    except OSError as e:
        raise FrontError('cannot run %s: %s' % (python, e))
    if r.returncode != 0:
        raise FrontError('cannot run %s' % python)
    c = json.loads(r.stdout)
    try:
        if src:
            cpython_src.check_version(src, c['version'])
        else:
            src = cpython_src.find_source(c['version'])
    except cpython_src.SourceError as e:
        raise FrontError(str(e))
    src = os.path.abspath(src)
    flags = {kind: _build_flags(c[kind], src)
             for kind in ('core', 'builtin', 'module')}
    return Target(src, c['version'], c['pyconfig'], flags,
                  frozenset(c['builtins']))


def _build_flags(cflags, src):
    out = []
    for f in shlex.split(cflags or ''):
        if f.startswith(('-D', '-U', '-std=')):
            out.append(f)
        elif f.startswith('-I') and not os.path.isabs(f[2:]):
            out.append('-I' + os.path.normpath(os.path.join(src, f[2:])))
    return out


def flags_for(t, cfile):
    """CPython's flags for `cfile`: a module in Modules/ is compiled as a
    built-in or as a shared module, as the target was built."""
    if not cfile.startswith('Modules/'):
        return t.flags['core']
    with open(os.path.join(t.src, cfile)) as f:
        m = re.search(r'\bPyInit_(\w+)', f.read())
    if not m:
        raise FrontError('%s: no PyInit_ function, so no module name' % cfile)
    return t.flags['builtin' if m.group(1) in t.builtins else 'module']


def vocabulary():
    with open(VOCAB) as f:
        return re.findall(r'^#undef\s+(\w+)', f.read(), re.M)


_DIRECTIVE = re.compile(r'\s*#\s*(\w+)\s*(\w*)')


def splice(text, cfile, names):
    """`text` with the vocabulary shim included after each #include and
    each #define or #undef of a vocabulary name. A #line after each
    insertion, and after each #else, #elif and #endif (an insertion in a
    group that was skipped is not counted), keeps line numbers the file's."""
    out = ['#line 1 "%s"' % cfile]
    lines = text.split('\n')
    i = 0
    while i < len(lines):
        start = i
        while lines[i].endswith('\\') and i + 1 < len(lines):
            i += 1
        i += 1
        out.extend(lines[start:i])
        m = _DIRECTIVE.match(lines[start])
        if not m:
            continue
        word, name = m.groups()
        shim = word == 'include' or (word in ('define', 'undef') and name in names)
        if shim:
            out.append('#include "%s"' % VOCAB)
        if shim or word in ('else', 'elif', 'endif'):
            out.append('#line %d "%s"' % (i + 1, cfile))
    return '\n'.join(out)


_MARKER = re.compile(r'^# (\d+) "([^"]*)"', re.M)


def _gcc(t, cfile, text):
    cmd = (['gcc', '-E', '-dD'] + flags_for(t, cfile) + DEPARTURES
           + ['-include', PRELUDE, '-idirafter', os.path.dirname(t.pyconfig),
              '-x', 'c', '-'])
    cwd = os.path.dirname(os.path.join(t.src, cfile))
    r = subprocess.run(cmd, input=text, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True,
                       cwd=cwd, timeout=300)
    if r.returncode != 0:
        raise FrontError('%s: gcc -E failed:\n%s' % (cfile, r.stderr[-2000:]))
    # a header found next to stdin is named relative to cwd
    return _MARKER.sub(lambda m: '# %s "%s"' % (m.group(1), os.path.join(
        cwd, m.group(2)) if m.group(2)[:1] not in '/<' else m.group(2)), r.stdout)


def check_vocabulary(dd, path, names):
    """Problems in `dd`, gcc -E -dD output: a vocabulary name whose
    definition is not the shim's where code of the file `path` follows."""
    names = set(names)
    stale = {}
    problems = []
    current, line = None, 0
    for text in dd.split('\n'):
        m = _MARKER.match(text)
        if m:
            current, line = m.group(2), int(m.group(1))
            continue
        d = _DIRECTIVE.match(text)
        if d and d.group(1) in ('define', 'undef') and d.group(2) in names:
            if current == VOCAB:
                stale.pop(d.group(2), None)
            else:
                stale[d.group(2)] = '%s:%d' % (current, line)
        elif stale and current == path and text.strip() and not d:
            for name, where in sorted(stale.items()):
                problems.append('%s:%d: %s is defined by %s, not the'
                                ' vocabulary shim' % (path, line, name, where))
            stale.clear()
        line += 1
    return problems


def preprocess(t, cfile):
    """The preprocessed text of `cfile` (a path relative to the source tree),
    with line markers. Raises FrontError if gcc fails or a vocabulary name
    is not the shim's where the file uses code."""
    path = os.path.join(t.src, cfile)
    with open(path) as f:
        text = f.read()
    names = vocabulary()
    dd = _gcc(t, cfile, splice(text, path, names))
    problems = check_vocabulary(dd, path, names)
    if problems:
        raise FrontError('\n'.join(problems))
    return '\n'.join('' if s.startswith(('#define ', '#undef ')) else s
                     for s in dd.split('\n'))


def parse(t, cfile):
    """pycparser's AST of `cfile`; a pycparser.c_parser.ParseError names the
    file and line."""
    return c_parser.CParser().parse(preprocess(t, cfile),
                                    os.path.join(t.src, cfile))
