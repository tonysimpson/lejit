#!/usr/bin/env python3
"""Detect CPython's features from an installed interpreter, with no source
tree: a binary release (python.org's installers, a distribution's package,
pyenv) is enough, as for any extension.

It answers the same features as features.py (its FEATURES list, read when
this runs), and writes the same ni_features_gen.h. Each feature is answered
one of two ways, in this order of preference:

  interpreter  in the interpreter itself, in a subprocess of it
               (installed_probes.py): its opcode module, which functions
               libpython exports, objects' attributes, and a few lines of
               Python whose result shows the behaviour the feature records
  headers      in the installed headers (sysconfig's include directory,
               with cpython/ and internal/), read with ctokens like the
               source detector reads Include/: only for what the
               interpreter can't show, a struct's field or a function's
               parameters

A feature neither can answer stops the build, like a question the source
can't answer: never a guess. features.py stays the reference: the check
tests/features/installed_test.py runs both and requires them to agree.

    installed.py --python EXE --out DIR
        write DIR/ni_features_gen.h for the interpreter EXE
    installed.py --python EXE
        print each feature, its value and how it was answered

It runs under any Python 3.7 or later; the probes run under EXE.
"""
import argparse
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ctokens  # noqa: E402
import features  # noqa: E402
import installed_probes  # noqa: E402

PROBES = os.path.join(HERE, 'installed_probes.py')
TIMEOUT = 120


class Unanswered(Exception):
    pass


# --- the interpreter -------------------------------------------------------

def run_probes(python, names=()):
    """The probes' JSON from interpreter python (installed_probes.py). A
    probe that crashes the interpreter is run again alone, so that it
    alone goes unanswered."""
    def run(names):
        try:
            r = subprocess.run([python, '-I', PROBES] + list(names),
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               universal_newlines=True, timeout=TIMEOUT)
        except (OSError, subprocess.TimeoutExpired) as e:
            return None, str(e)
        if r.returncode != 0:
            return None, 'exit status %d: %s' % (r.returncode, r.stderr.strip()[-300:])
        try:
            return json.loads(r.stdout), None
        except ValueError:
            return None, 'no JSON: %r' % r.stdout[-300:]
    out, err = run(names)
    if out is not None:
        return out
    every = list(names) or [f.__name__ for f in installed_probes.PROBES]
    out, err2 = run(['-'])     # no probe: the interpreter's own facts
    if out is None:
        raise features.FeatureError('cannot run %s: %s' % (python, err2))
    for name in every:
        one, why = run([name])
        out['answers'][name] = (one['answers'][name] if one is not None
                                else {'unanswered': 'the probe crashed the'
                                      ' interpreter (%s)' % why})
    return out


# --- the installed headers -------------------------------------------------

class Headers(features.Source):
    """The installed headers, read with the source detector's questions:
    paths are relative to the include directory (cpython/abstract.h,
    internal/pycore_pystate.h)."""

    def __init__(self, include):
        if not os.path.isfile(os.path.join(include, 'Python.h')):
            raise features.FeatureError('%s has no Python.h: install the'
                                        ' headers (python3-dev)' % include)
        self.src = include
        self.toks = {}
        self.headers = sorted(
            os.path.relpath(p, include) for p in
            glob.glob(os.path.join(include, '**', '*.h'), recursive=True))
        self.internal = os.path.isdir(os.path.join(include, 'internal'))

    def need_internal(self):
        if not self.internal:
            raise Unanswered('the installed headers have no internal/')


def _texts(toks):
    return [t.text for t in toks] if toks else []


def _field(h, struct, field):
    path, fields = h.struct(struct)
    if fields is None:
        raise Unanswered('no struct %s in the headers' % struct)
    return field in fields, '%s:%s.%s' % (path, struct, field)


def _struct_body(h, tag):
    """(path, token texts) of the body of `struct tag {...}`, or (None,
    None). Unlike Source.struct it keeps every token, for fields its
    {name: type} can't tell apart (two arrays sized by one macro)."""
    for path, toks in h.headers_with(tag):
        for i in range(len(toks) - 2):
            if (toks[i].text == 'struct' and toks[i + 1].text == tag
                    and toks[i + 2].text == '{'):
                end = ctokens.match_close(toks, i + 2)
                return path, _texts(toks[i + 3:end])
    return None, None


def _first_param(h, name, type_name):
    """(name's first parameter is a type_name, evidence)"""
    path, decl = h.declaration(name)
    if decl is None:
        raise Unanswered('no declaration of %s in the headers' % name)
    toks = h.tokens(path)
    for i, t in enumerate(toks[:-2]):
        if t.text == name and toks[i + 1].text == '(':
            return toks[i + 2].text == type_name, path + ':' + name
    raise Unanswered('%s: no parameters of %s' % (path, name))


def _declared(h, name):
    path = h.declares(name)
    return bool(path), '%s:%s' % (path or 'every header', name)


def _macro(h, name):
    for path, _ in h.headers_with(name):
        if name in h.macros(path):
            return True, '%s:%s' % (path, name)
    return False, 'every header: %s' % name


def check_result_tstate(h):
    """headers: _Py_CheckFunctionResult's first parameter is a
    PyThreadState"""
    return _first_param(h, '_Py_CheckFunctionResult', 'PyThreadState')


def eval_hook_tstate(h):
    """headers: the typedef _PyFrameEvalFunction's first parameter is a
    PyThreadState"""
    want = ['PyObject', '*', '(', '*', '_PyFrameEvalFunction', ')', '(']
    for path, toks in h.headers_with('_PyFrameEvalFunction'):
        texts = _texts(toks)
        for i in range(len(texts) - len(want)):
            if texts[i] == 'typedef' and texts[i + 1:i + 1 + len(want)] == want:
                return (texts[i + 1 + len(want)] == 'PyThreadState',
                        path + ':_PyFrameEvalFunction')
    raise Unanswered('no typedef of _PyFrameEvalFunction in the headers')


def pycore_header(name):
    def detect(h):
        h.need_internal()
        path = 'internal/' + name
        return h.exists(path), path
    detect.__doc__ = 'headers: the internal headers include %s' % name
    return detect


def signals_pending(h):
    """headers: struct _ceval_runtime_state has signals_pending"""
    h.need_internal()
    return _field(h, '_ceval_runtime_state', 'signals_pending')


def eval_frame_handle_pending(h):
    """headers: eval_breaker is a field of the interpreter's struct
    _ceval_state (where it is, ceval handles it in a function of its own);
    unanswered where the headers declare _Py_HandlePending, a sign the
    handling has left ceval"""
    h.need_internal()
    path, fields = h.struct('_ceval_state')
    if fields is None or 'eval_breaker' not in fields:
        return False, '%s:_ceval_state.eval_breaker' % (path or 'every header')
    if h.declares('_Py_HandlePending'):
        raise Unanswered('%s declares _Py_HandlePending'
                         % h.declares('_Py_HandlePending'))
    return True, '%s:_ceval_state.eval_breaker' % path


def interp_recursion_limit(h):
    """headers: struct _ceval_state has recursion_limit; where there is no
    _ceval_state, the global _Py_CheckRecursionLimit must be declared"""
    h.need_internal()
    path, fields = h.struct('_ceval_state')
    if fields is not None:
        return 'recursion_limit' in fields, path + ':_ceval_state.recursion_limit'
    if h.declares('_Py_CheckRecursionLimit') or h.declaration('_Py_CheckRecursionLimit')[0]:
        return False, 'no _ceval_state; _Py_CheckRecursionLimit'
    raise Unanswered('neither _ceval_state nor _Py_CheckRecursionLimit')


def frame_state(h):
    """headers: struct _frame has f_state (and not f_executing)"""
    path, fields = h.struct('_frame')
    if fields is None:
        raise Unanswered('no struct _frame in the headers')
    if 'f_state' not in fields and 'f_executing' not in fields:
        raise Unanswered('%s:_frame has neither f_state nor f_executing' % path)
    return 'f_state' in fields, path + ':_frame.f_state'


def cframe(h):
    """headers: use_tracing is a field of the thread state's CFrame, not
    of struct _ts"""
    path, ts = h.struct('_ts')
    if ts is None:
        raise Unanswered('no struct _ts in the headers')
    if 'use_tracing' in ts:
        return False, path + ':_ts.use_tracing'
    if 'cframe' in ts:
        name = [w for w in ts['cframe'].split() if w not in ('struct', '*')][-1]
        cpath, cf = h.struct(name)
        if cf is not None and 'use_tracing' in cf:
            return True, '%s:%s.use_tracing' % (cpath, name)
    raise Unanswered('%s:_ts has no use_tracing, nor a CFrame with one' % path)


def gc_tracked_by_next(h):
    """headers: _PyObject_GC_IS_TRACKED's definition reads _gc_next"""
    found = h.definitions('_PyObject_GC_IS_TRACKED')
    if not found:
        raise Unanswered('no definition of _PyObject_GC_IS_TRACKED')
    return (any('_gc_next' in _texts(t) for _, t in found),
            ', '.join(sorted({p for p, _ in found})) + ':_PyObject_GC_IS_TRACKED')


def gc_head_internal(h):
    """headers: PyGC_Head is defined only in an internal header, as
    _gc_next and _gc_prev"""
    h.need_internal()
    path, fields = h.struct('PyGC_Head')
    if path is None or not path.startswith('internal' + os.sep):
        return False, (path or 'every header') + ':PyGC_Head'
    if list(fields.items()) != [('_gc_next', 'uintptr_t'), ('_gc_prev', 'uintptr_t')]:
        raise Unanswered('%s:PyGC_Head is not _gc_next, _gc_prev' % path)
    return True, path + ':PyGC_Head'


def check_slot_result(h):
    """headers: _Py_CheckSlotResult is declared (libpython doesn't export
    it, so ctypes can't see it)"""
    return _declared(h, '_Py_CheckSlotResult')


def inline_call_tstate_get(h):
    """headers: PyObject_CallOneArg's definition (static inline) calls
    PyThreadState_Get(); no definition is no"""
    found = h.definitions('PyObject_CallOneArg')
    return (any('PyThreadState_Get' in _texts(t) for _, t in found),
            ', '.join(sorted({p for p, _ in found})) or 'every header')


def call_no_arg_function(h):
    """headers: _PyObject_CallNoArg is declared, and not as a macro"""
    is_macro, evidence = _macro(h, '_PyObject_CallNoArg')
    if is_macro:
        return False, evidence
    declared, evidence = _declared(h, '_PyObject_CallNoArg')
    if not declared:
        raise Unanswered('no _PyObject_CallNoArg in the headers')
    return True, evidence


def call_one_arg(h):
    """headers: PyObject_CallOneArg is declared (it is static inline, so
    ctypes can't see it)"""
    return _declared(h, 'PyObject_CallOneArg')


def tuple_get_empty(h):
    """headers: the interpreter state holds the tuple free lists, whose
    first entry is the empty tuple (struct _Py_tuple_state with free_list,
    the field tuple of struct _is)"""
    h.need_internal()
    path, body = _struct_body(h, '_Py_tuple_state')
    if body is None:
        return False, 'every header: no _Py_tuple_state'
    ipath, interp = h.struct('_is')
    if 'free_list' not in body or interp is None or 'tuple' not in interp:
        raise Unanswered('%s:_Py_tuple_state without free_list, or no _is.tuple' % path)
    return True, '%s:_Py_tuple_state.free_list, %s:_is.tuple' % (path, ipath)


HEADER_DETECTORS = {
    'CHECK_RESULT_TSTATE': check_result_tstate,
    'PYCORE_PYSTATE_H': pycore_header('pycore_pystate.h'),
    'PYCORE_INTERP_H': pycore_header('pycore_interp.h'),
    'PYCORE_FORMAT_H': pycore_header('pycore_format.h'),
    'EVAL_HOOK_TSTATE': eval_hook_tstate,
    'SIGNALS_PENDING': signals_pending,
    'EVAL_FRAME_HANDLE_PENDING': eval_frame_handle_pending,
    'INTERP_RECURSION_LIMIT': interp_recursion_limit,
    'FRAME_STATE': frame_state,
    'CFRAME': cframe,
    'GC_TRACKED_BY_NEXT': gc_tracked_by_next,
    'GC_HEAD_INTERNAL': gc_head_internal,
    'CHECK_SLOT_RESULT': check_slot_result,
    'INLINE_CALL_TSTATE_GET': inline_call_tstate_get,
    'CALL_NO_ARG_FUNCTION': call_no_arg_function,
    'CALL_ONE_ARG': call_one_arg,
    'TUPLE_GET_EMPTY': tuple_get_empty,
}

# features that neither the interpreter nor the headers show: the two forms
# Ni copies behave the same, and the difference is only in a C file
NOT_VISIBLE = {
    'ITER_CHECK_TYPE_LOCAL': 'PyIter_Check reading the type into a local behaves the same',
    'TUPLE_ALLOC': 'tuple_alloc is a static function of tupleobject.c; tuples behave the same',
    'UNICODE_EQ_ASSUMES_READY': 'unicode_eq asserting rather than readying shows only in a debug build',
    'BYTES_STRIP_NONE_DEFAULT': 'a NULL default is never seen from Python',
    'UNICODE_STRIP_NONE_DEFAULT': 'a NULL default is never seen from Python',
    'UNICODE_FILL': 'unicode_fill is a static function of unicodeobject.c',
    'MATH_UNDERFLOW_BELOW_1_5': 'shows only with a C library that sets ERANGE for results near -1',
    'VARARGS_FROM_ARRAY': 'how ceval builds *args is not visible: the tuple is the same',
    'TRACE_INFO': 'ceval\'s tracing state is a static type of ceval.c',
    'CEVAL_NAME_IDENTIFIER': 'where ceval.c declares its __name__ identifier is not visible',
    'SPECIAL_LOOKUP_FROM_ID': 'the AttributeError is the same either way',
    'FORMAT_VALUE_CHECKS_CONVERSION': 'the unknown-conversion branch cannot be reached (the'
                                      ' conversion is 2 bits, all 4 values known)',
}


# --- the features ----------------------------------------------------------

def _answer(name, probed, headers):
    """(value, kind, evidence) for feature name, or Unanswered."""
    answers = probed['answers']
    if name.startswith('OPCODE_'):
        op = name[len('OPCODE_'):]
        return (op in probed['real_opcodes'], 'interpreter',
                'opcode.opmap' + ('' if op in probed['real_opcodes'] else ': no ' + op))
    if name in answers:
        a = answers[name]
        if isinstance(a, dict):
            raise Unanswered(a['unanswered'])
        return a[0], 'interpreter', a[1]
    if name in HEADER_DETECTORS:
        if isinstance(headers, Exception):
            raise Unanswered(str(headers))
        try:
            value, evidence = HEADER_DETECTORS[name](headers)
        except SyntaxError as e:     # ctokens: unbalanced brackets
            raise Unanswered(str(e))
        return value, 'headers', evidence
    if name in NOT_VISIBLE:
        raise Unanswered('not visible in the interpreter or its headers: '
                         + NOT_VISIBLE[name])
    raise Unanswered('installed.py has no detector for it')


def how(name):
    """The docstring of the installed detector for feature name, or why
    there is none."""
    if name.startswith('OPCODE_'):
        return 'interpreter: opcode.opmap has %s' % name[len('OPCODE_'):]
    for f in installed_probes.PROBES:
        if f.__name__ == name:
            return 'interpreter: ' + ' '.join(f.__doc__.split())
    if name in HEADER_DETECTORS:
        return ' '.join(HEADER_DETECTORS[name].__doc__.split())
    return 'unanswered: ' + NOT_VISIBLE.get(name, 'no detector')


def detect(python):
    """(probed, [(NI_HAS_name, value, what it detects, evidence, kind)],
    {NI_HAS_name: why unanswered}) for interpreter python. what is
    features.py's description, so that the header reads the same."""
    probed = run_probes(python)
    # opcodes numbered 256 and up are the compiler's pseudo-instructions
    # (3.12), never in the bytecode
    probed['real_opcodes'] = {n for n, v in probed['opcodes'].items() if v < 256}
    try:
        headers = Headers(probed['include'])
    except features.FeatureError as e:
        headers = e
    found, unanswered = [], {}
    for name, d in features.FEATURES:
        try:
            value, kind, evidence = _answer(name, probed, headers)
        except Unanswered as e:
            unanswered['NI_HAS_' + name] = str(e)
            continue
        found.append(('NI_HAS_' + name, int(bool(value)), features.describe(d),
                      evidence, kind))
    return probed, found, unanswered


def jumps(probed):
    return ([tuple(x) for x in probed['hasjrel']],
            [tuple(x) for x in probed['hasjabs']])


def build(python):
    """(header text, version) for interpreter python; FeatureError when a
    feature goes unanswered."""
    probed, found, unanswered = detect(python)
    if unanswered:
        raise features.FeatureError(
            '%d feature(s) the installed interpreter %s does not answer'
            ' (features.py reads them in its source):\n%s' % (
                len(unanswered), python,
                '\n'.join('  %s: %s' % kv for kv in sorted(unanswered.items()))))
    jrel, jabs = jumps(probed)
    if not jrel or not jabs:
        raise features.FeatureError('opcode.hasjrel or hasjabs is empty')
    version = probed['version']
    text = features.render([f[:4] for f in found], (jrel, jabs), version,
                           'tools/features/installed.py from the installed'
                           ' CPython %s (%s)' % (version, python))
    return text, version


def write(python, out):
    path = os.path.join(out, features.HEADER)
    try:
        text, version = build(python)
    except features.FeatureError as e:
        if os.path.exists(path):
            os.remove(path)
        print('installed.py: %s' % e, file=sys.stderr)
        return 1
    os.makedirs(out, exist_ok=True)
    # rewritten only when its text changes, so that an unchanged header
    # doesn't rebuild ni.so
    if os.path.exists(path):
        with open(path) as f:
            if f.read() == text:
                return 0
    with open(path + '.tmp', 'w') as f:
        f.write(text)
    os.replace(path + '.tmp', path)
    print('%s: installed CPython %s, %d features'
          % (path, version, text.count('\n#define NI_HAS_')))
    return 0


def show(python):
    probed, found, unanswered = detect(python)
    print('%s (CPython %s, headers %s)' % (python, probed['version'], probed['include']))
    values = {f[0]: f for f in found}
    for name, _ in features.FEATURES:
        key = 'NI_HAS_' + name
        if key in values:
            _, value, _, evidence, kind = values[key]
            print('  %-38s %d  %-11s %s' % (key, value, kind, evidence))
        else:
            print('  %-38s ?  %-11s %s' % (key, 'unanswered', unanswered[key]))
    kinds = [f[4] for f in found]
    print('%d features: %d by the interpreter, %d by the headers, %d unanswered'
          % (len(features.FEATURES), kinds.count('interpreter'),
             kinds.count('headers'), len(unanswered)))
    return 1 if unanswered else 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--python', required=True)
    ap.add_argument('--out')
    args = ap.parse_args(argv)
    try:
        if args.out:
            return write(args.python, args.out)
        return show(args.python)
    except features.FeatureError as e:
        print('installed.py: %s' % e, file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
