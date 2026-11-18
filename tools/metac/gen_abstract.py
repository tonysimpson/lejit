#!/usr/bin/env python3
"""General translation of CPython C functions into Psyco metas (GENERATION
step 4, D19 stage 2, DECISIONS D74). First on Objects/abstract.c; written
into pabstract_gen.h, which pabstract.c includes.

What is translated: every abstract.c function whose meta Ni declares in
pabstract.h ('Psyco' + the name without 'Py'), and the abstract.c functions
those call. A function the policy table gives items this translator does
not implement (a shortcut, a virtual result, a whitelist) is left to its
hand translation, and so is one whose entry says Ni's function of that name
is not a translation of it.

How (doc/METACOMPILER.md "General translation"): the body is read into
statements and expressions (cparse.py) and walked once per path, each
value given a binding time:
  - compile-time (a C value in the meta): types, their slot tables,
    function pointers read from them, string and integer constants, the
    scalar parameters every caller passes a constant for, and the results
    of tests on these;
  - run-time (a vinfo_t*): objects, and integers computed from them;
  - known now (the generator folds it): a parameter compared with NULL,
    which a meta never receives.
The rules, none of them about one function:
  - a type read X->ob_type is Psyco_NeedType(X) (P01), only for a meta's
    parameter (a temporary can't be promoted, F24). Types read before any
    call on every path, and types read after a call, are promoted at entry
    in the order the C reads them (rule 1: no promotion after a residual
    call);
  - a field of a type that is a function pointer is a slot: read through
    Psyco_TypeSlot (rule slot_guard, D54); where it changed, the meta
    resumes CPython's algorithm at that point: the whole function if
    nothing has been called yet (by its own name, or a run-time copy
    'cimpl_NAME' of a static one), else a run-time copy of the rest from
    the statement that read it, with the live variables as arguments.
    PyType_IsSubtype on compile-time types is Psyco_TypeIsSubtype, the
    same way;
  - a call through a slot is Psyco_META (P09), with flags from the slot's
    return type and the error test the C applies to the result (rule 2);
    a call to another translated function is a call of its meta; a call to
    a function whose meta Ni declares ('Psyco' + name) is a call of it; any
    other CPython function is a residual call (P12) with flags from its
    prototype, the error test and the effect rules (effects.py);
  - a branch on a run-time integer is a condition code (P05), with the
    likelihood the policy table gives ('likely'); the path where 'x < 0'
    failed asserts x non-negative (P06);
  - Py_INCREF and Py_DECREF are vinfo_incref and vinfo_decref (P17); every
    run-time value the meta owns is released after its last use and at
    every exit (liveness over the C);
  - PyErr_Format and PyErr_SetString raise at compile time (P24); a
    comparison with Py_NotImplemented is IS_IMPLEMENTED (P25);
  - a construct with no compile-time form (a field of an object, an array,
    an out-parameter) resumes CPython's algorithm at run time from its
    statement, as for a changed slot. Each is listed in the report.
Anything else stops that function (refused, with the reason).

Run by generate.py; 'python3 gen_abstract.py --src DIR --pyconfig H'
prints the header and the report.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cparse  # noqa: E402
import csource  # noqa: E402
import ctokens  # noqa: E402
import cslots  # noqa: E402
import cpython_src  # noqa: E402
import policy_table  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
CFILE = 'Objects/abstract.c'
NI_DECLS = 'ni/Objects/pabstract.h'
# Ni headers searched for the meta of a CPython function ('Psyco' + name)
NI_HEADERS = ['ni/Objects/pabstract.h', 'ni/Objects/pobject.h',
              'ni/Objects/plongobject.h', 'ni/Objects/piterobject.h',
              'ni/Objects/ptupleobject.h', 'ni/Objects/plistobject.h',
              'ni/Objects/pdictobject.h']
# policy items the translator implements; any other item on a function's
# entry leaves it to its hand translation
IMPLEMENTED = {'likely'}
# the C names that are vocabulary, never expanded as macros
KEEP = {'Py_INCREF', 'Py_DECREF', 'Py_XINCREF', 'Py_XDECREF', 'assert',
        'NULL', 'Py_None', 'Py_NotImplemented', 'Py_True', 'Py_False',
        'Py_RETURN_NOTIMPLEMENTED', 'Py_RETURN_NONE', 'offsetof',
        '_Py_IDENTIFIER', 'PyErr_Occurred', 'Py_SAFE_DOWNCAST'}
# C types of objects (run-time values)
OBJ_TYPES = {'PyObject *'}
INT_TYPES = {'int', 'Py_ssize_t', 'long', 'const int', 'size_t'}
TABLE_TYPES = {'PyNumberMethods *', 'PySequenceMethods *',
               'PyMappingMethods *', 'PyAsyncMethods *', 'PyBufferProcs *'}
RET_FLAG = {'PyObject *': 'CfReturnTypePtr', 'int': 'CfReturnTypeInt',
            'long': 'CfReturnTypeLong', 'Py_ssize_t': 'CfReturnTypeNative'}
COMMON = {
    'CfReturnTypePtr|CfNewRef|CfPyErrIfNull': 'CfCommonNewRefPyObject',
    'CfReturnTypePtr|CfNewRef|CfPyErrNotImplemented': 'CfCommonCheckNotImplemented',
    'CfReturnTypeNative|CfNotNegative|CfPyErrCheckNeg': 'CfCommonPySSizeTResult',
    'CfReturnTypeInt|CfNotNegative|CfPyErrCheckNeg': 'CfCommonIntBoolOrError',
    'CfNoReturnValue|CfReturnTypeInt|CfPyErrIfNonNull': 'CfCommonIntZeroOk',
}
PY_OPS = {'<': 'Py_LT', '<=': 'Py_LE', '==': 'Py_EQ', '!=': 'Py_NE',
          '>': 'Py_GT', '>=': 'Py_GE'}
FLIP = {'<': '>', '>': '<', '<=': '>=', '>=': '<=', '==': '==', '!=': '!='}


class Refuse(Exception):
    """The function can't be generated."""


class Resume(Exception):
    """No compile-time form here: resume CPython's algorithm at run time."""


# ---- reading the source ------------------------------------------------------

class Func:
    def __init__(self, name, ret, params, toks, static):
        self.name, self.ret, self.params = name, ret, params
        self.toks = toks          # body tokens, the file's own macros expanded
        self.static = static
        self.ast = None
        self.why = None           # why it can't be read


def ctype_of(toks):
    """A declaration's type, normalised: 'PyObject *', 'const int'."""
    words = [t.text for t in toks if t.text not in ('static', 'inline', 'register')]
    stars = words.count('*')
    base = [w for w in words if w != '*']
    return ' '.join(base) + (' ' + '*' * stars if stars else '')


def split_params(toks):
    out = []
    for p in ctokens.split_top(toks, ','):
        if [t.text for t in p] == ['void']:
            continue
        name = [t.text for t in p if t.kind == 'id'][-1]
        out.append((ctype_of([t for t in p if t.text != name]), name))
    return out


class Source:
    def __init__(self, src, pyconfig, defines):
        self.src = src
        self.macros, pptoks = cparse.cpp(src, pyconfig, CFILE, defines)
        self.decls = cparse.Decls(pptoks)
        with open(os.path.join(src, CFILE), encoding='latin-1') as f:
            text = f.read()
        toks, self.local_macros = ctokens.tokenize(text, CFILE)
        pptoks_lines = ctokens.tokenize(text, CFILE, directives=True)[0]
        self.pp_lines = [t.line for t in pptoks_lines if t.kind == 'pp']
        keep_local = KEEP | {n for n, m in self.local_macros.items()
                             if m.params is None}
        self.funcs = {}
        defs = csource.scan(toks, {}, CFILE)
        for name, d in defs.items():
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
                exp = cparse.expand(toks[i:close + 1], self.local_macros,
                                    keep_local)
                for name, d in csource.scan(exp, {}, CFILE).items():
                    if d.kind == 'function':
                        self.add_func(name, d, keep_local)
                i = close + 1
                continue
            i += 1

    def add_func(self, name, d, keep_local):
        static = 'static' in [t.text for t in d.ret]
        ret = ctype_of(d.ret)
        params = split_params(d.params)
        lines = (d.body[0].line, d.body[-1].line) if d.body else (0, 0)
        body = cparse.expand(d.body, self.local_macros, keep_local)
        f = Func(name, ret, params, body, static)
        if any(lines[0] <= ln <= lines[1] for ln in self.pp_lines):
            f.why = 'a preprocessor conditional inside the body'
        else:
            f.ast = cparse.parse_body(body, self.decls.types)
        self.funcs[name] = f

    def expand_call(self, name, args):
        """A header macro call with these argument expressions, expanded
        and read again as an expression."""
        m = self.macros[name]
        argtoks = [ctokens.tokenize(cparse.c_expr(a))[0] for a in args]
        if len(argtoks) != len(m.params):
            raise Resume('macro %s with %d arguments' % (name, len(args)))
        env = dict(zip(m.params, argtoks))
        body = []
        for b in m.body:
            body.extend(env.get(b.text, [b]) if b.kind == 'id' else [b])
        body = cparse.expand(body, self.macros, KEEP | {name})
        try:
            return cparse.parse_expr(body, self.decls.types)
        except cparse.ParseError as e:
            raise Resume('macro %s: %s' % (name, e))

    def is_fnptr(self, ctype):
        return ctype.rstrip(' *') in self.decls.fnptr and not ctype.endswith('*') \
            or ctype.endswith('(*)()')

    def fn_ret(self, ctype):
        """Return type of a function-pointer type."""
        if ctype.endswith('(*)()'):
            return ctype[:-5].strip()
        return self.decls.fnptr[ctype.strip()]

    def field_type(self, struct_ctype, field):
        tag = struct_ctype.replace('*', '').strip()
        if tag == 'PyTypeObject':
            tag = '_typeobject'
        fields = self.decls.structs.get(tag) or self.decls.structs.get(
            struct_ctype.replace('*', '').replace('struct', '').strip())
        if not fields or field not in fields:
            return None
        return fields[field]


class NiDecl:
    def __init__(self, name, ret, params):
        self.name, self.ret, self.params = name, ret, params


def ni_decls(ni_root, headers):
    """{name: NiDecl} of the Psyco* functions Ni's headers declare."""
    out = {}
    for h in headers:
        path = os.path.join(ni_root, h)
        if not os.path.exists(path):
            continue
        with open(path) as f:
            toks = ctokens.tokenize(f.read(), h)[0]
        start = 0
        for i, t in enumerate(toks):
            if t.text in (';', '}', '{'):
                start = i + 1
                continue
            if (t.kind == 'id' and t.text.startswith('Psyco') and i + 1 < len(toks)
                    and toks[i + 1].text == '('):
                close = ctokens.match_close(toks, i + 1)
                if close + 1 < len(toks) and toks[close + 1].text == ';':
                    ret = [x for x in toks[start:i] if x.text not in (
                        'EXTERNFN', 'PSY_INLINE', 'static', 'extern', 'DEFINEFN')]
                    params = split_params(toks[i + 2:close])
                    out.setdefault(t.text, NiDecl(t.text, ctype_of(ret), params))
    return out


# ---- liveness ------------------------------------------------------------------

def uses_defs(e):
    """(names read, name assigned) of an expression statement."""
    if e[0] == 'assign' and e[2][0] == 'id':
        reads = cparse.names_in(e[3])
        if e[1] != '=':
            reads = [e[2][1]] + reads
        return reads, e[2][1]
    return cparse.names_in(e), None


class Liveness:
    """live_after[id(stmt)]: the variables read after the statement before
    being assigned again, on some path; live_before likewise."""

    def __init__(self, ast):
        self.after, self.before = {}, {}
        self.block(ast, frozenset())

    def block(self, stmts, after):
        live = after
        for s in reversed(stmts):
            live = self.stmt(s, live)
        return live

    def stmt(self, s, after):
        k = s[0]
        self.after[id(s)] = after
        if k == 'block':
            live = self.block(s[1], after)
        elif k == 'decl':
            live = set(after)
            for ctype, name, init, dims in reversed(s[1]):
                live.discard(name)
                if init is not None:
                    live |= set(cparse.names_in(init))
            live = frozenset(live)
        elif k == 'expr':
            reads, d = uses_defs(s[1])
            live = (after - {d}) | set(reads)
        elif k == 'if':
            t = self.stmt(s[2], after)
            e = self.stmt(s[3], after) if s[3] is not None else after
            live = t | e | set(cparse.names_in(s[1]))
        elif k == 'return':
            live = frozenset(cparse.names_in(s[1])) if s[1] is not None else frozenset()
        elif k == 'label':
            live = self.stmt(s[2], after)
        else:   # goto, unsupported: everything may be read
            live = after | frozenset(cparse.names_in(list(s[1:-1])))
        self.before[id(s)] = live
        return live


# ---- the meta's code -----------------------------------------------------------

class Out:
    def __init__(self, depth=1):
        self.lines, self.depth = [], depth

    def __call__(self, line):
        self.lines.append('  ' * self.depth + line)

    def extend(self, other):
        self.lines.extend(other.lines)


class Var:
    """A C variable of the function as the meta holds it."""

    def __init__(self, name, ctype, bt, cname):
        self.name, self.ctype, self.bt, self.cname = name, ctype, bt, cname
        # rt: 'no' (borrowed), 'yes', 'shadow' (owned through own_NAME),
        # 'dead' (released)
        self.owned = 'no'
        self.conv = None          # rt call result: how errors and NotImplemented show
        self.tp = None            # ct table: the meta variable of its type
        self.isparam = False


class State:
    def __init__(self):
        self.vars = {}            # C name -> Var (bt and ownership on this path)
        self.known = set()        # object variables whose type is promoted
        self.called = False       # a run-time call may have been emitted
        self.slots = {}           # slot address text -> meta variable
        self.ended = False        # the path returned
        self.early = False        # a path may have returned before here

    def copy(self):
        s = State()
        s.vars = {}
        for k, v in self.vars.items():
            w = Var(v.name, v.ctype, v.bt, v.cname)
            w.owned, w.conv, w.tp, w.isparam = v.owned, v.conv, v.tp, v.isparam
            s.vars[k] = w
        s.known, s.called = set(self.known), self.called
        s.slots, s.ended, s.early = dict(self.slots), self.ended, self.early
        return s


class Val:
    def __init__(self, bt, c, ctype, tp=None, lv=False, owned=False, conv=None,
                 var=None, const=None):
        self.bt, self.c, self.ctype = bt, c, ctype
        self.tp = tp              # ct: meta variable of the type it points into
        self.lv = lv              # ct: an address inside a type (a slot's)
        self.owned = owned        # rt: a new value the statement must release
        self.conv = conv
        self.var = var            # rt: the variable it is read from
        self.const = const        # known now: True/False


class Summary:
    """What a caller needs to know about a translated function."""

    def __init__(self):
        self.kind = None          # obj | val | ok | ct
        self.entry = []           # parameters promoted at entry, in order
        self.needs = set()        # parameters whose type it may read
        self.may_call = False
        self.always_error = False
        self.notimpl = False      # may return Py_NotImplemented
        self.pbt = []             # per parameter: 'rt' or 'ct'
        self.meta = None          # its meta's name


class Translator:
    def __init__(self, source, ni, policy, effects):
        self.s = source
        self.ni = ni              # {name: NiDecl}
        self.policy = policy      # {function: [items]}
        self.effects = effects    # effects.Graph, or None
        self.sums = {}
        self.metas = {}           # name -> meta text
        self.copies = {}          # copy name -> C text
        self.copy_order = []
        self.report = []
        self.resumes = []         # (function, statement text, reason)
        self.fails = {}

    # ---- which functions, in what order ----

    def roots(self):
        out = []
        for name, f in sorted(self.s.funcs.items()):
            meta = 'Psyco' + name[2:] if name.startswith('Py') else None
            if meta and meta in self.ni and self.ni[meta].name in self.declared:
                out.append(name)
        return out

    def callees(self, f):
        out = []
        for n in cparse.names_in(f.ast or []):
            if n in self.s.funcs and n != f.name and n not in out:
                out.append(n)
        return out

    def run(self, declared):
        self.declared = declared
        order, seen = [], set()

        def visit(n, stack):
            if n in seen:
                return
            if n in stack:
                self.fails[n] = 'recursive'
                return
            f = self.s.funcs[n]
            for c in self.callees(f):
                visit(c, stack | {n})
            seen.add(n)
            order.append(n)
        for r in self.roots():
            visit(r, frozenset())
        self.param_binding(order)
        for n in order:
            self.translate(n)
        return order

    def policy_items(self, name):
        return self.policy.get('%s:%s' % (CFILE, name), [])

    # ---- binding time of parameters ----

    def param_binding(self, order):
        """A static function's scalar parameter is compile-time when every
        call in the file passes a compile-time expression; objects are
        run-time; an exported function follows Ni's declaration."""
        ct_names = {}
        for n in order:
            f = self.s.funcs[n]
            meta = self.meta_name(f)
            decl = self.ni.get(meta)
            pbt = []
            for k, (ctype, pname) in enumerate(f.params):
                if ctype in OBJ_TYPES:
                    pbt.append('rt')
                elif decl is not None and k + 1 < len(decl.params):
                    pbt.append('rt' if decl.params[k + 1][0] == 'vinfo_t *' else 'ct')
                elif not f.static:
                    pbt.append('rt' if ctype in INT_TYPES else 'ct')
                else:
                    pbt.append(None)
            ct_names[n] = pbt
        changed = True
        while changed:
            changed = False
            for n in order:
                f = self.s.funcs[n]
                for k, b in enumerate(ct_names[n]):
                    if b is not None:
                        continue
                    sites = list(self.call_sites(n))
                    if sites and all(self.static_ct(caller, a, ct_names)
                                     for caller, a in ((c, args[k]) for c, args in sites)
                                     if a is not None):
                        ct_names[n][k] = 'ct'
                        changed = True
        for n in order:
            self.sums.setdefault(n, Summary()).pbt = [b or 'rt' for b in ct_names[n]]

    def call_sites(self, name):
        for cname, f in self.s.funcs.items():
            if f.ast is None:
                continue
            for call in self.find_calls(f.ast, name):
                yield cname, call[2]

    def find_calls(self, node, name, out=None):
        if out is None:
            out = []
        if isinstance(node, tuple):
            if node and node[0] == 'call' and node[1] == ('id', name):
                out.append(node)
            for x in node[1:]:
                self.find_calls(x, name, out)
        elif isinstance(node, list):
            for x in node:
                self.find_calls(x, name, out)
        return out

    def static_ct(self, caller, e, ct_names):
        f = self.s.funcs[caller]
        local = self.local_types(f)
        for n in cparse.names_in(e):
            pidx = [k for k, (_, p) in enumerate(f.params) if p == n]
            if pidx:
                b = ct_names.get(caller, [None] * len(f.params))[pidx[0]]
                if b != 'ct':
                    return False
            elif n in local:
                if not self.ct_ctype(local[n]):
                    return False
            elif n in self.s.funcs:
                return False
        return True

    def ct_ctype(self, ctype):
        return (ctype in TABLE_TYPES or ctype == 'PyTypeObject *'
                or self.s.is_fnptr(ctype) or ctype in ('const char *', 'char *'))

    def local_types(self, f):
        out = {}

        def walk(n):
            if isinstance(n, tuple) and n and n[0] == 'decl':
                for ctype, name, init, dims in n[1]:
                    out[name] = ctype if not dims else ctype + ' []'
            if isinstance(n, (tuple, list)):
                for x in (n[1:] if isinstance(n, tuple) else n):
                    if isinstance(x, (tuple, list)):
                        walk(x)
        walk(f.ast)
        return out

    def meta_name(self, f):
        if f.name.startswith('Py') and not f.static:
            return 'Psyco' + f.name[2:]
        return f.name

    # ---- one function ----

    def translate(self, name):
        f = self.s.funcs[name]
        sm = self.sums[name]
        sm.meta = self.meta_name(f)
        if name in self.fails:
            return
        if f.ast is None:
            self.fails[name] = f.why
            return
        items = self.policy_items(name)
        other = [i for i in items if i[0] not in IMPLEMENTED]
        if other:
            self.fails[name] = 'policy items left to the hand translation: %s' % (
                ', '.join('%s %s' % (i[0], i[1]) for i in other))
            return
        try:
            sm.kind = self.kind_of(f)
            self.live = Liveness(f.ast)
            hoist = []
            for _ in range(4):
                w = Walk(self, f, sm, hoist)
                w.run()
                new = [p for p in w.must + w.late if p not in hoist]
                if not new:
                    break
                hoist = hoist + [p for i, p in enumerate(new) if p not in new[:i]]
            else:
                raise Refuse('promotions after calls remain: %s' % w.late)
            if [p for p in w.late if p not in hoist]:
                raise Refuse('promotions after calls remain')
            sm.entry = hoist
            sm.needs = w.needs
            sm.may_call = w.any_call
            sm.notimpl = w.notimpl
            sm.always_error = w.always_error
            self.metas[name] = w.text()
            self.resumes.extend(w.resumed)
        except Refuse as e:
            self.fails[name] = str(e)

    def kind_of(self, f):
        if f.ret in OBJ_TYPES:
            return 'obj'
        if f.ret not in INT_TYPES:
            raise Refuse('returns %s' % f.ret)
        decl = self.ni.get(self.meta_name(f))
        if decl is not None:
            if decl.ret == 'bool':
                return 'ok'
            if decl.ret == 'vinfo_t *':
                return 'val'
            raise Refuse('Ni declares %s returning %s' % (decl.name, decl.ret))
        if f.static:
            return 'val'
        raise Refuse('an int result needs its convention: declare %s in %s'
                     % (self.meta_name(f), NI_DECLS))

    # ---- residual calls' flags ----

    def flags(self, ctype, conv, callee_key=None):
        """Cf flags of a run-time call returning ctype, whose errors show
        as conv says: 'null', 'notimpl', 'neg' (and the value is
        non-negative otherwise), 'minus1', 'zero_ok'."""
        if conv == 'zero_ok':
            parts = ['CfNoReturnValue', 'CfReturnTypeInt', 'CfPyErrIfNonNull']
        else:
            r = RET_FLAG.get(ctype.replace('const ', ''))
            if r is None:
                raise Refuse('a call returning %s' % ctype)
            parts = [r]
            if r == 'CfReturnTypePtr':
                parts.append('CfNewRef')
            if conv == 'neg':
                parts += ['CfNotNegative', 'CfPyErrCheckNeg']
            elif conv == 'minus1':
                parts.append('CfPyErrCheckMinus1')
            elif conv == 'notimpl':
                parts.append('CfPyErrNotImplemented')
            elif conv == 'null':
                parts.append('CfPyErrIfNull')
            else:
                raise Refuse('no error convention for a call returning %s' % ctype)
        text = '|'.join(parts)
        out = COMMON.get(text, ' | '.join(parts))
        extra = self.effect_flags(callee_key)
        return out + ''.join(' | ' + x for x in extra)

    def effect_flags(self, key):
        """CfPure, CfNoPython, CfNoBlock as the effect rules prove them (rule
        effects); a function that may run Python may block."""
        g = self.effects
        if g is None or key is None or key not in g.nodes:
            return []
        if key in g.python:
            return []
        out = []
        if key not in g.impure and key not in g.raises:
            out.append('CfPure')
        else:
            out.append('CfNoPython')
        if key not in g.blocks:
            out.append('CfNoBlock')
        return out

    def effects_key(self, name):
        g = self.effects
        if g is None:
            return None
        return g.by_file.get(CFILE, {}).get(name) or g.globals.get(name)

    # ---- run-time copies ----

    def copy_name(self, f, k):
        return 'cimpl_%s' % f.name if k == 0 else 'cimpl_%s_%d' % (f.name, k)

    def copy(self, f, frames, live, owned=frozenset()):
        """A run-time C copy of f from the statement frames[-1] names to
        the end: (its name, its parameters). frames: [(stmts, index)] from
        the function's top level inwards."""
        whole = len(frames) == 1 and frames[0][1] == 0
        key = (f.name, tuple((id(s), i) for s, i in frames), tuple(sorted(owned)))
        for name, (k, params) in self.copy_keys.items():
            if k == key:
                return name, params
        n = sum(1 for nm in self.copy_keys if nm.startswith('cimpl_%s_' % f.name))
        name = self.copy_name(f, 0 if whole else n + 1)
        locs = self.local_types(f)
        ptypes = dict((p, t) for t, p in f.params)
        ptypes.update(locs)
        if whole:
            params = list(f.params)
        else:
            params = [(ptypes[v], v) for v in self.ordered_live(f, live)]
        for t, p in params:
            if t.endswith('[]'):
                raise Refuse('copy: array %s is live' % p)
        # the text: the rest of each block, innermost first, each in braces
        parts = []
        declared_inside = set()
        stmts_used = []
        for stmts, i in reversed(frames):
            rest = stmts[i:] if not parts else stmts[i + 1:]
            stmts_used.extend(rest)
            text = ' '.join(self.stmt_text(f, s) for s in rest)
            parts.append(text)
        body = ''
        for p in parts:
            body = ('{ %s } ' % body if body else '') + p
        for s in stmts_used:
            for d in self.find_decls(s):
                declared_inside.add(d)
        used = set(cparse.names_in(stmts_used))
        pnames = {p for _, p in params}
        decls = ['%s %s;' % (locs[v], v) if not locs[v].endswith('[]') else None
                 for v in sorted(used) if v in locs and v not in pnames
                 and v not in declared_inside]
        if None in decls:
            raise Refuse('copy: an array declared outside it')
        body = self.rename_statics(body)
        body = ''.join('Py_INCREF(%s); ' % v for v in sorted(owned)) + body
        ret = f.ret
        text = 'static %s %s(%s) {\n  %s\n  %s\n}\n' % (
            ret, name, ', '.join('%s%s%s' % (t, '' if t.endswith('*') else ' ', p)
                                 for t, p in params) or 'void',
            ' '.join(decls), body)
        self.copy_keys[name] = (key, params)
        self.copies[name] = text
        self.copy_order.append(name)
        return name, params

    def ordered_live(self, f, live):
        order = [p for _, p in f.params] + sorted(self.local_types(f))
        return [v for v in order if v in live]

    def find_decls(self, node, out=None):
        if out is None:
            out = []
        if isinstance(node, tuple) and node and node[0] == 'decl':
            out.extend(n for _, n, _, _ in node[1])
        if isinstance(node, (tuple, list)):
            for x in (node[1:] if isinstance(node, tuple) else node):
                if isinstance(x, (tuple, list)):
                    self.find_decls(x, out)
        return out

    def stmt_text(self, f, s):
        lo, hi = s[-1]
        return cslots.text(f.toks[lo:hi])

    def rename_statics(self, text):
        toks = ctokens.tokenize(text)[0]
        out = []
        for i, t in enumerate(toks):
            if (t.kind == 'id' and t.text in self.s.funcs and self.s.funcs[t.text].static
                    and i + 1 < len(toks) and toks[i + 1].text == '('):
                g = self.s.funcs[t.text]
                if g.ast is None:
                    raise Refuse('copy calls %s: %s' % (g.name, g.why))
                self.copy(g, [(g.ast, 0)], set())
                t = t._replace(text='cimpl_' + t.text)
            out.append(t)
        return cslots.text(out)


class Walk:
    """One translation of a function: the meta's text, and what it found
    (promotions it had to do after a call, the parameters whose type it
    reads before any call on every path)."""

    def __init__(self, tr, f, sm, hoist):
        self.tr, self.f, self.sm, self.hoist = tr, f, sm, hoist
        self.s = tr.s
        self.decls = {}           # meta variable -> C declaration
        self.tmp = 0
        self.must, self.late = [], []
        self.needs = set()
        self.any_call = False
        self.ncalls = self.stmt_calls = 0
        self.sc_slots = None
        self.notimpl = False
        self.returns = []         # 'error' or 'value' per return
        self.resumed = []
        self.likely = {}
        for item in tr.policy_items(f.name):
            if item[0] == 'likely':
                self.likely[' '.join(item[2].split())] = item[1]
        self.used_likely = set()

    @property
    def always_error(self):
        return bool(self.returns) and all(r == 'error' for r in self.returns)

    def err(self):
        return {'obj': 'NULL', 'val': 'NULL', 'ok': 'false', 'ct': '-1'}[self.sm.kind]

    def new_tmp(self, ctype):
        self.tmp += 1
        n = '_t%d' % self.tmp
        self.decls[n] = '%s %s;' % (ctype, n) if not ctype.endswith('*') \
            else '%s%s;' % (ctype, n)
        return n

    # ---- the walk ----

    def run(self):
        f = self.f
        st = State()
        self.params = []
        for k, (ctype, p) in enumerate(f.params):
            bt = self.sm.pbt[k]
            if bt == 'rt':
                mtype = 'vinfo_t *'
            else:
                mtype = ctype if ctype != 'const int' else 'int'
            v = Var(p, ctype, bt, p)
            v.isparam = True
            st.vars[p] = v
            self.params.append((mtype, p))
        out = Out()
        self.out = out
        for p in self.hoist:
            self.promote(st, out, p, entry=True)
        self.block(f.ast, st, out, [])
        if not st.ended:
            raise Refuse('the end of the function is reachable without a return')
        if self.sm.kind == 'ct' and self.any_call:
            raise Refuse('a compile-time result after a run-time call')
        for name, how in self.likely.items():
            if name not in self.used_likely:
                raise Refuse("policy 'likely' %r matches no run-time branch" % name)

    def text(self):
        f, sm = self.f, self.sm
        ret = {'obj': 'vinfo_t *', 'val': 'vinfo_t *', 'ok': 'bool', 'ct': 'int'}[sm.kind]
        head = '%s%s %s(PsycoObject *po%s)' % (
            'static ' if f.static or sm.meta not in self.tr.declared else 'DEFINEFN ',
            ret, sm.meta, ''.join(', %s%s%s' % (t, '' if t.endswith('*') else ' ', p)
                                  for t, p in self.params))
        lines = ['/* %s:%s */' % (CFILE, f.name), head + ' {']
        lines += ['  ' + d for _, d in sorted(self.decls.items())]
        lines += self.out.lines
        lines.append('}')
        return '\n'.join(lines) + '\n'

    def block(self, stmts, st, out, frames):
        for i, s in enumerate(stmts):
            if st.ended:
                break
            self.statement(s, st, out, frames + [(stmts, i)])
            if not st.ended:
                self.release_dead(st, out, self.tr.live.after[id(s)])

    def release_dead(self, st, out, live):
        for v in list(st.vars.values()):
            if v.bt == 'rt' and v.owned in ('yes', 'shadow') and v.name not in live:
                self.release(v, out)

    def release(self, v, out):
        if v.owned == 'yes':
            out('vinfo_decref(%s, po);' % v.cname)
        elif v.owned == 'shadow':
            out('vinfo_xdecref(own_%s, po);' % v.cname)
        v.owned = 'dead' if not v.isparam else 'no'

    def release_all(self, st, out, keep=None):
        for v in st.vars.values():
            if v.bt == 'rt' and v.owned in ('yes', 'shadow') and v.name != keep:
                if v.owned == 'yes':
                    out('vinfo_decref(%s, po);' % v.cname)
                else:
                    out('vinfo_xdecref(own_%s, po);' % v.cname)

    def error_exit(self, st, out, test):
        """if (test) { release; return error; }"""
        out('if (%s) {' % test)
        sub = Out(out.depth + 1)
        self.release_all(st, sub)
        sub('return %s;' % self.err())
        out.extend(sub)
        out('}')

    # ---- statements ----

    def statement(self, s, st, out, frames):
        k = s[0]
        mark = len(out.lines)
        saved = st.copy()
        saved_decls = dict(self.decls)
        saved_lists = [list(x) for x in (self.must, self.late, self.returns,
                                         self.resumed)]
        saved_notimpl = self.notimpl
        outer_calls = self.stmt_calls
        self.stmt_calls = self.ncalls
        try:
            self.stmt(s, st, out, frames)
        except Resume as e:
            if k in ('block', 'if') and not isinstance(e, ResumeHere):
                raise
            del out.lines[mark:]
            self.decls = saved_decls
            self.must, self.late, self.returns, self.resumed = saved_lists
            self.notimpl = saved_notimpl
            st.__dict__.update(saved.__dict__)
            self.stmt_calls = self.ncalls
            self.resume(st, out, frames, str(e))
        self.stmt_calls = outer_calls

    def resume_inside(self, st, out, frames, why):
        """Resume from the statement being translated, part of whose code
        is already emitted: only if it made no run-time call (the copy
        evaluates the statement again)."""
        if self.ncalls > self.stmt_calls:
            raise Refuse('resuming in a statement after its call: %s' % why)
        self.resume(st, out, frames, why)

    def resume(self, st, out, frames, why):
        """Resume CPython's algorithm at run time from the statement frames
        name: the whole function if nothing has run yet."""
        f, sm = self.f, self.sm
        stmt = frames[-1][0][frames[-1][1]]
        self.resumed.append((f.name, self.tr.stmt_text(f, stmt)[:70], why))
        if not st.called:
            if not f.static:
                name, params = f.name, f.params
            else:
                name, params = self.tr.copy(f, [(f.ast, 0)], set())
        else:
            live = self.tr.live.before[id(stmt)]
            # the copy takes its own reference to what the C owns here; the
            # meta keeps and releases its own
            owned = set()
            for vname in live:
                v = st.vars.get(vname)
                if v is not None and v.bt == 'rt' and v.owned in ('yes', 'shadow') \
                        and v.ctype in OBJ_TYPES:
                    if v.owned == 'shadow':
                        raise Refuse('resuming with %s owned on some paths' % vname)
                    owned.add(vname)
            name, params = self.tr.copy(f, frames, live, owned)
        fmt, args = '', []
        for ctype, p in params:
            v = st.vars.get(p)
            if v is None or v.owned == 'dead':
                raise Refuse('resuming needs %s, which has no value here' % p)
            if v.bt == 'rt':
                fmt += 'v'
                args.append(v.cname)
            else:
                fmt += 'l'
                args.append('(long)(%s)' % v.cname)
        conv = {'obj': 'notimpl' if self.notimpl_fn() else 'null',
                'val': 'neg', 'ok': 'zero_ok'}.get(sm.kind)
        if conv is None:
            raise Refuse('resuming in a compile-time function')
        flags = self.tr.flags(f.ret, conv, self.tr.effects_key(f.name))
        call = 'psyco_generic_call(po, %s, %s, "%s"%s)' % (
            name, flags, fmt, ''.join(', ' + a for a in args))
        st.called = True
        self.any_call = True
        self.ret_value(st, out, call, sm.kind == 'ok')

    def notimpl_fn(self):
        return self.tr_notimpl(self.f)

    def tr_notimpl(self, f):
        found = []

        def walk(n):
            if isinstance(n, tuple) and n:
                if n[0] == 'expr' and n[1] == ('id', 'Py_RETURN_NOTIMPLEMENTED'):
                    found.append(1)
                if n[0] == 'return' and n[1] is not None:
                    e = n[1]
                    if e == ('id', 'Py_NotImplemented'):
                        found.append(1)
                    if e[0] == 'call' and e[1][0] == 'id' and e[1][1] in self.tr.sums \
                            and self.tr.sums[e[1][1]].notimpl:
                        found.append(1)
                for x in n[1:]:
                    if isinstance(x, (tuple, list)):
                        walk(x)
            elif isinstance(n, list):
                for x in n:
                    walk(x)
        walk(f.ast)
        return bool(found)

    def ret_value(self, st, out, cexpr, as_ok=False):
        """return a run-time value the meta owns, after releasing the rest"""
        expr = '%s != NULL' % cexpr if as_ok else cexpr
        owned = [v for v in st.vars.values() if v.bt == 'rt' and v.owned in ('yes', 'shadow')]
        if owned:
            r = self.new_tmp('bool' if self.sm.kind == 'ok' else 'vinfo_t *')
            out('%s = %s;' % (r, expr))
            self.release_all(st, out)
            out('return %s;' % r)
        else:
            out('return %s;' % expr)
        st.ended = True
        self.returns.append('value')

    def stmt(self, s, st, out, frames):
        k = s[0]
        if k == 'block':
            self.block(s[1], st, out, frames)    # its statements are a frame
            return
        if k == 'empty':
            return
        if k == 'unsupported':
            raise ResumeHere(s[1])
        if k in ('goto', 'label'):
            raise Refuse('goto')
        if k == 'decl':
            for ctype, name, init, dims in s[1]:
                if dims:
                    raise ResumeHere('an array %s' % name)
                self.declare(st, name, ctype)
                if init is not None:
                    self.assign(st, out, name, init, frames)
            return
        if k == 'expr':
            self.expr_stmt(s[1], st, out, frames)
            return
        if k == 'return':
            self.ret(s[1], st, out, frames)
            return
        if k == 'if':
            self.if_stmt(s, st, out, frames)
            return
        raise Refuse('statement %s' % k)

    def declare(self, st, name, ctype):
        if name in st.vars and not st.vars[name].isparam:
            old = st.vars[name]
            if old.ctype != ctype:
                raise Refuse('%s declared twice with different types' % name)
        if ctype in OBJ_TYPES:
            bt = 'rt'
        elif self.tr.ct_ctype(ctype) or ctype in ('const char *',):
            bt = 'ct'
        elif ctype in INT_TYPES:
            bt = None             # by its first assignment
        else:
            raise ResumeHere('a local of type %s' % ctype)
        v = Var(name, ctype, bt, name)
        v.owned = 'dead' if bt == 'rt' else 'no'
        st.vars[name] = v
        if bt == 'ct':
            self.decls[name] = '%s%s%s;' % (ctype, '' if ctype.endswith('*') else ' ', name)
        elif bt == 'rt':
            self.decls[name] = 'vinfo_t *%s;' % name

    # ---- expression statements ----

    def expr_stmt(self, e, st, out, frames):
        if e == ('id', 'Py_RETURN_NOTIMPLEMENTED'):
            if self.sm.kind != 'obj':
                raise Refuse('NotImplemented from a non-object function')
            self.notimpl = True
            self.release_all(st, out)
            out('return psyco_vi_NotImplemented();')
            st.ended = True
            self.returns.append('value')
            return
        if e[0] == 'assign':
            if e[2][0] != 'id':
                raise ResumeHere('a store into %s' % cparse.c_expr(e[2]))
            name = e[2][1]
            if e[1] == '=':
                self.assign(st, out, name, e[3], frames)
            elif e[1] in ('+=', '-='):
                v = st.vars.get(name)
                if v is None or v.bt != 'rt':
                    raise ResumeHere('%s on %s' % (e[1], name))
                self.assign(st, out, name, ('bin', e[1][0], e[2], e[3]), frames)
            else:
                raise ResumeHere('operator %s' % e[1])
            return
        if e[0] == 'call' and e[1][0] == 'id':
            fn = e[1][1]
            if fn == 'assert':
                return
            if fn in ('Py_INCREF', 'Py_XINCREF'):
                v = self.var_of(st, e[2][0])
                if v.owned != 'no':
                    raise Refuse('an incref of %s, which the meta owns' % v.name)
                out('vinfo_incref(%s);' % v.cname)
                v.owned = 'yes'
                return
            if fn in ('Py_DECREF', 'Py_XDECREF'):
                v = self.var_of(st, e[2][0])
                if v.owned not in ('yes', 'shadow'):
                    raise Refuse('a decref of %s, which the meta does not own' % v.name)
                self.release(v, out)
                v.owned = 'dead'
                return
        val = self.ev(e, st, out, frames, dest=None)
        if val.bt == 'rt' and val.owned:
            if not val.conv or val.conv.get('always_error'):
                return
            out('vinfo_xdecref(%s, po);' % val.c)

    def var_of(self, st, e):
        while e[0] == 'cast':
            e = e[2]
        if e[0] != 'id' or e[1] not in st.vars or st.vars[e[1]].bt != 'rt':
            raise Refuse('refcount of %s' % cparse.c_expr(e))
        return st.vars[e[1]]

    def assign(self, st, out, name, e, frames):
        v = st.vars.get(name)
        if v is None:
            raise Refuse('assignment to %s' % name)
        val = self.ev(e, st, out, frames, dest=name)
        if v.bt is None:
            v.bt = val.bt if val.bt != 'g' else 'ct'
            if v.bt == 'rt':
                v.owned = 'dead'
                self.decls[name] = 'vinfo_t *%s;' % name
            else:
                self.decls[name] = '%s %s;' % (v.ctype.replace('const ', ''), name)
        if val.bt == 'g':
            val = Val('ct', '1' if val.const else '0', 'int')
        if v.bt == 'ct':
            if val.bt != 'ct':
                raise ResumeHere('%s is compile-time, assigned a run-time value' % name)
            out('%s = %s;' % (v.cname, val.c))
            v.tp = val.tp
            return
        if val.bt != 'rt':
            if val.c in ('NULL', '((void *)0)'):
                # no value: what C reads next it assigns first
                if v.owned in ('yes', 'shadow'):
                    self.release(v, out)
                v.owned = 'dead'
                return
            raise ResumeHere('%s assigned a compile-time value' % name)
        if val.var is v:
            return
        old_owned = v.owned
        if old_owned in ('yes', 'shadow'):
            # the new value may read the old one: release after
            tmp = self.new_tmp('vinfo_t *')
            out('%s = %s;' % (tmp, val.c))
            self.release(v, out)
            out('%s = %s;' % (v.cname, tmp))
        else:
            out('%s = %s;' % (v.cname, val.c))
        if val.owned:
            v.owned = 'yes'
        elif val.var is not None:
            if val.var.owned in ('yes', 'shadow'):
                raise Refuse('%s aliases an owned value' % name)
            v.owned = 'no'
        else:
            v.owned = 'no'
        v.conv = val.conv
        if val.conv and val.conv.get('check'):
            out_check = val.conv.get('check')
            if out_check == 'now':
                owned, v.owned = v.owned, 'no'      # NULL: nothing to release
                self.error_exit(st, out, '%s == NULL' % v.cname)
                v.owned = owned
                v.conv = dict(val.conv, check=None)

    # ---- return ----

    def ret(self, e, st, out, frames):
        kind = self.sm.kind
        if e is None:
            raise Refuse('return without a value')
        val = self.ev(e, st, out, frames, dest='<return>')
        if val.bt == 'g':
            raise Refuse('returns a constant known now')
        if kind in ('obj', 'val'):
            if val.bt == 'ct':
                if val.c in ('NULL', '((void *)0)') or (kind == 'val' and val.c == '-1'):
                    self.release_all(st, out)
                    out('return NULL;')
                    st.ended = True
                    self.returns.append('error')
                    return
                if kind == 'val':
                    self.ret_value_owned(st, out, Val(
                        'rt', 'vinfo_new(CompileTime_New((long)(%s)))' % val.c,
                        'long', owned=True))
                    return
                raise ResumeHere('returns the constant %s' % val.c)
            if val.var is not None and val.var.owned == 'no':
                raise Refuse('returns %s, which it does not own' % val.var.name)
            if val.conv and val.conv.get('always_error'):
                self.release_all(st, out, keep=val.var.name if val.var else None)
                out('return %s;' % val.c)
                st.ended = True
                self.returns.append('error')
                return
            if val.conv and val.conv.get('notimpl'):
                self.notimpl = True
            self.ret_value_owned(st, out, val)
            return
        if kind == 'ok':
            if val.bt == 'ct':
                if val.c == '-1':
                    self.release_all(st, out)
                    out('return false;')
                    st.ended = True
                    self.returns.append('error')
                    return
                if val.c == '0':
                    self.release_all(st, out)
                    out('return true;')
                    st.ended = True
                    self.returns.append('value')
                    return
                raise Refuse('returns %s from a 0/-1 function' % val.c)
            if val.bt == 'okv':
                self.ret_value(st, out, val.c)
                return
            self.ret_value(st, out, val.c, as_ok=True)
            return
        raise Refuse('kind %s' % kind)

    def ret_value_owned(self, st, out, val):
        keep = val.var.name if val.var is not None else None
        owned = [v for v in st.vars.values() if v.bt == 'rt'
                 and v.owned in ('yes', 'shadow') and v.name != keep]
        if val.var is not None and val.var.owned == 'shadow':
            raise Refuse('returns a value owned on some paths only')
        if owned:
            if val.var is None:
                r = self.new_tmp('vinfo_t *')
                out('%s = %s;' % (r, val.c))
            else:
                r = val.c
            self.release_all(st, out, keep=keep)
            out('return %s;' % r)
        else:
            out('return %s;' % val.c)
        if val.var is not None:
            val.var.owned = 'dead'
        st.ended = True
        self.returns.append('value')

    # ---- if ----

    def if_stmt(self, s, st, out, frames):
        _, c, then, els, span = s
        facts_t, facts_f = [], []
        self.sc_slots = None
        cexpr = self.cond(c, st, out, frames, facts_t, facts_f)
        sc = self.sc_slots if c[0] == 'bin' and c[1] in ('&&', '||') else None
        if isinstance(cexpr, bool):
            branch = then if cexpr else els
            if branch is not None:
                self.branch(branch, st, out, frames)
            return
        st_t, st_f = st.copy(), st.copy()
        nt = self.null_test(c, st)
        if nt is not None:
            # where the error test held, the value is NULL: nothing to release
            name, when = nt
            x = (st_t if when else st_f).vars[name]
            x.owned = 'dead' if not x.isparam else 'no'
            (st_f if when else st_t).vars[name].conv = None
        it = self.impl_test(c, st)
        if it is not None:
            # where the test says implemented, the value isn't NotImplemented
            name, when = it
            x = (st_t if when else st_f).vars[name]
            if x.conv:
                x.conv = dict(x.conv, notimpl=False)
        if sc is not None and sc[0] == c[1]:
            (st_t if sc[0] == '&&' else st_f).slots.update(sc[1])
        out_t, out_f = Out(out.depth + 1), Out(out.depth + 1)
        for line in facts_t:
            out_t(line)
        for line in facts_f:
            out_f(line)
        self.release_dead(st_t, out_t, self.tr.live.before[id(then)])
        self.branch(then, st_t, out_t, frames)
        if els is not None:
            self.release_dead(st_f, out_f, self.tr.live.before[id(els)])
            self.branch(els, st_f, out_f, frames)
        else:
            self.release_dead(st_f, out_f, self.tr.live.after[id(s)])
        self.merge(st, st_t, st_f, out_t, out_f)
        out('if (%s) {' % cexpr)
        out.extend(out_t)
        if out_f.lines:
            out('}')
            out('else {')
            out.extend(out_f)
        out('}')

    def branch(self, s, st, out, frames):
        if s[0] == 'block':
            stmts = s[1]
        else:
            stmts = [s]
        self.block(stmts, st, out, frames)

    def merge(self, st, a, b, out_a, out_b):
        if a.ended and b.ended:
            st.__dict__.update(a.__dict__)
            st.ended = True
            st.early = True
            return
        if a.ended or b.ended:
            live = b if a.ended else a
            st.__dict__.update(live.__dict__)
            st.early = True
            return
        st.known = a.known & b.known
        st.called = a.called or b.called
        st.early = a.early or b.early
        st.slots = {k: v for k, v in a.slots.items() if b.slots.get(k) == v}
        for name in set(a.vars) | set(b.vars):
            va, vb = a.vars.get(name), b.vars.get(name)
            if va is None or vb is None:
                st.vars.pop(name, None)
                continue
            if va.bt != vb.bt:
                raise Refuse('%s has a different binding time on two paths' % name)
            v = va
            if va.bt == 'rt' and va.owned != vb.owned:
                states = {va.owned, vb.owned}
                if states <= {'no', 'yes', 'shadow'}:
                    self.decls['own_' + name] = 'vinfo_t *own_%s;' % name
                    for o, x in ((out_a, va), (out_b, vb)):
                        if x.owned == 'yes':
                            o('own_%s = %s;' % (name, name))
                        elif x.owned == 'no':
                            o('own_%s = NULL;' % name)
                    v.owned = 'shadow'
                elif 'dead' in states and states - {'dead'} <= {'no'}:
                    v.owned = 'dead' if 'dead' in states and not v.isparam else 'no'
                    if v.isparam:
                        v.owned = 'no'
                else:
                    raise Refuse('%s owned on one path, released on the other' % name)
            if va.bt == 'ct' and va.tp != vb.tp:
                v.tp = None
            if va.conv != vb.conv:
                v.conv = None
            st.vars[name] = v
        st.ended = False

    # ---- conditions: a compile-time C boolean, or a Python bool if known now ----

    def cond(self, e, st, out, frames, facts_t=None, facts_f=None):
        k = e[0]
        if k == 'un' and e[1] == '!' and self.is_obj_param(e[2], st):
            return False
        if k == 'un' and e[1] == '!' and self.checkable(e[2], st):
            return '%s == NULL' % self.checkable(e[2], st).cname
        if self.checkable(e, st):
            return '%s != NULL' % self.checkable(e, st).cname
        if k == 'bin' and e[1] == '&&' and e[3] == ('call', ('id', 'PyErr_Occurred'), []) \
                and e[2][0] == 'bin' and self.error_test(e[2], st) is not None:
            # 'x == -1 && PyErr_Occurred()': a meta's error is the NULL vinfo
            return self.error_test(e[2], st)
        if self.is_obj_param(e, st):
            return True
        if k == 'un' and e[1] == '!':
            r = self.cond(e[2], st, out, frames, facts_f, facts_t)
            return (not r) if isinstance(r, bool) else '!(%s)' % r
        if k == 'bin' and e[1] in ('&&', '||'):
            return self.short_circuit(e, st, out, frames)
        if k == 'bin' and e[1] in PY_OPS:
            r = self.compare(e, st, out, frames, facts_t, facts_f)
            if r is not None:
                return r
        if k == 'call' and e[1][0] == 'id' and e[1][1] in self.s.macros \
                and e[1][1] not in KEEP and self.s.macros[e[1][1]].params is not None:
            return self.cond(self.s.expand_call(e[1][1], e[2]), st, out, frames,
                             facts_t, facts_f)
        val = self.ev(e, st, out, frames)
        if val.bt == 'g':
            return bool(val.const)
        if val.bt == 'ct':
            return '%s != 0' % paren(val.c) if not val.c.startswith('(') else val.c
        raise ResumeHere('a test of the run-time value %s' % cparse.c_expr(e))

    def short_circuit(self, e, st, out, frames):
        op = e[1]
        lc = self.cond(e[2], st, out, frames)
        if isinstance(lc, bool):
            if op == '&&' and not lc:
                return False
            if op == '||' and lc:
                return True
            return self.cond(e[3], st, out, frames)
        sub_st = st.copy()
        left = self.sc_slots
        if left is not None and left[0] == op and e[2][0] == 'bin' and e[2][1] == op:
            sub_st.slots.update(left[1])
        sub = Out(out.depth + 1)
        try:
            rc = self.cond(e[3], sub_st, sub, frames)
        except ResumeHere as r:
            # the right operand runs only when the left allows: resume
            # there, on that path
            sub = Out(out.depth + 1)
            sub_st = st.copy()
            self.resume_inside(sub_st, sub, frames, str(r))
            rc = '0' if op == '&&' else '1'
        if isinstance(rc, bool):
            rc = '1' if rc else '0'
        # the slots read on the right are known where the whole test
        # holds (&&) or fails (||)
        self.sc_slots = (op, dict(sub_st.slots)) if not sub_st.ended else None
        if not sub.lines:
            self.merge_cond(st, sub_st)
            return '(%s %s %s)' % (lc, op, rc)
        t = self.new_tmp('int')
        out('%s = %s;' % (t, lc))
        out('if (%s%s) {' % ('' if op == '&&' else '!', t))
        out.extend(sub)
        if not sub_st.ended:
            out.lines.append('  ' * (out.depth + 1) + '%s = %s;' % (t, rc))
        out('}')
        self.merge_cond(st, sub_st)
        return t

    def merge_cond(self, st, sub):
        """State after 'if (c) { sub }': what sub learnt is not known after."""
        st.called = st.called or sub.called
        st.early = st.early or sub.ended
        st.slots = {k: v for k, v in st.slots.items() if sub.slots.get(k) == v}

    def compare(self, e, st, out, frames, facts_t, facts_f):
        op, a, b = e[1], e[2], e[3]
        # an error test of a call's result
        r = self.error_test(e, st)
        if r is not None:
            return r
        # Py_NotImplemented
        for x, y in ((a, b), (b, a)):
            if y == ('id', 'Py_NotImplemented') and op in ('==', '!='):
                v = self.ev(x, st, out, frames)
                if v.bt != 'rt' or not v.conv or not v.conv.get('notimpl'):
                    raise ResumeHere('a compare with NotImplemented of %s'
                                     % cparse.c_expr(x))
                return ('IS_IMPLEMENTED(%s)' if op == '!=' else '!IS_IMPLEMENTED(%s)') % v.c
        # a meta's object parameter is never NULL (known now)
        for x, y in ((a, b), (b, a)):
            if y == ('id', 'NULL') and op in ('==', '!=') and self.is_obj_param(x, st):
                return op == '!='
        va = self.ev(a, st, out, frames)
        vb = self.ev(b, st, out, frames)
        if va.bt == 'g' or vb.bt == 'g':
            if va.bt == 'g' and vb.bt == 'g':
                return (va.const == vb.const) == (op == '==')
            return None
        if va.bt == 'ct' and vb.bt == 'ct':
            return '%s %s %s' % (paren(va.c), op, paren(vb.c))
        # a run-time integer compared: a condition code (P05)
        if va.bt == 'ct':
            va, vb, op = vb, va, FLIP[op]
            a, b = b, a
        if va.ctype in OBJ_TYPES or vb.ctype in OBJ_TYPES:
            raise ResumeHere('a compare of the object %s' % cparse.c_expr(a))
        cc = self.new_tmp('condition_code_t')
        call, inverted = int_condition(op, va.c, vb.c, vb.bt == 'ct')
        out('%s = %s;' % (cc, call))
        self.error_exit(st, out, '%s == CC_ERROR' % cc)
        if inverted:
            out('%s = INVERT_CC(%s);' % (cc, cc))
        text = ' '.join(cparse.c_expr(e).replace('(', '').replace(')', '').split())
        how = self.likely.get(text)
        if how is None:
            raise Refuse("no 'likely' item in the policy table for the run-time"
                         " branch %r" % text)
        self.used_likely.add(text)
        t = self.new_tmp('int')
        out('%s = runtime_condition_%s(po, %s);' % (t, 't' if how == 'true' else 'f', cc))
        # P06: where x < 0 failed, x is not negative
        if vb.bt == 'ct' and vb.c == '0' and va.var is not None and facts_f is not None:
            if op == '<':
                facts_f.append('assert_nonneg(%s);' % va.c)
            elif op == '>=':
                facts_t.append('assert_nonneg(%s);' % va.c)
        return t

    def null_test(self, c, st):
        """(variable, True if NULL where c holds) when c is just the error
        test of a call's result."""
        neg = False
        while c[0] == 'un' and c[1] == '!':
            inner = c[2]
            if self.checkable(inner, st):
                return (self.checkable(inner, st).name, not neg)
            c, neg = inner, not neg
        if c[0] == 'bin' and c[1] == '&&' and c[3] == ('call', ('id', 'PyErr_Occurred'), []):
            c = c[2]
        if c[0] == 'bin' and self.error_test(c, st) is not None:
            v = self.checkable(c[2], st)
            return (v.name, neg == (c[1] == '!='))
        return None

    def impl_test(self, c, st):
        """(variable, True if implemented where c holds) when c compares a
        call's result with Py_NotImplemented."""
        neg = False
        while c[0] == 'un' and c[1] == '!':
            c, neg = c[2], not neg
        if c[0] == 'bin' and c[1] in ('==', '!=') and c[3] == ('id', 'Py_NotImplemented'):
            x = c[2]
            while x[0] == 'cast':
                x = x[2]
            if x[0] == 'id' and x[1] in st.vars:
                return (x[1], (c[1] == '!=') != neg)
        return None

    def checkable(self, x, st):
        while x[0] == 'cast':
            x = x[2]
        if x[0] == 'id' and x[1] in st.vars:
            v = st.vars[x[1]]
            if v.bt == 'rt' and v.conv and v.conv.get('checkable'):
                return v
        return None

    def is_obj_param(self, x, st):
        while x[0] == 'cast':
            x = x[2]
        if x[0] != 'id' or x[1] not in st.vars:
            return False
        v = st.vars[x[1]]
        return v.isparam and v.bt == 'rt' and v.ctype in OBJ_TYPES and v.owned != 'dead'

    def error_test(self, e, st):
        """The C's test of a call's error result: in the meta, the vinfo
        is NULL (a meta signals every error that way)."""
        op, a, b = e[1], e[2], e[3]
        if e[1] == '&&':
            return None

        def var(x):
            while x[0] == 'cast':
                x = x[2]
            if x[0] == 'id' and x[1] in st.vars:
                v = st.vars[x[1]]
                if v.bt == 'rt' and v.conv and v.conv.get('checkable'):
                    return v
            return None
        v = var(a)
        if v is None:
            return None
        if op in ('==', '!=') and b in (('id', 'NULL'),):
            return '%s %s NULL' % (v.cname, op)
        if op == '<' and b == ('num', '0'):
            return '%s == NULL' % v.cname
        if op == '==' and b == ('un', '-', ('num', '1')):
            return '%s == NULL' % v.cname
        return None

    # ---- expressions ----

    def ev(self, e, st, out, frames, dest=None):
        k = e[0]
        if k == 'num':
            return Val('ct', e[1], 'int')
        if k == 'str':
            return Val('ct', ' '.join(e[1]), 'const char *')
        if k == 'id':
            return self.ev_id(e[1], st, out)
        if k == 'cast':
            v = self.ev(e[2], st, out, frames, dest)
            t = e[1].replace(' *', ' *').strip()
            ctype = ctype_of(ctokens.tokenize(t)[0])
            if v.bt == 'ct':
                return Val('ct', '((%s)%s)' % (t, v.c), ctype, tp=v.tp, lv=False)
            if v.bt == 'rt':
                if ctype in OBJ_TYPES or ctype in INT_TYPES:
                    return v
                raise ResumeHere('a cast of a run-time value to %s' % t)
            return v
        if k == 'arrow':
            return self.ev_arrow(e, st, out, frames)
        if k == 'un':
            return self.ev_unary(e, st, out, frames)
        if k == 'index':
            raise ResumeHere('an index %s' % cparse.c_expr(e))
        if k == 'call':
            return self.ev_call(e, st, out, frames, dest)
        if k == 'bin':
            if e[1] in ('&&', '||') or e[1] in PY_OPS:
                c = self.cond(e, st, out, frames)
                if isinstance(c, bool):
                    return Val('g', None, 'int', const=c)
                return Val('ct', c, 'int')
            va = self.ev(e[2], st, out, frames)
            vb = self.ev(e[3], st, out, frames)
            if va.bt == 'ct' and vb.bt == 'ct':
                return Val('ct', '(%s %s %s)' % (va.c, e[1], vb.c), va.ctype)
            if e[1] == '+' and va.bt == 'rt' and vb.bt == 'rt':
                r = self.new_tmp('vinfo_t *')
                out('%s = integer_add(po, %s, %s, false);' % (r, va.c, vb.c))
                return Val('rt', r, va.ctype, owned=True,
                           conv={'check': 'now', 'checkable': False})
            raise ResumeHere('run-time arithmetic %s' % cparse.c_expr(e))
        if k == 'cond':
            c = self.cond(e[1], st, out, frames)
            va = self.ev(e[2], st, out, frames)
            vb = self.ev(e[3], st, out, frames)
            if isinstance(c, bool):
                return va if c else vb
            if va.bt == 'ct' and vb.bt == 'ct':
                return Val('ct', '(%s ? %s : %s)' % (c, va.c, vb.c), va.ctype)
            raise ResumeHere('a run-time choice %s' % cparse.c_expr(e))
        if k == 'sizeof':
            return Val('ct', e[1], 'size_t')
        raise ResumeHere('expression %s' % k)

    def ev_id(self, name, st, out):
        if name in st.vars:
            v = st.vars[name]
            if v.bt == 'rt':
                if v.owned == 'dead' and not v.isparam:
                    raise Refuse('%s read with no value' % name)
                return Val('rt', v.cname, v.ctype, var=v, conv=v.conv)
            if v.bt is None:
                raise Refuse('%s read before it is assigned' % name)
            return Val('ct', v.cname, v.ctype, tp=v.tp)
        if name == 'NULL':
            return Val('ct', 'NULL', 'void *')
        if name in ('Py_None', 'Py_NotImplemented', 'Py_True', 'Py_False'):
            raise ResumeHere('the object %s as a value' % name)
        if name in self.s.decls.vars or (name in self.s.macros
                                         and self.s.macros[name].params is None):
            return Val('ct', name, 'global')
        raise ResumeHere('the name %s' % name)

    def promote(self, st, out, name, entry=False):
        """The type of the object variable 'name' (P01): its meta
        variable."""
        v = st.vars.get(name)
        tpv = 'tp_' + name
        if name in st.known:
            return tpv
        if v is None or not v.isparam:
            raise ResumeHere("the type of %s, which is not a parameter (F24)" % name)
        pidx = [p for _, p in self.f.params].index(name)
        self.needs.add(pidx)
        if not entry:
            if st.called:
                if name not in self.late:
                    self.late.append(name)
            elif out.depth == 1 and not st.early and name not in self.must:
                self.must.append(name)
        self.decls[tpv] = 'PyTypeObject *%s;' % tpv
        out('%s = Psyco_NeedType(po, %s);' % (tpv, v.cname))
        self.error_exit(st, out, '%s == NULL' % tpv)
        st.known.add(name)
        return tpv

    def ev_arrow(self, e, st, out, frames):
        base, field = e[1], e[2]
        b = self.ev(base, st, out, frames)
        if b.bt == 'rt':
            if b.ctype in OBJ_TYPES and field == 'ob_type' and b.var is not None:
                tpv = self.promote(st, out, b.var.name)
                return Val('ct', tpv, 'PyTypeObject *', tp=tpv)
            raise ResumeHere('the field %s of a run-time value' % field)
        if b.bt != 'ct':
            raise ResumeHere('a field of %s' % cparse.c_expr(base))
        ftype = self.s.field_type(b.ctype, field)
        if b.tp is None or ftype is None:
            raise ResumeHere('the field %s of %s' % (field, cparse.c_expr(base)))
        if self.s.is_fnptr(ftype):
            return self.slot(st, out, frames, b.tp, '&%s->%s' % (paren(b.c), field), ftype)
        tp = b.tp if ftype in TABLE_TYPES else None
        return Val('ct', '%s->%s' % (paren(b.c), field), ftype, tp=tp)

    def ev_unary(self, e, st, out, frames):
        op, x = e[1], e[2]
        if op == '&':
            if x[0] == 'index':
                arr = self.ev(x[1], st, out, frames)
                idx = self.ev(x[2], st, out, frames)
                if arr.bt == 'ct' and idx.bt == 'ct' and arr.tp is not None:
                    return Val('ct', '&%s[%s]' % (paren(arr.c), idx.c),
                               arr.ctype, tp=arr.tp, lv=True)
            if x[0] == 'id' and x[1] in self.s.decls.vars:
                return Val('ct', '&' + x[1], 'global')
            raise ResumeHere('an address %s' % cparse.c_expr(e))
        if op == '*':
            # a load from a type: *(T *)ADDRESS
            p = x
            ctype = None
            if p[0] == 'cast':
                ctype = ctype_of(ctokens.tokenize(p[1])[0])
                p = p[2]
            a = self.ev(p, st, out, frames)
            if a.bt == 'ct' and a.lv and a.tp is not None and ctype and ctype.endswith('*'):
                ftype = ctype[:-1].strip()
                if self.s.is_fnptr(ftype):
                    return self.slot(st, out, frames, a.tp, '(void *)%s' % a.c, ftype)
            if a.bt == 'ct' and self.s.is_fnptr(a.ctype):
                return a          # (*f)(args): f
            raise ResumeHere('a load through %s' % cparse.c_expr(x))
        v = self.ev(x, st, out, frames)
        if v.bt == 'ct':
            return Val('ct', '%s%s' % (op, paren(v.c)), v.ctype)
        if v.bt == 'g' and op == '!':
            return Val('g', None, 'int', const=not v.const)
        raise ResumeHere('%s of a run-time value' % op)

    def slot(self, st, out, frames, tpv, addr, ftype):
        """A slot of a type (rule slot_guard, D54): read through
        Psyco_TypeSlot; where a heap type's has changed, resume."""
        if addr in st.slots:
            return Val('ct', st.slots[addr], ftype)
        n = self.new_tmp(ftype)
        sw = 'switch (Psyco_TypeSlot(po, %s, %s, (void **)&%s)) {' % (tpv, addr, n)
        out(sw)
        out('case 1:')
        out('  break;')
        out('case 0:')
        sub = Out(out.depth + 1)
        self.resume_inside(st.copy(), sub, frames, 'a slot of a heap type changed')
        out.extend(sub)
        out('default:')
        sub = Out(out.depth + 1)
        self.release_all(st, sub)
        sub('return %s;' % self.err())
        out.extend(sub)
        out('}')
        st.slots[addr] = n
        return Val('ct', n, ftype)

    # ---- calls ----

    def ev_call(self, e, st, out, frames, dest):
        fn, args = e[1], e[2]
        if fn[0] == 'id':
            name = fn[1]
            if name in st.vars:
                v = st.vars[name]
                if v.bt == 'ct' and self.s.is_fnptr(v.ctype):
                    return self.slot_call(Val('ct', v.cname, v.ctype), args, st, out,
                                          frames, dest)
                raise ResumeHere('a call through %s' % name)
            if name in ('PyErr_Format', 'PyErr_SetString'):
                return self.raise_(name, args, st, out, frames)
            if name == 'PyType_IsSubtype':
                return self.subtype(args, st, out, frames)
            if name == 'offsetof':
                return Val('ct', 'offsetof(%s)' % ', '.join(cparse.c_expr(a) for a in args),
                           'size_t')
            if name == 'Py_SAFE_DOWNCAST':
                return self.ev(args[0], st, out, frames, dest)
            if name in self.tr.sums and name not in self.tr.fails and name in self.tr.metas:
                return self.meta_call(name, args, st, out, frames, dest)
            if name in self.s.macros and self.s.macros[name].params is not None \
                    and name not in KEEP:
                return self.ev(self.s.expand_call(name, args), st, out, frames, dest)
            pname = 'Psyco' + name[2:] if name.startswith('Py') else None
            if pname in self.tr.ni and pname not in self.tr.declared_generated:
                return self.ni_call(self.tr.ni[pname], name, args, st, out, frames, dest)
            if name in self.s.funcs:
                # not translated: CPython's function at run time (a copy
                # of a static one)
                g = self.s.funcs[name]
                if g.static:
                    if g.ast is None:
                        raise Refuse('calls %s: %s' % (name, g.why))
                    cname = self.tr.copy(g, [(g.ast, 0)], set())[0]
                    return self.residual(name, args, st, out, frames, dest,
                                         cname=cname, rtype=g.ret)
                return self.residual(name, args, st, out, frames, dest)
            if name in policy_table.EXTERNAL and policy_table.EXTERNAL[name] == 'pure':
                vals = [self.ev(a, st, out, frames) for a in args]
                if all(v.bt == 'ct' for v in vals):
                    return Val('ct', '%s(%s)' % (name, ', '.join(v.c for v in vals)), 'int')
                raise ResumeHere('%s of a run-time value' % name)
            if name in self.s.decls.protos:
                return self.residual(name, args, st, out, frames, dest)
            raise ResumeHere('a call of %s' % name)
        callee = self.ev(fn, st, out, frames)
        if callee.bt == 'ct' and self.s.is_fnptr(callee.ctype):
            return self.slot_call(callee, args, st, out, frames, dest)
        raise ResumeHere('a call through %s' % cparse.c_expr(fn))

    def args_of(self, args, st, out, frames):
        vals = []
        for a in args:
            v = self.ev(a, st, out, frames)
            if v.bt == 'g':
                v = Val('ct', '1' if v.const else '0', 'int')
            if v.bt == 'rt' and v.owned:
                # a nested call's result: hold it for the statement
                raise ResumeHere('a call as an argument')
            vals.append(v)
        return vals

    def fmt(self, vals):
        f, a = '', []
        for v in vals:
            if v.bt == 'rt':
                f += 'v'
                a.append(v.c)
            else:
                f += 'l'
                a.append('(long)(%s)' % v.c if v.c != 'NULL' else '0')
        return f, a

    def convention(self, dest, frames, ctype):
        """How the C tests a call's result for an error: from the next
        statement that reads it, or from what the function returns."""
        if dest == '<return>':
            kind = self.sm.kind
            if kind == 'ok':
                return 'zero_ok', False
            if kind == 'val':
                return 'neg', False
            if ctype in OBJ_TYPES:
                return 'null', False
            raise Refuse('returns a call of type %s' % ctype)
        if dest is None:
            return ('null' if ctype in OBJ_TYPES else 'neg'), False
        nxt = self.next_use(dest, frames)
        if nxt is None:
            raise Refuse('the result in %s is never read' % dest)
        k = nxt[0]
        if k == 'expr' and nxt[1][0] == 'call' and nxt[1][1] == ('id', 'assert'):
            t = ' '.join(cparse.c_expr(nxt[1][2][0]).replace('(', ' ').replace(')', ' ').split())
            if t.startswith('%s >= 0 ||' % dest):
                return 'neg', True
        if k == 'if':
            c = nxt[1]
            txt = ' '.join(cparse.c_expr(c).replace('(', ' ').replace(')', ' ').split())
            if txt in ('%s < 0' % dest,):
                return 'neg', True
            if txt.startswith('%s == - 1 && PyErr_Occurred' % dest) or \
                    txt.startswith('%s == -1 && PyErr_Occurred' % dest):
                return 'minus1', True
            if txt in ('%s == NULL' % dest, '! %s' % dest, '%s != NULL' % dest) or \
                    txt.startswith('! %s ||' % dest) or txt.startswith('%s == NULL ||' % dest):
                return 'null', True
            if txt in ('%s != Py_NotImplemented' % dest, '%s == Py_NotImplemented' % dest):
                return 'notimpl', False
        if k == 'return' and nxt[1] is not None:
            r = nxt[1]
            while r[0] == 'cast' or (r[0] == 'call' and r[1] == ('id', 'Py_SAFE_DOWNCAST')):
                r = r[2] if r[0] == 'cast' else r[2][0]
            if r == ('id', dest):
                return self.convention('<return>', frames, ctype)
        raise Refuse('cannot tell how the C checks %s for an error (%s)'
                     % (dest, cparse.c_expr(nxt[1]) if k in ('expr', 'if', 'return')
                        and nxt[1] is not None else k))

    def next_use(self, name, frames):
        """The first statement after the current one that reads name."""
        for stmts, i in reversed(frames):
            for s in stmts[i + 1:]:
                if name in cparse.names_in(list(s[1:-1])):
                    if s[0] == 'expr' and s[1][0] == 'call' and \
                            s[1][1] == ('id', 'assert') and not ' '.join(
                                cparse.c_expr(s[1][2][0]).replace('(', ' ').replace(
                                    ')', ' ').split()).startswith(name + ' >= 0 ||'):
                        continue    # an assert that says nothing of errors
                    return s
        return None

    def note_call(self, st):
        self.ncalls += 1
        st.called = True
        st.slots = {}
        self.any_call = True

    def slot_call(self, callee, args, st, out, frames, dest):
        vals = self.args_of(args, st, out, frames)
        rtype = self.s.fn_ret(callee.ctype)
        conv, checkable = self.convention(dest, frames, rtype)
        flags = self.tr.flags(rtype, conv)
        f, a = self.fmt(vals)
        if len(vals) > 3:
            raise Refuse('a slot call with %d arguments' % len(vals))
        self.note_call(st)
        call = 'Psyco_META%d(po, %s, %s, "%s"%s)' % (
            len(vals), callee.c, flags, f, ''.join(', ' + x for x in a))
        if conv == 'zero_ok':
            return Val('rt', call, 'int', owned=False, conv={'zero_ok': True})
        r = self.new_tmp('vinfo_t *')
        out('%s = %s;' % (r, call))
        return Val('rt', r, rtype, owned=True,
                   conv={'notimpl': conv == 'notimpl', 'checkable': checkable})

    def meta_call(self, name, args, st, out, frames, dest):
        g = self.tr.sums[name]
        gf = self.s.funcs[name]
        vals = []
        for k, a in enumerate(args):
            v = self.ev(a, st, out, frames)
            if v.bt == 'g':
                v = Val('ct', '1' if v.const else '0', 'int')
            want = g.pbt[k]
            if want == 'ct' and v.bt != 'ct':
                raise Refuse('%s: argument %d is run-time' % (name, k + 1))
            if want == 'rt' and v.bt != 'rt':
                raise ResumeHere('%s: a compile-time argument %d' % (name, k + 1))
            if v.bt == 'rt' and v.owned:
                raise ResumeHere('a call as an argument')
            vals.append(v)
        # the callee reads these parameters' types (rule 1)
        for k in sorted(g.needs, key=lambda k: (g.entry.index(gf.params[k][1])
                                                if gf.params[k][1] in g.entry else 99, k)):
            v = vals[k]
            if v.var is not None and v.var.isparam and v.var.name not in st.known:
                if st.called:
                    if v.var.name not in self.late:
                        self.late.append(v.var.name)
                elif gf.params[k][1] in g.entry and out.depth == 1 and not st.early \
                        and v.var.name not in self.must:
                    self.must.append(v.var.name)
            elif v.var is not None and not v.var.isparam and v.ctype in OBJ_TYPES:
                raise Refuse('%s reads the type of %s, which is not a parameter (F24)'
                             % (name, v.var.name))
        for k, v in enumerate(vals):
            if k in g.needs and gf.params[k][1] in g.entry and v.var is not None:
                st.known.add(v.var.name)
        if g.may_call:
            self.note_call(st)
        call = '%s(po%s)' % (g.meta, ''.join(', ' + v.c for v in vals))
        if g.kind == 'ok':
            return Val('okv', call, 'int', conv={'zero_ok': True})
        if g.kind == 'ct':
            r = self.new_tmp('int')
            out('%s = %s;' % (r, call))
            self.error_exit(st, out, '%s == -1' % r)
            return Val('ct', r, 'int')
        if dest is None and g.always_error:
            out('%s;' % call)
            return Val('ct', 'NULL', 'void *')
        r = self.new_tmp('vinfo_t *')
        out('%s = %s;' % (r, call))
        return Val('rt', r, gf.ret, owned=True,
                   conv={'notimpl': g.notimpl, 'checkable': True,
                         'always_error': g.always_error})

    def ni_call(self, decl, name, args, st, out, frames, dest):
        if len(decl.params) != len(args) + 1:
            raise ResumeHere('%s has %d parameters' % (decl.name, len(decl.params)))
        vals = []
        for (ptype, _), a in zip(decl.params[1:], args):
            v = self.ev(a, st, out, frames)
            if ptype == 'vinfo_t *' and v.bt != 'rt':
                raise ResumeHere('%s: a compile-time argument' % decl.name)
            if ptype != 'vinfo_t *' and v.bt != 'ct':
                raise ResumeHere('%s: a run-time argument' % decl.name)
            if v.bt == 'rt' and v.owned:
                raise ResumeHere('a call as an argument')
            if v.bt == 'rt' and v.ctype in OBJ_TYPES:
                if v.var is None or not v.var.isparam:
                    raise ResumeHere('%s of a value that is not a parameter' % decl.name)
                if v.var.name not in st.known and st.called:
                    if v.var.name not in self.late:
                        self.late.append(v.var.name)
            vals.append(v)
        self.note_call(st)
        call = '%s(po%s)' % (decl.name, ''.join(', ' + v.c for v in vals))
        if decl.ret == 'bool':
            return Val('okv', call, 'int', conv={'zero_ok': True})
        if decl.ret != 'vinfo_t *':
            raise ResumeHere('%s returns %s' % (decl.name, decl.ret))
        r = self.new_tmp('vinfo_t *')
        out('%s = %s;' % (r, call))
        ctype = self.s.decls.protos.get(name, 'PyObject *')
        return Val('rt', r, ctype, owned=True,
                   conv={'checkable': True, 'notimpl': False})

    def residual(self, name, args, st, out, frames, dest, cname=None, rtype=None):
        vals = self.args_of(args, st, out, frames)
        if rtype is None:
            rtype = ctype_of(ctokens.tokenize(self.s.decls.protos[name])[0])
        conv, checkable = self.convention(dest, frames, rtype)
        flags = self.tr.flags(rtype, conv, self.tr.effects_key(name))
        f, a = self.fmt(vals)
        self.note_call(st)
        call = 'psyco_generic_call(po, %s, %s, "%s"%s)' % (
            cname or name, flags, f, ''.join(', ' + x for x in a))
        if conv == 'zero_ok':
            return Val('rt', call, 'int', conv={'zero_ok': True})
        r = self.new_tmp('vinfo_t *')
        out('%s = %s;' % (r, call))
        return Val('rt', r, rtype, owned=True,
                   conv={'notimpl': conv == 'notimpl', 'checkable': checkable})

    def raise_(self, name, args, st, out, frames):
        vals = [self.ev(a, st, out, frames) for a in args]
        if any(v.bt != 'ct' for v in vals):
            raise ResumeHere('%s with a run-time argument' % name)
        fn = 'PycException_SetFormat' if name == 'PyErr_Format' else 'PycException_SetString'
        out('%s(po, %s);' % (fn, ', '.join(v.c for v in vals)))
        return Val('ct', 'NULL', 'void *')

    def subtype(self, args, st, out, frames):
        a = self.ev(args[0], st, out, frames)
        b = self.ev(args[1], st, out, frames)
        if a.bt != 'ct' or b.bt != 'ct':
            raise ResumeHere('PyType_IsSubtype of run-time types')
        r = self.new_tmp('bool')
        out('switch (Psyco_TypeIsSubtype(po, %s, %s, &%s)) {' % (a.c, b.c, r))
        out('case 1:')
        out('  break;')
        out('case 0:')
        sub = Out(out.depth + 1)
        self.resume_inside(st.copy(), sub, frames, 'the mro of a heap type changed')
        out.extend(sub)
        out('default:')
        sub = Out(out.depth + 1)
        self.release_all(st, sub)
        sub('return %s;' % self.err())
        out.extend(sub)
        out('}')
        return Val('ct', r, 'int')


class ResumeHere(Resume):
    """Resume at the statement being translated."""


def int_condition(op, a, b, b_ct, nonneg=False):
    """P05: the call giving the condition code of 'a op b', a a run-time
    integer (a vinfo_t * expression), b a compile-time C value (b_ct) or
    another run-time integer, and whether the code must be inverted. A
    test of equality with 0 is the nonzero test (integer_non_null, as for
    a NULL test); with a known non-negative, '> 0' is '!= 0' and '<= 0'
    is '== 0'. Shared with gen_ceval.py."""
    if b_ct and b == '0':
        if nonneg and op in ('>', '<='):
            op = '!=' if op == '>' else '=='
        if op in ('!=', '=='):
            return 'integer_non_null(po, %s)' % a, op == '=='
    if b_ct:
        return 'integer_cmp_i(po, %s, %s, %s)' % (a, b, PY_OPS[op]), False
    return 'integer_cmp(po, %s, %s, %s)' % (a, b, PY_OPS[op]), False


def paren(c):
    c = c.strip()
    if all(ch.isalnum() or ch == '_' for ch in c):
        return c
    return '(%s)' % c


# ---- the header -----------------------------------------------------------------

def generate(src, pyconfig, defines, ni_root=ROOT, effects=None):
    source = Source(src, pyconfig, defines)
    ni = ni_decls(ni_root, NI_HEADERS)
    declared = set(ni_decls(ni_root, [NI_DECLS]))
    policy = {k: e.get('policy', []) for k, e in policy_table.ENTRIES.items()}
    tr = Translator(source, ni, policy, effects)
    tr.copy_keys = {}
    # functions whose entry says Ni's meta of that name is not a translation
    for k, items in policy.items():
        for item in items:
            if item[:2] == ('other', 'not_translation'):
                fname = k.split(':')[1]
                if fname in source.funcs:
                    tr.fails[fname] = 'Ni\'s %s is not a translation of it: %s' % (
                        'Psyco' + fname[2:], item[2] if len(item) > 2 else '')
    tr.declared_generated = set()
    tr.declared = declared
    order = []
    # translated metas take the place of Ni's declarations of them: the
    # first pass finds which, the second uses the translated ones
    for _ in range(2):
        tr.sums, tr.metas, tr.copies, tr.copy_order = {}, {}, {}, []
        tr.copy_keys = {}
        tr.resumes = []
        keep_fails = {k: v for k, v in tr.fails.items() if 'not a translation' in v}
        tr.fails = keep_fails
        order = tr.run(declared)
        tr.declared_generated = {tr.sums[n].meta for n in order if n in tr.metas}
    # keep what Ni's declarations reach: a static meta or run-time copy no
    # kept text names is left out (its callers were not generated)
    by_meta = {tr.sums[n].meta: n for n in order if n in tr.metas}
    texts = dict((m, tr.metas[n]) for m, n in by_meta.items())
    texts.update(tr.copies)
    used = {m for m in by_meta if m in declared}
    work = list(used)
    while work:
        names = set(t.text for t in ctokens.tokenize(texts[work.pop()])[0]
                    if t.kind == 'id')
        for m in names & set(texts):
            if m not in used:
                used.add(m)
                work.append(m)
    order = [n for n in order if n not in tr.metas or tr.sums[n].meta in used]
    tr.metas = {n: t for n, t in tr.metas.items() if tr.sums[n].meta in used}
    tr.copy_order = [c for c in tr.copy_order if c in used]
    tr.resumes = [r for r in tr.resumes if r[0] in tr.metas]
    head = ['/* GENERATED by tools/metac/gen_abstract.py from CPython %s\'s'
            % cpython_src.source_version(src),
            '   %s: do not edit. Included by pabstract.c. */' % CFILE, '']
    body = []
    if tr.copy_order:
        body.append('/* run-time copies: where a heap type\'s slot changed, or'
                    ' where the meta has no compile-time form, CPython\'s'
                    ' algorithm resumes in these */')
        for name in tr.copy_order:      # a copy follows the copies it calls
            body.append(tr.copies[name])
    statics = [n for n in order if n in tr.metas and
               (source.funcs[n].static or tr.sums[n].meta not in declared)]
    for n in statics:
        sm = tr.sums[n]
        ret = {'obj': 'vinfo_t *', 'val': 'vinfo_t *', 'ok': 'bool', 'ct': 'int'}[sm.kind]
        f = source.funcs[n]
        body.append('static %s %s(PsycoObject *po%s);' % (
            ret, sm.meta, ''.join(', %s' % ('vinfo_t *' if b == 'rt' else (
                t if t != 'const int' else 'int')) for (t, _), b in zip(f.params, sm.pbt))))
    body.append('')
    for n in order:
        if n in tr.metas:
            body.append(tr.metas[n])
    report = []
    for n in order:
        if n in tr.metas:
            report.append('generated  %-32s %s' % (n, tr.sums[n].meta))
    for n in sorted(tr.fails):
        report.append('skipped    %-32s %s' % (n, tr.fails[n]))
    for fname, stmt, why in tr.resumes:
        report.append('resumes    %-32s %s: %s' % (fname, why, stmt))
    text = '\n'.join(head + body) + '\n/* not generated:\n' + '\n'.join(
        '   ' + r for r in report if not r.startswith('generated')) + '\n*/\n'
    return text, report


def target_defines(python):
    """-D options the target's CPython was compiled with (sysconfig's
    CFLAGS, e.g. NDEBUG), and Py_BUILD_CORE, as abstract.c is."""
    code = 'import sysconfig; print(sysconfig.get_config_var("CFLAGS") or "")'
    r = subprocess.run([python, '-I', '-c', code], stdout=subprocess.PIPE,
                       universal_newlines=True, timeout=60)
    out = ['Py_BUILD_CORE']
    for w in r.stdout.split():
        if w.startswith('-D'):
            out.append(w[2:])
    return out


def build(target):
    try:
        import effects
        g = effects.analyse(target.src)
    except Exception as e:  # noqa: BLE001 - the build must say why
        raise cslots.GenError('effects.py: %s' % e)
    try:
        text, report = generate(target.src, target.pyconfig,
                                target_defines(target.python), effects=g)
    except cparse.ParseError as e:
        raise cslots.GenError(str(e))
    return text, report


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', required=True)
    ap.add_argument('--pyconfig', required=True)
    ap.add_argument('--python', default=None)
    ap.add_argument('--no-effects', action='store_true')
    ap.add_argument('--report', action='store_true')
    args = ap.parse_args(argv)
    g = None
    if not args.no_effects:
        import effects
        g = effects.analyse(args.src)
    defines = target_defines(args.python) if args.python else ['Py_BUILD_CORE', 'NDEBUG']
    text, report = generate(args.src, args.pyconfig, defines, effects=g)
    if args.report:
        print('\n'.join(report))
    else:
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
