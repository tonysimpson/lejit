"""What gen_ceval.py and gen_virtual.py share of the first translator's
reading of C and Ni's conventions: a C file's macros, declarations and
functions as tokens (Source), the Cf flags of a run-time call, the
condition of a run-time integer test, the loops rule, and the target's
defines. gen2 has its own of each (gen2/front.py, gen2/conventions.py).
"""
import os
import subprocess

import cparse
import csource
import ctokens
import policy_table

# the C names that are vocabulary, never expanded as macros
KEEP = {'Py_INCREF', 'Py_DECREF', 'Py_XINCREF', 'Py_XDECREF', 'assert',
        'NULL', 'Py_None', 'Py_NotImplemented', 'Py_True', 'Py_False',
        'Py_RETURN_NOTIMPLEMENTED', 'Py_RETURN_NONE', 'Py_RETURN_TRUE',
        'Py_RETURN_FALSE', 'offsetof',
        '_Py_IDENTIFIER', 'PyErr_Occurred', 'Py_SAFE_DOWNCAST'}
RET_FLAG = {'PyObject *': 'CfReturnTypePtr', 'int': 'CfReturnTypeInt',
            'long': 'CfReturnTypeLong', 'Py_ssize_t': 'CfReturnTypeNative',
            'double': 'CfReturnTypeDouble'}
COMMON = {
    'CfReturnTypePtr|CfNewRef|CfPyErrIfNull': 'CfCommonNewRefPyObject',
    'CfReturnTypePtr|CfNewRef|CfPyErrNotImplemented': 'CfCommonCheckNotImplemented',
    'CfReturnTypeNative|CfNotNegative|CfPyErrCheckNeg': 'CfCommonPySSizeTResult',
    'CfReturnTypeInt|CfNotNegative|CfPyErrCheckNeg': 'CfCommonIntBoolOrError',
    'CfNoReturnValue|CfReturnTypeInt|CfPyErrIfNonNull': 'CfCommonIntZeroOk',
}
PY_OPS = {'<': 'Py_LT', '<=': 'Py_LE', '==': 'Py_EQ', '!=': 'Py_NE',
          '>': 'Py_GT', '>=': 'Py_GE'}


class NoFlags(Exception):
    """A run-time call whose result has no Cf description."""


# ---- reading the source ------------------------------------------------------

class Func:
    def __init__(self, name, ret, params, toks, static):
        self.name, self.ret, self.params = name, ret, params
        self.toks = toks          # body tokens, the file's own macros expanded
        self.static = static


def ctype_of(toks):
    """A declaration's type, normalised: 'PyObject *', 'const int'."""
    words = [t.text for t in toks if t.text not in ('static', 'inline', 'register')]
    stars = words.count('*')
    base = [w for w in words if w != '*']
    return ' '.join(base) + (' ' + '*' * stars if stars else '')


def split_params(toks):
    out = []
    for k, p in enumerate(ctokens.split_top(toks, ',')):
        if [t.text for t in p] == ['void']:
            continue
        ids = [t.text for t in p if t.kind == 'id']
        if not ids:         # a declaration with no parameter name
            out.append((ctype_of(p), '_arg%d' % k))
            continue
        w = [t.text for t in p]
        if '(' in w and w[w.index('(') + 1:w.index('(') + 3] == ['*', ids[-1]] or \
                '(' in w and len(ids) > 1 and w[w.index('(') + 1:w.index('(') + 2] == ['*']:
            out.append(fnptr_param(p))
            continue
        name = ids[-1]
        out.append((ctype_of([t for t in p if t.text != name]), name))
    return out


def fnptr_param(p):
    """'RET (*name)(ARGS)': ('RET (*)(ARGS)', name), the prototype kept."""
    w = [t.text for t in p]
    j = w.index('(')
    close = j + w[j:].index(')')
    name = w[close - 1]
    ret = ctype_of(p[:j])
    args = ' '.join(w[close + 2:-1])
    return ('%s (*)(%s)' % (ret, args), name)


class Source:
    """A CPython file as the target's build reads it: its macros (the
    headers' and its own), declarations, and functions' bodies as tokens,
    the file's macros expanded."""

    def __init__(self, src, pyconfig, defines, cfile):
        self.src = src
        self.pyconfig, self.defines = pyconfig, defines
        self.cfile = cfile
        self.macros, pptoks = cparse.cpp(src, pyconfig, cfile, defines)
        self.decls = cparse.Decls(pptoks)
        # the file's #if branches as the target's build takes them (rule
        # preprocess_conditionals): the rest of the text is empty lines
        text, undefs = cparse.conditionals(src, pyconfig, cfile, defines, self.read_text())
        # macros the file #undefs: in force for the functions before
        self.undefined = {}
        for line, name, macro in undefs:
            if name not in self.macros and name not in self.undefined and macro is not None:
                self.undefined[name] = (line, macro)
        toks, self.local_macros = ctokens.tokenize(text, cfile)
        keep_local = KEEP | {n for n, m in self.local_macros.items() if m.params is None}
        self.funcs = {}
        for name, d in csource.scan(toks, {}, cfile).items():
            if d.kind == 'function':
                self.add_func(name, d, keep_local)
        # functions a file macro defines: BINARY_FUNC(PyNumber_Or, ...)
        i = 0
        depth = 0
        while i < len(toks):
            t = toks[i]
            if t.text == '{':
                depth += 1
            elif t.text == '}':
                depth -= 1
            elif (depth == 0 and t.kind == 'id' and t.text in self.local_macros
                    and self.local_macros[t.text].params
                    and any(b.text == '{' for b in self.local_macros[t.text].body)
                    and i + 1 < len(toks) and toks[i + 1].text == '('):
                close = ctokens.match_close(toks, i + 1)
                exp = cparse.expand(toks[i:close + 1], self.local_macros, keep_local)
                for name, d in csource.scan(exp, {}, self.cfile).items():
                    if d.kind == 'function':
                        self.add_func(name, d, keep_local)
                i = close + 1
                continue
            i += 1

    def read_text(self):
        with open(os.path.join(self.src, self.cfile), encoding='latin-1') as f:
            return f.read()

    def add_func(self, name, d, keep_local):
        static = 'static' in [t.text for t in d.ret]
        lines = (d.body[0].line, d.body[-1].line) if d.body else (0, 0)
        in_force = dict(self.local_macros)
        in_force.update((n, m) for n, (ln, m) in self.undefined.items() if lines[0] < ln)
        # a header's statement macro ('do { ... } while (0)') is read as
        # the statements it stands for (D84), and one defined empty
        # stands for nothing
        for n, m in self.macros.items():
            w = [t.text for t in m.body]
            if m.params is not None and n not in KEEP and (
                    not m.body or (w[:1] == ['do'] and w[-4:] == ['while', '(', '0', ')'])):
                in_force[n] = m
        # an object-like macro the function defines inside its body is
        # an alias of its own, expanded like a function-like one
        keep = keep_local - {n for n, m in self.local_macros.items()
                             if m.params is None and lines[0] <= m.line <= lines[1]}
        body = cparse.expand(d.body, in_force, keep)
        self.funcs[name] = Func(name, ctype_of(d.ret), split_params(d.params), body, static)


# ---- Ni's conventions ----------------------------------------------------------

def flags(ctype, conv, effects=None, key=None):
    """Cf flags of a run-time call returning ctype, whose errors show as
    conv says ('null', 'notimpl', 'neg' and non-negative otherwise,
    'minus1', 'zero_ok', 'never'), with the effect flags effects.py proves
    for the function `key`."""
    if conv == 'zero_ok':
        parts = ['CfNoReturnValue', 'CfReturnTypeInt', 'CfPyErrIfNonNull']
    else:
        r = RET_FLAG.get(ctype.replace('const ', ''))
        if r is None or (conv == 'never' and r == 'CfReturnTypePtr'):
            raise NoFlags('a call returning %s' % ctype)
        parts = [r]
        if conv != 'never' and r == 'CfReturnTypePtr':
            parts.append('CfNewRef')
        if conv == 'never':
            pass
        elif r == 'CfReturnTypeDouble' and conv in ('neg', 'minus1'):
            # -1.0 and an exception (D87): the word is no integer
            parts.append('CfPyErrCheck')
        elif conv == 'neg':
            parts += ['CfNotNegative', 'CfPyErrCheckNeg']
        elif conv == 'minus1':
            parts.append('CfPyErrCheckMinus1')
        elif conv == 'notimpl':
            parts.append('CfPyErrNotImplemented')
        elif conv == 'null':
            parts.append('CfPyErrIfNull')
        else:
            raise NoFlags('no error convention for a call returning %s' % ctype)
    out = COMMON.get('|'.join(parts), ' | '.join(parts))
    return out + ''.join(' | ' + x for x in effect_flags(effects, key))


def effect_flags(effects, key):
    """CfPure, CfNoPython, CfNoBlock as effects.py proves them."""
    return effects.cf_flags(key) if effects is not None else []


def psyco_name(name):
    """Ni's name for the meta of a CPython function: PyX -> PsycoX,
    _PyX -> _PsycoX; None for other names."""
    if name.startswith('Py'):
        return 'Psyco' + name[2:]
    if name.startswith('_Py'):
        return '_Psyco' + name[3:]
    return None


def ni_short(path_name):
    """'ni/Objects/x.h:Name' -> 'Name'."""
    return path_name.rpartition(':')[2]


def known_length_of(ctype):
    """policy_table.TYPES' 'known_length' of an object struct pointer."""
    struct = ctype.replace('const ', '').rstrip(' *').replace('struct ', '').strip()
    for key, t in policy_table.TYPES.items():
        if key.split(':')[1] == struct and 'known_length' in t:
            kl = t['known_length']
            if kl['size'] in t.get('immutable', ()) and kl['items'] in t.get('immutable', ()):
                return kl
    return None


def int_condition(op, a, b, b_ct, nonneg=False):
    """P05: the call giving the condition code of 'a op b', a a run-time
    integer (a vinfo_t * expression), b a compile-time C value (b_ct) or
    another run-time integer, and whether the code must be inverted. A
    test of equality with 0 is the nonzero test (integer_non_null, as for
    a NULL test); with a known non-negative, '> 0' is '!= 0' and '<= 0'
    is '== 0'."""
    if b_ct and b == '0':
        if nonneg and op in ('>', '<='):
            op = '!=' if op == '>' else '=='
        if op in ('!=', '=='):
            return 'integer_non_null(po, %s)' % a, op == '=='
    if b_ct:
        return 'integer_cmp_i(po, %s, %s, %s)' % (a, b, PY_OPS[op]), False
    return 'integer_cmp(po, %s, %s, %s)' % (a, b, PY_OPS[op]), False


# ---- the loops rule (cparse trees) ------------------------------------------------

def assigned_in(node, out=None):
    """Names a statement or expression assigns (=, op=, ++, --, a
    declaration)."""
    if out is None:
        out = set()
    if isinstance(node, tuple) and node:
        if node[0] == 'assign' and node[2][0] == 'id':
            out.add(node[2][1])
        elif node[0] in ('pre', 'post') and node[2][0] == 'id':
            out.add(node[2][1])
        elif node[0] == 'decl':
            out.update(n for _, n, _, _ in node[1])
        for x in node[1:]:
            if isinstance(x, (tuple, list)):
                assigned_in(x, out)
    elif isinstance(node, list):
        for x in node:
            assigned_in(x, out)
    return out


def counted_loop(s):
    """(variable, bound, start) when the cparse loop s (parsed with
    loops=True) has a bound fixed by the C: a 'for' whose condition has a
    conjunct 'V < E', 'V <= E' or 'V != E' with V stepped up by one in the
    loop's head, or 'V > E', 'V >= E' or 'V != E' with V stepped down by
    one, V assigned nowhere else in the loop and E assigned nowhere in it.
    Any other loop runs as long as run-time values say: None. start is
    what the loop's head assigns V, or None."""
    if s[0] != 'for' or s[2] is None:
        return None
    steps = {}
    for x in s[3]:
        if x[0] in ('pre', 'post') and x[1] in ('++', '--') and x[2][0] == 'id':
            steps[x[2][1]] = x[1]
        elif x[0] == 'assign' and x[1] in ('+=', '-=') and x[2][0] == 'id' \
                and x[3] == ('num', '1'):
            steps[x[2][1]] = x[1][0] * 2
    body_assigned = assigned_in(s[4])
    conj = []

    def chain(e):
        if e[0] == 'bin' and e[1] == '&&':
            chain(e[2])
            chain(e[3])
        else:
            conj.append(e)
    chain(s[2])
    ops = {'++': ('<', '<=', '!='), '--': ('>', '>=', '!=')}
    for c in conj:
        if c[0] == 'bin' and c[2][0] == 'id' and c[2][1] in steps \
                and c[1] in ops[steps[c[2][1]]] \
                and c[2][1] not in body_assigned \
                and not set(cparse.names_in(c[3])) & (body_assigned | set(steps)):
            return c[2][1], c[3], loop_start(s[1], c[2][1])
    return None


def loop_start(init, name):
    """The expression a for loop's head (cparse init) assigns name."""
    if init is None:
        return None
    if init[0] == 'expr':
        e = init[1]
        for x in (e[1] if e[0] == 'comma' else [e]):
            if x[0] == 'assign' and x[1] == '=' and x[2] == ('id', name):
                return x[3]
    elif init[0] == 'decl':
        for d in init[1]:
            if d[1] == name:
                return d[2]
    return None


def loop_rule(s, bound_known):
    """The loops rule (D78, policy_table.RULES['loops']) for the cparse
    loop s. bound_known(e) says whether the C expression e is known at
    compile time. ('unroll', variable, bound, start) when the bound and
    the start (if the head sets one) are known: the meta runs the loop
    and emits its body once per turn; else ('residual', why): the loop is
    CPython's."""
    c = counted_loop(s)
    if c is None:
        return ('residual', 'a loop with a run-time bound')
    for e in (c[1], c[2]):
        if e is not None and not bound_known(e):
            return ('residual', '%s is not known at compile time' % cparse.c_expr(e))
    return ('unroll',) + c


def target_defines(python):
    """-D options the target's CPython was compiled with (sysconfig's
    CFLAGS, e.g. NDEBUG), and Py_BUILD_CORE, as the core's files are."""
    code = 'import sysconfig; print(sysconfig.get_config_var("CFLAGS") or "")'
    r = subprocess.run([python, '-I', '-c', code], stdout=subprocess.PIPE,
                       universal_newlines=True, timeout=60)
    out = ['Py_BUILD_CORE']
    for w in r.stdout.split():
        if w.startswith('-D'):
            out.append(w[2:])
    return out
