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
