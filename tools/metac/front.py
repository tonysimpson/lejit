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

parse() is cached (cache.py). Preprocessing is keyed on the .c file, its
flags, gcc, the shims and this front end, and checked against the hash of
every file its line markers name, so an edited header is seen; a header
newly added where the include path would find it first is not (NI_NO_CACHE
bypasses the cache). The AST is keyed on the preprocessed text's hash.

One departure from CPython's flags: -DUSE_COMPUTED_GOTOS=0, since computed
gotos don't parse; with them off ceval.c's TARGETs are cases of one switch.
"""
import collections
import functools
import glob
import hashlib
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
import cache  # noqa: E402

PYCPARSER_VERSION = '3.11'
if pycparser.__version__ != PYCPARSER_VERSION:
    raise ImportError('front.py needs the vendored pycparser %s, got %s from %s'
                      % (PYCPARSER_VERSION, pycparser.__version__,
                         pycparser.__file__))

PRELUDE = os.path.join(HERE, 'shim', 'prelude.h')
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


_LISTS = ('OBJECT_OBJS', 'PYTHON_OBJS', 'POBJS', 'PARSER_OBJS', 'MODULE_OBJS')


@functools.lru_cache()
def library_files(src):
    """The .c files Makefile.pre.in links into libpython, from those of its
    object lists that don't depend on configure, and Setup's modules."""
    with open(os.path.join(src, 'Makefile.pre.in')) as f:
        text = f.read().replace('\\\n', ' ')
    out = set()
    for var in _LISTS:
        m = re.search(r'^%s=(.*)$' % var, text, re.M)
        if not m:
            raise FrontError('Makefile.pre.in has no %s' % var)
        for w in m.group(1).split():
            c = w[:-2] + '.c'
            if w.endswith('.o') and '$' not in w and os.path.isfile(os.path.join(src, c)):
                out.add(c)
    return out | set(setup_modules(src))


@functools.lru_cache()
def setup_modules(src):
    """{cfile: flags} for the modules Modules/Setup (Setup.dist until 3.7)
    builds into every interpreter: its lines before any marker, with their
    own -D and -I flags."""
    path = os.path.join(src, 'Modules', 'Setup')
    if os.path.isfile(path + '.dist'):
        path += '.dist'
    out = {}
    with open(path) as f:
        for line in f:
            words = line.split('#')[0].split()
            if words and words[0].startswith('*'):
                break
            if not words or '=' in words[0]:
                continue
            own = [w.replace('$(srcdir)', src) for w in words[1:] if w.startswith(('-D', '-I'))]
            for w in words[1:]:
                if w.endswith('.c'):
                    out['Modules/' + w] = own
    return out


@functools.lru_cache()
def rule_defines(src):
    """{cfile: flags} for the files whose own Makefile.pre.in rule defines
    macros. Their values are strings made from make variables, so each is
    an empty string here."""
    with open(os.path.join(src, 'Makefile.pre.in')) as f:
        text = f.read().replace('\\\n', ' ')
    out = {}
    for m in re.finditer(r'^(\S+)\.o:.*\n((?:\t.*\n)+)', text, re.M):
        names = re.findall(r'-D(\w+)=', m.group(2))
        if names:
            out[m.group(1) + '.c'] = ['-D%s=""' % n for n in names]
    return out


def flags_for(t, cfile):
    """CPython's flags for `cfile`: Setup's modules are built in; another
    module in Modules/ outside the library is compiled as a built-in or as a
    shared module, as the target was built."""
    own = rule_defines(t.src).get(cfile, [])
    setup = setup_modules(t.src)
    if cfile in setup:
        return t.flags['builtin'] + setup[cfile]
    if not cfile.startswith('Modules/') or cfile in library_files(t.src):
        return t.flags['core'] + own
    with open(os.path.join(t.src, cfile)) as f:
        m = re.search(r'\bPyInit_(\w+)', f.read())
    if not m:
        raise FrontError('%s: no PyInit_ function, so no module name' % cfile)
    return t.flags['builtin' if m.group(1) in t.builtins else 'module']


def mirrored_files():
    """Every .c file Ni mirrors: those the policy table names, those the
    generators read, and ceval.c."""
    import policy_table
    files = {k.split(':')[0] for k in policy_table.ENTRIES}
    files |= set(policy_table.TRANSLATED_SLOTS)
    from gen2 import scope
    files |= {f.cfile for f in scope.FAMILIES.values()}
    for path in glob.glob(os.path.join(HERE, 'gen_*.py')):
        with open(path) as f:
            files |= set(re.findall(r"'((?:Objects|Python|Modules)/[\w/-]+\.c)'",
                                    f.read()))
    files.add('Python/ceval.c')
    return sorted(f for f in files if f.endswith('.c'))


def vocabulary():
    from gen2 import vocabulary
    return vocabulary.names()


def vocab_shim():
    from gen2 import vocabulary
    return vocabulary.shim_path()


_DIRECTIVE = re.compile(r'\s*#\s*(\w+)\s*(\w*)')


def splice(text, cfile, names, shim):
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
        insert = word == 'include' or (word in ('define', 'undef') and name in names)
        if insert:
            out.append('#include "%s"' % shim)
        if insert or word in ('else', 'elif', 'endif'):
            out.append('#line %d "%s"' % (i + 1, cfile))
    return '\n'.join(out)


_MARKER = re.compile(r'^# (\d+) "([^"]*)"', re.M)


def _gcc_cmd(t, cfile):
    cmd = (['gcc', '-E', '-dD'] + flags_for(t, cfile) + DEPARTURES
           + ['-include', PRELUDE, '-idirafter', os.path.dirname(t.pyconfig),
              '-x', 'c', '-'])
    return cmd, os.path.dirname(os.path.join(t.src, cfile))


def _gcc(t, cfile, text):
    cmd, cwd = _gcc_cmd(t, cfile)
    r = subprocess.run(cmd, input=text, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, universal_newlines=True,
                       cwd=cwd, timeout=300)
    if r.returncode != 0:
        raise FrontError('%s: gcc -E failed:\n%s' % (cfile, r.stderr[-2000:]))
    # a header found next to stdin is named relative to cwd
    return _MARKER.sub(lambda m: '# %s "%s"' % (m.group(1), os.path.join(
        cwd, m.group(2)) if m.group(2)[:1] not in '/<' else m.group(2)), r.stdout)


def check_vocabulary(dd, path, names, shim):
    """Problems in `dd`, gcc -E -dD output: a vocabulary name whose
    definition is not the shim's (the file `shim`) where code of the file
    `path` follows."""
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
            if current == shim:
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


def preprocess(t, cfile, extra=''):
    """The preprocessed text of `cfile` (a path relative to the source tree),
    with line markers. `extra` is C put after the file's own text (the
    rules' C, read in the file's scope). Raises FrontError if gcc fails or
    a vocabulary name is not the shim's where the file uses code."""
    path = os.path.join(t.src, cfile)
    with open(path) as f:
        text = f.read() + ('\n#line 1 "<rules>"\n' + extra if extra else '')
    names, shim = vocabulary(), vocab_shim()
    dd = _gcc(t, cfile, splice(text, path, names, shim))
    problems = check_vocabulary(dd, path, names, shim)
    if problems:
        raise FrontError('\n'.join(problems))
    return '\n'.join('' if s.startswith(('#define ', '#undef ')) else s
                     for s in dd.split('\n'))


@functools.lru_cache()
def code_digest():
    """What the front end's results depend on besides their inputs: this
    file, the vendored pycparser and the prelude."""
    pkg = os.path.dirname(pycparser.__file__)
    paths = [os.path.abspath(__file__), PRELUDE] + sorted(
        glob.glob(os.path.join(pkg, '*.py')))
    return cache.key(PYCPARSER_VERSION, *[cache.file_digest(p) for p in paths])


@functools.lru_cache()
def _gcc_identity():
    r = subprocess.run(['gcc', '-v'], stderr=subprocess.PIPE,
                       stdout=subprocess.DEVNULL, universal_newlines=True,
                       timeout=60)
    return r.stderr


def _deps_same(deps):
    try:
        return all(cache.file_digest(p) == d for p, d in deps)
    except OSError:
        return False


def _preprocessed(t, cfile, extra):
    """(the sha256 of preprocess's text, a function returning the text)."""
    path = os.path.join(t.src, cfile)
    names, shim = vocabulary(), vocab_shim()
    cmd, cwd = _gcc_cmd(t, cfile)
    k = cache.key('pp', code_digest(), path, cache.file_digest(path), extra,
                  '\0'.join(cmd), cwd, _gcc_identity(), '\0'.join(names),
                  cache.file_digest(shim))
    e = cache.load('pp', k)
    if isinstance(e, tuple) and len(e) == 2 and _deps_same(e[0]):
        sha = e[1]
        return sha, lambda: cache.load('text', sha) or preprocess(t, cfile, extra)
    text = preprocess(t, cfile, extra)
    sha = hashlib.sha256(text.encode()).hexdigest()
    deps = sorted({p for _, p in _MARKER.findall(text) if os.path.isabs(p)})
    cache.store('text', sha, text)
    cache.store('pp', k, ([(p, cache.file_digest(p)) for p in deps], sha))
    return sha, lambda: text


def source_digest(t, cfile, extra=''):
    """The sha256 of preprocess's text, from the cache when it can be."""
    return _preprocessed(t, cfile, extra)[0]


def parse(t, cfile, extra=''):
    """pycparser's AST of `cfile` (and `extra`, as preprocess); a
    pycparser.c_parser.ParseError names the file and line."""
    sha, text = _preprocessed(t, cfile, extra)
    filename = os.path.join(t.src, cfile)
    k = cache.key('ast', code_digest(), sha, filename)
    e = cache.load('ast', k)
    if isinstance(e, tuple) and len(e) == 2 and e[0] == 'ast':
        return e[1]
    if isinstance(e, tuple) and len(e) == 2 and e[0] == 'error':
        raise c_parser.ParseError(e[1])
    try:
        ast = c_parser.CParser().parse(text(), filename)
    except c_parser.ParseError as ex:
        cache.store('ast', k, ('error', str(ex)))
        raise
    cache.store('ast', k, ('ast', ast))
    return ast
