"""Detect CPython's features from the source Ni is built against, never
from its version number (D136): a release with a backport, a development
version, a distribution's patches or a fork all say what they have only in
their source.

Each feature is a short question on the source (mechanisms.Source: a
struct's fields, a declaration and its parameters, a macro, a function
body, an opcode), answering 1 or 0 with the definition it read. build()
writes ni_features_gen.h, one `#define NI_HAS_<NAME> 0|1` per feature, and
the report lines generate.py puts in report.txt, so a feature that changes
shows as a baseline difference. A question the source can't answer (the
struct or file it reads is missing) stops generation: a new CPython that
moves a definition needs the detector updated, never a guess. Run alone it
prints the features of one or more source trees:

    features.py SRC [SRC...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cslots  # noqa: E402
from mechanisms import Source  # noqa: E402

HEADER = 'ni_features_gen.h'


class Unanswered(Exception):
    pass


def _texts(toks):
    return [t.text for t in toks] if toks else []


def _field(s, struct, field):
    path, fields = s.struct(struct)
    if fields is None:
        raise Unanswered('no struct %s in Include' % struct)
    return field in fields, '%s:%s.%s' % (path, struct, field)


def _exported(s, name):
    path, decl = s.declaration(name)
    if decl is None:
        return False, 'Include, every header: %s' % name
    return 'PyAPI_FUNC' in _texts(decl), '%s:%s' % (path, name)


def _macro(s, name):
    for path, _ in s.headers_with(name):
        if name in s.macros(path):
            return True, '%s:%s' % (path, name)
    return False, 'Include, every header: %s' % name


def _body_uses(s, cfile, function, word):
    body = s.body_of(s.tokens(cfile) or [], function)
    if body is None:
        raise Unanswered('no %s in %s' % (function, cfile))
    return word in _texts(body), '%s:%s' % (cfile, function)


def _opcode(name):
    def detect(s):
        macros = s.macros('Include/opcode.h')
        if macros is None:
            raise Unanswered('no Include/opcode.h')
        return name in macros, 'Include/opcode.h:' + name
    detect.__doc__ = 'the %s opcode exists' % name
    return detect


def pydict_newpresized(s):
    """_PyDict_NewPresized is exported, so compiled code can call it"""
    return _exported(s, '_PyDict_NewPresized')


def int_max_str_digits(s):
    """int <-> str conversion has a digit limit (backported to 3.7.14)"""
    return _macro(s, '_PY_LONG_DEFAULT_MAX_STR_DIGITS')


def vectorcall(s):
    """types have a vectorcall slot, tp_vectorcall_offset (PEP 590)"""
    return _field(s, '_typeobject', 'tp_vectorcall_offset')


def posonlyargs(s):
    """code objects count positional-only parameters (PEP 570)"""
    # from 3.11 the struct's body is a macro's (_PyCode_DEF), so look for
    # the member's declaration itself
    for path, toks in s.headers_with('co_posonlyargcount'):
        bodies = [toks] + [m.body for m in s.macros(path).values()]
        for body in bodies:
            for i in range(1, len(body) - 1):
                if (body[i].text == 'co_posonlyargcount'
                        and body[i - 1].text == 'int' and body[i + 1].text == ';'):
                    return True, path + ':co_posonlyargcount'
    return False, 'Include, every header: co_posonlyargcount'


def audit_hooks(s):
    """PySys_Audit is exported (audit hooks, PEP 578)"""
    return _exported(s, 'PySys_Audit')


def arg_check_positional(s):
    """Argument Clinic's wrappers check positional counts with
    _PyArg_CheckPositional"""
    path = s.declares('_PyArg_CheckPositional')
    return (bool(path), '%s:_PyArg_CheckPositional'
            % (path or 'Include, every header: '))


def tstate_errors(s):
    """the internal error functions take the thread state
    (_PyErr_SetObject(tstate, ...))"""
    path, decl = s.declaration('_PyErr_SetObject')
    if decl is None:
        return False, 'Include, every header: _PyErr_SetObject'
    # the parameters follow the name, which ends the declaration's tokens
    toks = s.tokens(path)
    for i, t in enumerate(toks[:-2]):
        if t.text == '_PyErr_SetObject' and toks[i + 1].text == '(':
            return toks[i + 2].text == 'PyThreadState', path + ':_PyErr_SetObject'
    return False, path + ':_PyErr_SetObject'


def dict_new_empty_keys(s):
    """PyDict_New shares one empty keys object (Py_EMPTY_KEYS)"""
    return _body_uses(s, 'Objects/dictobject.c', 'PyDict_New', 'Py_EMPTY_KEYS')


def float_asdouble_index(s):
    """PyFloat_AsDouble accepts objects with only __index__ (nb_index)"""
    return _body_uses(s, 'Objects/floatobject.c', 'PyFloat_AsDouble', 'nb_index')


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


def pycore_pystate(s):
    """the runtime state's internal header is internal/pycore_pystate.h"""
    rel = 'Include/internal/pycore_pystate.h'
    return os.path.isfile(os.path.join(s.src, rel)), rel


# NI_HAS_<name> -> detector; its docstring's first line is the header's
# comment
FEATURES = [
    ('PYDICT_NEWPRESIZED', pydict_newpresized),
    ('INT_MAX_STR_DIGITS', int_max_str_digits),
    ('VECTORCALL', vectorcall),
    ('POSONLYARGS', posonlyargs),
    ('AUDIT_HOOKS', audit_hooks),
    ('ARG_CHECK_POSITIONAL', arg_check_positional),
    ('TSTATE_ERRORS', tstate_errors),
    ('DICT_NEW_EMPTY_KEYS', dict_new_empty_keys),
    ('FLOAT_ASDOUBLE_INDEX', float_asdouble_index),
    ('MAP_ADD_VALUE_ON_TOP', map_add_value_on_top),
    ('PYCORE_PYSTATE_H', pycore_pystate),
] + [('OPCODE_' + op, _opcode(op)) for op in (
    'SETUP_LOOP', 'SETUP_EXCEPT', 'BREAK_LOOP', 'CONTINUE_LOOP',
    'BEGIN_FINALLY', 'CALL_FINALLY', 'POP_FINALLY', 'END_ASYNC_FOR',
    'ROT_FOUR')]


def describe(detector):
    return ' '.join(detector.__doc__.split())


def detect(src):
    """[(NI_HAS_name, value, what it detects, evidence)]; GenError if a
    detector can't answer."""
    s = Source(src)
    out = []
    for name, d in FEATURES:
        try:
            value, evidence = d(s)
        except Unanswered as e:
            raise cslots.GenError('feature NI_HAS_%s (tools/metac/features.py:'
                                  '%s): %s' % (name, d.__name__, e))
        out.append(('NI_HAS_' + name, int(value), describe(d), evidence))
    return out


def build(target):
    """(header text, report lines: name, value, '', evidence, detect's
    list)."""
    found = detect(target.src)
    lines = ['/* Generated by tools/metac/features.py from CPython %s: do not'
             ' edit. */' % target.version,
             '#ifndef NI_FEATURES_GEN_H', '#define NI_FEATURES_GEN_H', '']
    for name, value, what, _ in found:
        lines += ['/* %s */' % what.replace('*/', '* /'),
                  '#define %s %d' % (name, value)]
    lines += ['', '#endif /* NI_FEATURES_GEN_H */', '']
    report = sorted('\t'.join((name, str(value), '', evidence))
                    for name, value, _, evidence in found)
    return '\n'.join(lines), report, found


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    import cpython_src
    for src in argv:
        print('%s (%s)' % (src, cpython_src.source_version(src)))
        for name, value, _, evidence in detect(src):
            print('  %-30s %d  %s' % (name, value, evidence))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
