#!/usr/bin/env python3
"""Detect CPython's features from the source Ni is built against, never
from its version number: a release with a backport, a development version,
a distribution's patches or a fork all say what they have only in their
source.

Each feature is a short question on the source (a struct's fields, a
declaration and its parameters, a macro, a function body, an opcode),
answering 1 or 0 with the definition it read. setup.py runs this before
compiling, to write ni_features_gen.h into the build directory: one
`#define NI_HAS_<NAME> 0|1` per feature, the opcodes the source lacks, the
jump opcodes, and what ni.build_info reports. A question the source can't
answer (the struct or file it reads is missing) stops the build: a new
CPython that moves a definition needs the detector updated, never a guess.

    features.py --python EXE --out DIR [--src DIR]
        write DIR/ni_features_gen.h for the interpreter EXE; the source is
        --src, else NI_CPYTHON_SRC, else the release tarball from the
        cache (cpython_src.py)
    features.py SRC [SRC...]
        print the features of one or more source trees

It runs under the Python being built for (3.7 and later).
"""
import argparse
import ast
import glob
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctokens  # noqa: E402

HEADER = 'ni_features_gen.h'


class FeatureError(Exception):
    """The source doesn't answer a feature's question: stop the build."""


class Unanswered(Exception):
    pass


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

    def exists(self, path):
        return os.path.isfile(os.path.join(self.src, path))

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
                        and toks[i + 2].text != name
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


def _texts(toks):
    return [t.text for t in toks] if toks else []


def _has_run(texts, want):
    n = len(want)
    return any(texts[i:i + n] == want for i in range(len(texts) - n + 1))


def _c(text):
    """The token texts of a piece of C."""
    return _texts(ctokens.tokenize(text)[0])


# --- the questions' building blocks: each (answer, evidence) -------------

def _field(s, struct, field, missing=None):
    """struct has field; a missing struct is `missing`'s answer, or
    unanswered when that is None."""
    path, fields = s.struct(struct)
    if fields is None:
        if missing is None:
            raise Unanswered('no struct %s in Include' % struct)
        return missing, 'Include, every header: %s' % struct
    return field in fields, '%s:%s.%s' % (path, struct, field)


def _exported(s, name):
    path, decl = s.declaration(name)
    if decl is None:
        return False, 'Include, every header: %s' % name
    return 'PyAPI_FUNC' in _texts(decl), '%s:%s' % (path, name)


def _declared(s, name):
    path = s.declares(name)
    return bool(path), '%s:%s' % (path or 'Include, every header', name)


def _macro(s, name):
    for path, _ in s.headers_with(name):
        if name in s.macros(path):
            return True, '%s:%s' % (path, name)
    return False, 'Include, every header: %s' % name


def _file(s, path):
    return s.exists(path), path


def _defines(s, path, function):
    """path defines function"""
    toks = s.tokens(path)
    if toks is None:
        raise Unanswered('no %s' % path)
    return s.body_of(toks, function) is not None, '%s:%s' % (path, function)


def _body_has(s, path, function, text, missing=None):
    """function's body in path has the tokens of text in a row; a missing
    function is `missing`'s answer, or unanswered when that is None"""
    body = s.body_of(s.tokens(path) or [], function)
    if body is None:
        if missing is None:
            raise Unanswered('no %s in %s' % (function, path))
        return missing, '%s: no %s' % (path, function)
    return _has_run(_texts(body), _c(text)), '%s:%s' % (path, function)


def _file_has(s, path, text):
    """path has the tokens of text in a row"""
    toks = s.tokens(path)
    if toks is None:
        raise Unanswered('no %s' % path)
    return _has_run(_texts(toks), _c(text)), path


def _case_tokens(s, opcode):
    """ceval's case for opcode, to the next case; None where there is none"""
    texts = _texts(s.tokens('Python/ceval.c'))
    for i in range(len(texts) - 3):
        if texts[i:i + 4] == ['TARGET', '(', opcode, ')']:
            j = i + 4
            while j < len(texts) - 1 and texts[j:j + 2] != ['TARGET', '(']:
                j += 1
            return texts[i + 4:j]
    return None


def _case_uses(s, opcode, word):
    """whether ceval's case for opcode names word before the next case;
    False where ceval has no such case"""
    return _case_has(s, opcode, word, missing=False)


def _case_has(s, opcode, text, missing=None):
    ce = 'Python/ceval.c'
    case = _case_tokens(s, opcode)
    if case is None:
        if missing is None:
            raise Unanswered('%s: no TARGET(%s)' % (ce, opcode))
        return missing, '%s: no TARGET(%s)' % (ce, opcode)
    return _has_run(case, _c(text)), '%s:TARGET(%s)' % (ce, opcode)


def _ceval_macro_uses(s, name, word):
    ce = 'Python/ceval.c'
    m = (s.macros(ce) or {}).get(name)
    if m is None:
        raise Unanswered('no %s macro in %s' % (name, ce))
    return word in _texts(m.body), '%s:%s' % (ce, name)


def _takes_tstate(s, name):
    path, decl = s.declaration(name)
    if decl is None:
        return False, 'Include, every header: ' + name
    # the parameters follow the name, which ends the declaration's tokens
    toks = s.tokens(path)
    for i, t in enumerate(toks[:-2]):
        if t.text == name and toks[i + 1].text == '(':
            return toks[i + 2].text == 'PyThreadState', path + ':' + name
    return False, path + ':' + name


def _linkable(s, name):
    """(name can be linked to by an extension, evidence): declared
    PyAPI_FUNC, or any function of a build that doesn't hide the rest
    (configure's -fvisibility=hidden, from 3.9)."""
    path, decl = s.declaration(name)
    if decl is None:
        raise Unanswered('no declaration of %s' % name)
    if 'PyAPI_FUNC' in _texts(decl):
        return True, '%s:%s' % (path, name)
    with open(os.path.join(s.src, 'configure.ac')) as f:
        hides = '-fvisibility=hidden' in f.read()
    return not hides, '%s:%s, configure.ac' % (path, name)


def _not(answer):
    value, evidence = answer
    return not value, evidence


def _detector(doc, ask):
    """A detector from a docstring and a function of the Source."""
    def detect(s):
        return ask(s)
    detect.__doc__ = doc
    return detect


# --- the questions -------------------------------------------------------

def pydict_newpresized(s):
    """_PyDict_NewPresized is exported, so compiled code can call it"""
    return _exported(s, '_PyDict_NewPresized')


def audit_hooks(s):
    """PySys_Audit is exported (audit hooks, PEP 578)"""
    return _exported(s, 'PySys_Audit')


def arg_check_positional(s):
    """Argument Clinic's wrappers check positional counts with
    _PyArg_CheckPositional"""
    return _declared(s, '_PyArg_CheckPositional')


def objectdict_setitem_linkable(s):
    """_PyObjectDict_SetItem, the instance dict store that keeps a class's
    shared keys, can be called from an extension"""
    return _linkable(s, '_PyObjectDict_SetItem')


def check_result_tstate(s):
    """_Py_CheckFunctionResult takes the thread state first"""
    return _takes_tstate(s, '_Py_CheckFunctionResult')


def float_asdouble_index(s):
    """PyFloat_AsDouble accepts objects with only __index__ (nb_index)"""
    return _body_has(s, 'Objects/floatobject.c', 'PyFloat_AsDouble', 'nb_index')


def map_add_value_on_top(s):
    """MAP_ADD finds the value on top of the stack and the key under it"""
    ce = 'Python/ceval.c'
    toks = s.tokens(ce) or []
    for i in range(len(toks) - 3):
        if [t.text for t in toks[i:i + 4]] == ['TARGET', '(', 'MAP_ADD', ')']:
            for j in range(i + 4, min(i + 200, len(toks) - 4)):
                if [t.text for t in toks[j + 1:j + 5]] == ['=', 'TOP', '(', ')']:
                    if toks[j].text in ('key', 'value'):
                        return toks[j].text == 'value', ce + ':TARGET(MAP_ADD)'
                    break
            break
    # from 3.12 the instruction is bytecodes.c's: inst(MAP_ADD, (in -- out)),
    # its last input on top
    bc = 'Python/bytecodes.c'
    toks = s.tokens(bc) or []
    for i in range(len(toks) - 4):
        if [t.text for t in toks[i:i + 4]] == ['inst', '(', 'MAP_ADD', ',']:
            j = i + 5
            names = []
            while j < len(toks) and toks[j].text != '--':
                if toks[j].kind == 'id':
                    names.append(toks[j].text)
                j += 1
            if names and names[-1] in ('key', 'value'):
                return names[-1] == 'value', bc + ':inst(MAP_ADD)'
            break
    raise Unanswered('%s: no TARGET(MAP_ADD) reading key and value with TOP()' % ce)


def pop_block_unwinds(s):
    """POP_BLOCK drops the value stack down to the block's level"""
    return _case_uses(s, 'POP_BLOCK', 'UNWIND_BLOCK')


def pop_except_unwinds(s):
    """POP_EXCEPT drops the value stack down to the handler's three items"""
    return _case_uses(s, 'POP_EXCEPT', 'UNWIND_EXCEPT_HANDLER')


def jump_in_instructions(s):
    """a jump's argument counts instructions, not bytes (ceval's JUMPTO has no / sizeof)"""
    return _not(_ceval_macro_uses(s, 'JUMPTO', 'sizeof'))


def lasti_in_instructions(s):
    """a frame's f_lasti and a block's b_handler count instructions, not bytes (ceval's INSTR_OFFSET has no sizeof)"""
    return _not(_ceval_macro_uses(s, 'INSTR_OFFSET', 'sizeof'))


def lineno_when_set(s):
    """a frame's f_lineno is its line whenever it is not 0, traced or not (PyFrame_GetLineNumber tests f_lineno, not f_trace)"""
    fo = 'Objects/frameobject.c'
    body = s.body_of(s.tokens(fo) or [], 'PyFrame_GetLineNumber')
    if body is None:
        raise Unanswered('no PyFrame_GetLineNumber in %s' % fo)
    texts = _texts(body)
    return 'f_trace' not in texts and 'f_lineno' in texts, fo + ':PyFrame_GetLineNumber'


def frame_builtins_of_function(s):
    """a new frame's builtins are its function's (func_builtins, read through
    the PyFrameConstructor), not looked up in its globals"""
    return _body_has(s, 'Objects/frameobject.c', '_PyFrame_New_NoTrack', 'fc_builtins')


def immutable_type_flag(s):
    """a type's attributes and its instances' __class__ are fixed when it has Py_TPFLAGS_IMMUTABLETYPE (type_setattro and object_set_class test the flag), not when it is a static type"""
    to = 'Objects/typeobject.c'
    a, _ = _body_has(s, to, 'type_setattro', 'Py_TPFLAGS_IMMUTABLETYPE')
    b, _ = _body_has(s, to, 'object_set_class', 'Py_TPFLAGS_IMMUTABLETYPE')
    if a != b:
        raise Unanswered('%s: type_setattro and object_set_class differ on '
                         'Py_TPFLAGS_IMMUTABLETYPE' % to)
    return a, to + ':type_setattro, object_set_class'


def eval_hook_tstate(s):
    """the PEP 523 frame-evaluation hook is given the thread state first"""
    for path, toks in s.headers_with('_PyFrameEvalFunction'):
        texts = _texts(toks)
        want = ['PyObject', '*', '(', '*', '_PyFrameEvalFunction', ')', '(']
        for i in range(len(texts) - len(want)):
            if texts[i] == 'typedef' and texts[i + 1:i + 1 + len(want)] == want:
                return (texts[i + 1 + len(want)] == 'PyThreadState',
                        path + ':_PyFrameEvalFunction')
    raise Unanswered('no typedef of _PyFrameEvalFunction in Include')


def gc_tracked_by_next(s):
    """_PyObject_GC_IS_TRACKED tests the GC header's _gc_next, not gc_refs"""
    found = s.definitions('_PyObject_GC_IS_TRACKED')
    if not found:
        raise Unanswered('no definition of _PyObject_GC_IS_TRACKED in Include')
    return (any('_gc_next' in _texts(toks) for _, toks in found),
            ', '.join(sorted({p for p, _ in found})) + ':_PyObject_GC_IS_TRACKED')


def gc_head_internal(s):
    """the GC header (PyGC_Head) is described only in an internal header,
    as the two words _gc_next and _gc_prev"""
    path, fields = s.struct('PyGC_Head')
    if path is None or not path.startswith('Include/internal/'):
        return False, (path or 'Include, every header') + ':PyGC_Head'
    if list(fields.items()) != [('_gc_next', 'uintptr_t'), ('_gc_prev', 'uintptr_t')]:
        raise Unanswered('%s:PyGC_Head is not _gc_next, _gc_prev' % path)
    return True, path + ':PyGC_Head'


def frame_state(s):
    """a frame says whether it is running in f_state, not f_executing"""
    path, fields = s.struct('_frame')
    if fields is None:
        raise Unanswered('no struct _frame in Include')
    if 'f_state' not in fields and 'f_executing' not in fields:
        raise Unanswered('%s:_frame has neither f_state nor f_executing' % path)
    return 'f_state' in fields, path + ':_frame.f_state'


def cframe(s):
    """a thread's "tracing or profiling is on" flag is its CFrame's
    use_tracing, not the thread state's"""
    path, ts = s.struct('_ts')
    if ts is None:
        raise Unanswered('no struct _ts in Include')
    if 'use_tracing' in ts:
        return False, path + ':_ts.use_tracing'
    if 'cframe' in ts:
        name = [w for w in ts['cframe'].split() if w not in ('struct', '*')][-1]
        cpath, cf = s.struct(name)
        if cf is not None and 'use_tracing' in cf:
            return True, '%s:%s.use_tracing' % (cpath, name)
    raise Unanswered('%s:_ts has no use_tracing, nor a CFrame with one' % path)


def interp_recursion_limit(s):
    """the recursion limit is the interpreter's ceval.recursion_limit, not
    the global _Py_CheckRecursionLimit"""
    return _field(s, '_ceval_state', 'recursion_limit', missing=False)


def signals_pending(s):
    """the runtime's ceval state flags signals apart from the pending calls
    (signals_pending)"""
    return _field(s, '_ceval_runtime_state', 'signals_pending')


def eval_frame_handle_pending(s):
    """eval_breaker is in the interpreter's ceval state, and ceval handles
    it in the static eval_frame_handle_pending"""
    found, evidence = _defines(s, 'Python/ceval.c', 'eval_frame_handle_pending')
    if found:
        home, _ = _field(s, '_ceval_state', 'eval_breaker', missing=False)
        if not home:
            raise Unanswered('eval_frame_handle_pending, but no '
                             '_ceval_state.eval_breaker')
    return found, evidence


def tuplecontains_item_first(s):
    """`in` on a tuple compares each item with the value, the item first"""
    return _body_has(s, 'Objects/tupleobject.c', 'tuplecontains',
                     'PyObject_RichCompareBool(PyTuple_GET_ITEM(a, i), el')


def aenter_lookup_first(s):
    """BEFORE_ASYNC_WITH looks __aenter__ up before __aexit__"""
    case = _case_tokens(s, 'BEFORE_ASYNC_WITH')
    if case is None or 'PyId___aenter__' not in case \
            or 'PyId___aexit__' not in case:
        raise Unanswered('Python/ceval.c: no TARGET(BEFORE_ASYNC_WITH) naming'
                         ' __aenter__ and __aexit__')
    return (case.index('PyId___aenter__') < case.index('PyId___aexit__'),
            'Python/ceval.c:TARGET(BEFORE_ASYNC_WITH)')


def _opcode(name):
    def detect(s):
        macros = s.macros('Include/opcode.h')
        if macros is None:
            raise Unanswered('no Include/opcode.h')
        return name in macros, 'Include/opcode.h:' + name
    detect.__doc__ = 'the %s opcode exists' % name
    return detect


# the opcodes Ni's core names that some CPython lacks: where the source
# lacks one, ni_features_gen.h defines it as a value no instruction has
# (256 and up), so a case for it never matches and the core needs no #if
OPCODES = ('SETUP_LOOP', 'SETUP_EXCEPT', 'BREAK_LOOP', 'CONTINUE_LOOP',
           'BEGIN_FINALLY', 'CALL_FINALLY', 'POP_FINALLY', 'END_ASYNC_FOR',
           'ROT_FOUR', 'END_FINALLY', 'WITH_CLEANUP_START',
           'WITH_CLEANUP_FINISH', 'RERAISE', 'WITH_EXCEPT_START',
           'JUMP_IF_NOT_EXC_MATCH', 'GEN_START', 'ROT_N')
# and the opcodes with a case of their own only where they exist
CASE_OPCODES = ('LOAD_ASSERTION_ERROR', 'LIST_TO_TUPLE', 'LIST_EXTEND',
                'SET_UPDATE', 'DICT_UPDATE', 'DICT_MERGE', 'IS_OP',
                'CONTAINS_OP', 'GET_LEN', 'MATCH_CLASS', 'MATCH_MAPPING',
                'MATCH_SEQUENCE', 'MATCH_KEYS', 'COPY_DICT_WITHOUT_KEYS',
                'BUILD_TUPLE_UNPACK_WITH_CALL', 'BUILD_TUPLE_UNPACK',
                'BUILD_LIST_UNPACK', 'BUILD_SET_UNPACK', 'BUILD_MAP_UNPACK',
                'BUILD_MAP_UNPACK_WITH_CALL')


def _constant(node):
    """A string or number literal's value (ast.Str and ast.Num before 3.8),
    else None."""
    for field in ('value', 's', 'n'):
        if type(node).__name__ in ('Constant', 'Str', 'Num') and hasattr(node, field):
            return getattr(node, field)
    return None


def jumps(src):
    """(hasjrel, hasjabs) of the source's Lib/opcode.py, each [(name,
    number)]: the opcodes whose argument is a jump, relative to the next
    instruction or absolute. Read from the jrel_op and jabs_op calls."""
    path = os.path.join(src, 'Lib', 'opcode.py')
    with open(path) as f:
        tree = ast.parse(f.read(), path)
    out = {'jrel_op': [], 'jabs_op': []}
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) \
                and node.func.id in out and len(node.args) == 2:
            name, num = node.args
            name, num = _constant(name), _constant(num)
            if name is None or num is None:
                raise FeatureError('%s: a %s call that is not of constants'
                                   % (path, node.func.id))
            out[node.func.id].append((name, num))
    if not out['jrel_op'] or not out['jabs_op']:
        raise FeatureError('%s: no jrel_op or jabs_op calls' % path)
    return out['jrel_op'], out['jabs_op']


D = _detector
AB = 'Objects/abstract.c'
CE = 'Python/ceval.c'
UO = 'Objects/unicodeobject.c'

# NI_HAS_<name> -> detector; its docstring's first line is the header's
# comment
FEATURES = [
    ('PYDICT_NEWPRESIZED', pydict_newpresized),
    ('AUDIT_HOOKS', audit_hooks),
    ('ARG_CHECK_POSITIONAL', arg_check_positional),
    ('CHECK_RESULT_TSTATE', check_result_tstate),
    ('OBJECTDICT_SETITEM_LINKABLE', objectdict_setitem_linkable),
    ('FLOAT_ASDOUBLE_INDEX', float_asdouble_index),
    ('MAP_ADD_VALUE_ON_TOP', map_add_value_on_top),
    ('POP_BLOCK_UNWINDS', pop_block_unwinds),
    ('POP_EXCEPT_UNWINDS', pop_except_unwinds),
    ('JUMP_IN_INSTRUCTIONS', jump_in_instructions),
    ('LASTI_IN_INSTRUCTIONS', lasti_in_instructions),
    ('LINENO_WHEN_SET', lineno_when_set),
    ('FRAME_BUILTINS_OF_FUNCTION', frame_builtins_of_function),
    ('IMMUTABLE_TYPE_FLAG', immutable_type_flag),
    # how Ni's own mechanisms reach the interpreter's state
    ('PYCORE_PYSTATE_H', D('the runtime and interpreter states are described in the internal pycore_pystate.h',
                           lambda s: _file(s, 'Include/internal/pycore_pystate.h'))),
    ('PYCORE_INTERP_H', D('the interpreter state has an internal header of its own, pycore_interp.h',
                          lambda s: _file(s, 'Include/internal/pycore_interp.h'))),
    ('EVAL_HOOK_TSTATE', eval_hook_tstate),
    ('SIGNALS_PENDING', signals_pending),
    ('EVAL_FRAME_HANDLE_PENDING', eval_frame_handle_pending),
    ('INTERP_RECURSION_LIMIT', interp_recursion_limit),
    ('FRAME_STATE', frame_state),
    ('CFRAME', cframe),
    ('GC_TRACKED_BY_NEXT', gc_tracked_by_next),
    ('GC_HEAD_INTERNAL', gc_head_internal),
    # Objects/abstract.c
    ('CHECK_SLOT_RESULT', D('abstract.c asserts each slot\'s result with _Py_CheckSlotResult',
                            lambda s: _declared(s, '_Py_CheckSlotResult'))),
    ('TERNARY_IOP', D('abstract.c has ternary_iop, the in-place ternary operation (**=)',
                      lambda s: _defines(s, AB, 'ternary_iop'))),
    ('ITER_CHECK_FUNCTION', D('PyIter_Check is a function, not a macro',
                              lambda s: _defines(s, AB, 'PyIter_Check'))),
    ('ITER_CHECK_TYPE_LOCAL', D('PyIter_Check reads the object\'s type once, into a local',
                                lambda s: _body_has(s, AB, 'PyIter_Check', 'PyTypeObject *tp = Py_TYPE(obj);',
                                                    missing=False))),
    ('NOT_A_SEQUENCE_ERROR', D('indexing a mapping as a sequence says it "is not a sequence"',
                               lambda s: _body_has(s, AB, 'PySequence_GetItem', '"%.200s is not a sequence"'))),
    ('PRIVATE_NUMBER_INDEX', D('_PyNumber_Index, __index__ giving an exact int or a subclass, exists',
                               lambda s: _defines(s, AB, '_PyNumber_Index'))),
    ('INLINE_CALL_TSTATE_GET', D('the inline call helpers (PyObject_CallOneArg) take the thread state with PyThreadState_Get(), not PyThreadState_GET()',
                                 lambda s: (any('PyThreadState_Get' in _texts(t) for _, t in s.definitions('PyObject_CallOneArg')),
                                            'Include, every header: PyObject_CallOneArg'))),
    ('CALL_NO_ARG_FUNCTION', D('_PyObject_CallNoArg is a function, not a macro over _PyObject_FastCallDict',
                               lambda s: _not(_macro(s, '_PyObject_CallNoArg')))),
    ('CALL_ONE_ARG', D('PyObject_CallOneArg exists',
                       lambda s: _declared(s, 'PyObject_CallOneArg'))),
    ('FUNCTION_STR', D('_PyObject_FunctionStr names a function in error messages',
                       lambda s: _declared(s, '_PyObject_FunctionStr'))),
    ('FASTCALL_DICT_TSTATE', D('_PyObject_FastCallDictTstate exists',
                               lambda s: _declared(s, '_PyObject_FastCallDictTstate'))),
    # the other objects
    ('TUPLE_GET_EMPTY', D('the empty tuple is the interpreter\'s, read with tuple_get_empty',
                          lambda s: _defines(s, 'Objects/tupleobject.c', 'tuple_get_empty'))),
    ('TUPLE_ALLOC', D('tuples are allocated with tuple_alloc, and an empty result is PyTuple_New(0)',
                      lambda s: _defines(s, 'Objects/tupleobject.c', 'tuple_alloc'))),
    ('CONTAINS_ITEM_FIRST', tuplecontains_item_first),
    ('UNICODE_EQ_ASSUMES_READY', D('unicode_eq asserts its strings are ready, not readies them',
                                   lambda s: _not(_body_has(s, 'Objects/stringlib/eq.h', 'unicode_eq', 'PyUnicode_READY')))),
    ('BYTES_STRIP_NONE_DEFAULT', D('bytes.strip\'s argument defaults to None, not NULL',
                                   lambda s: _not(_body_has(s, 'Objects/bytesobject.c', 'do_argstrip', 'bytes != NULL')))),
    ('UNICODE_STRIP_NONE_DEFAULT', D('str.strip\'s argument defaults to None, not NULL',
                                     lambda s: _not(_body_has(s, UO, 'do_argstrip', 'sep != NULL')))),
    ('UNICODE_FILL', D('unicodeobject.c fills with unicode_fill, not the FILL macro',
                       lambda s: _defines(s, UO, 'unicode_fill'))),
    ('PYCORE_FORMAT_H', D('the format flags F_LJUST... are the internal pycore_format.h\'s',
                          lambda s: _file(s, 'Include/internal/pycore_format.h'))),
    ('FORMATCHAR_INDEX_ONLY', D('%c takes an int, by PyLong_AsLongAndOverflow (no __int__)',
                                lambda s: _body_has(s, UO, 'formatchar', 'PyLong_AsLongAndOverflow'))),
    ('HASH_DOUBLE_OBJECT', D('_Py_HashDouble is given the float object too',
                             lambda s: _file_has(s, 'Python/pyhash.c', '_Py_HashDouble(PyObject *'))),
    ('MATH_UNDERFLOW_BELOW_1_5', D('math\'s is_error takes ERANGE as underflow below 1.5, not 1.0',
                                   lambda s: _body_has(s, 'Modules/mathmodule.c', 'is_error', 'fabs(x) < 1.5'))),
    ('MEMBER_EX_ERROR_FROM_TYPE', D('PyMember_GetOne keeps obj_addr, and a missing T_OBJECT_EX member\'s error names its type',
                                    lambda s: _body_has(s, 'Python/structmember.c', 'PyMember_GetOne',
                                                        'PyObject *obj = (PyObject *)obj_addr'))),
    ('POW_KEYWORDS', D('pow() takes keyword arguments base, exp and mod',
                       lambda s: _file_has(s, 'Python/clinic/bltinmodule.c.h',
                                           'builtin_pow_impl(PyObject *module, PyObject *base'))),
    ('CORO_GET_AWAITABLE_ITER_LINKABLE', D('_PyCoro_GetAwaitableIter can be called from an extension',
                                           lambda s: _linkable(s, '_PyCoro_GetAwaitableIter'))),
    # Python/ceval.c
    ('VARARGS_FROM_ARRAY', D('ceval builds a function\'s *args tuple with _PyTuple_FromArray',
                             lambda s: _file_has(s, CE, 'u = _PyTuple_FromArray('))),
    ('CMP_OUTCOME', D('COMPARE_OP also does is, in and exception matches (cmp_outcome)',
                      lambda s: _defines(s, CE, 'cmp_outcome'))),
    ('TRACE_INFO', D('ceval keeps the tracing state of a frame in a PyTraceInfo',
                     lambda s: _file_has(s, CE, '} PyTraceInfo;'))),
    ('CALL_TRACE_METHOD_DESCR', D('do_call_core traces a method descriptor\'s call as a C call',
                                  lambda s: _body_has(s, CE, 'do_call_core', 'PyMethodDescr_Type'))),
    ('NAME_ERROR_NAME', D('format_exc_check_arg sets a NameError\'s name',
                          lambda s: _body_has(s, CE, 'format_exc_check_arg', 'PyExc_NameError'))),
    ('IMPORT_FROM_SPEC', D('import_from\'s error tells a partially initialised module',
                           lambda s: _body_has(s, CE, 'import_from', '_PyModuleSpec_IsInitializing'))),
    ('CEVAL_NAME_IDENTIFIER', D('ceval.c\'s __name__ identifier is the file\'s, not import_from\'s',
                                lambda s: _not(_body_has(s, CE, 'import_from', '_Py_IDENTIFIER(__name__)')))),
    ('SPECIAL_LOOKUP_FROM_ID', D('special_lookup\'s AttributeError reads the name with _PyUnicode_FromId',
                                 lambda s: _body_has(s, CE, 'special_lookup', '_PyUnicode_FromId'))),
    ('KWARGS_MERGE_EX', D('CALL_FUNCTION_EX merges **kwargs with _PyDict_MergeEx, and format_kwargs_error reports it',
                          lambda s: _case_has(s, 'CALL_FUNCTION_EX', '_PyDict_MergeEx'))),
    ('DELETE_GLOBAL_KEYERROR_ONLY', D('DELETE_GLOBAL raises NameError only for a KeyError',
                                      lambda s: _case_has(s, 'DELETE_GLOBAL', 'PyExc_KeyError'))),
    ('AENTER_LOOKUP_FIRST', aenter_lookup_first),
    ('FORMAT_VALUE_CHECKS_CONVERSION', D('FORMAT_VALUE raises SystemError for an unknown conversion',
                                         lambda s: _case_has(s, 'FORMAT_VALUE', '"unexpected conversion flag %d"'))),
] + [('OPCODE_' + op, _opcode(op)) for op in OPCODES + CASE_OPCODES]


def describe(detector):
    return ' '.join(detector.__doc__.split())


def detect(src):
    """[(NI_HAS_name, value, what it detects, evidence)]; FeatureError if a
    detector can't answer."""
    s = Source(src)
    out = []
    for name, d in FEATURES:
        try:
            value, evidence = d(s)
        except Unanswered as e:
            raise FeatureError('feature NI_HAS_%s (tools/features/features.py): %s'
                               % (name, e))
        out.append(('NI_HAS_' + name, int(value), describe(d), evidence))
    return out


def build(src, version):
    """The header's text for source tree src, CPython version."""
    found = detect(src)
    lines = ['/* Generated by tools/features/features.py from CPython %s: do not'
             ' edit. */' % version,
             '#ifndef NI_FEATURES_GEN_H', '#define NI_FEATURES_GEN_H', '']
    for name, value, what, _ in found:
        lines += ['/* %s */' % what.replace('*/', '* /'),
                  '#define %s %d' % (name, value)]
    values = dict((name, value) for name, value, _, _ in found)
    absent = [op for op in OPCODES if not values['NI_HAS_OPCODE_' + op]]
    if absent:
        lines += ['', '/* opcodes this source lacks: no instruction has these values */']
        lines += ['#define %s %d' % (op, 256 + OPCODES.index(op)) for op in absent]
    jrel, jabs = jumps(src)
    lines += ['', '/* the opcodes whose argument is a jump (Lib/opcode.py hasjrel,'
              ' hasjabs) */']
    for macro, ops in (('NI_IS_JREL', jrel), ('NI_IS_JABS', jabs)):
        lines.append('#define %s(o) (%s)' % (
            macro, ' || '.join('(o) == %d' % n for _, n in ops)))
    lines += ['', '/* for ni.build_info: the source read, and every feature */',
              '#define NI_FEATURES_SOURCE_VERSION %s' % json.dumps(version),
              '#define NI_FEATURES(F) \\']
    lines += ['  F(%s) \\' % name for name, _, _, _ in found]
    lines += ['', '#endif /* NI_FEATURES_GEN_H */', '']
    return '\n'.join(lines)


def target_version(python):
    """The version (sys.version) of the interpreter being built for."""
    r = subprocess.run([python, '-I', '-c', 'import sys; print(sys.version.split()[0])'],
                       stdout=subprocess.PIPE, universal_newlines=True, timeout=60)
    if r.returncode != 0:
        raise FeatureError('cannot run %s' % python)
    return r.stdout.strip()


def write(argv):
    import cpython_src
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--python', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--src')
    args = ap.parse_args(argv)
    path = os.path.join(args.out, HEADER)
    try:
        version = target_version(args.python)
        if args.src:
            cpython_src.check_version(args.src, version)
            src = args.src
        else:
            src = cpython_src.find_source(version)
        text = build(src, version)
    except (FeatureError, cpython_src.SourceError) as e:
        if os.path.exists(path):
            os.remove(path)
        print('features.py: %s' % e, file=sys.stderr)
        return 1
    os.makedirs(args.out, exist_ok=True)
    # rewritten only when its text changes, so that an unchanged header
    # doesn't rebuild ni.so
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return 0
    with open(path + '.tmp', 'w') as f:
        f.write(text)
    os.replace(path + '.tmp', path)
    print('%s: CPython %s, %d features' % (path, version, text.count('\n#define NI_HAS_')))
    return 0


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    if argv[0].startswith('-'):
        return write(argv)
    import cpython_src
    for src in argv:
        print('%s (%s)' % (src, cpython_src.source_version(src)))
        for name, value, _, evidence in detect(src):
            print('  %-36s %d  %s' % (name, value, evidence))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
