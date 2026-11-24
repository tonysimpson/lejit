#!/usr/bin/env python3
"""Detect CPython's features from the source LeJIT is built against, never
from its version number: a release with a backport, a development version,
a distribution's patches or a fork all say what they have only in their
source.

Each feature is a short question on the source (a struct's fields, a
declaration and its parameters, a macro, a function body, an opcode),
answering 1 or 0 with the definition it read. setup.py runs this before
compiling, to write le_features_gen.h into the build directory: one
`#define LE_HAS_<NAME> 0|1` per feature, the opcodes the source lacks, the
jump opcodes, the inline caches, and what lejit._core._build reports. A question the source can't
answer (the struct or file it reads is missing) stops the build: a new
CPython that moves a definition needs the detector updated, never a guess.

    features.py --python EXE --out DIR [--src DIR]
        write DIR/le_features_gen.h for the interpreter EXE; the source is
        --src, else the release tarball from the cache (cpython_src.py)
    features.py SRC [SRC...]
        print the features of one or more source trees

It runs under the Python being built for (3.7 and later).
"""
import argparse
import ast
import collections
import glob
import json
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctokens  # noqa: E402

HEADER = 'le_features_gen.h'


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


def _run_at(texts, want):
    """where texts first has the run want, or None"""
    n = len(want)
    for i in range(len(texts) - n + 1):
        if texts[i:i + n] == want:
            return i
    return None


def _has_run(texts, want):
    return _run_at(texts, want) is not None


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


def _public(s, name):
    """a header outside internal/ declares function name or defines it as
    a macro: what an extension can call"""
    for path, toks in s.headers_with(name):
        if 'internal' in path.split(os.sep):
            continue
        if name in s.macros(path) or any(
                t.text == name and toks[i + 1].text == '('
                for i, t in enumerate(toks[:-1])):
            return True, '%s:%s' % (path, name)
    return False, 'every header outside internal/: %s' % name


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


def _first_body_has(s, places, text):
    """the first of places, [(path, function)], whose function is there
    has the tokens of text in a row in its body"""
    for path, function in places:
        if s.body_of(s.tokens(path) or [], function) is not None:
            return _body_has(s, path, function, text)
    raise Unanswered('no %s' % ' or '.join('%s in %s' % (f, p) for p, f in places))


def _file_has(s, path, text):
    """path has the tokens of text in a row"""
    toks = s.tokens(path)
    if toks is None:
        raise Unanswered('no %s' % path)
    return _has_run(_texts(toks), _c(text)), path


# ceval's cases are in ceval.c, or (from 3.12) generated from bytecodes.c
# into the file ceval.c includes
CASE_FILES = ('Python/ceval.c', 'Python/generated_cases.c.h')


def _case_file(s, opcode):
    """(path, case tokens): ceval's case for opcode, to the next case; (None,
    None) where there is none"""
    for path in CASE_FILES:
        texts = _texts(s.tokens(path))
        for i in range(len(texts) - 3):
            if texts[i:i + 4] == ['TARGET', '(', opcode, ')']:
                j = i + 4
                while j < len(texts) - 1 and texts[j:j + 2] != ['TARGET', '(']:
                    j += 1
                return path, texts[i + 4:j]
    return None, None


def _case_tokens(s, opcode):
    """ceval's case for opcode, to the next case; None where there is none"""
    return _case_file(s, opcode)[1]


def _case_uses(s, opcode, word):
    """whether ceval's case for opcode names word before the next case;
    False where ceval has no such case"""
    return _case_has(s, opcode, word, missing=False)


def _case_has(s, opcode, text, missing=None):
    path, case = _case_file(s, opcode)
    if case is None:
        where = ', '.join(CASE_FILES)
        if missing is None:
            raise Unanswered('%s: no TARGET(%s)' % (where, opcode))
        return missing, '%s: no TARGET(%s)' % (where, opcode)
    return _has_run(case, _c(text)), '%s:TARGET(%s)' % (path, opcode)


# ceval's macros, in ceval.c or (from 3.12) the ceval_macros.h it includes
MACRO_FILES = ('Python/ceval.c', 'Python/ceval_macros.h')


def _ceval_macro_uses(s, names, word):
    """the first of names that ceval defines as a macro uses word"""
    for name in names:
        for path in MACRO_FILES:
            m = (s.macros(path) or {}).get(name)
            if m is not None:
                return word in _texts(m.body), '%s:%s' % (path, name)
    raise Unanswered('no %s macro in %s' % (' or '.join(names), ', '.join(MACRO_FILES)))


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


def _parameters(s, name):
    """(path, [each parameter's token texts]) of function name's header
    declaration; Unanswered if there is none"""
    path, decl = s.declaration(name)
    if decl is None:
        raise Unanswered('no declaration of %s' % name)
    toks = s.tokens(path)
    for i, t in enumerate(toks[:-1]):
        if t.text == name and toks[i + 1].text == '(':
            inside = toks[i + 2:ctokens.match_close(toks, i + 1)]
            return path, [_texts(p) for p in ctokens.split_top(inside, ',')]
    raise Unanswered('%s: no parameters of %s' % (path, name))


def objectdict_setitem_object(s):
    """_PyObjectDict_SetItem takes the object after its type: (tp, obj,
    dictptr, name, value)"""
    path, params = _parameters(s, '_PyObjectDict_SetItem')
    if len(params) not in (4, 5):
        raise Unanswered('%s:_PyObjectDict_SetItem has %d parameters'
                         % (path, len(params)))
    return len(params) == 5, path + ':_PyObjectDict_SetItem'


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
    return _not(_ceval_macro_uses(s, ('JUMPTO', 'JUMPBY'), 'sizeof'))


def lasti_in_instructions(s):
    """a frame's f_lasti and a block's b_handler count instructions, not bytes (ceval's INSTR_OFFSET has no sizeof)"""
    has, evidence = _field(s, '_frame', 'f_lasti')
    if not has:
        return interpreter_frame_lasti(s)
    return _not(_ceval_macro_uses(s, ('INSTR_OFFSET',), 'sizeof'))


def interpreter_frame_lasti(s):
    """Where the frame object keeps no f_lasti, the position of an
    interpreter frame (_PyInterpreterFrame_LASTI) counts instructions; the
    exception table's handlers count them too. Unanswered without either."""
    found = s.definitions('_PyInterpreterFrame_LASTI')
    if not found:
        raise Unanswered('no _frame.f_lasti, and no _PyInterpreterFrame_LASTI')
    path, body = found[0]
    return 'sizeof' not in _texts(body), path + ':_PyInterpreterFrame_LASTI'


def code_localsplus(s):
    """a code object names its locals, cells and free variables in one
    tuple, co_localsplusnames, with a kind each (CO_FAST_LOCAL, CO_FAST_CELL,
    CO_FAST_FREE): an argument's cell is in the argument's place"""
    found = s.definitions('_PyLocals_GetKind')
    return bool(found), (found[0][0] if found else 'every header') + ':_PyLocals_GetKind'


def interpreter_frame(s):
    """the frame-evaluation hook is given an interpreter frame
    (_PyInterpreterFrame), the thread's frames are a chain of them, and a
    frame object is made for one only when asked"""
    for path, toks in s.headers_with('_PyFrameEvalFunction'):
        texts = _texts(toks)
        want = ['PyObject', '*', '(', '*', '_PyFrameEvalFunction', ')', '(']
        for i in range(len(texts) - len(want)):
            if texts[i] == 'typedef' and texts[i + 1:i + 1 + len(want)] == want:
                params = texts[i + 1 + len(want):]
                params = params[:params.index(')')]
                return ('_PyInterpreterFrame' in params,
                        path + ':_PyFrameEvalFunction')
    raise Unanswered('no typedef of _PyFrameEvalFunction in Include')


def lineno_when_set(s):
    """a frame's f_lineno is its line whenever it is not 0, traced or not (PyFrame_GetLineNumber tests f_lineno, not f_trace)"""
    fo = 'Objects/frameobject.c'
    body = s.body_of(s.tokens(fo) or [], 'PyFrame_GetLineNumber')
    if body is None:
        raise Unanswered('no PyFrame_GetLineNumber in %s' % fo)
    texts = _texts(body)
    return 'f_trace' not in texts and 'f_lineno' in texts, fo + ':PyFrame_GetLineNumber'


def lasti_past_prefixes(s):
    """an instruction's place (a frame's lasti, a traceback's tb_lasti) is its own unit, past its EXTENDED_ARG prefixes, not the first prefix (ceval's EXTENDED_ARG dispatches the instruction, which sets the place, rather than going to dispatch_opcode)"""
    return _not(_case_has(s, 'EXTENDED_ARG', 'dispatch_opcode'))


def frame_builtins_of_function(s):
    """a new frame's builtins are its function's (func_builtins, read through
    the PyFrameConstructor), not looked up in its globals"""
    found, evidence = _body_has(s, 'Objects/frameobject.c', '_PyFrame_New_NoTrack',
                                'fc_builtins', missing=False)
    if found:
        return found, evidence
    # from 3.11 an internal header sets up the interpreter's frames:
    # frame->f_builtins = func->func_builtins
    for path, toks in s.headers_with('func_builtins'):
        texts = _texts(toks)
        for i in range(len(texts) - 4):
            if texts[i:i + 2] == ['f_builtins', '='] \
                    and texts[i + 3:i + 5] == ['->', 'func_builtins']:
                return True, path + ': f_builtins = ...->func_builtins'
    return False, evidence


def immutable_type_flag(s):
    """a type's attributes and its instances' __class__ are fixed when it has Py_TPFLAGS_IMMUTABLETYPE (type_setattro and object_set_class, or the object_set_class_world_stopped it calls, test the flag), not when it is a static type"""
    to = 'Objects/typeobject.c'
    set_class = 'object_set_class'
    if _defines(s, to, 'object_set_class_world_stopped')[0]:
        set_class = 'object_set_class_world_stopped'
    a, _ = _body_has(s, to, 'type_setattro', 'Py_TPFLAGS_IMMUTABLETYPE')
    b, _ = _body_has(s, to, set_class, 'Py_TPFLAGS_IMMUTABLETYPE')
    if a != b:
        raise Unanswered('%s: type_setattro and %s differ on '
                         'Py_TPFLAGS_IMMUTABLETYPE' % (to, set_class))
    return a, '%s:type_setattro, %s' % (to, set_class)


def name_error_name(s):
    """ceval's format_exc_check_arg (_PyEval_FormatExcCheckArg where ceval exports it) sets a NameError's name"""
    name = 'format_exc_check_arg'
    if _defines(s, CE, '_PyEval_FormatExcCheckArg')[0]:
        name = '_PyEval_FormatExcCheckArg'
    return _body_has(s, CE, name, 'PyExc_NameError')


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
    if not gc_head_two_words(fields):
        raise Unanswered('%s:PyGC_Head is not _gc_next, _gc_prev' % path)
    return True, path + ':PyGC_Head'


def gc_head_two_words(fields):
    """PyGC_Head's fields are the words _gc_next and _gc_prev; _gc_next may
    be declared through _Py_ALIGNED_DEF(N, uintptr_t), which keeps its size
    and offset (store_gc.h asserts both)"""
    return (list(fields) == ['_gc_next', '_gc_prev']
            and fields['_gc_prev'] == 'uintptr_t'
            and re.fullmatch(r'uintptr_t|_Py_ALIGNED_DEF \( \w+ , uintptr_t \)',
                             fields['_gc_next']) is not None)


def frame_state(s):
    """a frame says whether it is running in f_state, not f_executing"""
    path, fields = s.struct('_frame')
    if fields is None:
        raise Unanswered('no struct _frame in Include')
    if 'f_state' not in fields and 'f_executing' not in fields:
        # a view of an interpreter frame, whose owner says it is running:
        # LeJIT tests INTERPRETER_FRAME first
        if 'f_frame' in fields:
            return False, '%s:_frame has f_frame, not f_state' % path
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
    has_monitoring, evidence = monitoring(s)
    if has_monitoring:
        return False, '%s:_ts has no use_tracing, %s' % (path, evidence)
    raise Unanswered('%s:_ts has no use_tracing, nor a CFrame with one' % path)


def monitoring(s):
    """tracing and profiling are sys.monitoring's events (the interpreter's
    monitors): a thread has no use_tracing flag"""
    return _field(s, '_is', 'monitors', missing=False)


def interp_monitoring_version(s):
    """the interpreter's monitoring version, which changes with every
    change to its tools table, is its own monitoring_version, not its ceval
    state's instrumentation_version"""
    return _field(s, '_is', 'monitoring_version', missing=False)


def thread_local_tstate(s):
    """the running thread's state is a thread-local of libpython's, not
    _PyRuntime.gilstate.tstate_current"""
    return _not(_field(s, '_gilstate_runtime_state', 'tstate_current'))


def py_recursion_remaining(s):
    """a thread counts down the Python calls it may still make in
    py_recursion_remaining, apart from the C calls"""
    return _field(s, '_ts', 'py_recursion_remaining')


def current_exception(s):
    """the exception being raised is kept as its value alone
    (current_exception, no curexc_type)"""
    return _field(s, '_ts', 'current_exception')


def eval_breaker_gc(s):
    """the eval breaker also runs the cyclic GC (the interpreter's
    ceval.gc_scheduled), in _Py_HandlePending"""
    return _field(s, '_ceval_state', 'gc_scheduled', missing=False)


def eval_frame_pops(s):
    """ceval clears and pops the frame it is given when it ends, from an
    entry frame of its own, not its caller (an interpreter frame has no
    is_entry)"""
    path, fields = s.struct('_PyInterpreterFrame')
    if fields is None:
        return False, 'Include, every header: _PyInterpreterFrame'
    return 'is_entry' not in fields, '%s:_PyInterpreterFrame.is_entry' % path


def frame_executable(s):
    """an interpreter frame keeps its code as f_executable, a plain object,
    not f_code"""
    return _field(s, '_PyInterpreterFrame', 'f_executable', missing=False)


def frame_instr_ptr(s):
    """an interpreter frame's place is the instruction it runs or is about
    to run (instr_ptr), not the one before (prev_instr)"""
    return _field(s, '_PyInterpreterFrame', 'instr_ptr', missing=False)


def tstate_current_frame(s):
    """a thread's innermost interpreter frame is its state's current_frame,
    not its CFrame's"""
    return _field(s, '_ts', 'current_frame')


def tstate_eval_breaker(s):
    """each thread has its own eval_breaker, in its state: its low bits are
    the events ceval handles with _Py_HandlePending"""
    return _field(s, '_ts', 'eval_breaker')


def frame_funcobj(s):
    """an interpreter frame keeps its function as a plain object (f_funcobj),
    not a PyFunctionObject (f_func)"""
    return _field(s, '_PyInterpreterFrame', 'f_funcobj', missing=False)


def c_recursion_remaining(s):
    """a thread counts down the C calls it may still make, in
    c_recursion_remaining, not only by its stack pointer"""
    return _field(s, '_ts', 'c_recursion_remaining')


def frame_stackrefs(s):
    """an interpreter frame's words are _PyStackRefs (f_executable,
    f_funcobj, localsplus): a word with its low bit set holds no reference
    of its own, and NULL is that bit alone; its value stack's top is
    stackpointer, not a count (stacktop)"""
    path, fields = s.struct('_PyInterpreterFrame')
    if fields is None:
        return False, 'Include, every header: _PyInterpreterFrame'
    refs = fields.get('localsplus') == '_PyStackRef'
    if refs != ('stackpointer' in fields):
        raise Unanswered('%s:_PyInterpreterFrame has only one of _PyStackRef'
                         ' localsplus and stackpointer' % path)
    return refs, '%s:_PyInterpreterFrame.localsplus, stackpointer' % path


def frame_initialize_tstate(s):
    """_PyFrame_Initialize takes the thread state first, the function as a
    _PyStackRef and the frame before as its last argument"""
    for path, toks in s.headers_with('_PyFrame_Initialize'):
        if _has_run(_texts(toks), ['_PyFrame_Initialize', '(',
                                   'PyThreadState', '*']):
            return True, '%s:_PyFrame_Initialize(PyThreadState *' % path
    return False, 'Include, every header: _PyFrame_Initialize'


def refcount_32(s):
    """an object's refcount is a 32-bit ob_refcnt, the low half of the
    word ob_refcnt_full, whose high half holds ob_overflow and ob_flags"""
    for path, toks in s.headers_with('ob_refcnt_full'):
        if _has_run(_texts(toks), ['uint32_t', 'ob_refcnt', ';']):
            return True, '%s:ob_refcnt_full, uint32_t ob_refcnt' % path
    return False, 'Include, every header: ob_refcnt_full'


def dealloc_defers_to_trashcan(s):
    """_Py_Dealloc itself defers a dealloc deep in the C stack to the
    trashcan, and empties the chain after, so tp_dealloc can't be called in
    its place: Py_TRASHCAN_BEGIN, which deallocs did that with before, is
    empty"""
    for path, _ in s.headers_with('Py_TRASHCAN_BEGIN'):
        m = (s.macros(path) or {}).get('Py_TRASHCAN_BEGIN')
        if m is not None:
            return not _texts(m.body), '%s:Py_TRASHCAN_BEGIN' % path
    return False, 'Include, every header: no Py_TRASHCAN_BEGIN'


def dict_version_tag(s):
    """a dict has a version tag (ma_version_tag) that every change to it
    changes"""
    return _field(s, 'PyDictObject', 'ma_version_tag')


def long_value(s):
    """an int's sign, size and digits are its long_value (lv_tag and
    ob_digit), not ob_size and ob_digit"""
    return _field(s, '_longobject', 'long_value')


def recursion_remaining(s):
    """a thread counts down the Python calls it may still make, in its
    recursion_remaining, not up in recursion_depth"""
    return _field(s, '_ts', 'recursion_remaining')


def exc_info_value_only(s):
    """an exception being handled is kept as its value alone
    (_PyErr_StackItem.exc_value, no exc_type or exc_traceback)"""
    # the last of 'PyObject *exc_type, *exc_value, *exc_traceback;', the
    # one name the struct parser keeps of the declaration
    return _not(_field(s, '_err_stackitem', 'exc_traceback'))


def dict_values_struct(s):
    """a split dict's values are a PyDictValues (ma_values), whose array of
    values is its only field, not a bare array"""
    path, fields = s.struct('PyDictObject')
    if fields is None:
        raise Unanswered('no PyDictObject in Include')
    if 'ma_values' not in fields:
        raise Unanswered('%s:PyDictObject has no ma_values' % path)
    return 'PyDictValues' in fields['ma_values'], path + ':PyDictObject.ma_values'


def dict_keys_kind(s):
    """dict keys have a kind (dk_kind) and keep their size as a power of
    two (dk_log2_size, dk_log2_index_bytes); an entry of keys that are all
    str holds no hash (PyDictUnicodeEntry)"""
    path, fields = s.struct('_dictkeysobject')
    if fields is not None:
        return 'dk_kind' in fields, path + ':_dictkeysobject.dk_kind'
    # until 3.10 the struct is private to Objects/
    found, path = _file_has(s, 'Objects/dict-common.h', 'dict_lookup_func dk_lookup;')
    if found:
        return False, path + ':_dictkeysobject.dk_lookup'
    raise Unanswered('no struct _dictkeysobject in Include or Objects/dict-common.h')


def managed_values_pointer(s):
    """an instance of a class with a managed dict keeps a pointer to its
    values four words before it, and its dict three words before
    (_PyObject_ValuesPointer, _PyObject_ManagedDictPointer)"""
    if s.tokens('Include/internal/pycore_object.h') is None:
        return False, 'no Include/internal/pycore_object.h'
    return _file_has(s, 'Include/internal/pycore_object.h',
                     'return ((PyDictValues **)obj)-4;')


def tagged_dict_or_values(s):
    """an instance of a class with a managed dict keeps one word three words
    before it: its dict, or its values pointer less one, whose low bit is
    then set (_PyObject_DictOrValuesPointer, _PyDictOrValues_SetValues)"""
    obj = 'Include/internal/pycore_object.h'
    if s.tokens(obj) is None:
        return False, 'no %s' % obj
    word = _file_has(s, obj, 'return ((PyDictOrValues *)obj)-3;')[0]
    tag = _file_has(s, obj, 'ptr->values = ((char *)values) - 1;')[0]
    if word != tag:
        raise Unanswered('%s: PyDictOrValues three words back %r, tagged %r'
                         % (obj, word, tag))
    if not word:
        return False, '%s: no PyDictOrValues' % obj
    return True, '%s:_PyObject_DictOrValuesPointer, _PyDictOrValues_SetValues' % obj


def range_iter_index(s):
    """a range iterator counts the items it has given in index, from a start
    that stays put; without it, it moves start on and counts len down"""
    path, fields = s.struct('_PyRangeIterObject')
    if fields is not None:
        return 'index' in fields, path + ':_PyRangeIterObject.index'
    # until 3.11 the struct is private to Objects/rangeobject.c
    return _body_has(s, 'Objects/rangeobject.c', 'rangeiter_next', 'r->index')


def inline_values(s):
    """a class with Py_TPFLAGS_INLINE_VALUES keeps its instances' values in
    the object, at its tp_basicsize (_PyObject_InlineValues): a PyDictValues
    whose byte `valid`, its fourth, says whether they are still the
    attributes, then the values, a word on; their insertion order is
    `size` bytes after the `capacity` values, each new index appended
    (_PyDictValues_AddToInsertionOrder); a dict made for them is three
    words before the object (MANAGED_DICT_OFFSET)"""
    path, fields = s.struct('_dictvalues')
    if fields is None or 'valid' not in fields:
        return False, '%s: no _dictvalues.valid' % (path or 'Include')
    if list(fields) != ['capacity', 'size', 'embedded', 'valid', 'values'] or \
            any(fields[f] != 'uint8_t' for f in list(fields)[:4]):
        raise Unanswered('%s:_dictvalues is %r' % (path, fields))
    # the stores are plain but for the free-threaded build
    if not (_body_has(s, path, 'get_insertion_order_array',
                      'return (uint8_t *)&values->values[values->capacity];')[0] and
            any(_body_has(s, path, '_PyDictValues_AddToInsertionOrder', stores)[0] for stores in (
                'array[size] = (uint8_t)ix; values->size = size+1;',
                'FT_ATOMIC_STORE_UINT8_RELAXED(array[size], (uint8_t)ix);'
                ' FT_ATOMIC_STORE_UINT8_RELEASE(values->size, size+1);'))):
        raise Unanswered('%s: the insertion order is elsewhere' % path)
    obj = 'Include/internal/pycore_object.h'
    # where it adds sizeof(PyObject), that is the basicsize of every class
    # with the flag (typeobject.c:type_new_impl sets it only then)
    at = [_body_has(s, obj, '_PyObject_InlineValues', 'return (PyDictValues *)((char *)obj + ' + where)[0]
          for where in ('sizeof(PyObject));', 'tp->tp_basicsize);')]
    if not any(at):
        raise Unanswered('%s:_PyObject_InlineValues: values elsewhere' % obj)
    dict_at = s.macros(obj).get('MANAGED_DICT_OFFSET')
    if dict_at is None or _texts(dict_at.body)[-4:] != ['*', '-', '3', ')']:
        raise Unanswered('%s:MANAGED_DICT_OFFSET is not three words back' % obj)
    return True, '%s:_dictvalues, %s:_PyObject_InlineValues, MANAGED_DICT_OFFSET' % (path, obj)


def dict_watcher_bits(s):
    """a dict's version tag keeps in its low 8 bits which watchers watch it
    (DICT_MAX_WATCHERS is 8), and a change steps it by 1 << 8
    (DICT_VERSION_INCREMENT); unanswered for another number of watchers"""
    found = s.definitions('DICT_MAX_WATCHERS')
    if not found:
        return False, 'Include, every header: DICT_MAX_WATCHERS'
    path, body = found[0]
    if _texts(body) != ['8']:
        raise Unanswered('%s:DICT_MAX_WATCHERS is %s' % (path, ' '.join(_texts(body))))
    return True, '%s:DICT_MAX_WATCHERS' % path


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
    elif eval_breaker_gc(s)[0]:
        # the handling has left ceval, with the GC (3.12)
        return False, '%s, %s' % (evidence, eval_breaker_gc(s)[1])
    elif tstate_eval_breaker(s)[0]:
        # each thread has its own (3.13)
        return False, '%s, %s' % (evidence, tstate_eval_breaker(s)[1])
    elif s.declares('_Py_HandlePending'):
        # the handling has left ceval: neither form LeJIT copies
        raise Unanswered('no eval_frame_handle_pending in Python/ceval.c, and %s'
                         ' declares _Py_HandlePending' % s.declares('_Py_HandlePending'))
    return found, evidence


def call_trace_method_descr(s):
    """do_call_core traces a method descriptor's call as a C call"""
    found, evidence = _body_has(s, CE, 'do_call_core', 'PyMethodDescr_Type',
                                missing=None if not monitoring(s)[0] else 'none')
    if found != 'none':
        return found, evidence
    # sys.monitoring's profiler, where ceval has no do_call_core (3.12)
    return _body_has(s, 'Python/legacy_tracing.c', 'sys_profile_call_or_return',
                     'PyMethodDescr_Type')


def function_annotate(s):
    """SET_FUNCTION_ATTRIBUTE can set a function's __annotate__
    (MAKE_FUNCTION_ANNOTATE), the annotations made lazily: named in its
    case, or in the offsets table the case reads"""
    found, evidence = _case_has(s, 'SET_FUNCTION_ATTRIBUTE', 'MAKE_FUNCTION_ANNOTATE', missing=False)
    if found or not _case_has(s, 'SET_FUNCTION_ATTRIBUTE', '_Py_FunctionAttributeOffsets[oparg]', missing=False)[0]:
        return found, evidence
    return _file_has(s, CE, '[MAKE_FUNCTION_ANNOTATE] = offsetof(PyFunctionObject, func_annotate)')


def tuplecontains_item_first(s):
    """`in` on a tuple compares each item with the value, the item first"""
    tuples = 'Objects/tupleobject.c'
    return _first_body_has(s, [(tuples, 'tuplecontains'), (tuples, 'tuple_contains')],
                           'PyObject_RichCompareBool(PyTuple_GET_ITEM(a, i), el')


def valid_version_tag_flag(s):
    """a type's version tag is valid while it has
    Py_TPFLAGS_VALID_VERSION_TAG, which a change clears; without it, a
    change sets the tag to 0"""
    return _body_has(s, 'Objects/typeobject.c', 'assign_version_tag',
                     'Py_TPFLAGS_VALID_VERSION_TAG')


def member_ex_error_from_type(s):
    """PyMember_GetOne keeps obj_addr, and a missing T_OBJECT_EX member's error names its type"""
    sm = 'Python/structmember.c'
    found, evidence = _body_has(s, sm, 'PyMember_GetOne',
                                'PyObject *obj = (PyObject *)obj_addr')
    if found:
        return found, evidence
    # from 3.13 the error is raised by a helper given obj_addr
    return (any(t.kind == 'str' and 'object has no attribute' in t.text
                for t in s.tokens(sm)), sm + ': "... object has no attribute ..."')


def delete_global_keyerror_only(s):
    """DELETE_GLOBAL raises NameError only for a KeyError"""
    found, evidence = _case_has(s, 'DELETE_GLOBAL', 'PyExc_KeyError')
    if found:
        return found, evidence
    # from 3.13 PyDict_Pop tells a missing name from an error
    return _case_has(s, 'DELETE_GLOBAL', 'PyDict_Pop(GLOBALS(), name, NULL)')


def _instructions(s):
    """{name: number} of the numbers below 256 that Include/opcode.h
    defines, itself or (from 3.13) in the opcode_ids.h it includes: the
    instructions, and HAVE_ARGUMENT. The compiler's pseudo-instructions (256
    and up, from 3.12) are left out: they are never in the bytecode."""
    path = 'Include/opcode.h'
    macros = s.macros(path)
    if macros is None:
        raise Unanswered('no ' + path)
    with open(os.path.join(s.src, path), encoding='latin-1') as f:
        if re.search(r'^\s*#\s*include\s+"opcode_ids\.h"', f.read(), re.M):
            ids = s.macros('Include/opcode_ids.h')
            if ids is None:
                raise Unanswered(path + ' includes opcode_ids.h, which is missing')
            macros = dict(macros, **ids)
    out = {}
    for name, m in macros.items():
        if m.params is None and len(m.body) == 1 and m.body[0].kind == 'num':
            n = int(m.body[0].text, 0)
            if n < 256:
                out[name] = n
    return out


def _opcode(name):
    def detect(s):
        return name in _instructions(s), 'Include/opcode.h:' + name
    detect.__doc__ = 'the %s opcode exists' % name
    return detect


# the opcodes LeJIT's core names that some CPython lacks: where the source
# lacks one, le_features_gen.h defines it as a value no opcode has (256 and
# up, above the compiler's pseudo-instructions too), so a case for it never
# matches and the core needs no #if
OPCODES = ('SETUP_LOOP', 'SETUP_EXCEPT', 'BREAK_LOOP', 'CONTINUE_LOOP',
           'BEGIN_FINALLY', 'CALL_FINALLY', 'POP_FINALLY', 'END_ASYNC_FOR',
           'ROT_FOUR', 'END_FINALLY', 'WITH_CLEANUP_START',
           'WITH_CLEANUP_FINISH', 'RERAISE', 'WITH_EXCEPT_START',
           'JUMP_IF_NOT_EXC_MATCH', 'GEN_START', 'ROT_N',
           # gone from 3.11: the operators are BINARY_OP's, calls CALL's,
           # the stack shuffles COPY's and SWAP's, and handlers the
           # exception table's
           'BINARY_ADD', 'BINARY_AND', 'BINARY_FLOOR_DIVIDE', 'BINARY_LSHIFT',
           'BINARY_MATRIX_MULTIPLY', 'BINARY_MODULO', 'BINARY_MULTIPLY',
           'BINARY_OR', 'BINARY_POWER', 'BINARY_RSHIFT', 'BINARY_SUBTRACT',
           'BINARY_TRUE_DIVIDE', 'BINARY_XOR', 'INPLACE_ADD', 'INPLACE_AND',
           'INPLACE_FLOOR_DIVIDE', 'INPLACE_LSHIFT', 'INPLACE_MATRIX_MULTIPLY',
           'INPLACE_MODULO', 'INPLACE_MULTIPLY', 'INPLACE_OR', 'INPLACE_POWER',
           'INPLACE_RSHIFT', 'INPLACE_SUBTRACT', 'INPLACE_TRUE_DIVIDE',
           'INPLACE_XOR', 'CALL_FUNCTION', 'CALL_FUNCTION_KW', 'CALL_METHOD',
           'DUP_TOP', 'DUP_TOP_TWO', 'ROT_TWO', 'ROT_THREE', 'JUMP_ABSOLUTE',
           'POP_JUMP_IF_FALSE', 'POP_JUMP_IF_TRUE', 'SETUP_FINALLY',
           'SETUP_WITH', 'SETUP_ASYNC_WITH', 'POP_BLOCK', 'YIELD_FROM',
           # new in 3.11
           'RESUME', 'COPY', 'SWAP', 'PUSH_NULL', 'BINARY_OP', 'JUMP_BACKWARD',
           'JUMP_BACKWARD_NO_INTERRUPT', 'POP_JUMP_FORWARD_IF_FALSE',
           'POP_JUMP_FORWARD_IF_TRUE', 'POP_JUMP_FORWARD_IF_NONE',
           'POP_JUMP_FORWARD_IF_NOT_NONE', 'POP_JUMP_BACKWARD_IF_FALSE',
           'POP_JUMP_BACKWARD_IF_TRUE', 'POP_JUMP_BACKWARD_IF_NONE',
           'POP_JUMP_BACKWARD_IF_NOT_NONE', 'PRECALL', 'KW_NAMES', 'CALL',
           'MAKE_CELL', 'COPY_FREE_VARS', 'RETURN_GENERATOR',
           # gone from 3.12: the unary plus, the interactive print, import *
           # and LIST_TO_TUPLE are CALL_INTRINSIC_1's
           'UNARY_POSITIVE', 'PRINT_EXPR', 'IMPORT_STAR', 'LIST_TO_TUPLE',
           'JUMP_IF_FALSE_OR_POP', 'JUMP_IF_TRUE_OR_POP',
           # new in 3.12
           'END_FOR', 'RETURN_CONST', 'LOAD_FAST_CHECK', 'LOAD_FAST_AND_CLEAR',
           'BINARY_SLICE', 'STORE_SLICE', 'CALL_INTRINSIC_1',
           'CALL_INTRINSIC_2', 'LOAD_SUPER_ATTR', 'POP_JUMP_IF_NONE',
           'POP_JUMP_IF_NOT_NONE',
           # gone from 3.13: f-strings are CONVERT_VALUE's and FORMAT_SIMPLE's
           'FORMAT_VALUE',
           # new in 3.13
           'TO_BOOL', 'CALL_KW', 'CONVERT_VALUE', 'FORMAT_SIMPLE',
           'FORMAT_WITH_SPEC', 'SET_FUNCTION_ATTRIBUTE', 'LOAD_FAST_LOAD_FAST',
           'STORE_FAST_LOAD_FAST', 'STORE_FAST_STORE_FAST',
           # gone from 3.14: subscripts are BINARY_OP's, with blocks
           # LOAD_SPECIAL's, constant-key maps BUILD_MAP's
           'BINARY_SUBSCR', 'BEFORE_WITH', 'BEFORE_ASYNC_WITH',
           'BUILD_CONST_KEY_MAP', 'LOAD_METHOD',
           # new in 3.14
           'LOAD_FAST_BORROW', 'LOAD_FAST_BORROW_LOAD_FAST_BORROW',
           'LOAD_SMALL_INT', 'LOAD_COMMON_CONSTANT', 'NOT_TAKEN', 'POP_ITER',
           'LOAD_SPECIAL', 'BUILD_TEMPLATE', 'BUILD_INTERPOLATION',
           # gone from 3.15: yield from's is GET_ITER with an argument
           'GET_YIELD_FROM_ITER')
# and the opcodes with a case of their own only where they exist
CASE_OPCODES = ('LOAD_ASSERTION_ERROR', 'LIST_EXTEND',
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


def _lib(src, name):
    """(path, parsed module) of the source's Lib/name, or (path, None)."""
    path = os.path.join(src, 'Lib', name)
    if not os.path.isfile(path):
        return path, None
    with open(path, encoding='utf-8') as f:
        return path, ast.parse(f.read(), path)


def opmap(src):
    """{name: number} of every opcode, as opcode.opmap has them (from 3.12
    with the compiler's pseudo-instructions, 256 and up): from 3.13 the dict
    Lib/_opcode_metadata.py assigns to opmap, before that the def_op,
    jrel_op... calls of Lib/opcode.py."""
    path, tree = _lib(src, '_opcode_metadata.py')
    if tree is not None:
        for node in tree.body:
            if isinstance(node, ast.Assign) and any(
                    isinstance(x, ast.Name) and x.id == 'opmap' for x in node.targets):
                value = node.value
                try:
                    if isinstance(value, ast.Call):    # frozendict(...), from 3.15
                        return dict(ast.literal_eval(value.args[0]) if value.args else {},
                                    **{k.arg: ast.literal_eval(k.value)
                                       for k in value.keywords})
                    return dict(ast.literal_eval(value))
                except ValueError:
                    break
        raise FeatureError('%s: no opmap = {...}' % path)
    path, tree = _lib(src, 'opcode.py')
    out = {}
    for node in ast.walk(tree):
        if isinstance(node, ast.Call) and isinstance(node.func, ast.Name) \
                and node.func.id.endswith('_op') and len(node.args) >= 2:
            name, num = _constant(node.args[0]), _constant(node.args[1])
            if isinstance(name, str) and isinstance(num, int):
                out[name] = num
    if not out:
        raise FeatureError('%s: no def_op calls' % path)
    return out


def _designated(s, array):
    """(path, {NAME: value tokens}) of the header definition
    `array[...] = { [NAME] = value, ... }`, or (None, None)."""
    for path, toks in s.headers_with(array):
        for i in range(len(toks) - 1):
            if toks[i].text != array or toks[i + 1].text != '[':
                continue
            j = ctokens.match_close(toks, i + 1)
            if toks[j + 1].text != '=' or toks[j + 2].text != '{':
                continue
            out = {}
            for item in ctokens.split_top(toks[j + 3:ctokens.match_close(toks, j + 2)], ','):
                texts = _texts(item)
                if texts[:1] == ['['] and texts[2:4] == [']', '=']:
                    out[texts[1]] = texts[4:]
            return path, out
    return None, None


def jumps(s, ops):
    """(hasjrel, hasjabs) of the source s, each [(name, number)] by number:
    the instructions whose argument is a jump, relative to the next
    instruction or absolute. Read from Lib/opcode.py's jrel_op and jabs_op
    calls; from 3.13, where it has none, the instructions
    pycore_opcode_metadata.h flags HAS_JUMP_FLAG, every one relative. ops is
    opmap(); only instructions (below 256) are kept."""
    path, tree = _lib(s.src, 'opcode.py')
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
    if not out['jrel_op'] and not out['jabs_op']:
        mpath, meta = _designated(s, '_PyOpcode_opcode_metadata')
        if meta is None:
            raise FeatureError('%s: no jrel_op or jabs_op calls, and no'
                               ' _PyOpcode_opcode_metadata in Include' % path)
        out['jrel_op'] = [(name, ops[name]) for name, value in meta.items()
                          if 'HAS_JUMP_FLAG' in value and name in ops]
        if not out['jrel_op']:
            raise FeatureError('%s: no instruction has HAS_JUMP_FLAG' % mpath)
    return tuple(sorted((x for x in out[k] if x[1] < 256), key=lambda x: x[1])
                 for k in ('jrel_op', 'jabs_op'))


def caches(s, ops):
    """[(name, number, entries)] by number: the instructions followed by
    inline cache entries (from 3.11), and how many code units of them, from
    the header's _PyOpcode_Caches. ops is opmap()."""
    _, table = _designated(s, '_PyOpcode_Caches')
    out = []
    for name, value in (table or {}).items():
        if name not in ops or ops[name] >= 256:
            continue
        if len(value) != 1 or not value[0].isdigit():
            raise FeatureError('_PyOpcode_Caches[%s] is not a number: %s'
                               % (name, ' '.join(value)))
        if int(value[0]):
            out.append((name, ops[name], int(value[0])))
    return sorted(out, key=lambda x: x[1])


def backward(src, jrel):
    """[(name, number)] of the relative jumps in jrel whose argument counts
    backward (from 3.11), as Lib/dis.py's _is_backward_jump tells them:
    `'JUMP_BACKWARD' in opname[op]`, or `opname[op] in (NAME, ...)`. None
    where dis has no _is_backward_jump."""
    path, tree = _lib(src, 'dis.py')
    for node in tree.body:
        if isinstance(node, ast.FunctionDef) and node.name == '_is_backward_jump':
            ret = node.body[-1]
            test = ret.value if isinstance(ret, ast.Return) else None
            if isinstance(test, ast.Compare) and len(test.ops) == 1 \
                    and isinstance(test.ops[0], ast.In):
                part = _constant(test.left)
                if isinstance(part, str):
                    return [x for x in jrel if part in x[0]]
                right = test.comparators[0]
                if isinstance(right, ast.Tuple):
                    names = [_constant(e) for e in right.elts]
                    return [x for x in jrel if x[0] in names]
            raise FeatureError('%s: _is_backward_jump is neither form' % path)
    return []


# what the header says of the opcodes: jrel and jabs as jumps() gives them,
# jback as backward() gives it, the highest opcode number, caches as
# caches() gives them, and pseudo as pseudo() gives it
Opcodes = collections.namedtuple('Opcodes', 'jrel jabs jback highest caches pseudo')


def pseudo(ops):
    """The names in OPCODES that the opcode table ops gives to the
    compiler's pseudo-instructions (256 and up, from 3.12): opcode.h
    defines them, so le_features_gen.h doesn't."""
    return tuple(op for op in OPCODES if ops.get(op, 0) >= 256)


def opcodes(src):
    """The Opcodes of source tree src."""
    s = Source(src)
    ops = opmap(src)
    jrel, jabs = jumps(s, ops)
    return Opcodes(jrel, jabs, backward(src, jrel), max(ops.values()), caches(s, ops),
                   pseudo(ops))


D = _detector
AB = 'Objects/abstract.c'
CE = 'Python/ceval.c'

# C API functions that LeJIT calls where libpython exports them, and
# replaces where it doesn't
def _sum_compensated(s):
    """builtin_sum_impl's float loop carries a compensation term: c += ... in
    its own loop, or a cs_add helper"""
    for text in ('c += (f_result - t) + x', 'cs_add('):
        found, evidence = _body_has(s, 'Python/bltinmodule.c', 'builtin_sum_impl', text, missing=False)
        if found:
            return found, evidence
    return found, evidence


EXPORTS = [
    ('DICT_MERGE_EX_LINKABLE', '_PyDict_MergeEx'),
    ('HASH_BYTES_LINKABLE', '_Py_HashBytes'),
    ('PRIVATE_IS_FINALIZING_LINKABLE', '_Py_IsFinalizing'),
    ('LOOKUP_ATTR_LINKABLE', '_PyObject_LookupAttr'),
    ('FRAME_CLEAR_AND_POP_LINKABLE', '_PyEval_FrameClearAndPop'),
    ('HANDLE_PENDING_LINKABLE', '_Py_HandlePending'),
    ('SUPER_LOOKUP_LINKABLE', '_PySuper_Lookup'),
]


def _export_detector(function):
    return D('libpython exports %s' % function,
             lambda s: _exported(s, function))


# LE_HAS_<name> -> detector; its docstring's first line is the header's
# comment. One list per area, so that work on different areas adds to
# different lists; the header has them in the order of FEATURES below.

# the C API: what libpython exports, and its functions' signatures and
# behaviour
C_API = [
    ('PYDICT_NEWPRESIZED', pydict_newpresized),
    ('AUDIT_HOOKS', audit_hooks),
    ('ARG_CHECK_POSITIONAL', arg_check_positional),
    ('CHECK_RESULT_TSTATE', check_result_tstate),
    ('OBJECTDICT_SETITEM_LINKABLE', objectdict_setitem_linkable),
    ('OBJECTDICT_SETITEM_OBJECT', objectdict_setitem_object),
    ('FLOAT_ASDOUBLE_INDEX', float_asdouble_index),
    ('TRACEBACK_FROM_FRAME_API', D('_PyTraceBack_FromFrame is declared PyAPI_FUNC (in an internal header)',
                                   lambda s: _exported(s, '_PyTraceBack_FromFrame'))),
    ('LOOKUP_SPECIAL_ID', D('an extension can look up a special method by _Py_Identifier, with _PyObject_LookupSpecial',
                            lambda s: _public(s, '_PyObject_LookupSpecial'))),
] + [(name, _export_detector(f)) for name, f in EXPORTS]

# the bytecode and what its instructions do
BYTECODE = [
    ('MAP_ADD_VALUE_ON_TOP', map_add_value_on_top),
    ('POP_BLOCK_UNWINDS', pop_block_unwinds),
    ('POP_EXCEPT_UNWINDS', pop_except_unwinds),
    ('JUMP_IN_INSTRUCTIONS', jump_in_instructions),
    ('CODE_GET_CODE', D('PyCode_GetCode gives a code object\'s bytecode as compiled, unspecialised',
                        lambda s: _declared(s, 'PyCode_GetCode'))),
    ('EXCEPTION_TABLE', D('a code object\'s handlers are in its exception table (co_exceptiontable), not set up at run time on a block stack',
                          lambda s: _file_has(s, 'Objects/codeobject.c', '"co_exceptiontable"'))),
    ('CODE_LOCALSPLUS', code_localsplus),
    ('CODE_POSONLY_ARGS', D('a code object counts its positional-only parameters (co_posonlyargcount), whose names a keyword argument can\'t bind',
                            lambda s: _file_has(s, 'Objects/codeobject.c', '"co_posonlyargcount"'))),
    ('CODE_QUALNAME', D('a code object has its function\'s qualified name (co_qualname): MAKE_FUNCTION takes it from there, with no name on the stack',
                        lambda s: _file_has(s, 'Objects/codeobject.c', '"co_qualname"'))),
    ('LOAD_GLOBAL_NULL_BIT', D('LOAD_GLOBAL\'s argument is the name\'s index shifted left once, its low bit pushing a NULL under the global',
                               lambda s: _case_has(s, 'LOAD_GLOBAL', 'oparg>>1'))),
    ('IMPORT_NAME_LAZY_BITS', D('IMPORT_NAME\'s argument is the name\'s index shifted left twice, its low two bits saying whether the import is lazy (1) or always eager (2)',
                                lambda s: _case_has(s, 'IMPORT_NAME', 'oparg>>2'))),
    ('AWAIT_ERROR_BY_ARG', D('GET_AWAITABLE\'s argument tells its error which of async with\'s awaits it is (1 __aenter__\'s, 2 __aexit__\'s), not the instructions before it',
                             lambda s: _case_uses(s, 'GET_AWAITABLE', 'oparg'))),
    ('AWITH_EXIT_FIRST', D('BEFORE_ASYNC_WITH looks up __aexit__ before __aenter__',
                           lambda s: _case_has(s, 'BEFORE_ASYNC_WITH', 'PyId___aexit__),*enter', missing=False))),
    ('LOAD_ATTR_METHOD_BIT', D('LOAD_ATTR\'s argument is the name\'s index shifted left once, its low bit asking for the method form (a NULL or the method under the object)',
                               lambda s: _case_has(s, 'LOAD_ATTR', 'oparg>>1'))),
    ('COMPARE_OP_SHIFT_4', D('COMPARE_OP\'s comparison is its argument shifted right four times, the low bits the specialiser\'s',
                             lambda s: _case_has(s, 'COMPARE_OP', 'oparg>>4'))),
    ('COMPARE_OP_SHIFT_5', D('COMPARE_OP\'s comparison is its argument shifted right five times, the low bits the specialiser\'s and the bool conversion\'s',
                             lambda s: _case_has(s, 'COMPARE_OP', 'oparg>>5'))),
    ('MATCH_CLASS_PUSHES_BOOL', D('MATCH_CLASS pushes a bool saying whether the class matched, over the attributes or the subject',
                                  lambda s: _case_has(s, 'MATCH_CLASS', 'PyBool_FromLong', missing=False))),
    ('MATCH_KEYS_PUSHES_BOOL', D('MATCH_KEYS pushes a bool saying whether the keys matched, over the values or None',
                                 lambda s: _case_has(s, 'MATCH_KEYS', 'PUSH(Py_True)', missing=False))),
    ('CALLABLE_UNDER_NULL', D('a call\'s callable is under its NULL or self (CALL, CALL_FUNCTION_EX): LOAD_GLOBAL, LOAD_ATTR and LOAD_SUPER_ATTR push the NULL or self over what they load',
                              lambda s: _case_has(s, 'CALL', 'callable = stack_pointer[-2 - oparg]', missing=False))),
    ('FOR_ITER_EXIT_PAST_POP_TOP', D('a for loop ends with END_FOR, which pops only the item, then a POP_TOP for the iterator; FOR_ITER\'s exit jumps past both',
                                     lambda s: _case_has(s, 'FOR_ITER', 'JUMPBY(oparg + 2)'))),
    ('FOR_ITER_EXIT_KEEPS_ITER', D('FOR_ITER\'s exit leaves the iterator on the stack and jumps past END_FOR to POP_ITER, which pops it',
                                   lambda s: _case_has(s, 'FOR_ITER', 'JUMPBY(oparg + 1)'))),
    ('CALL_EX_KWARGS_SLOT', D('CALL_FUNCTION_EX always has a slot for the keywords dict, NULL where the call has none',
                              lambda s: _case_has(s, 'CALL_FUNCTION_EX', 'kwargs_st = stack_pointer[-1]', missing=False))),
    ('BINARY_OP_SUBSCR', D('a subscript is BINARY_OP with NB_SUBSCR',
                           lambda s: ('NB_SUBSCR' in (s.macros('Include/opcode.h') or {}), 'Include/opcode.h: NB_SUBSCR'))),
    ('FUNCTION_ANNOTATE', function_annotate),
]

# frames
FRAMES = [
    ('FRAME_CLEARED_LAST_FIRST', D('a frame that ends releases its value stack and locals last to first (_PyFrame_ClearLocals), not first to last',
                                   lambda s: _body_has(s, 'Python/frame.c', '_PyFrame_ClearLocals', 'sp--', missing=False))),
    ('FINISHED_GENERATOR_UNLINKED', D('a generator\'s frame is cut from its caller before it is cleared (clear_gen_frame, or before it had one, each time it stops), so a generator that has ended or raised leaves no f_back',
                                      lambda s: _body_has(s, CE, 'clear_gen_frame', 'frame->previous = NULL; _PyFrame_ClearExceptCode(frame);', missing=True))),
    ('LASTI_IN_INSTRUCTIONS', lasti_in_instructions),
    ('LINENO_WHEN_SET', lineno_when_set),
    ('FRAME_BUILTINS_OF_FUNCTION', frame_builtins_of_function),
    ('INTERPRETER_FRAME', interpreter_frame),
    ('LASTI_PAST_PREFIXES', lasti_past_prefixes),
    ('PYCORE_INTERPFRAME_H', D('the interpreter frame and its inline functions are in the internal pycore_interpframe.h',
                               lambda s: _file(s, 'Include/internal/pycore_interpframe.h'))),
    ('FRAME_STACKREFS', frame_stackrefs),
    ('FRAME_INITIALIZE_TSTATE', frame_initialize_tstate),
]

# types
TYPES = [
    ('IMMUTABLE_TYPE_FLAG', immutable_type_flag),
    ('VALID_VERSION_TAG_FLAG', valid_version_tag_flag),
    ('MANAGED_DICT', D('a class with Py_TPFLAGS_MANAGED_DICT keeps its instances\' attributes where the interpreter manages them, maybe in values with no dict, which _PyObject_GetDictPtr makes',
                       lambda s: _body_has(s, 'Objects/object.c', '_PyObject_GetDictPtr', 'Py_TPFLAGS_MANAGED_DICT'))),
]

# how LeJIT's own mechanisms reach the interpreter's state
STATE = [
    ('PYCORE_PYSTATE_H', D('the runtime and interpreter states are described in the internal pycore_pystate.h',
                           lambda s: _file(s, 'Include/internal/pycore_pystate.h'))),
    ('PYCORE_INTERP_H', D('the interpreter state has an internal header of its own, pycore_interp.h',
                          lambda s: _file(s, 'Include/internal/pycore_interp.h'))),
    ('EVAL_HOOK_TSTATE', eval_hook_tstate),
    ('SIGNALS_PENDING', signals_pending),
    ('EVAL_FRAME_HANDLE_PENDING', eval_frame_handle_pending),
    ('INTERP_RECURSION_LIMIT', interp_recursion_limit),
    ('RECURSION_REMAINING', recursion_remaining),
    ('EXC_INFO_VALUE_ONLY', exc_info_value_only),
    ('FRAME_STATE', frame_state),
    ('CFRAME', cframe),
    ('GC_TRACKED_BY_NEXT', gc_tracked_by_next),
    ('GC_HEAD_INTERNAL', gc_head_internal),
    ('MONITORING', monitoring),
    ('INTERP_MONITORING_VERSION', interp_monitoring_version),
    ('THREAD_LOCAL_TSTATE', thread_local_tstate),
    ('PY_RECURSION_REMAINING', py_recursion_remaining),
    ('C_RECURSION_REMAINING', c_recursion_remaining),
    ('CURRENT_EXCEPTION', current_exception),
    ('EVAL_BREAKER_GC', eval_breaker_gc),
    ('EVAL_FRAME_POPS', eval_frame_pops),
    ('FRAME_FUNCOBJ', frame_funcobj),
    ('FRAME_EXECUTABLE', frame_executable),
    ('FRAME_INSTR_PTR', frame_instr_ptr),
    ('TSTATE_CURRENT_FRAME', tstate_current_frame),
    ('TSTATE_EVAL_BREAKER', tstate_eval_breaker),
]

# Objects/abstract.c
ABSTRACT = [
    ('TERNARY_IOP', D('abstract.c has ternary_iop, the in-place ternary operation (**=)',
                      lambda s: _defines(s, AB, 'ternary_iop'))),
    ('ITER_CHECK_FUNCTION', D('PyIter_Check is a function, not a macro',
                              lambda s: _defines(s, AB, 'PyIter_Check'))),
    ('NOT_A_SEQUENCE_ERROR', D('indexing a mapping as a sequence says it "is not a sequence"',
                               lambda s: _body_has(s, AB, 'PySequence_GetItem', '"%.200s is not a sequence"'))),
    ('PRIVATE_NUMBER_INDEX', D('_PyNumber_Index, __index__ giving an exact int or a subclass, exists',
                               lambda s: _defines(s, AB, '_PyNumber_Index'))),
    ('INLINE_CALL_TSTATE_GET', D('the inline call helpers (PyObject_CallOneArg) take the thread state with PyThreadState_Get(), not PyThreadState_GET()',
                                 lambda s: (any('PyThreadState_Get' in _texts(t) for _, t in s.definitions('PyObject_CallOneArg')),
                                            'Include, every header: PyObject_CallOneArg'))),
    ('CALL_ONE_ARG', D('PyObject_CallOneArg exists',
                       lambda s: _declared(s, 'PyObject_CallOneArg'))),
    ('VECTORCALL', D('PyObject_Vectorcall exists: ceval calls with the arguments in an array, no tuple',
                     lambda s: _declared(s, 'PyObject_Vectorcall'))),
    ('COROUTINE_WRAPPER', D('a thread state has a coroutine wrapper (sys.set_coroutine_wrapper), which ceval calls on each new coroutine',
                             lambda s: _field(s, '_ts', 'coroutine_wrapper'))),
    ('ITER_SEND', D('PyIter_Send sends a value into an iterator, telling a yield from a return',
                    lambda s: _declared(s, 'PyIter_Send'))),
    ('FUNCTION_STR', D('_PyObject_FunctionStr names a function in error messages',
                       lambda s: _declared(s, '_PyObject_FunctionStr'))),
    ('FASTCALL_DICT_TSTATE', D('_PyObject_FastCallDictTstate exists',
                               lambda s: _declared(s, '_PyObject_FastCallDictTstate'))),
]

# the other objects
OBJECTS = [
    ('TUPLE_UNTRACKED_AT_BUILD', D('a tuple built from items none of whose types the GC tracks is left untracked from the start (tupleobject.c:maybe_tracked)',
                                   lambda s: _defines(s, 'Objects/tupleobject.c', 'maybe_tracked'))),
    ('TUPLE_GET_EMPTY', D('the empty tuple is the interpreter\'s, read with tuple_get_empty',
                          lambda s: _defines(s, 'Objects/tupleobject.c', 'tuple_get_empty'))),
    ('CONTAINS_ITEM_FIRST', tuplecontains_item_first),
    ('DICT_VALUES_STRUCT', dict_values_struct),
    ('HASH_DOUBLE_OBJECT', D('_Py_HashDouble is given the float object too',
                             lambda s: _file_has(s, 'Python/pyhash.c', '_Py_HashDouble(PyObject *'))),
    ('MEMBER_EX_ERROR_FROM_TYPE', member_ex_error_from_type),
    ('SUM_COMPENSATED', D('sum() adds floats with compensated (Neumaier) summation, not plain +',
                          _sum_compensated)),
    ('SUM_INT_AS_DOUBLE', D('sum()\'s float loop adds an int item with PyLong_AsDouble and compensation, raising its OverflowError, not as a C long with plain +',
                            lambda s: _body_has(s, 'Python/bltinmodule.c', 'builtin_sum_impl',
                                                'PyLong_AsDouble(item)', missing=False))),
    ('SUM_COMPLEX_COMPENSATED', D('sum() adds complex numbers with compensated summation of each part',
                                  lambda s: _body_has(s, 'Python/bltinmodule.c', 'builtin_sum_impl',
                                                      'PyComplex_CheckExact(result)', missing=False))),
    ('POW_KEYWORDS', D('pow() takes keyword arguments base, exp and mod',
                       lambda s: _file_has(s, 'Python/clinic/bltinmodule.c.h',
                                           'builtin_pow_impl(PyObject *module, PyObject *base'))),
    ('DICT_KEYS_KIND', dict_keys_kind),
    ('LONG_VALUE', long_value),
    ('TYPE_GET_DICT', D('PyType_GetDict gives a type\'s dict, which a static builtin type keeps in the interpreter (its tp_dict is NULL)',
                        lambda s: _public(s, 'PyType_GetDict'))),
    ('IMMORTAL_OBJECTS', D('an object whose refcount\'s low 32 bits are negative is immortal: Py_INCREF and Py_DECREF leave it alone (_Py_IsImmortal)',
                           lambda s: _public(s, '_Py_IsImmortal'))),
    ('DEALLOC_HOOKS', D('_Py_Dealloc does more than call tp_dealloc: it reports to the ref tracer (PyRefTracer_SetTracer), and may defer a deep dealloc to the trashcan',
                        lambda s: _public(s, 'PyRefTracer_SetTracer'))),
    ('DEALLOC_DEFERS_TO_TRASHCAN', dealloc_defers_to_trashcan),
    ('MANAGED_VALUES_POINTER', managed_values_pointer),
    ('RANGE_ITER_INDEX', range_iter_index),
    ('TAGGED_DICT_OR_VALUES', tagged_dict_or_values),


    ('INLINE_VALUES', inline_values),
    ('DICT_WATCHER_BITS', dict_watcher_bits),
    ('REFCOUNT_32', refcount_32),
    ('DICT_VERSION_TAG', dict_version_tag),
]

# Python/ceval.c
CEVAL = [
    ('CMP_OUTCOME', D('COMPARE_OP also does is, in and exception matches (cmp_outcome)',
                      lambda s: _defines(s, CE, 'cmp_outcome'))),
    ('CEVAL_TRACE_INFO', D('ceval keeps the line bounds it traces with, and the frame\'s CFrame, in a PyTraceInfo of its own (call_trace, do_call_core)',
                           lambda s: _file_has(s, CE, 'CFrame cframe; } PyTraceInfo;'))),
    ('CALL_TRACE_METHOD_DESCR', call_trace_method_descr),
    ('NAME_ERROR_NAME', name_error_name),
    ('IMPORT_FROM_SPEC', D('import_from\'s error tells a partially initialised module',
                           lambda s: _first_body_has(s, [(CE, 'import_from'), (CE, '_PyEval_ImportFrom')],
                                                     '_PyModuleSpec_IsInitializing'))),
    ('KWARGS_MERGE_EX', D('CALL_FUNCTION_EX merges **kwargs with _PyDict_MergeEx, and format_kwargs_error reports it',
                          lambda s: _case_has(s, 'CALL_FUNCTION_EX', '_PyDict_MergeEx'))),
    ('DELETE_GLOBAL_KEYERROR_ONLY', delete_global_keyerror_only),
    ('RAISE_CAUSE_CHECKED', D('raise ... from a class whose call doesn\'t make an exception is a TypeError (do_raise)',
                              lambda s: _first_body_has(s, [(CE, 'do_raise'), ('Python/ceval.h', 'do_raise')],
                                                        'PyExceptionInstance_Check(fixed_cause)'))),
]

# the opcodes that some CPython lacks
OPCODE_FEATURES = [('OPCODE_' + op, _opcode(op)) for op in OPCODES + CASE_OPCODES]

FEATURES = (C_API + BYTECODE + FRAMES + TYPES + STATE + ABSTRACT + OBJECTS
            + CEVAL + OPCODE_FEATURES)


def describe(detector):
    return ' '.join(detector.__doc__.split())


def detect(src):
    """[(LE_HAS_name, value, what it detects, evidence)]; FeatureError if a
    detector can't answer."""
    s = Source(src)
    out = []
    for name, d in FEATURES:
        try:
            value, evidence = d(s)
        except Unanswered as e:
            raise FeatureError('feature LE_HAS_%s (tools/features/features.py): %s'
                               % (name, e))
        out.append(('LE_HAS_' + name, int(value), describe(d), evidence))
    return out


def build(src, version):
    """The header's text for source tree src, CPython version."""
    return render(detect(src), opcodes(src), version,
                  'tools/features/features.py from CPython %s' % version)


def render(found, ops, version, origin):
    """The header's text: found as detect() gives it, ops the Opcodes, for
    CPython version; origin, in its first line, says what made it."""
    lines = ['/* Generated by %s: do not edit. */' % origin,
             '#ifndef LE_FEATURES_GEN_H', '#define LE_FEATURES_GEN_H', '']
    for name, value, what, _ in found:
        lines += ['/* %s */' % what.replace('*/', '* /'),
                  '#define %s %d' % (name, value)]
    values = dict((name, value) for name, value, _, _ in found)
    absent = [op for op in OPCODES
              if not values['LE_HAS_OPCODE_' + op] and op not in ops.pseudo]
    if absent:
        lines += ['', '/* opcodes this source lacks: no instruction has these values */']
        # above every opcode, the compiler's pseudo-instructions too, so
        # that no case for one of these matches another's
        base = max(256, ops.highest + 1)
        lines += ['#define %s %d' % (op, base + OPCODES.index(op)) for op in absent]
    lines +=['', '/* the opcodes whose argument is a jump (Lib/opcode.py hasjrel,'
              ' hasjabs) */']
    for macro, jump_ops in (('LE_IS_JREL', ops.jrel), ('LE_IS_JABS', ops.jabs)):
        lines.append('#define %s(o) (%s)' % (
            macro, ' || '.join('(o) == %d' % n for _, n in jump_ops) or '0'))
    if ops.jback:
        lines += ['', '/* the relative jumps that jump backward */',
                  '#define LE_IS_JBACK(o) (%s)' % ' || '.join(
                      '(o) == %d' % n for _, n in ops.jback)]
    if ops.caches:
        lines += ['', '/* the code units of inline cache that follow each'
                      ' instruction (_PyOpcode_Caches) */',
                  '#define LE_CACHE_ENTRIES(o) (%s0)' % ''.join(
                      '(o) == %d ? %d : ' % (n, c) for _, n, c in ops.caches)]
    lines += ['', '/* for lejit._core._build: the source read, and every'
                  ' feature */',
              '#define LE_FEATURES_SOURCE_VERSION %s' % json.dumps(version),
              '#define LE_FEATURES(F) \\']
    lines += ['  F(%s) \\' % name for name, _, _, _ in found]
    lines += ['', '#endif /* LE_FEATURES_GEN_H */', '']
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
    # doesn't rebuild lejit._core
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return 0
    with open(path + '.tmp', 'w') as f:
        f.write(text)
    os.replace(path + '.tmp', path)
    print('%s: CPython %s, %d features' % (path, version, text.count('\n#define LE_HAS_')))
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
