"""Ni's calling conventions (doc/GENERATOR_DESIGN.md "Return kinds",
"Rules"): what a meta returns, how a residual call's result is described
to Ni (ni/cf_flags.h), and Ni's own metas of CPython functions, read from
Ni's headers.

Return kinds, by what the meta returns and how it says it failed:
  obj   a vinfo of an object, NULL
  val   a vinfo of a C value, NULL
  ok    a bool (CPython's 0 or -1), false
  ct    an int known when compiling, -1; never after a run-time call

Error conventions, how a C function's result shows its error, which the
translator infers from the C's own test of it:
  null     NULL                    notimpl  NULL, and NotImplemented
  neg      negative                minus1   -1 with an exception set
  zero_ok  0 is success            never    it cannot fail
"""
import collections
import os
import sys

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import ctokens  # noqa: E402

KINDS = {'obj': ('vinfo_t *', 'NULL'), 'val': ('vinfo_t *', 'NULL'),
         'ok': ('bool', 'false'), 'ct': ('int', '-1')}

# the Cf return type of a C result type (named as the source names it, or
# a typedef it stands for)
RET_FLAG = {'int': 'CfReturnTypeInt', 'long': 'CfReturnTypeLong',
            'Py_ssize_t': 'CfReturnTypeNative', 'double': 'CfReturnTypeDouble'}
# Ni's names for the common combinations (cf_flags.h)
COMMON = {
    'CfReturnTypePtr|CfNewRef|CfPyErrIfNull': 'CfCommonNewRefPyObject',
    'CfReturnTypePtr|CfNewRef|CfPyErrNotImplemented': 'CfCommonCheckNotImplemented',
    'CfReturnTypeNative|CfNotNegative|CfPyErrCheckNeg': 'CfCommonPySSizeTResult',
    'CfReturnTypeInt|CfNotNegative|CfPyErrCheckNeg': 'CfCommonIntBoolOrError',
    'CfNoReturnValue|CfReturnTypeInt|CfPyErrIfNonNull': 'CfCommonIntZeroOk',
}
CONVENTIONS = ('null', 'notimpl', 'neg', 'minus1', 'zero_ok', 'never')


class NoConvention(Exception):
    pass


def ret_flag(unit, t):
    """The Cf return type of C type t, through typedefs; None if Ni has
    none for it."""
    if unit.is_object(t) or unit.pointee(t) is not None:
        return 'CfReturnTypePtr'
    from .ctyping import text
    seen = 0
    while seen < 20:
        name = text(t).replace('const ', '').strip()
        if name in RET_FLAG:
            return RET_FLAG[name]
        nt = unit.typedefs.get(name)
        if nt is None:
            return None
        t, seen = nt, seen + 1
    return None


def flags(unit, t, conv, effect_flags=()):
    """The Cf flags of a run-time call returning C type t whose errors
    show as conv says, with the effect flags effects.py proves."""
    if conv not in CONVENTIONS:
        raise NoConvention('no error convention %r' % conv)
    if conv == 'zero_ok':
        parts = ['CfNoReturnValue', 'CfReturnTypeInt', 'CfPyErrIfNonNull']
    else:
        r = ret_flag(unit, t)
        if r is None:
            from .ctyping import text
            raise NoConvention('a call returning %s' % text(t))
        if conv == 'never':
            if r == 'CfReturnTypePtr':
                raise NoConvention('a call returning a pointer that cannot fail')
            parts = [r]
        else:
            parts = [r]
            if r == 'CfReturnTypePtr':
                parts.append('CfNewRef')
            if r == 'CfReturnTypeDouble' and conv in ('neg', 'minus1'):
                # -1.0 and an exception: the word is no integer
                parts.append('CfPyErrCheck')
            elif conv == 'neg':
                parts += ['CfNotNegative', 'CfPyErrCheckNeg']
            elif conv == 'minus1':
                parts.append('CfPyErrCheckMinus1')
            elif conv == 'notimpl':
                parts.append('CfPyErrNotImplemented')
            else:
                parts.append('CfPyErrIfNull')
    out = COMMON.get('|'.join(parts), ' | '.join(parts))
    return out + ''.join(' | ' + x for x in effect_flags)


def slot_format(refs):
    """How Ni calls the meta of the slot a function fills (refs: its
    [(slot reference, slot)]): policy_table.SLOT_ARGS' format, 'v' a vinfo
    and 'l' a C value known when compiling; None for vinfos only."""
    import policy_table
    for _, slot in refs or ():
        spec = policy_table.SLOT_ARGS.get(slot)
        if spec is not None:
            return spec['format']
    return None


# A METH_FASTCALL builtin's C array and count are the one tuple Ni passes
# (METH_ARGS 'array', 'count'). These calls check the count: each
# succeeds exactly when min <= count <= max, the first also setting its
# outputs (the '...', one per item) to the array's items. By parameter
# position; where the declaration names its parameters, the names must
# be these. 3.8's _PyArg_CheckPositional is a macro testing the bounds
# before it calls the function, so only its bounds are read.
ARGUMENT_CHECKS = {
    '_PyArg_UnpackStack': {'array': (0, 'args'), 'count': (1, 'nargs'), 'min': (3, 'min'),
                           'max': (4, 'max'), 'outputs': 5},
    '_PyArg_CheckPositional': {'array': None, 'count': (1, 'nargs'), 'min': (2, 'min'),
                               'max': (3, 'max'), 'outputs': None},
}

def shortcut(name, kind):
    """The notes of policy_table's ('shortcut', kind, note) items on the
    CPython function `name`, whatever file defines it."""
    import policy_table
    return [i[2] if len(i) > 2 else '' for key, e in policy_table.ENTRIES.items()
            if key.endswith(':' + name) for i in e.get('policy', ())
            if tuple(i[:2]) == ('shortcut', kind)]


def zero_or_one(name):
    """The int result of `name` is 0 or 1 when it is no error (-1 with an
    exception set; 'zero_or_one'): a meta's vinfo of it, never NULL but
    for the error, is 0 or 1."""
    return bool(shortcut(name, 'zero_or_one'))


def bool_word(name):
    """Ni's meta of `name` for an argument known to be 0 or 1 (the note of
    its 'bool_word' item), or None."""
    notes = shortcut(name, 'bool_word')
    return notes[0] if notes else None


def null_args(name):
    """The parameters of Ni's meta of CPython's function `name` that take
    a NULL vinfo for the C's NULL (policy_table's ('ref', 'null_arg', P)
    items, P named as Ni's declaration names it)."""
    import policy_table
    return {i[2] for key, e in policy_table.ENTRIES.items() if key.endswith(':' + name)
            for i in e.get('policy', ()) if tuple(i[:2]) == ('ref', 'null_arg')}


def resume_conv(kind, notimpl):
    """How a run-time copy's result is described, by the meta's kind."""
    if kind == 'obj':
        return 'notimpl' if notimpl else 'null'
    return {'val': 'neg', 'ok': 'zero_ok'}.get(kind)


def psyco_name(name):
    """Ni's name for the meta of CPython's function: PyX -> PsycoX,
    _PyX -> _PsycoX; None for other names."""
    if name.startswith('Py'):
        return 'Psyco' + name[2:]
    if name.startswith('_Py'):
        return '_Psyco' + name[3:]
    return None


NiDecl = collections.namedtuple('NiDecl', 'name ret params')


def ni_decls(root, headers):
    """{name: NiDecl} of the Psyco* functions Ni's headers declare; params
    are (type text, name) pairs, the PsycoObject first. A tokeniser, not
    a C parser: Ni's headers are Ni's."""
    out = {}
    for h in headers:
        path = os.path.join(root, h)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            toks = ctokens.tokenize(f.read(), h)[0]
        start = 0
        for i, t in enumerate(toks):
            if t.text in (';', '}', '{'):
                start = i + 1
                continue
            if t.kind == 'id' and t.text.lstrip('_').startswith('Psyco') \
                    and i + 1 < len(toks) and toks[i + 1].text == '(':
                close = ctokens.match_close(toks, i + 1)
                if close + 1 < len(toks) and toks[close + 1].text == ';':
                    ret = [x.text for x in toks[start:i] if x.text not in (
                        'EXTERNFN', 'PSY_INLINE', 'static', 'extern', 'DEFINEFN')]
                    out.setdefault(t.text, NiDecl(t.text, _type(ret),
                                                  _params(toks[i + 2:close])))
    return out


def _type(words):
    stars = words.count('*')
    base = [w for w in words if w != '*']
    return ' '.join(base) + (' ' + '*' * stars if stars else '')


def _params(toks):
    out = []
    for k, p in enumerate(ctokens.split_top(toks, ',')):
        words = [t.text for t in p]
        if words == ['void']:
            continue
        ids = [t.text for t in p if t.kind == 'id']
        name = ids[-1] if len(ids) > 1 else '_arg%d' % k
        out.append((_type([w for w in words if w != name]), name))
    return out
