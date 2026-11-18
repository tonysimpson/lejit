"""Choose Ni's own mechanisms from what a CPython source offers (D67 (4), D75).

Where Ni's own mechanism must differ between CPython versions (how compiled
code is entered, how a thread says it is tracing, how frames are shown to
Python, D75; where eval_breaker and the current thread state are and what
ceval does on eval_breaker, how calls are counted, how the exception state
is held, how the GC tells a tracked object, D77), it is chosen here, never
by a version number:

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

    def macros(self, path):
        """{name: Macro} of one file's #defines, or None."""
        key = (path, 'macros')
        if key not in self.toks:
            full = os.path.join(self.src, path)
            if not os.path.isfile(full):
                self.toks[key] = None
            else:
                with open(full, encoding='latin-1') as f:
                    self.toks[key] = ctokens.tokenize(f.read(), path)[1]
        return self.toks[key]

    @staticmethod
    def body_of(toks, name):
        """Body tokens of the first definition NAME ( ... ) { ... } in
        `toks`, found where it is: unlike ctokens.functions, braces split
        by #if branches elsewhere in the file don't hide it. None if there
        is none, or its own braces don't match."""
        for i, t in enumerate(toks[:-1]):
            if t.text != name or toks[i + 1].text != '(':
                continue
            try:
                close = ctokens.match_close(toks, i + 1)
                if close + 1 < len(toks) and toks[close + 1].text == '{':
                    return toks[close + 2:ctokens.match_close(toks, close + 1)]
            except SyntaxError:
                return None
        return None

    def definitions(self, name):
        """[(path, tokens)] of every header definition of `name`: a macro's
        body, a function's body (both #if branches count)."""
        out = []
        for path, toks in self.headers_with(name):
            m = self.macros(path).get(name)
            if m is not None:
                out.append((path, m.body))
            body = self.body_of(toks, name)
            if body is not None:
                out.append((path, body))
        return out

    def declaration(self, name):
        """(path, tokens) of the header statement declaring function or
        variable `name` (from the previous ';', '}' or '{'), or (None,
        None)."""
        for path, toks in self.headers_with(name):
            for i, t in enumerate(toks[:-1]):
                if t.text == name and toks[i + 1].text in ('(', ';'):
                    j = i
                    while j > 0 and toks[j - 1].text not in (';', '}', '{'):
                        j -= 1
                    if toks[j].text in ('define', 'return'):
                        continue
                    return path, toks[j:i + 1]
        return None, None

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


def _texts(toks):
    return [t.text for t in toks] if toks else []


def _field_of(s, structs, field):
    """(struct, path) of the first struct in `structs` (tags or typedef
    names) declaring `field`, or (None, None)."""
    for name in structs:
        path, fields = s.struct(name)
        if fields is not None and field in fields:
            return name, path
    return None, None


def eval_breaker_home(s):
    """Where the word that tells the evaluator to break off (eval_breaker)
    is: 'runtime' (_PyRuntime.ceval), 'interpreter' (the interpreter's
    ceval state), 'thread' (the thread state) or 'unknown'."""
    for struct, answer in (('_ts', 'thread'), ('_ceval_state', 'interpreter'),
                           ('_ceval_runtime_state', 'runtime')):
        name, path = _field_of(s, (struct,), 'eval_breaker')
        if name:
            return answer, '%s:%s.eval_breaker' % (path, name)
    return 'unknown', 'Include, every header: eval_breaker'


def pending_handler(s):
    """What ceval does when eval_breaker is set, as Ni can reach it:
    'api_function' (_Py_HandlePending, exported: Ni can call it),
    'internal_function' (_Py_HandlePending, not exported), 'static_function'
    (ceval.c's static eval_frame_handle_pending), 'loop_top' (inline at the
    top of _PyEval_EvalFrameDefault's loop) or 'unknown'. Anything but the
    first means Ni holds a copy."""
    path, decl = s.declaration('_Py_HandlePending')
    if decl:
        api = 'PyAPI_FUNC' in _texts(decl)
        return ('api_function' if api else 'internal_function'), \
            path + ':_Py_HandlePending'
    ce = 'Python/ceval.c'
    toks = s.tokens(ce) or []
    if s.body_of(toks, 'eval_frame_handle_pending') is not None:
        return 'static_function', ce + ':eval_frame_handle_pending'
    if 'eval_breaker' in _texts(s.body_of(toks, '_PyEval_EvalFrameDefault')):
        return 'loop_top', ce + ':_PyEval_EvalFrameDefault'
    return 'unknown', ce


def signals_pending(s):
    """Whether a signal sets a flag of its own (signals_pending) that the
    handler tests before the pending calls: 'yes', or 'no' (a signal comes
    as a pending call)."""
    name, path = _field_of(s, ('_ceval_runtime_state', '_ceval_state'),
                           'signals_pending')
    if name:
        return 'yes', '%s:%s.signals_pending' % (path, name)
    return 'no', 'Include, every header: _ceval_runtime_state, _ceval_state'


def tstate_current(s):
    """Where the running thread's state is: 'runtime_gilstate' (a word in
    _PyRuntime, gilstate.tstate_current, at a fixed address),
    'thread_local' (a C thread-local, _Py_tss_tstate) or 'unknown'."""
    name, path = _field_of(s, ('_gilstate_runtime_state',), 'tstate_current')
    if name:
        return 'runtime_gilstate', '%s:%s.tstate_current' % (path, name)
    path, decl = s.declaration('_Py_tss_tstate')
    if decl and '_Py_thread_local' in _texts(decl):
        return 'thread_local', path + ':_Py_tss_tstate'
    return 'unknown', 'Include, every header: tstate_current, _Py_tss_tstate'


def recursion_counter(s):
    """How a thread counts the Python calls it is in: 'depth_up'
    (recursion_depth, incremented), 'remaining_down' (recursion_remaining,
    decremented), 'py_remaining_down' (py_recursion_remaining, Python calls
    only, beside a C counter) or 'unknown'."""
    path, ts = s.struct('_ts')
    if ts is None:
        return 'unknown', 'Include, every header: _ts'
    for field, answer in (('py_recursion_remaining', 'py_remaining_down'),
                          ('recursion_remaining', 'remaining_down'),
                          ('recursion_depth', 'depth_up')):
        if field in ts:
            return answer, '%s:_ts.%s' % (path, field)
    return 'unknown', path + ':_ts'


def recursion_limit(s):
    """Where the limit the counter is compared with is: 'global'
    (_Py_CheckRecursionLimit, an exported int), 'tstate' (a field of the
    thread state) or 'unknown'."""
    path, decl = s.declaration('_Py_CheckRecursionLimit')
    if decl and 'PyAPI_DATA' in _texts(decl):
        return 'global', path + ':_Py_CheckRecursionLimit'
    path, ts = s.struct('_ts')
    for field in ('py_recursion_limit', 'recursion_limit'):
        if ts is not None and field in ts:
            return 'tstate', '%s:_ts.%s' % (path, field)
    return 'unknown', 'Include, every header: _Py_CheckRecursionLimit, _ts'


def recursion_overflow(s):
    """How a thread allows the calls that handle a RecursionError:
    'overflowed' (a flag, cleared below a low-water mark), 'headroom' (a
    count of extra calls) or 'none'."""
    path, ts = s.struct('_ts')
    if ts is None:
        return 'unknown', 'Include, every header: _ts'
    for field, answer in (('overflowed', 'overflowed'),
                          ('recursion_headroom', 'headroom')):
        if field in ts:
            return answer, '%s:_ts.%s' % (path, field)
    return 'none', path + ':_ts'


def exc_current(s):
    """How the thread state holds the exception being raised: 'triple'
    (curexc_type, curexc_value, curexc_traceback), 'one'
    (current_exception, its traceback on the object) or 'unknown'."""
    path, ts = s.struct('_ts')
    if ts is None:
        return 'unknown', 'Include, every header: _ts'
    if 'curexc_type' in ts:
        return 'triple', path + ':_ts.curexc_type'
    if 'current_exception' in ts:
        return 'one', path + ':_ts.current_exception'
    return 'unknown', path + ':_ts'


def exc_info(s):
    """How an entry of the handled-exception stack (sys.exc_info(),
    tstate->exc_info) holds its exception: 'triple' (exc_type, exc_value,
    exc_traceback), 'value' (exc_value alone) or 'unknown'."""
    path, item = s.struct('_PyErr_StackItem')
    if item is None:
        return 'unknown', 'Include, every header: _PyErr_StackItem'
    # 'PyObject *exc_type, *exc_value, *exc_traceback;' is one declaration:
    # its earlier names are in the type text
    names = set(item) | {w for v in item.values() for w in v.split()}
    if 'exc_type' in names:
        return 'triple', path + ':_PyErr_StackItem.exc_type'
    if 'exc_value' in names:
        return 'value', path + ':_PyErr_StackItem.exc_value'
    return 'unknown', path + ':_PyErr_StackItem'


def gc_tracked_test(s):
    """How _PyObject_GC_IS_TRACKED tells a tracked object, from its header
    definitions (both #if branches): 'gc_refs' (PyGC_Head's gc_refs isn't
    _PyGC_REFS_UNTRACKED), 'gc_next' (_gc_next isn't 0), 'ob_gc_bits' (a
    bit in the object header, free-threaded builds); several joined by '+',
    or 'unknown'."""
    found, where = set(), set()
    for path, toks in s.definitions('_PyObject_GC_IS_TRACKED'):
        texts = _texts(toks)
        for marker, answer in (('_PyGC_REFS_UNTRACKED', 'gc_refs'),
                               ('_gc_next', 'gc_next'),
                               ('_PyGC_BITS_TRACKED', 'ob_gc_bits')):
            if marker in texts:
                found.add(answer)
                where.add(path)
    if not found:
        return 'unknown', 'Include, every header: _PyObject_GC_IS_TRACKED'
    return '+'.join(sorted(found)), \
        ', '.join(sorted(where)) + ':_PyObject_GC_IS_TRACKED'


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
    # Python/pyruntime.c (Py_BUILD_CORE): where eval_breaker is, and what
    # ceval does when it is set
    'eval_breaker_runtime': (
        (eval_breaker_home, pending_handler, signals_pending),
        {('runtime', 'loop_top', 'no'): 'ceval_loop_top',
         ('runtime', 'loop_top', 'yes'): 'ceval_loop_top_signals',
         ('interpreter', 'static_function', 'yes'): 'static_handler_copy',
         ('interpreter', 'internal_function', 'yes'): 'internal_handler_copy',
         ('thread', 'api_function', 'no'): 'handler_call'}),
    # compiled code's test of it (psyfunc.c)
    'eval_breaker': (
        (eval_breaker_home,),
        {('runtime',): 'int32_address',
         ('interpreter',): 'int32_address',
         ('thread',): 'tstate_word'}),
    # Python/pyruntime.c: where the current thread state is
    'tstate_runtime': (
        (tstate_current,),
        {('runtime_gilstate',): 'runtime_address',
         ('thread_local',): 'thread_local'}),
    # compiled code's load of it (psyfunc.c, pmethodobject.c)
    'tstate': (
        (tstate_current,),
        {('runtime_gilstate',): 'runtime_address',
         ('thread_local',): 'thread_local'}),
    # the recursion count of compiled calls and entries (psyfunc.c,
    # eval_hook.c, frames.c, pycompiler.c)
    'recursion': (
        (recursion_counter, recursion_limit, recursion_overflow),
        {('depth_up', 'global', 'overflowed'): 'depth_up',
         ('remaining_down', 'tstate', 'headroom'): 'remaining_down',
         ('py_remaining_down', 'tstate', 'headroom'): 'py_remaining_down'}),
    # the exception being raised and the one being handled (pycompiler.h,
    # pycompiler.c, pmethodobject.c, x64/iencoding.c)
    'exceptions': (
        (exc_current, exc_info),
        {('triple', 'triple'): 'curexc_triple',
         ('triple', 'value'): 'curexc_triple_info_value',
         ('one', 'value'): 'current_exception'}),
    # the inline attribute store's test that the dict is tracked by the GC
    # (x64/ipyencoding.c)
    'store_gc': (
        (gc_tracked_test,),
        {('gc_refs',): 'gc_refs',
         ('gc_next',): 'gc_next',
         ('gc_next+ob_gc_bits',): 'gc_next'}),
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
