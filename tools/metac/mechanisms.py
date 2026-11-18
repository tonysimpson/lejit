"""Choose Ni's own mechanisms from what a CPython source offers (D67 (4), D75).

Where Ni's own mechanism must differ between CPython versions (how compiled
code is entered, how a thread says it is tracing, how frames are shown to
Python), it is chosen here, never by a version number:

- a *detector* is a small function over the target's source (headers or C)
  that answers one capability question, with the definition it read;
- VARIANTS maps each mechanism's answers to the snippet that implements it:
  a small piece of C kept in Ni as ni/mechanisms/<mechanism>/<variant>.h;
- build() writes one header per mechanism (mechanism_<name>_gen.h) holding
  the chosen snippet, with the answers that chose it.

An answer this file doesn't map, or a detector that can't answer, stops the
build with the answers found: a new CPython that changes a mechanism needs a
new snippet and a VARIANTS line, never a guess. Run alone it prints the
answers for one or more source trees:

    mechanisms.py SRC [SRC...]
"""
import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctokens  # noqa: E402
import cslots  # noqa: E402

SNIPPETS = os.path.join(cslots.ROOT, 'ni', 'mechanisms')


class Source:
    """A CPython tree's headers and C files, tokenised as questions need
    them. Header questions look in every Include/**/*.h, so a definition
    moving between headers (pystate.h, cpython/pystate.h, internal/...)
    doesn't change the answer."""

    def __init__(self, src):
        self.src = src
        self.toks = {}
        self.headers = sorted(
            os.path.relpath(p, src) for p in
            glob.glob(os.path.join(src, 'Include', '**', '*.h'),
                      recursive=True))

    def tokens(self, path):
        """Tokens of one file (relative path), or None if it doesn't exist."""
        if path not in self.toks:
            full = os.path.join(self.src, path)
            if not os.path.isfile(full):
                self.toks[path] = None
            else:
                with open(full, encoding='latin-1') as f:
                    self.toks[path] = ctokens.tokenize(f.read(), path)[0]
        return self.toks[path]

    def headers_with(self, word):
        """(path, tokens) of each header whose text has `word`."""
        for path in self.headers:
            with open(os.path.join(self.src, path), encoding='latin-1') as f:
                if word in f.read():
                    yield path, self.tokens(path)

    def struct(self, name):
        """(path, {field: type text}) of the struct whose tag or typedef name
        is `name`, or (None, None). Both #if branches count, since the
        tokens keep both."""
        for path, toks in self.headers_with(name):
            for i, t in enumerate(toks):
                if t.text != '{' or i < 2:
                    continue
                if toks[i - 1].text == 'struct':
                    tag = None
                elif toks[i - 2].text == 'struct':
                    tag = toks[i - 1].text
                else:
                    continue
                try:
                    end = ctokens.match_close(toks, i)
                except SyntaxError:     # braces split by #if branches
                    continue
                if tag == name or (end + 1 < len(toks)
                                   and toks[end + 1].text == name):
                    return path, _fields(toks[i + 1:end])
            # typedef struct TAG NAME;
            for i in range(len(toks) - 3):
                if (toks[i].text == 'typedef' and toks[i + 1].text == 'struct'
                        and toks[i + 3].text == name
                        and toks[i + 4].text == ';'):
                    return self.struct(toks[i + 2].text)
        return None, None

    def declares(self, name):
        """The header declaring function `name` (NAME followed by '('), or
        None."""
        for path, toks in self.headers_with(name):
            for i, t in enumerate(toks[:-1]):
                if t.text == name and toks[i + 1].text == '(':
                    return path
        return None

    def function(self, path, name):
        """Body tokens of the C function `name` in `path`, or None."""
        toks = self.tokens(path)
        if toks is None:
            return None
        found = ctokens.functions(toks).get(name)
        return found[1] if found else None


def _fields(toks):
    """{name: type text} of a struct body's declarations."""
    out = {}
    for decl in ctokens.split_top(toks, ';'):
        ids = [k for k, t in enumerate(decl) if t.kind == 'id']
        if not ids or decl[0].text in ('struct', 'union') and \
                any(t.text == '{' for t in decl):
            continue
        k = ids[-1]
        out[decl[k].text] = ' '.join(t.text for t in decl[:k])
    return out


def _type_name(type_text):
    """'struct _PyCFrame *' -> '_PyCFrame'."""
    words = [w for w in type_text.split() if w not in ('struct', 'const', '*')]
    return words[-1] if words else None


# --- detectors: each (answer, evidence); evidence names what was read ----

def eval_hook_frame(s):
    """What the PEP 523 frame-evaluation hook is given: 'frame_object' (a
    PyFrameObject, struct _frame), 'interpreter_frame' (struct
    _PyInterpreterFrame), 'none' (no hook) or 'unknown'."""
    for path, toks in s.headers_with('_PyFrameEvalFunction'):
        for i in range(len(toks) - 3):
            if (toks[i].text == 'typedef' and '_PyFrameEvalFunction' in
                    [t.text for t in toks[i:i + 8]]):
                end = next(j for j in range(i, len(toks)) if toks[j].text == ';')
                params = [t.text for t in toks[i:end]]
                where = path + ':_PyFrameEvalFunction'
                if '_PyInterpreterFrame' in params:
                    return 'interpreter_frame', where
                if '_frame' in params or 'PyFrameObject' in params:
                    return 'frame_object', where
                return 'unknown', where
    return 'none', 'Include, every header'


def function_vectorcall(s):
    """Whether a function object's vectorcall can be replaced through API:
    'yes' if PyFunction_SetVectorcall is declared."""
    path = s.declares('PyFunction_SetVectorcall')
    return ('yes', path + ':PyFunction_SetVectorcall') if path else \
        ('no', 'Include, every header')


def function_watchers(s):
    """Whether Ni can be told when a function changes (__code__,
    __defaults__...): 'yes' if PyFunction_AddWatcher is declared."""
    path = s.declares('PyFunction_AddWatcher')
    return ('yes', path + ':PyFunction_AddWatcher') if path else \
        ('no', 'Include, every header')


def calls_bypass_vectorcall(s):
    """Whether the interpreter's specialised calls to Python functions
    (CALL_PY_EXACT_ARGS) can skip a function's replaced vectorcall: 'none'
    if there are no such calls; 'no' if a function whose vectorcall was
    replaced gets no version, so it is never specialised (the version
    function refuses it, or PyFunction_SetVectorcall clears the version);
    'yes' otherwise (the specialised call would run the bytecode
    interpreted, without Ni)."""
    spec = s.tokens('Python/specialize.c')
    if spec is None or 'CALL_PY_EXACT_ARGS' not in [t.text for t in spec]:
        return 'none', 'Python/specialize.c'
    fo = 'Objects/funcobject.c'
    getter = s.function(fo, '_PyFunction_GetVersionForCurrentState')
    setter = s.function(fo, 'PyFunction_SetVectorcall')
    if getter is None:
        return 'unknown', fo + ':_PyFunction_GetVersionForCurrentState'
    if 'vectorcall' in [t.text for t in getter]:
        return 'no', fo + ':_PyFunction_GetVersionForCurrentState'
    if setter is not None:
        text = ' '.join(t.text for t in setter)
        if 'func_version = 0' in text or '_PyFunction_SetVersion ( func , 0 )' in text:
            return 'no', fo + ':PyFunction_SetVectorcall'
    return 'yes', fo + ':_PyFunction_GetVersionForCurrentState'


def tracing_flag(s):
    """Where the per-thread "tracing or profiling is on" flag is:
    'tstate' (PyThreadState.use_tracing), 'cframe' (the thread's CFrame's
    use_tracing) or 'none'."""
    path, ts = s.struct('_ts')
    if ts is None:
        return 'unknown', 'Include, every header: _ts'
    if 'use_tracing' in ts:
        return 'tstate', path + ':_ts.use_tracing'
    if 'cframe' in ts:
        cpath, cf = s.struct(_type_name(ts['cframe']))
        if cf is not None and 'use_tracing' in cf:
            return 'cframe', '%s:%s.use_tracing' % (cpath, _type_name(ts['cframe']))
    return 'none', path + ':_ts'


def monitoring(s):
    """Whether sys.monitoring (PEP 669) exists, through which 3.12's
    settrace and setprofile also work: 'yes' if Python/instrumentation.c
    does."""
    if s.tokens('Python/instrumentation.c') is not None:
        return 'yes', 'Python/instrumentation.c'
    return 'no', 'Python/instrumentation.c'


def frame_locals(s):
    """Where a running frame's arguments and locals are:
    'frame_object' (PyFrameObject.f_localsplus), 'interpreter_frame'
    (_PyInterpreterFrame.localsplus) or 'unknown'."""
    path, fo = s.struct('_frame')
    if fo is not None and 'f_localsplus' in fo:
        return 'frame_object', path + ':_frame.f_localsplus'
    ipath, fi = s.struct('_PyInterpreterFrame')
    if fi is not None and 'localsplus' in fi:
        return 'interpreter_frame', ipath + ':_PyInterpreterFrame.localsplus'
    return 'unknown', 'Include, every header: _frame'


def current_frame(s):
    """Where the thread's innermost frame is: 'tstate.frame',
    'cframe.current_frame', 'tstate.current_frame' or 'unknown'."""
    path, ts = s.struct('_ts')
    if ts is None:
        return 'unknown', 'Include, every header: _ts'
    if 'frame' in ts:
        return 'tstate.frame', path + ':_ts.frame'
    if 'current_frame' in ts:
        return 'tstate.current_frame', path + ':_ts.current_frame'
    if 'cframe' in ts:
        name = _type_name(ts['cframe'])
        cpath, cf = s.struct(name)
        if cf is not None and 'current_frame' in cf:
            return 'cframe.current_frame', '%s:%s.current_frame' % (cpath, name)
    return 'unknown', path + ':_ts'


# --- the one place answers become snippets --------------------------------

# mechanism -> (its detectors in order, {answers: variant}). A variant is
# the file ni/mechanisms/<mechanism>/<variant>.h. Any answers not listed
# stop the build.
VARIANTS = {
    'entry': (
        (eval_hook_frame, function_vectorcall, function_watchers,
         calls_bypass_vectorcall),
        {('frame_object', 'no', 'no', 'none'): 'frame_eval_hook',
         ('interpreter_frame', 'no', 'no', 'yes'): 'frame_eval_hook_interp',
         ('interpreter_frame', 'yes', 'yes', 'no'): 'vectorcall'}),
    'tracing': (
        (tracing_flag, monitoring),
        {('tstate', 'no'): 'tstate_use_tracing',
         ('cframe', 'no'): 'cframe_use_tracing',
         ('none', 'yes'): 'monitoring'}),
    'frames': (
        (frame_locals, current_frame),
        {('frame_object', 'tstate.frame'): 'frame_object',
         ('interpreter_frame', 'cframe.current_frame'): 'interpreter_frame_cframe',
         ('interpreter_frame', 'tstate.current_frame'): 'interpreter_frame'}),
}


def header_name(mechanism):
    return 'mechanism_%s_gen.h' % mechanism


HEADERS = [header_name(m) for m in VARIANTS]


def detect(src):
    """{mechanism: ([(question, answer, evidence)], variant or None)}."""
    s = Source(src)
    out = {}
    for mech, (detectors, table) in VARIANTS.items():
        answers = [(d.__name__,) + d(s) for d in detectors]
        out[mech] = (answers, table.get(tuple(a for _, a, _ in answers)))
    return out


def build(target):
    """[(header, text, report)] for each mechanism; GenError on answers
    with no snippet."""
    out = []
    for mech, (answers, variant) in detect(target.src).items():
        said = ', '.join('%s=%s' % (q, a) for q, a, _ in answers)
        if variant is None:
            raise cslots.GenError(
                'mechanism %r: no snippet for these answers (%s); write'
                ' ni/mechanisms/%s/<variant>.h and map the answers to it in'
                ' tools/metac/mechanisms.py:VARIANTS' % (mech, said, mech))
        rel = os.path.join('ni', 'mechanisms', mech, variant + '.h')
        path = os.path.join(cslots.ROOT, rel)
        if not os.path.isfile(path):
            raise cslots.GenError('mechanism %r: %s is missing' % (mech, rel))
        with open(path) as f:
            snippet = f.read()
        guard = 'NI_' + header_name(mech).upper().replace('.', '_')
        lines = ['/* Generated by tools/metac/mechanisms.py from CPython %s:'
                 ' do not edit.' % target.version,
                 '   Mechanism %r, variant %r (%s), because:'
                 % (mech, variant, rel)]
        lines += ['     %s = %s  (%s)' % (q, a, e.replace('*/', '* /'))
                  for q, a, e in answers]
        lines += ['*/', '#ifndef ' + guard, '#define ' + guard,
                  '#line 1 "%s"' % rel, snippet.rstrip('\n'),
                  '#endif /* %s */' % guard, '']
        out.append((header_name(mech), '\n'.join(lines),
                    ['generated %s: %s (%s)' % (mech, variant, said)]))
    return out


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    for src in argv:
        import cpython_src
        print('%s (%s)' % (src, cpython_src.source_version(src)))
        for mech, (answers, variant) in detect(src).items():
            print('  %-8s -> %s' % (mech, variant or 'NO SNIPPET'))
            for q, a, e in answers:
                print('    %-24s %-22s %s' % (q, a, e))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
