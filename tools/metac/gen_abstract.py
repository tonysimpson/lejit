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
    statement, as for a changed slot. Each is listed in the report;
  - loops (D78, rule loops; loop_rule and counted_loop, shared with
    gen_ceval.py): a for counting a variable by one to a bound neither
    changes, the bound and start known at compile time, runs in the meta,
    which emits its body once per turn; a bound that is the length of a
    type with a known_length (policy_table.TYPES) is asked of Ni's
    representation (PsycoTuple_Load), CPython's function where unknown;
    any other loop is CPython's, resumed at it;
  - constructors (D80): a call of a virtual type's constructor
    (policy_table.TYPES, gen_virtual.py) is Ni's virtual object; an
    array type's size must be known now and within its item limit, and
    'P[i] = x' or '*P = x' through a pointer into its items stores x's
    vinfo (a PyObject ** local is an offset in the meta). Such a virtual
    is never NULL. A length read outside a loop is loaded as a loop's is;
  - ('shortcut', 'if_known', n): the integer parameter n is used as its
    value when its vinfo is a constant, else CPython's function runs.
Anything else stops that function (refused, with the reason).

Units: abstract.c by default; --unit tuple translates the slots
policy_table.TRANSLATED_SLOTS names for tupleobject.c into
ptupleobject_gen.h, with gen_tupleobject_init() registering them.

Run by generate.py; 'python3 gen_abstract.py --src DIR --pyconfig H'
prints the header and the report.
"""
import argparse
import os
import re
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
import scalars  # noqa: E402
import conditions  # noqa: E402
import pointers  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
CFILE = 'Objects/abstract.c'
NI_DECLS = 'ni/Objects/pabstract.h'
# Ni headers searched for the meta of a CPython function ('Psyco' + name)
NI_HEADERS = ['ni/Objects/pabstract.h', 'ni/Objects/pobject.h', 'ni/Objects/pfloatobject.h',
              'ni/Objects/plongobject.h', 'ni/Objects/piterobject.h',
              'ni/Objects/ptupleobject.h', 'ni/Objects/plistobject.h',
              'ni/Objects/pdictobject.h', 'ni/Objects/pboolobject.h',
              'ni/Objects/pstructmember.h', 'ni/Objects/pmethodobject.h']
# policy items the translator implements; any other item on a function's
# entry leaves it to its hand translation
IMPLEMENTED = {'likely', 'restype', ('shortcut', 'skip_check'), ('shortcut', 'if_known'),
               ('shortcut', 'const_fold'), ('promote', 'known_only'),
               ('ref', 'need_reference'), ('other', 'mutable_fields')}
# the C names that are vocabulary, never expanded as macros
KEEP = {'Py_INCREF', 'Py_DECREF', 'Py_XINCREF', 'Py_XDECREF', 'assert',
        'NULL', 'Py_None', 'Py_NotImplemented', 'Py_True', 'Py_False',
        'Py_RETURN_NOTIMPLEMENTED', 'Py_RETURN_NONE', 'Py_RETURN_TRUE',
        'Py_RETURN_FALSE', 'offsetof',
        '_Py_IDENTIFIER', 'PyErr_Occurred', 'Py_SAFE_DOWNCAST'}
# C types of objects (run-time values)
OBJ_TYPES = {'PyObject *'}
INT_TYPES = {'int', 'Py_ssize_t', 'long', 'const int', 'size_t'}
TABLE_TYPES = {'PyNumberMethods *', 'PySequenceMethods *',
               'PyMappingMethods *', 'PyAsyncMethods *', 'PyBufferProcs *'}
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
# the constant returns of object.h and boolobject.h: Ni's constant
CONST_RETURNS = {'Py_RETURN_TRUE': 'psyco_vi_Py_True',
                 'Py_RETURN_FALSE': 'psyco_vi_Py_False',
                 'Py_RETURN_NONE': 'psyco_vi_None'}
RETURN_MACROS = set(CONST_RETURNS) | {'Py_RETURN_NOTIMPLEMENTED'}
PY_OPS ={'<': 'Py_LT', '<=': 'Py_LE', '==': 'Py_EQ', '!=': 'Py_NE',
          '>': 'Py_GT', '>=': 'Py_GE'}
# a function-pointer parameter declared in place: 'double (*)(double)'
FNPTR_DECL = re.compile(r'\(\*\)\((.*)\)$')
FLIP = {'<': '>', '>': '<', '<=': '>=', '>=': '<=', '==': '==', '!=': '!='}


class Refuse(Exception):
    """The function can't be generated."""


class Resume(Exception):
    """No compile-time form here: resume CPython's algorithm at run time."""


class Rewalk(Exception):
    """The walk learnt something it needs from the start: an integer
    that is run-time around a loop, or a promotion due at a loop's entry."""


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
    def __init__(self, src, pyconfig, defines, cfile=CFILE):
        self.src = src
        self.pyconfig, self.defines = pyconfig, defines
        self.cfile = CFILE = cfile
        self.macros, pptoks = cparse.cpp(src, pyconfig, CFILE, defines)
        self.decls = cparse.Decls(pptoks)
        # the file's #if branches as the target's build takes them (rule
        # preprocess_conditionals): the rest of the text is empty lines
        text, undefs = cparse.conditionals(src, pyconfig, CFILE, defines,
                                           self.read_text())
        # macros the file #undefs: in force for the functions before
        self.undefined = {}
        for line, name, macro in undefs:
            if name not in self.macros and name not in self.undefined \
                    and macro is not None:
                self.undefined[name] = (line, macro)
        toks, self.local_macros = ctokens.tokenize(text, CFILE)
        pptoks_lines = ctokens.tokenize(text, CFILE, directives=True)[0]
        # a conditional chooses between texts; an #undef ends a macro the
        # function defines for itself (tupleconcat's 'b')
        self.pp_lines = [t.line for t in pptoks_lines if t.kind == 'pp'
                         and not t.text.startswith('#undef')]
        keep_local = KEEP | {n for n, m in self.local_macros.items()
                             if m.params is None}
        # the file's macros a representation rule names (D83) stay calls
        keep_local |= {n for n in rule_macros() if n in self.local_macros}
        self.keep_local = keep_local
        self.funcs = {}
        self.toks = toks
        self.obj_structs = self.object_structs()
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
                for name, d in csource.scan(exp, {}, self.cfile).items():
                    if d.kind == 'function':
                        self.add_func(name, d, keep_local)
                i = close + 1
                continue
            i += 1
        # static functions given an out-parameter or a function (D87)
        scalars.inline_calls(self)

    def read_text(self):
        """The C file's text (a subclass may add what it includes)."""
        with open(os.path.join(self.src, self.cfile), encoding='latin-1') as f:
            return f.read()

    def add_func(self, name, d, keep_local):
        static = 'static' in [t.text for t in d.ret]
        ret = ctype_of(d.ret)
        params = split_params(d.params)
        lines = (d.body[0].line, d.body[-1].line) if d.body else (0, 0)
        in_force = dict(self.local_macros)
        in_force.update((n, m) for n, (ln, m) in self.undefined.items()
                        if lines[0] < ln)
        # a header's statement macro ('do { ... } while (0)') is read as
        # the statements it stands for (D84)
        in_force.update(self.statement_macros())
        # a header macro defined empty stands for nothing (pyfpe.h's
        # PyFPE_START_PROTECT without WANT_SIGFPE_HANDLER, D87)
        in_force.update(self.empty_macros())
        # an object-like macro the function defines inside its body is
        # an alias of its own, expanded like a function-like one
        keep = keep_local - {n for n, m in self.local_macros.items()
                             if m.params is None and lines[0] <= m.line <= lines[1]}
        body = cparse.expand(d.body, in_force, keep)
        f = Func(name, ret, params, body, static)
        if any(lines[0] <= ln <= lines[1] for ln in self.pp_lines):
            f.why = 'a preprocessor conditional inside the body'
        else:
            f.ast = cparse.parse_body(body, self.decls.types, loops=True,
                                      switches=True)
        self.funcs[name] = f

    def statement_macros(self):
        """The header macros whose body is one statement, 'do { ... }
        while (0)' (Py_RETURN_RICHCOMPARE), except the vocabulary (KEEP)."""
        if getattr(self, '_stmt_macros', None) is None:
            self._stmt_macros = {}
            for n, m in self.macros.items():
                w = [t.text for t in m.body]
                if m.params is not None and n not in KEEP and w[:1] == ['do'] \
                        and w[-4:] == ['while', '(', '0', ')']:
                    self._stmt_macros[n] = m
        return self._stmt_macros

    def empty_macros(self):
        """The header function-like macros whose body is empty, except the
        vocabulary (KEEP)."""
        return {n: m for n, m in self.macros.items()
                if m.params is not None and not m.body and n not in KEEP}

    def object_structs(self):
        """Struct names (tags and typedefs) whose first member is a
        PyObject or PyVarObject header, directly or through another such
        struct: a pointer to one is an object (a run-time value). Types
        are not: the translator holds them at compile time."""
        out = {'PyObject', 'PyVarObject', '_object'}
        types = {'_typeobject', 'PyTypeObject'}
        changed = True
        while changed:
            changed = False
            for name, fields in self.decls.structs.items():
                if name in out or name in types or not fields:
                    continue
                first = next(iter(fields.items()))
                base = first[1].replace('struct ', '')
                if first[0] == 'ob_base' and base in types:
                    types.add(name)
                    changed = True
                elif first[0] == 'ob_base' and base in out:
                    out.add(name)
                    changed = True
        # 'typedef struct _longobject PyLongObject;' apart from the struct
        for name, what in self.decls.aliases.items():
            if what.replace('struct ', '').strip() in out:
                out.add(name)
        return out

    def is_obj(self, ctype):
        ctype = ctype.replace('const ', '').strip()
        if not ctype.endswith('*') or ctype.endswith('**'):
            return False
        base = ctype[:-1].strip().replace('struct ', '')
        return base in self.obj_structs

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

    def int_macro(self, name):
        """The value of an integer macro of the target (casts to integer
        types dropped)."""
        toks = cparse.expand(ctokens.tokenize(name)[0], self.macros, set())
        words, i = [], 0
        while i < len(toks):
            if toks[i].text == '(' and i + 2 < len(toks) and toks[i + 2].text == ')' \
                    and (toks[i + 1].text in self.decls.types
                         or toks[i + 1].text in cparse.TYPE_WORDS):
                i += 3
                continue
            t = toks[i].text
            if t[:1].isdigit():
                t = t.rstrip('uUlL')
            elif t not in ('(', ')', '<<', '>>', '+', '-', '*', '|', '&', '~'):
                raise cparse.ParseError('macro %s: %r is not an integer constant' % (name, t))
            words.append(t)
            i += 1
        return int(eval(' '.join(words), {'__builtins__': {}}))  # noqa: S307 - integer tokens only

    def is_fnptr(self, ctype):
        return ctype.rstrip(' *') in self.decls.fnptr and not ctype.endswith('*') \
            or ctype.endswith('(*)()') or FNPTR_DECL.search(ctype) is not None

    def fn_ret(self, ctype):
        """Return type of a function-pointer type."""
        if ctype.endswith('(*)()'):
            return ctype[:-5].strip()
        if FNPTR_DECL.search(ctype):
            return ctype[:FNPTR_DECL.search(ctype).start()].strip()
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
            if (t.kind == 'id' and t.text.lstrip('_').startswith('Psyco')
                    and i + 1 < len(toks)
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
    if e[0] in ('pre', 'post') and e[2][0] == 'id':
        return [e[2][1]], None      # read, then written: still live before
    return cparse.names_in(e), None


def expr_live(e, after):
    """Live before an expression (a comma list read left to right)."""
    if e[0] == 'comma':
        live = after
        for x in reversed(e[1]):
            live = expr_live(x, live)
        return live
    reads, d = uses_defs(e)
    return (after - {d}) | set(reads)


class Liveness:
    """live_after[id(stmt)]: the variables read after the statement before
    being assigned again, on some path; live_before likewise. Loops are
    iterated to a fixed point; a goto is live as its label is (a label
    after it: forward gotos only, which are all the translator follows)."""

    def __init__(self, ast):
        self.after, self.before = {}, {}
        self.labels = {}
        self.loops = []           # (live at break, live at continue)
        for _ in range(2):        # labels found on the first pass
            self.block(ast, frozenset())

    def block(self, stmts, after):
        live = after
        for s in reversed(stmts):
            live = self.stmt(s, live)
        return live

    def loop(self, s, after):
        k = s[0]
        if k == 'for':
            _, init, cond, step, body = s[:5]
        elif k == 'while':
            _, cond, body = s[:3]
            init, step = None, []
        else:
            _, body, cond = s[:3]
            init, step = None, []
        cnames = set(cparse.names_in(cond)) if cond is not None else set()
        head = frozenset(after | cnames)
        for _ in range(20):
            cont = head
            for x in reversed(step):
                cont = expr_live(x, cont)
            self.loops.append((after, cont))
            b = self.stmt(body, cont)
            self.loops.pop()
            new = frozenset(head | b) if k != 'do' else frozenset(after | cnames | b)
            if new == head:
                break
            head = new
        top = head if k != 'do' else b
        if init is not None:
            if init[0] == 'expr':
                top = expr_live(init[1], top)
            else:
                top = self.stmt(init, top)
        return frozenset(top)

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
        elif k == 'expr' and s[1][0] == 'id' and s[1][1] in RETURN_MACROS:
            live = frozenset()      # Py_RETURN_NOTIMPLEMENTED and the like return
        elif k == 'expr':
            live = frozenset(expr_live(s[1], after))
        elif k == 'if':
            t = self.stmt(s[2], after)
            e = self.stmt(s[3], after) if s[3] is not None else after
            live = t | e | set(cparse.names_in(s[1]))
        elif k == 'return':
            live = frozenset(cparse.names_in(s[1])) if s[1] is not None else frozenset()
        elif k == 'label':
            live = self.stmt(s[2], after)
            self.labels[s[1]] = live
        elif k in ('for', 'while', 'do'):
            live = self.loop(s, after)
        elif k == 'switch':
            # break leaves the switch; each case is entered from its head
            cont = self.loops[-1][1] if self.loops else after
            self.loops.append((after, cont))
            self.cases = getattr(self, 'cases', [])
            self.cases.append(set())
            b = self.stmt(s[2], after)
            entries = self.cases.pop()
            self.loops.pop()
            live = frozenset(after | b | set(cparse.names_in(s[1])).union(*entries)
                             if entries else after | b | set(cparse.names_in(s[1])))
        elif k in ('case', 'default'):
            live = after
            if getattr(self, 'cases', None):
                self.cases[-1].add(after)
        elif k == 'break' and self.loops:
            live = self.loops[-1][0]
        elif k == 'continue' and self.loops:
            live = self.loops[-1][1]
        elif k == 'goto' and s[1] in self.labels:
            live = self.labels[s[1]]
        else:   # unsupported: everything may be read
            live = after | frozenset(cparse.names_in(list(s[1:-1])))
            if k == 'goto':
                live = live | frozenset().union(*self.labels.values())
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
        self.virt = None          # rt: a virtual object the meta builds (D80)
        self.base = None          # ptr: the object whose items it points into
        self.alias = None         # rt: the parameter it is a copy of (a cast of it)
        self.rng = None           # rt integer: (lo, hi, bits) its value is in (D83)
        self.word_of = None       # rt: the word of this object variable (D83)


class State:
    def __init__(self):
        self.vars = {}            # C name -> Var (bt and ownership on this path)
        self.known = set()        # object variables whose type is promoted
        self.called = False       # a run-time call may have been emitted
        self.slots = {}           # slot address text -> meta variable
        self.ended = False        # the path returned
        self.early = False        # a path may have returned before here
        self.loaded = {}          # object variable -> meta variable of its known length
        self.failed = False       # a call's error test held: the meta has an error
        self.pending = None       # (lvalue, variable): a borrowed value stored
        #                           into a new object, owing its incref

    def copy(self):
        s = State()
        s.vars = {}
        for k, v in self.vars.items():
            w = Var(v.name, v.ctype, v.bt, v.cname)
            w.__dict__.update(v.__dict__)
            s.vars[k] = w
        s.known, s.called = set(self.known), self.called
        s.slots, s.ended, s.early = dict(self.slots), self.ended, self.early
        s.loaded = dict(self.loaded)
        s.failed = self.failed
        s.pending = self.pending
        return s


class Val:
    def __init__(self, bt, c, ctype, tp=None, lv=False, owned=False, conv=None,
                 var=None, const=None, item_of=None, virt=None, base=None):
        self.bt, self.c, self.ctype = bt, c, ctype
        self.virt = virt          # rt: a virtual object being built (Var.virt)
        self.base = base          # ptr: the variable whose items it points into
        self.item_of = item_of    # rt: an item of this loaded object (frame value)
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
        # learnt by one walk for the next (Rewalk), per function: integer
        # locals that are run-time because a loop makes them so, and the
        # promotions due at a loop's entry ({id(loop): [name or ('items', name)]})
        self.force_rt = {}
        self.loop_promos = {}
        self.slot_roots = {}      # function -> (type field, table, slot) to register
        self.vspecs = virtual_specs()   # constructor -> what a call of it builds

    # ---- which functions, in what order ----

    def roots(self):
        out = []
        for name, f in sorted(self.s.funcs.items()):
            meta = psyco_name(name)
            if (meta and meta in self.ni and self.ni[meta].name in self.declared) \
                    or name in self.slot_roots:
                out.append(name)
        return out

    def callees(self, f):
        out = []
        for n in cparse.names_in(f.ast or []):
            if n in self.s.funcs and n != f.name and n not in out \
                    and not self.hand_meta(n):
                out.append(n)
        return out

    def hand_meta(self, name):
        """Has Ni its own meta of this exported function, one the unit does
        not translate (D87: PyLong_AsDouble's)? Calls go to it."""
        meta = psyco_name(name)
        f = self.s.funcs[name]
        return bool(meta) and not f.static and meta in self.ni \
            and self.ni[meta].name not in self.declared \
            and name not in self.slot_roots and scalars.scalar_result(self, f)

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
        return self.policy.get('%s:%s' % (self.s.cfile, name), [])

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
            fmt = slot_format(self, n)
            if fmt is not None:
                ct_names[n] = slot_binding(f, fmt)
                continue
            pbt = []
            for k, (ctype, pname) in enumerate(f.params):
                if self.s.is_obj(ctype):
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
        if psyco_name(f.name) and not f.static:
            return psyco_name(f.name)
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
        other = [i for i in items if i[0] not in IMPLEMENTED
                 and tuple(i[:2]) not in IMPLEMENTED]
        if other:
            self.fails[name] = 'policy items left to the hand translation: %s' % (
                ', '.join('%s %s' % (i[0], i[1]) for i in other))
            return
        try:
            sm.kind = self.kind_of(f)
            self.live = Liveness(f.ast)
            hoist = []
            self.force_rt[name] = set()
            self.loop_promos[name] = {}
            rounds = 0
            while True:
                rounds += 1
                if rounds > 16:
                    raise Refuse('promotions after calls remain: %s' % w.late)
                w = self.new_walk(f, sm, hoist)
                try:
                    w.run()
                except Rewalk:
                    continue
                new = [p for p in w.must + w.late if p not in hoist]
                if not new:
                    break
                hoist = hoist + [p for i, p in enumerate(new) if p not in new[:i]]
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

    def new_walk(self, f, sm, hoist):
        return Walk(self, f, sm, hoist)

    def kind_of(self, f):
        if self.s.is_obj(f.ret):
            return 'obj'
        if scalars.scalar_result(self, f):
            return 'val'
        if int_typedef(self.s, f.ret) not in INT_TYPES:
            raise Refuse('returns %s' % f.ret)
        decl = self.ni.get(self.meta_name(f))
        if decl is not None:
            if decl.ret == 'bool':
                return 'ok'
            if decl.ret == 'vinfo_t *':
                return 'val'
            raise Refuse('Ni declares %s returning %s' % (decl.name, decl.ret))
        if f.static or f.name in getattr(self, 'slot_roots', {}):
            # a slot's meta returns its int as a vinfo, NULL for the error
            return 'val'
        raise Refuse('an int result needs its convention: declare %s in %s'
                     % (self.meta_name(f), NI_DECLS))

    # ---- loops: which are counted, which paths run one (rule loops) ----

    def counted(self, s):
        return counted_loop(s)

    def every_path_loops(self, f, frames):
        """Does every path from the statement frames name run a loop with a
        run-time bound, or fail, before it returns a value?"""
        def outer(level):
            if level < 0:
                return lambda: False
            stmts, j = frames[level]
            return lambda: self.seq_loops(f, stmts, j + 1, outer(level - 1), 0)
        stmts, i = frames[-1]
        return self.seq_loops(f, stmts, i, outer(len(frames) - 2), 0) == 'loop'

    def seq_loops(self, f, stmts, i, cont, depth):
        """'loop' when every path runs a run-time loop or fails and one
        runs a loop, 'error' when every path fails, else False."""
        if depth > 8:
            return False
        for k in range(i, len(stmts)):
            s = stmts[k]
            kind = s[0]

            def rest(k=k):
                return self.seq_loops(f, stmts, k + 1, cont, depth)
            if once_do(s):
                return self.seq_loops(f, block_of(s[1]), 0, rest, depth)
            if kind in ('for', 'while', 'do'):
                return 'loop' if self.counted(s) is None else False
            if kind == 'return':
                return 'error' if self.error_value(f, s[1]) else False
            if kind == 'if':
                a = self.seq_loops(f, block_of(s[2]), 0, rest, depth)
                if not a:
                    return False
                b = (self.seq_loops(f, block_of(s[3]), 0, rest, depth)
                     if s[3] is not None else rest())
                if not b:
                    return False
                return 'loop' if 'loop' in (a, b) else 'error'
            if kind == 'block':
                return self.seq_loops(f, s[1], 0, rest, depth)
            if kind == 'label':
                return self.seq_loops(f, [s[2]], 0, rest, depth)
            if kind == 'goto':
                j = next((n for n, x in enumerate(f.ast)
                          if x[0] == 'label' and x[1] == s[1]), None)
                if j is None:
                    return False
                return self.seq_loops(f, f.ast, j, lambda: False, depth + 1)
            if kind in ('unsupported', 'break', 'continue'):
                return False
        return cont()

    def error_value(self, f, e):
        """Is 'return e' the function's error return?"""
        if e is None:
            return False
        while e[0] == 'cast':
            e = e[2]
        if e == ('id', 'NULL') and self.s.is_obj(f.ret):
            return True
        if e == ('un', '-', ('num', '1')) and f.ret in INT_TYPES:
            return True
        if scalars.error_value(self, f, e):
            return True
        if e[0] == 'call' and e[1][0] == 'id' and e[1][1] in self.s.funcs:
            g = self.s.funcs[e[1][1]]
            rets = []

            def walk(n):
                if isinstance(n, tuple) and n:
                    if n[0] == 'return':
                        rets.append(n[1])
                    for x in n[1:]:
                        if isinstance(x, (tuple, list)):
                            walk(x)
                elif isinstance(n, list):
                    for x in n:
                        walk(x)
            walk(g.ast or [])
            return bool(rets) and g is not f and all(
                r is not None and r[0] != 'call' and self.error_value(g, r) for r in rets)
        return False

    def find_any_call(self, node):
        if isinstance(node, tuple) and node:
            if node[0] == 'call' and not (node[1][0] == 'id' and node[1][1] in (
                    'assert', 'Py_INCREF', 'Py_XINCREF')):
                return True
            return any(self.find_any_call(x) for x in node[1:]
                       if isinstance(x, (tuple, list)))
        if isinstance(node, list):
            return any(self.find_any_call(x) for x in node)
        return False

    # ---- residual calls' flags ----

    def flags(self, ctype, conv, callee_key=None):
        """Cf flags of a run-time call returning ctype, whose errors show
        as conv says: 'null', 'notimpl', 'neg' (and the value is
        non-negative otherwise), 'minus1', 'zero_ok'."""
        if conv == 'zero_ok':
            parts = ['CfNoReturnValue', 'CfReturnTypeInt', 'CfPyErrIfNonNull']
        else:
            r = RET_FLAG.get(ctype.replace('const ', ''))
            if r is None and self.s is not None:
                r = RET_FLAG.get(int_typedef(self.s, ctype.replace('const ', '')))
            if r is None:
                raise Refuse('a call returning %s' % ctype)
            parts = [r]
            if r == 'CfReturnTypePtr':
                parts.append('CfNewRef')
            if r == 'CfReturnTypeDouble' and conv in ('neg', 'minus1'):
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
        return g.by_file.get(self.s.cfile, {}).get(name) or g.globals.get(name)

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
        while name in self.copies:
            # a scalar helper has the name (rule copy_through, D87)
            n += 1
            name = self.copy_name(f, n + 1)
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
        # a static that reaches itself again (k_mul and k_lopsided_mul) is
        # not copied
        making = self.__dict__.setdefault('copies_making', set())
        if key in making:
            raise Refuse('copy: %s calls itself' % f.name)
        making.add(key)
        try:
            body = self.rename_statics(body)
        finally:
            making.discard(key)
        body = ''.join('Py_INCREF(%s); ' % v for v in sorted(owned)) + body
        ret = f.ret
        text = 'static %s %s(%s) {\n  %s\n  %s\n}\n' % (
            ret, name, ', '.join(scalars.declarator(t, p) for t, p in params) or 'void',
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
        if s[-1] == scalars.INL:
            return scalars.stmt_text(s)     # inlined from another function
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
                t = t._replace(text=self.copy(g, [(g.ast, 0)], set())[0])
            out.append(t)
        return cslots.text(out)


# ---- representation rules: values Ni holds in its own form (D83) ---------------
#
# A TYPES entry's 'word' section says that an object of that struct is, on
# a path where a C test holds, one machine word: Ni's virtual int holds it
# in its array, a real one has it read by Ni's 'load' function. The rules
# read the C's own expressions (the 'test', 'value' and 'sign' texts, with
# the file's macros expanded as the bodies are), never a function's name:
#   - the test is a load: where it fails, CPython's function resumes
#     (the rest of the function has digits, which Ni never holds);
#   - the value is the word, with the range 'bound' gives, and C
#     arithmetic on such values (+ - * unary -, casts, the integer types
#     of the target) is word arithmetic when an interval check proves that
#     the C can't overflow its types and the result fits a word;
#   - the sign field compared with 0 is the word compared with 0 (for a
#     real int, the field itself: Ni's 'sign' function);
#   - the constructors given the word build the virtual (Ni's 'new');
#   - a compare of such values that the function returns as its int
#     result is the condition as a value, not a branch (rule
#     condition_value).

# widths of C's signed integer types on x86-64 (LP64), asserted in the
# header of a unit that uses them
C_INT_BITS = {'signed char': 8, 'short': 16, 'short int': 16, 'int': 32,
              'signed': 32, 'signed int': 32, 'long': 64, 'long int': 64,
              'signed long': 64, 'long long': 64, 'long long int': 64,
              'Py_ssize_t': 64}
# the INT_TYPES name the meta declares a local of that width as
CANON_INT = {32: 'int', 64: 'long'}
WORD_BITS = 64
# word arithmetic: the backend's functions
WORD_OPS = {'+': 'integer_add', '-': 'integer_sub', '*': 'integer_mul'}


def unify(p, e, env):
    """Does the expression e have the shape of the pattern p, whose
    identifier 'x' stands for any variable (bound in env)?"""
    if isinstance(p, tuple) and len(p) == 2 and p[0] == 'id' and p[1] == 'x':
        if not (isinstance(e, tuple) and e and e[0] == 'id'):
            return False
        if 'x' in env:
            return env['x'] == e[1]
        env['x'] = e[1]
        return True
    if isinstance(p, (tuple, list)):
        if type(p) is not type(e) or len(p) != len(e):
            return False
        return all(unify(a, b, env) for a, b in zip(p, e))
    return p == e


def match(p, e):
    env = {}
    return env['x'] if unify(p, e, env) and 'x' in env else None


def fits(lo, hi, bits):
    return -2 ** (bits - 1) <= lo and hi < 2 ** (bits - 1)


class WordRep:
    """One TYPES 'word' section, read against the unit's source."""

    def __init__(self, source, struct, spec):
        self.s = source
        self.struct, self.spec = struct, spec
        self.test = self.pattern(spec['test'])
        self.value = self.pattern(spec['value'])
        self.sign = self.pattern(spec['sign']) if 'sign' in spec else None
        self.bound = source.int_macro(spec['bound'])
        self.value_bits = int_bits(source, spec['value_ctype'])
        if self.value_bits is None:
            raise Refuse('%s: %s is not a signed integer type'
                         % (struct, spec['value_ctype']))
        self.load = ni_short(spec['load'])
        self.sign_load = ni_short(spec['sign_load']) if 'sign_load' in spec else None
        self.new = ni_short(spec['new'])
        self.ctors = {c.split(':')[1] for c in spec['constructors']}
        self.used = False

    def pattern(self, text):
        toks = ctokens.tokenize(text)[0]
        toks = cparse.expand(toks, self.s.local_macros, self.s.keep_local)
        return cparse.parse_expr(toks, self.s.decls.types, loops=True)

    def asserts(self):
        return ['_Static_assert(%s == %d, "the word range of %s (D83)");'
                % (self.spec['bound'], self.bound, self.struct)]


def int_bits(source, ctype):
    """Width of a signed integer C type (through typedefs), or None."""
    ctype = ctype.replace('const ', '').strip()
    for _ in range(8):
        words = ctype.split()
        if 'unsigned' in words or 'char' in words:
            return None
        words = [w for w in words if w != 'signed']
        if ('long' in words or 'short' in words) and 'int' in words:
            words.remove('int')
        if ' '.join(words) in C_INT_BITS:
            return C_INT_BITS[' '.join(words)]
        if not words:
            return C_INT_BITS['int']
        nxt = source.decls.aliases.get(ctype)
        if nxt is None or nxt == ctype:
            return None
        ctype = nxt.replace('const ', '').strip()
    return None


def int_typedef(source, ctype):
    """An integer typedef (Py_hash_t) as the INT_TYPES name it stands for,
    through the file's typedefs; any other type unchanged."""
    for _ in range(4):
        if ctype in INT_TYPES:
            break
        base = source.decls.aliases.get(ctype)
        if base is None:
            break
        ctype = base
    return ctype


def rule_macros():
    """Names called in the representation rules' C texts (D83)."""
    out = set()
    for t in policy_table.TYPES.values():
        for k in ('test', 'value', 'sign'):
            text = t.get('word', {}).get(k)
            toks = ctokens.tokenize(text)[0] if text else []
            out |= {x.text for i, x in enumerate(toks[:-1])
                    if x.kind == 'id' and toks[i + 1].text == '('}
    return out


def word_reps(source):
    """{struct or typedef name: WordRep} for the TYPES entries with a
    'word' section whose struct the unit's source declares."""
    out = {}
    for key, t in policy_table.TYPES.items():
        if 'word' not in t:
            continue
        struct = key.split(':')[1]
        if struct not in source.decls.structs:
            continue
        r = WordRep(source, struct, dict(t['word'], type=t['type']))
        out[struct] = r
        for name, what in source.decls.aliases.items():
            if what.replace('struct ', '').strip() == struct:
                out[name] = r
    return out


class RepresentationRules:
    """Walk's representation rules (D83). Each hook answers None when the
    expression is not one it knows, and the walk goes on as before."""

    # -- what applies --

    def reps(self):
        tr = self.tr
        if getattr(tr, 'word_reps', None) is None:
            tr.word_reps = word_reps(tr.s)
        return tr.word_reps

    def rep_of_var(self, st, name):
        v = st.vars.get(name)
        if v is None or v.bt != 'rt' or not self.s.is_obj(v.ctype) or v.owned == 'dead' \
                and not v.isparam:
            return None
        base = v.ctype.replace('const ', '').rstrip(' *').replace('struct ', '').strip()
        return self.reps().get(base)

    def word_var(self, st, x):
        return st.vars.get('$w_' + x)

    def rep_match(self, which, e, st):
        """(x, rep) when e is the rep's 'test', 'value' or 'sign' of a
        variable x of its struct."""
        for r in set(self.reps().values()):
            p = getattr(r, which)
            if p is None:
                continue
            x = match(p, e)
            if x is not None and self.rep_of_var(st, x) is r:
                return x, r
        return None

    # -- the test: load the word, CPython's function where there is none --

    def rep_cond(self, e, st, out, frames):
        m = self.rep_match('test', e, st)
        if m is not None:
            x, r = m
            if self.word_var(st, x) is None:
                self.load_word(st, out, frames, x, r)
            return True
        if e[0] == 'bin' and e[1] in PY_OPS:
            return self.sign_compare(e, st, out, frames)
        return None

    def load_word(self, st, out, frames, x, r):
        r.used = True
        xv = st.vars[x]
        w = Var('$w_' + x, 'long', 'rt', 'w_' + x)
        w.word_of = x
        w.rng = (-r.bound, r.bound, r.value_bits)
        self.decls[w.cname] = 'vinfo_t *%s;' % w.cname
        out('switch (%s(po, %s, &%s)) {' % (r.load, xv.cname, w.cname))
        out('case 1:')
        out('  break;')
        out('case 0:')
        sub = Out(out.depth + 1)
        self.resume_inside(st.copy(), sub, frames,
                           '%s is not one word (%s)' % (x, r.spec['test']))
        out.extend(sub)
        out('default:')
        sub = Out(out.depth + 1)
        self.release_all(st, sub)
        sub('return %s;' % self.err())
        out.extend(sub)
        out('}')
        w.owned = 'yes'
        st.vars[w.name] = w

    # -- the sign field compared with 0 --

    def sign_operand(self, e, st, out):
        """The sign vinfo (borrowed) when e is the sign field of a
        variable of a struct with a word; None otherwise."""
        m = self.rep_match('sign', e, st)
        if m is None:
            return None
        x, r = m
        if r.sign_load is None:
            return None
        r.used = True
        t = self.new_tmp('vinfo_t *')
        out('%s = %s(po, %s);' % (t, r.sign_load, st.vars[x].cname))
        self.error_exit(st, out, '%s == NULL' % t)
        return t

    def sign_cc(self, e, st, out):
        """(condition code variable, C text) of 'SIGN op 0', or None."""
        op, a, b = e[1], e[2], e[3]
        if b != ('num', '0'):
            if a != ('num', '0'):
                return None
            a, b, op = b, a, FLIP[op]
        if self.rep_match('sign', a, st) is None:
            return None
        s = self.sign_operand(a, st, out)
        cc = self.new_tmp('condition_code_t')
        call, inverted = int_condition(op, s, '0', True)
        out('%s = %s;' % (cc, call))
        self.error_exit(st, out, '%s == CC_ERROR' % cc)
        if inverted:
            out('%s = INVERT_CC(%s);' % (cc, cc))
        return cc

    def sign_compare(self, e, st, out, frames):
        cc = self.sign_cc(e, st, out)
        if cc is None:
            return None
        text = ' '.join(cparse.c_expr(e).replace('(', '').replace(')', '').split())
        if text not in self.likely:
            text = self.likely_key(e) or text   # the C as written
        how = self.likely.get(text)
        if how is None:
            raise Refuse("no 'likely' item in the policy table for the run-time"
                         " branch %r" % text)
        self.used_likely.add(text)
        t = self.new_tmp('int')
        out('%s = runtime_condition_%s(po, %s);' % (t, 't' if how == 'true' else 'f', cc))
        return t

    def rep_condition_value(self, e, st, out, frames):
        """Rule condition_value: 'return A op B;' from a function whose
        result is an int, A and B values of a word representation: the
        condition as a 0/1 value (psyco_vinfo_condition), no branch."""
        if not (e[0] == 'bin' and e[1] in PY_OPS) or self.sm.kind != 'val':
            return None
        cc = self.sign_cc(e, st, out)
        if cc is None:
            return None
        t = self.new_tmp('vinfo_t *')
        out('%s = psyco_vinfo_condition(po, %s);' % (t, cc))
        return Val('rt', t, 'int', owned=True)

    # -- the word, and arithmetic on it --

    def ranged_tree(self, e, st):
        """Does e read a word or a ranged integer variable?"""
        if self.rep_match('value', e, st) is not None:
            return True
        if isinstance(e, tuple) and e:
            if e[0] == 'id':
                v = st.vars.get(e[1])
                return v is not None and v.rng is not None and v.owned != 'dead'
            return any(self.ranged_tree(x, st) for x in e[1:]
                       if isinstance(x, (tuple, list)))
        if isinstance(e, list):
            return any(self.ranged_tree(x, st) for x in e)
        return False

    def word_uses(self, frames, x):
        """Reads of x's word left in the statement being translated."""
        s = frames[-1][0][frames[-1][1]] if frames else None
        key = id(s)
        if getattr(self, '_wcount_key', None) != key:
            self._wcount_key = key
            self._wcount = {}
        if x not in self._wcount:
            if s[0] == 'decl':
                own = [d[2] for d in s[1] if d[2] is not None]
            elif s[0] in ('if', 'expr', 'return'):
                own = [s[1]]
            else:
                own = [s]
            n = [0]

            def walk(t):
                if self.rep_match_x('value', t, x):
                    n[0] += 1
                    return
                if isinstance(t, (tuple, list)):
                    for y in t:
                        walk(y)
            walk(own)
            self._wcount[x] = n[0]
        return s

    def rep_match_x(self, which, e, x):
        for r in set(self.reps().values()):
            if match(getattr(r, which), e) == x:
                return True
        return False

    def word_read(self, e, st, out, frames):
        m = self.rep_match('value', e, st)
        if m is None:
            return None
        x, r = m
        w = self.word_var(st, x)
        if w is None or w.owned != 'yes':
            raise ResumeHere('%s read where %s has no word' % (cparse.c_expr(e), x))
        s = self.word_uses(frames, x)
        self._wcount[x] -= 1
        val = Val('rt', w.cname, 'long', var=w)
        val.rng = w.rng
        # its last read here, and x is not read after: released after use
        val.last = (self._wcount[x] == 0 and not self.loop_ctx
                    and (s[0] == 'return' or x not in self.tr.live.after.get(id(s), {x})))
        return val

    def int_operand(self, e, st, out, frames):
        v = self.ev(e, st, out, frames)
        if v.bt == 'ct' and v.c.lstrip('-').isdigit():
            n = int(v.c)
            v.rng = (n, n, 32 if fits(n, n, 32) else 64)
            v.const = n
            return v
        if v.bt == 'rt' and getattr(v, 'rng', None) is not None:
            return v
        raise ResumeHere('%s: arithmetic on a value with no known range'
                         % cparse.c_expr(e))

    def consumed(self, out, vals):
        """Release what an operation consumed: temporaries, and words at
        their last read."""
        for v in vals:
            if v.bt != 'rt':
                continue
            if v.owned and v.var is None:
                out('vinfo_decref(%s, po);' % v.c)
            elif getattr(v, 'last', False) and v.var is not None and v.var.owned == 'yes':
                out('vinfo_decref(%s, po);' % v.c)
                v.var.owned = 'dead'

    def word_op(self, e, st, out, frames):
        k = e[0]
        if k == 'un' and e[1] == '-':
            a = self.int_operand(e[2], st, out, frames)
            if a.bt == 'ct':
                raise ResumeHere('a constant expression')
            lo, hi, bits = -a.rng[1], -a.rng[0], max(32, a.rng[2])
            if not fits(lo, hi, bits):
                raise ResumeHere('%s may overflow its C type' % cparse.c_expr(e))
            return self.emit_op('integer_neg(po, %s, false)' % a.c, [a], (lo, hi, bits), e, st, out)
        if k == 'cast':
            ctype = ctype_of(ctokens.tokenize(e[1])[0])
            bits = int_bits(self.s, ctype)
            a = self.int_operand(e[2], st, out, frames)
            if bits is None:
                raise ResumeHere('a cast to %s' % ctype)
            if not fits(a.rng[0], a.rng[1], bits):
                raise ResumeHere('a cast to %s may truncate' % ctype)
            v = Val(a.bt, a.c, CANON_INT.get(bits, ctype), owned=a.owned, var=a.var)
            v.rng = (a.rng[0], a.rng[1], bits)
            v.last = getattr(a, 'last', False)
            if a.bt == 'ct':
                v.const = a.const
            return v
        op = e[1]
        if op not in WORD_OPS:
            raise ResumeHere('the operator %s on a word' % op)
        a = self.int_operand(e[2], st, out, frames)
        b = self.int_operand(e[3], st, out, frames)
        if a.bt == 'ct' and b.bt == 'ct':
            raise ResumeHere('a constant expression')
        bits = max(32, a.rng[2], b.rng[2])
        if op == '*':
            c = [a.rng[0] * b.rng[0], a.rng[0] * b.rng[1], a.rng[1] * b.rng[0],
                 a.rng[1] * b.rng[1]]
            lo, hi = min(c), max(c)
        elif op == '+':
            lo, hi = a.rng[0] + b.rng[0], a.rng[1] + b.rng[1]
        else:
            lo, hi = a.rng[0] - b.rng[1], a.rng[1] - b.rng[0]
        if not fits(lo, hi, bits):
            # signed overflow is undefined in C: the C itself must not
            raise ResumeHere('%s may overflow its %d-bit C type' % (cparse.c_expr(e), bits))
        if b.bt == 'ct' and op in '+-':
            call = 'integer_add_i(po, %s, %d, false)' % (a.c, b.const if op == '+' else -b.const)
        elif a.bt == 'ct' and op == '+':
            call = 'integer_add_i(po, %s, %d, false)' % (b.c, a.const)
        elif 'ct' in (a.bt, b.bt):
            raise ResumeHere('%s with a constant' % op)
        else:
            call = '%s(po, %s, %s, false)' % (WORD_OPS[op], a.c, b.c)
        return self.emit_op(call, [a, b], (lo, hi, bits), e, st, out)

    def emit_op(self, call, vals, rng, e, st, out):
        out('/* %s: in [%d, %d] */' % (cparse.c_expr(e), rng[0], rng[1]))
        t = self.new_tmp('vinfo_t *')
        out('%s = %s;' % (t, call))
        self.consumed(out, vals)
        self.error_exit(st, out, '%s == NULL' % t)
        v = Val('rt', t, CANON_INT.get(rng[2], 'long'), owned=True)
        v.rng = rng
        return v

    def word_ctor(self, e, st, out, frames):
        """A constructor given a word: Ni's virtual."""
        if not (e[0] == 'call' and e[1][0] == 'id' and len(e[2]) == 1):
            return None
        for r in set(self.reps().values()):
            if e[1][1] not in r.ctors or not self.ranged_tree(e[2][0], st):
                continue
            a = self.int_operand(e[2][0], st, out, frames)
            ptype = ctype_of(ctokens.tokenize(self.s.decls.protos.get(e[1][1], 'long'))[0])
            if a.bt != 'rt' or not fits(a.rng[0], a.rng[1], WORD_BITS):
                raise ResumeHere('%s of a value that may not fit a word' % e[1][1])
            r.used = True
            if not (a.owned and a.var is None):
                if getattr(a, 'last', False) and a.var is not None and a.var.owned == 'yes':
                    a.var.owned = 'dead'      # the virtual takes its reference
                else:
                    out('vinfo_incref(%s);' % a.c)
            t = self.new_tmp('vinfo_t *')
            out('%s = %s(%s);' % (t, r.new, a.c))
            del ptype
            return Val('rt', t, 'PyObject *', owned=True,
                       virt={'array': False, 'type': r.spec['type']})
        return None

    def rep_ev(self, e, st, out, frames):
        if not self.reps() or not isinstance(e, tuple):
            return None
        v = self.word_read(e, st, out, frames)
        if v is not None:
            return v
        v = self.word_ctor(e, st, out, frames)
        if v is not None:
            return v
        if e[0] == 'id' and e[1] in st.vars and st.vars[e[1]].rng is not None \
                and st.vars[e[1]].owned != 'dead':
            v = self.ev_id(e[1], st, out)
            v.rng = st.vars[e[1]].rng
            return v
        if e[0] in ('bin', 'un', 'cast') and not (e[0] == 'bin' and (
                e[1] in PY_OPS or e[1] in ('&&', '||'))) and self.ranged_tree(e, st):
            return self.word_op(e, st, out, frames)
        return None

    def rep_int_ctype(self, ctype):
        """A signed integer typedef as the INT_TYPES name of its width."""
        if ctype in INT_TYPES or not self.reps():
            return ctype
        bits = int_bits(self.s, ctype)
        return CANON_INT.get(bits, ctype) if bits else ctype

    def rep_assigning(self, v, val, out):
        """A ranged value stored in an integer variable keeps its range if
        the variable's type holds it (C would truncate otherwise); a word
        read is the variable's own reference (taken at its last read)."""
        rng = getattr(val, 'rng', None)
        if rng is None or val.bt != 'rt':
            v.rng = None
            return val
        bits = int_bits(self.s, v.ctype)
        if bits is None or not fits(rng[0], rng[1], bits):
            raise ResumeHere('%s may not hold the value' % v.name)
        v.rng = (rng[0], rng[1], bits)
        if val.var is not None and val.var.word_of is not None:
            if getattr(val, 'last', False) and val.var.owned == 'yes':
                val.var.owned = 'dead'
            else:
                out('vinfo_incref(%s);' % val.c)
            val = Val('rt', val.c, val.ctype, owned=True)
            val.rng = rng
        return val

    # -- resuming: does the rest return NotImplemented? --

    def notimpl_after(self, frames):
        """Can the C from the statement frames name to the end return
        NotImplemented? (conservative: a goto may go anywhere)"""
        rest = []
        for level, (stmts, i) in enumerate(reversed(frames)):
            rest.extend(stmts[i:] if level == 0 else stmts[i + 1:])
        found = []

        def walk(n):
            if isinstance(n, tuple) and n:
                if n[0] == 'goto':
                    found.append(1)
                if n[0] == 'expr' and n[1] == ('id', 'Py_RETURN_NOTIMPLEMENTED'):
                    found.append(1)
                if n[0] == 'return' and n[1] is not None:
                    r = n[1]
                    if r == ('id', 'Py_NotImplemented'):
                        found.append(1)
                    if r[0] == 'call' and r[1][0] == 'id' and r[1][1] in self.tr.sums \
                            and self.tr.sums[r[1][1]].notimpl:
                        found.append(1)
                for y in n[1:]:
                    if isinstance(y, (tuple, list)):
                        walk(y)
            elif isinstance(n, list):
                for y in n:
                    walk(y)
        walk(rest)
        return bool(found)

    def resume_notimpl(self, frames):
        """The resumed call's result may be NotImplemented: only when the
        rest can return it, if the function's result type is asserted."""
        if not self.notimpl_fn():
            return False
        return not self.restype or self.notimpl_after(frames)


def once_do(s):
    """'do S while (0);' with no break or continue of its own: S, once."""
    if s[0] != 'do' or s[2] != ('num', '0'):
        return False
    found = []

    def walk(n):
        if isinstance(n, tuple) and n:
            if n[0] in ('break', 'continue'):
                found.append(1)
            if n[0] in ('for', 'while', 'do'):
                return
            for y in n[1:]:
                if isinstance(y, (tuple, list)):
                    walk(y)
        elif isinstance(n, list):
            for y in n:
                walk(y)
    walk(s[1])
    return not found


class Walk(RepresentationRules, scalars.ScalarRules, conditions.ConditionRules,
           pointers.PointerRules):
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
        self.loop_ctx = []        # LoopCtx of the loops being translated, outermost first
        self.kl_rep = {}          # parameter -> its known_length representation
        self.goto_depth = 0
        self.restype = [i[1] for i in tr.policy_items(f.name) if i[0] == 'restype']
        self.switch_depth = 0     # switches being translated (switch_stmt)

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
        for item in self.tr.policy_items(f.name):
            if tuple(item[:2]) == ('shortcut', 'if_known'):
                self.if_known(st, out, item[2])
            if tuple(item[:2]) == ('shortcut', 'const_fold'):
                self.const_fold(st, out)
        self.block(f.ast, st, out, [])
        if st.pending is not None:
            raise Refuse('a borrowed reference stored into a new object, never'
                         ' increfed')
        if not st.ended:
            raise Refuse('the end of the function is reachable without a return')
        if self.sm.kind == 'ct' and self.any_call:
            raise Refuse('a compile-time result after a run-time call')
        for name, how in self.likely.items():
            if name not in self.used_likely:
                raise Refuse("policy 'likely' %r matches no run-time branch" % name)

    def if_known(self, st, out, name):
        """Policy ('shortcut', 'if_known', name): the integer parameter is
        used as its compile-time value when its vinfo is a constant; any
        other value runs CPython's function (no promotion)."""
        v = st.vars.get(name)
        if v is None or not v.isparam or v.bt != 'rt' or v.ctype not in INT_TYPES:
            raise Refuse("'if_known' %s is not an integer parameter" % name)
        out('if (!is_compiletime(%s->source)) {' % v.cname)
        sub = Out(out.depth + 1)
        self.resume(st.copy(), sub, [(self.f.ast, 0)],
                    '%s is not known at compile time' % name)
        out.extend(sub)
        out('}')
        k = Var(name, v.ctype, 'ct', name + '_k')
        k.isparam = True
        self.decls[k.cname] = '%s %s;' % (v.ctype, k.cname)
        out('%s = (%s)CompileTime_Get(%s->source)->value;' % (k.cname, v.ctype, v.cname))
        st.vars[name] = k

    def text(self):
        f, sm = self.f, self.sm
        ret = {'obj': 'vinfo_t *', 'val': 'vinfo_t *', 'ok': 'bool', 'ct': 'int'}[sm.kind]
        head = '%s%s %s(PsycoObject *po%s)' % (
            'static ' if f.static or sm.meta not in self.tr.declared else 'DEFINEFN ',
            ret, sm.meta, ''.join(', %s%s%s' % (t, '' if t.endswith('*') else ' ', p)
                                  for t, p in self.params))
        lines = ['/* %s:%s */' % (self.s.cfile, f.name), head + ' {']
        used = set(t.text for t in ctokens.tokenize('\n'.join(self.out.lines))[0]
                   if t.kind == 'id')
        lines += ['  ' + d for n, d in sorted(self.decls.items()) if n in used]
        lines += self.out.lines
        lines.append('}')
        return '\n'.join(lines) + '\n'

    def block(self, stmts, st, out, frames):
        skip = 0
        for i, s in enumerate(stmts):
            if st.ended:
                break
            if skip:
                skip -= 1
                continue
            here = frames + [(stmts, i)]
            took = self.scalar_run(stmts, i, st, out, here)
            if took:
                # rule copy_through: statements i to i + took in a helper
                skip = took - 1
                if not st.ended:
                    self.release_dead(st, out, self.tr.live.after[id(stmts[i + took - 1])])
                continue
            if (not st.called and not self.loop_ctx and self.sm.kind != 'ct'
                    and s[0] not in ('decl', 'empty')
                    and self.tr.every_path_loops(self.f, here)):
                # rule loops: every path from here runs a loop with a
                # run-time bound (or fails): CPython's function does it all
                self.resume(st, out, here, 'every path from here runs a loop'
                            ' with a run-time bound')
                break
            self.statement(s, st, out, here)
            if not st.ended:
                self.release_dead(st, out, self.tr.live.after[id(s)])

    def release_dead(self, st, out, live):
        for v in list(st.vars.values()):
            if v.bt == 'rt' and v.owned in ('yes', 'shadow') and (v.word_of or v.name) not in live:
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
        outer_frames, self.stmt_frames = getattr(self, 'stmt_frames', None), frames
        pending = st.pending
        try:
            self.stmt(s, st, out, frames)
            if pending is not None and st.pending is pending:
                raise Refuse('a borrowed reference stored into a new object'
                             ' is not increfed by the next statement')
        except Resume as e:
            if k in ('block', 'if') and not isinstance(e, ResumeHere):
                raise
            if self.loop_ctx:
                # no copy can start inside a loop's body: the meta resumes
                # at the outermost loop being translated
                raise Resume(str(e))
            del out.lines[mark:]
            self.decls = saved_decls
            self.must, self.late, self.returns, self.resumed = saved_lists
            self.notimpl = saved_notimpl
            st.__dict__.update(saved.__dict__)
            self.stmt_calls = self.ncalls
            self.resume(st, out, frames, str(e))
        self.stmt_calls = outer_calls
        self.stmt_frames = outer_frames

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
        if st.failed:
            # the meta's error is raised at compile time or run time: the
            # C's handling of it can't run as a copy
            raise Refuse('resuming where a call has failed: %s' % why)
        if self.switch_depth and st.called:
            # a copy of the rest would start among a switch's case labels
            raise Refuse('resuming inside a switch: %s' % why)
        for v in st.vars.values():
            if v.bt == 'rt' and v.virt is not None and v.owned == 'yes':
                # its items may not all be stored yet
                raise Refuse('resuming while %s is being built: %s' % (v.name, why))
        self.resumed.append((f.name, self.tr.stmt_text(f, stmt)[:70], why))
        if not st.called:
            if not f.static:
                name, params = f.name, f.params
            elif f.name in self.tr.slot_roots:
                # a static type's slot: CPython's function through it
                name = slot_ref(self.tr.slot_types[f.name], *self.tr.slot_roots[f.name])
                params = f.params
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
                        and self.s.is_obj(v.ctype):
                    if v.owned == 'shadow':
                        raise Refuse('resuming with %s owned on some paths' % vname)
                    owned.add(vname)
            name, params = self.tr.copy(f, frames, live, owned)
        fmt, args = '', []
        for ctype, p in params:
            v = st.vars.get(p)
            if v is None or v.owned == 'dead':
                raise Refuse('resuming needs %s, which has no value here' % p)
            if v.bt == 'ptr':
                raise Refuse('resuming needs %s, a pointer the meta holds as an offset' % p)
            if v.bt == 'rt':
                fmt += 'v'
                args.append(v.cname)
            else:
                fmt += 'l'
                args.append('(long)(%s)' % v.cname)
        conv = {'obj': 'notimpl' if self.resume_notimpl(frames) else 'null',
                'val': 'neg', 'ok': 'zero_ok'}.get(sm.kind)
        if conv is None:
            raise Refuse('resuming in a compile-time function')
        flags = self.tr.flags(f.ret, conv, self.tr.effects_key(f.name))
        call = 'psyco_generic_call(po, %s, %s, "%s"%s)' % (
            name, flags, fmt, ''.join(', ' + a for a in args))
        st.called = True
        self.any_call = True
        self.ret_value(st, out, call, sm.kind == 'ok', notimpl=conv == 'notimpl')

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

    def ret_value(self, st, out, cexpr, as_ok=False, notimpl=False):
        """return a run-time value the meta owns, after releasing the rest"""
        expr = '%s != NULL' % cexpr if as_ok else cexpr
        owned = [v for v in st.vars.values() if v.bt == 'rt' and v.owned in ('yes', 'shadow')]
        if self.restype and not as_ok:
            r = self.new_tmp('vinfo_t *')
            out('%s = %s;' % (r, expr))
            self.assert_restype(out, r, notimpl)
            self.release_all(st, out)
            out('return %s;' % r)
        elif owned:
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
        if once_do(s):
            self.stmt(s[1], st, out, frames)     # do S while (0)
            return
        if k in ('for', 'while', 'do'):
            self.loop_stmt(s, st, out, frames)
            return
        if k == 'switch':
            self.switch_stmt(s, st, out, frames)
            return
        if k in ('case', 'default'):
            raise Refuse('a %s label outside a switch' % k)
        if k in ('break', 'continue'):
            self.jump(k, st, out)
            return
        if k == 'goto':
            self.goto(s, st, out, frames)
            return
        if k == 'label':
            self.stmt(s[2], st, out, frames)
            return
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
        if self.scalar_declare(st, name, ctype):
            return
        ctype = self.rep_int_ctype(ctype)
        if name in st.vars and not st.vars[name].isparam:
            old = st.vars[name]
            if old.ctype != ctype:
                raise Refuse('%s declared twice with different types' % name)
        if self.s.is_obj(ctype):
            bt = 'rt'
        elif self.tr.ct_ctype(ctype) or ctype in ('const char *',):
            bt = 'ct'
        elif ctype in INT_TYPES:
            # by its first assignment, unless a loop makes it run-time
            bt = 'rt' if name in self.tr.force_rt.get(self.f.name, ()) else None
        elif ctype == 'PyObject **':
            # a pointer into an object's items (D80): the object, and an
            # offset the meta holds
            bt = 'ptr'
        else:
            raise ResumeHere('a local of type %s' % ctype)
        v = Var(name, ctype, bt, name)
        v.owned = 'dead' if bt == 'rt' else 'no'
        st.vars[name] = v
        if bt == 'ptr':
            self.decls[name] = 'Py_ssize_t %s;' % name
        elif bt == 'ct':
            self.decls[name] = '%s%s%s;' % (ctype, '' if ctype.endswith('*') else ' ', name)
        elif bt == 'rt':
            self.decls[name] = 'vinfo_t *%s;' % name

    # ---- expression statements ----

    def expr_stmt(self, e, st, out, frames):
        if e[0] == 'id' and e[1] in CONST_RETURNS:
            self.const_return(st, out, e[1])
            return
        if e in (('call', ('id', 'abort'), []), ('call', ('id', 'Py_UNREACHABLE'), [])):
            # Py_UNREACHABLE(): a path the C says never runs
            self.release_all(st, out)
            out('abort();')
            st.ended = True
            return
        if e == ('id', 'Py_RETURN_NOTIMPLEMENTED'):
            if self.sm.kind != 'obj':
                raise Refuse('NotImplemented from a non-object function')
            self.notimpl = True
            self.release_all(st, out)
            out('return psyco_vi_NotImplemented();')
            st.ended = True
            self.returns.append('value')
            return
        if e[0] == 'comma':
            for x in e[1]:
                self.expr_stmt(x, st, out, frames)
            return
        if e[0] in ('pre', 'post'):
            out('%s;' % self.ev(e, st, out, frames).c)
            return
        if e[0] == 'assign':
            if e[1] == '=' and (e[2][0] == 'index' or e[2][:2] == ('un', '*')):
                self.store_item(e[2], e[3], st, out, frames)
                return
            if e[2][0] != 'id':
                raise ResumeHere('a store into %s' % cparse.c_expr(e[2]))
            name = e[2][1]
            if self.raw_step(st, out, name, e[1], e[3], frames):
                return
            if e[1] == '=':
                self.assign(st, out, name, e[3], frames)
            elif e[1] in ('+=', '-=') and st.vars.get(name) is not None \
                    and st.vars[name].bt == 'ct':
                val = self.ev(e[3], st, out, frames)
                if val.bt != 'ct':
                    self.force_rt(name)
                out('%s %s %s;' % (st.vars[name].cname, e[1], val.c))
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
            if fn in ('Py_INCREF', 'Py_XINCREF') and st.pending is not None \
                    and (cparse.c_expr(e[2][0]) == st.pending[0]
                         or e[2][0] == ('id', st.pending[1])):
                st.pending = None     # the store took the reference
                return
            if fn == 'Py_INCREF' and self.incref_returned(e[2][0], st, frames):
                return
            if fn in ('Py_INCREF', 'Py_XINCREF') and e[2][0] in (
                    ('id', 'Py_None'), ('id', 'Py_True'), ('id', 'Py_False')):
                return      # Ni's constant for it is made where it is used
            if fn == 'Py_XINCREF' and e[2][0][0] == 'id' and getattr(
                    st.vars.get(e[2][0][1]), 'known_null', False):
                return      # NULL here
            if fn in ('Py_INCREF', 'Py_XINCREF') and getattr(
                    self.var_of(st, e[2][0]), 'singleton', False):
                return      # it holds Ni's constant, a new reference already
            if fn in ('Py_INCREF', 'Py_XINCREF') and self.noref_incref(st, out, e[2][0]):
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
                if fn == 'Py_XDECREF' and v.owned == 'dead':
                    return        # NULL on this path: nothing to release
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

    def incref_returned(self, x, st, frames):
        """'Py_INCREF(E); return E;' with E no variable (an item read): the
        return gives the reference (rule incref_return in ret); True when
        the incref is left to it."""
        while x[0] == 'cast':
            x = x[2]
        if x[0] == 'id':
            return False
        stmts, i = frames[-1]
        nxt = stmts[i + 1] if i + 1 < len(stmts) else None
        if nxt is None or nxt[0] != 'return' or nxt[1] is None:
            return False
        r = nxt[1]
        while r[0] == 'cast':
            r = r[2]
        if cparse.c_expr(r) != cparse.c_expr(x):
            return False
        st.incref_return = cparse.c_expr(x)
        return True

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
        val = self.raw_load(e, st, out, owned=True) or self.singleton(e, st, out) \
            or self.ev(e, st, out, frames, dest=name)
        noref = getattr(val, 'noref', False) or getattr(val.var, 'noref', False)
        v.known_null = False
        singleton = getattr(val, 'singleton', False)
        if not v.isparam:
            st.known.discard(name)      # a known type (known_type) is the old value's
        if v.bt == 'ptr':
            if val.bt != 'ptr':
                raise ResumeHere('%s assigned a pointer the meta cannot follow' % name)
            out('%s = %s;' % (v.cname, val.c))
            v.base = val.base
            return
        if val.bt == 'ptr':
            raise ResumeHere('a pointer into items assigned to %s' % name)
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
                if v.ctype in INT_TYPES and not v.isparam:
                    self.force_rt(name)
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
                v.known_null = True     # returned, it is the error
                return
            if v.ctype in INT_TYPES and val.bt == 'ct':
                # an integer that is run-time around a loop: its first
                # value as a compile-time vinfo
                val = Val('rt', 'vinfo_new(CompileTime_New((long)(%s)))' % val.c,
                          v.ctype, owned=True)
            else:
                raise ResumeHere('%s assigned a compile-time value' % name)
        if val.var is v:
            return
        val = self.rep_assigning(v, val, out)
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
        v.alias = param_alias(val.var) if not val.owned else None
        v.conv = val.conv
        v.noref, v.singleton = noref, singleton
        v.virt = val.virt
        if val.conv and val.conv.get('checkable') and not val.conv.get('check') \
                and self.loop_ctx and self.errors_leave_loop(name, frames):
            val.conv = dict(val.conv, check='now')
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
        if e[0] == 'id' and getattr(st.vars.get(e[1]), 'known_null', False) \
                and st.vars[e[1]].owned == 'dead':
            e = ('id', 'NULL')      # 'v = NULL; ... return v;'
        val = self.rep_condition_value(e, st, out, frames) \
            or self.returned_length(e, st, out, frames) \
            or self.ev(e, st, out, frames, dest='<return>')
        x = e
        while x[0] == 'cast':
            x = x[2]
        if getattr(st, 'incref_return', None) is not None:
            # rule incref_return: the incref left to this return
            if cparse.c_expr(x) != st.incref_return or val.bt != 'rt':
                raise Refuse('an incref of %s not returned' % st.incref_return)
            st.incref_return = None
            if not val.owned and val.var is None:
                out('vinfo_incref(%s);' % val.c)
                val = Val('rt', val.c, val.ctype, owned=True, conv=val.conv)
        if val.bt == 'g':
            raise Refuse('returns a constant known now')
        if kind in ('obj', 'val'):
            if val.bt == 'ct':
                if val.c in ('NULL', '((void *)0)') or (kind == 'val' and (
                        val.c == '-1' or self.tr.error_value(self.f, e))):
                    # (a double's error is -1.0, scalars.error_value)
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
            if getattr(val.var, 'noref', False):
                raise Refuse('returns %s, read from memory with no reference' % val.var.name)
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

    def returned_length(self, e, st, out, frames):
        """Rule known_length, for a length that is only returned
        (tuplelength): the field read through its TYPES descriptor, which
        is the constant where Ni knows the length, so the meta needs no
        compile-time length and nothing resumes. None otherwise."""
        return self.length_value(e, st, out, frames)

    def length_value(self, e, st, out, frames):
        """e, the length of a parameter whose type has a known_length, read
        as a run-time value through the size field's TYPES descriptor
        (psyco_get_field: the constant where Ni knows it), when it is not
        loaded yet; None for anything else. For a length that is only a
        value (returned, or a bound tested: rules returned_length,
        bounds_test), not one a loop or a virtual needs now."""
        while e[0] == 'call' and e[1][0] == 'id' and e[1][1] in self.s.macros \
                and e[1][1] not in KEEP and self.s.macros[e[1][1]].params is not None:
            try:
                e = self.s.expand_call(e[1][1], e[2])
            except Resume:
                return None
        if e[0] != 'arrow':
            return None
        kl = self.known_length(e[1], st)
        if kl is None or e[2] != kl[1]['size'] or kl[0] in st.loaded:
            return None
        b = self.ev(e[1], st, out, frames)
        return self.object_field(b, e[2], st, out)

    def bounds_test(self, e, st, out, frames, facts_t, facts_f):
        """'x < 0 || x >= n', x a run-time integer and n a length (never
        negative): one unsigned compare, as C compilers make it. The
        branch's likelihood is a 'likely' item, as for any run-time
        branch. None for anything else."""
        if not (e[0] == 'bin' and e[1] == '||' and e[2][0] == 'bin' and e[2][1] == '<'
                and e[2][3] == ('num', '0') and e[3][0] == 'bin' and e[3][1] == '>='
                and e[2][2] == e[3][2] and e[2][2][0] == 'id'):
            return None
        v = st.vars.get(e[2][2][1])
        if v is None or v.bt != 'rt' or v.ctype not in INT_TYPES:
            return None
        n = self.length_value(e[3][3], st, out, frames)
        if n is None:
            return None
        cc = self.new_tmp('condition_code_t')
        out('%s = integer_cmp(po, %s, %s, Py_GE|COMPARE_UNSIGNED);' % (cc, v.cname, n.c))
        out('vinfo_decref(%s, po);' % n.c)
        self.error_exit(st, out, '%s == CC_ERROR' % cc)
        text = ' '.join(cparse.c_expr(e).replace('(', '').replace(')', '').split())
        how = self.likely.get(text)
        if how is None:
            raise Refuse("no 'likely' item in the policy table for the run-time"
                         " branch %r" % text)
        self.used_likely.add(text)
        t = self.new_tmp('int')
        out('%s = runtime_condition_%s(po, %s);' % (t, 't' if how == 'true' else 'f', cc))
        return t

    def field_array_read(self, e, st, out, frames):
        """'p->f[i]', i a run-time integer and f an array of objects that
        policy_table.TYPES gives a descriptor for (or the items of a
        known_length type, whose descriptor gen_virtual.py writes): the
        item read at run time (psyco_get_field_array), a new vinfo. None
        for anything else."""
        arr, idx = e[1], e[2]
        if arr[0] != 'arrow' or idx[0] != 'id':
            return None
        v = st.vars.get(idx[1])
        if v is None or v.bt != 'rt' or v.ctype not in INT_TYPES:
            return None
        b = self.ev(arr[1], st, out, frames)
        if b.var is None or not self.s.is_obj(b.ctype):
            return None
        bv = b.var
        if bv.alias is not None and bv.alias in st.vars:
            bv = st.vars[bv.alias]
        desc = field_descriptor(bv.ctype, arr[2]) or items_descriptor(bv.ctype, arr[2])
        ftype = self.s.field_type(bv.ctype, arr[2])
        # (an indexed field of object type is an array of them: cparse
        # keeps the element type, ob_item[1] is 'PyObject *')
        if desc is None or ftype is None or not self.s.is_obj(ftype):
            return None
        r = self.new_tmp('vinfo_t *')
        out('%s = psyco_get_field_array(po, %s, FPYREF(%s), %s);' % (r, bv.cname, desc, v.cname))
        self.error_exit(st, out, '%s == NULL' % r)
        return Val('rt', r, 'PyObject *', owned=True)

    def assert_restype(self, out, r, notimpl=False):
        for t in self.restype:
            # (a result that may be NotImplemented is asserted where not)
            out(('if (%s != NULL && !IS_NOTIMPLEMENTED(%s))' % (r, r)) if notimpl
                else 'if (%s != NULL)' % r)
            out('  Psyco_AssertType(po, %s, &%s);' % (r, t))

    def ret_value_owned(self, st, out, val):
        keep = val.var.name if val.var is not None else None
        if self.restype and val.var is None and val.virt is None:
            # (a virtual's type is in its array)
            r = val.c
            if not (r.startswith('_t') and r[2:].isdigit()):
                r = self.new_tmp('vinfo_t *')
                out('%s = %s;' % (r, val.c))
            self.assert_restype(out, r)
            val = Val('rt', r, val.ctype, owned=True, conv=val.conv)
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
            (st_t if when else st_f).failed = True
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
        rn = self.raw_null_test(c, st)
        if rn is not None:
            # rule raw_pointer: where the pointer read is NULL, the meta's
            # vinfo holds no object; that path is translated to its end
            name, when = rn
            sn, on = (st_t, out_t) if when else (st_f, out_f)
            x = sn.vars[name]
            on('vinfo_decref(%s, po);' % x.cname)
            x.owned, x.known_null = 'dead', True
        self.release_dead(st_t, out_t, self.tr.live.before[id(then)])
        self.branch(then, st_t, out_t, frames)
        if rn is not None and rn[1] and not st_t.ended and \
                getattr(st_t.vars[rn[0]], 'known_null', False):
            self.rest_of_path(st_t, out_t, frames)
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
        st.loaded = {k: v for k, v in a.loaded.items() if b.loaded.get(k) == v}
        for name in set(a.vars) | set(b.vars):
            va, vb = a.vars.get(name), b.vars.get(name)
            if va is None or vb is None:
                st.vars.pop(name, None)
                continue
            if va.bt != vb.bt:
                if va.ctype in INT_TYPES and not va.isparam and None not in (va.bt, vb.bt):
                    self.force_rt(name)
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
            v.noref = getattr(va, 'noref', False) or getattr(vb, 'noref', False)
            v.singleton = getattr(va, 'singleton', False) and getattr(vb, 'singleton', False)
            st.vars[name] = v
        st.ended = False

    # ---- conditions: a compile-time C boolean, or a Python bool if known now ----

    def cond(self, e, st, out, frames, facts_t=None, facts_f=None):
        r = self.rep_cond(e, st, out, frames) if self.reps() else None
        if r is None:
            r = self.assumed(e)
        if r is None:
            r = self.bit_test(e, st, out, frames)
        if r is not None:
            return r
        k = e[0]
        if k == 'un' and e[1] == '!' and (self.is_obj_param(e[2], st)
                                          or self.is_virtual(e[2], st)):
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
        if k == 'bin' and e[1] == '||':
            r = self.bounds_test(e, st, out, frames, facts_t, facts_f)
            if r is not None:
                return r
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
        if k == 'id' and e[1] in st.vars and st.vars[e[1]].bt == 'rt' \
                and st.vars[e[1]].ctype in INT_TYPES:
            # the truth of a run-time integer is 'x != 0' (P05)
            return self.cond(('bin', '!=', e, ('num', '0')), st, out, frames,
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
            if y == ('id', 'NULL') and op in ('==', '!=') and (
                    self.is_obj_param(x, st) or self.is_virtual(x, st)):
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
        if va.bt == 'rt' and vb.c == 'NULL' and op in ('==', '!=') and \
                getattr(va.var, 'noref', False):
            # rule raw_pointer: an object read from memory may be NULL
            vb = Val('ct', '0', 'long')
            va = Val('rt', va.c, 'long', var=va.var)
        if va.bt == 'rt' and vb.c == 'NULL' and op in ('==', '!=') and \
                va.ctype.endswith('*'):
            vb = Val('ct', '0', 'long')     # a C pointer: a word
        if self.s.is_obj(va.ctype) or self.s.is_obj(vb.ctype):
            raise ResumeHere('a compare of the object %s' % cparse.c_expr(a))
        cc = self.new_tmp('condition_code_t')
        call, inverted = int_condition(op, va.c, vb.c, vb.bt == 'ct')
        out('%s = %s;' % (cc, call))
        self.error_exit(st, out, '%s == CC_ERROR' % cc)
        if inverted:
            out('%s = INVERT_CC(%s);' % (cc, cc))
        text = ' '.join(cparse.c_expr(e).replace('(', '').replace(')', '').split())
        if text not in self.likely:
            text = self.likely_key(e) or text   # the C as written
        how = self.likely.get(text)
        if how is None:
            raise Refuse("no 'likely' item in the policy table for the run-time"
                         " branch %r" % text)
        self.used_likely.add(text)
        t = self.new_tmp('int')
        out('%s = runtime_condition_%s(po, %s);' % (t, 't' if how == 'true' else 'f', cc))
        self.choice_likely()[t] = how == 'true'
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
        return v.isparam and v.bt == 'rt' and self.s.is_obj(v.ctype) and v.owned != 'dead'

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
        if op == '==' and b in (('un', '-', ('num', '1')), ('un', '-', ('num', '1.0'))):
            return '%s == NULL' % v.cname
        return None

    # ---- expressions ----

    def ev(self, e, st, out, frames, dest=None):
        r = self.rep_ev(e, st, out, frames) or self.scalar_ev(e, st, out, frames)
        if r is not None:
            return r
        k = e[0]
        if k in ('id', 'cast'):
            r = self.raw_value(e, st, out)
            if r is not None:
                return r
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
                if self.s.is_obj(ctype) and self.s.is_obj(v.ctype):
                    return Val('rt', v.c, ctype, owned=v.owned, conv=v.conv,
                               var=v.var, item_of=v.item_of, virt=v.virt)
                if self.s.is_obj(ctype) or ctype in INT_TYPES:
                    return v
                if ctype in pointers.RAW_TYPES and self.s.is_obj(v.ctype) and \
                        dest is None:
                    # rule raw_pointer: an object's bytes, passed on
                    return Val('rt', v.c, ctype, owned=v.owned, var=v.var)
                raise ResumeHere('a cast of a run-time value to %s' % t)
            return v
        if k == 'arrow':
            return self.ev_arrow(e, st, out, frames)
        if k == 'un':
            return self.ev_unary(e, st, out, frames)
        if k == 'index':
            return self.ev_index(e, st, out, frames)
        if k in ('pre', 'post'):
            x = e[2]
            if x[0] == 'id' and x[1] in st.vars and st.vars[x[1]].bt in ('ct', 'ptr'):
                v = st.vars[x[1]]
                return Val(v.bt, ('%s%s' if k == 'pre' else '%s%s')
                           % ((e[1], v.cname) if k == 'pre' else (v.cname, e[1])), v.ctype,
                           base=v.base)
            if x[0] == 'id' and x[1] in st.vars and st.vars[x[1]].ctype in INT_TYPES \
                    and not st.vars[x[1]].isparam and st.vars[x[1]].bt is None:
                raise Refuse('%s read before it is assigned' % x[1])
            raise ResumeHere('%s of a run-time value' % e[1])
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
            if va.bt == 'ptr' and vb.bt == 'ct' and e[1] in ('+', '-'):
                return Val('ptr', '(%s %s %s)' % (va.c, e[1], vb.c), va.ctype, base=va.base)
            if e[1] == '+' and va.bt == 'rt' and vb.bt == 'rt':
                r = self.new_tmp('vinfo_t *')
                out('%s = integer_add(po, %s, %s, false);' % (r, va.c, vb.c))
                return Val('rt', r, va.ctype, owned=True,
                           conv={'check': 'now', 'checkable': False})
            raise ResumeHere('run-time arithmetic %s' % cparse.c_expr(e))
        if k == 'cond':
            c = self.cond(e[1], st, out, frames)
            r = self.choice(e, c, st, out, frames)
            if r is not None:
                return r
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
            if v.bt == 'ptr':
                if v.base is None:
                    raise Refuse('%s read before it is assigned' % name)
                return Val('ptr', v.cname, v.ctype, base=v.base)
            if v.bt == 'rt':
                if v.owned == 'dead' and not v.isparam:
                    raise Refuse('%s read with no value' % name)
                return Val('rt', v.cname, v.ctype, var=v, conv=v.conv, virt=v.virt)
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
        if v is not None and (not v.isparam or self.known_only()) and v.bt == 'rt' \
                and not entry:
            return self.known_type(st, out, v)
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

    def known_type(self, st, out, v):
        """The type of a value that is not a parameter (F24: it can't be
        promoted): the one Ni knows (a virtual's, a constant's), else CPython
        resumes from the statement."""
        tpv = 'tp_' + v.name
        if self.ncalls > self.stmt_calls:
            raise ResumeHere("the type of %s, which is not a parameter (F24)" % v.name)
        self.decls[tpv] = 'PyTypeObject *%s;' % tpv
        out('%s = Psyco_KnownType(%s);' % (tpv, v.cname))
        out('if (%s == NULL) {' % tpv)
        sub = Out(out.depth + 1)
        self.resume_inside(st.copy(), sub, self.stmt_frames,
                           'the type of %s is not known (F24)' % v.name)
        out.extend(sub)
        out('}')
        st.known.add(v.name)
        return tpv

    def ev_arrow(self, e, st, out, frames):
        base, field = e[1], e[2]
        b = self.ev(base, st, out, frames)
        if b.bt == 'rt':
            if self.s.is_obj(b.ctype) and field == 'ob_type' and b.var is not None:
                tpv = self.promote(st, out, b.var.name)
                return Val('ct', tpv, 'PyTypeObject *', tp=tpv)
            virt = b.virt if b.var is not None else None
            if virt is not None and virt['array']:
                # a virtual the meta builds: its size and items are its own
                if field == virt['size']:
                    return Val('ct', virt['size_c'], 'Py_ssize_t')
                if field == virt['items']:
                    return Val('ptr', '0', 'PyObject **', base=b.var.name)
            kl = self.known_length(base, st)
            if kl is not None and field in (kl[1]['size'], kl[1]['items']):
                # a length or the items of an object of known length: asked
                # of Ni's representation, CPython's function where unknown
                # (rules loops and virtual_types)
                self.ensure_loaded(kl, st, out, frames)
                if field == kl[1]['size']:
                    return Val('ct', st.loaded[kl[0]], 'Py_ssize_t')
                return Val('ptr', '0', 'PyObject **', base=kl[0])
            fv = self.object_field(b, field, st, out)
            if fv is not None:
                return fv
            raise ResumeHere('the field %s of a run-time value' % field)
        if b.bt != 'ct':
            raise ResumeHere('a field of %s' % cparse.c_expr(base))
        ftype = self.s.field_type(b.ctype, field)
        if b.tp is None and ftype is not None and b.ctype != 'global' \
                and b.ctype.replace('const ', '').rstrip(' *') not in ('PyTypeObject', '_typeobject'):
            # rule ct_struct (D89): a field of a C struct the compiler holds
            # (a PyMemberDef, a descriptor an 'l' parameter gives) is read
            # when compiling; such structs don't change once made
            return Val('ct', '%s->%s' % (paren(b.c), field), ftype)
        if b.tp is None or ftype is None:
            raise ResumeHere('the field %s of %s' % (field, cparse.c_expr(base)))
        if self.s.is_fnptr(ftype):
            return self.slot(st, out, frames, b.tp, '&%s->%s' % (paren(b.c), field), ftype)
        tp = b.tp if ftype in TABLE_TYPES else None
        return Val('ct', '%s->%s' % (paren(b.c), field), ftype, tp=tp)

    def object_field(self, b, field, st, out):
        """A field of a run-time object whose struct policy_table.TYPES
        lists with a descriptor for it (D84): read with psyco_get_field,
        again after every call if the field is mutable (the descriptor's
        FMUT). The struct is the variable's own type (through a cast like
        Py_SIZE's)."""
        if b.var is None or not self.s.is_obj(b.ctype):
            return None
        v = b.var
        if v.alias is not None and v.alias in st.vars:
            v = st.vars[v.alias]
        ctype = v.ctype
        desc = field_descriptor(ctype, field)
        if desc is None and b.ctype != v.ctype:
            # the struct a cast names ('((PyASCIIObject *)op)->length')
            ctype = b.ctype
            desc = field_descriptor(ctype, field)
        if desc is None:
            return None
        ftype = self.s.field_type(ctype, field) or \
            self.s.field_type('PyVarObject *', field)
        if ftype not in INT_TYPES:
            raise ResumeHere('the field %s, of type %s' % (field, ftype))
        r = self.new_tmp('vinfo_t *')
        out('%s = psyco_get_field(po, %s, %s);' % (r, v.cname, desc))
        self.error_exit(st, out, '%s == NULL' % r)
        return Val('rt', r, ftype, owned=True, conv={'checkable': False})

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
            r = self.raw_load(e, st, out)
            if r is not None:
                return r
            # a load from a type: *(T *)ADDRESS
            p = x
            ctype = None
            if p[0] == 'cast':
                ctype = ctype_of(ctokens.tokenize(p[1])[0])
                p = p[2]
            a = self.ev(p, st, out, frames)
            if a.bt == 'ptr' and ctype is None:
                return self.item_val(st, a, '0')
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
            if name in self.tr.vspecs:
                return self.construct(name, args, st, out, frames)
            if name in self.tr.sums and name not in self.tr.fails and name in self.tr.metas:
                return self.meta_call(name, args, st, out, frames, dest)
            if name in self.s.macros and self.s.macros[name].params is not None \
                    and name not in KEEP:
                return self.ev(self.s.expand_call(name, args), st, out, frames, dest)
            pname = psyco_name(name)
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

    def construct(self, name, args, st, out, frames):
        """A call of a virtual type's constructor (policy_table.TYPES,
        D80): the virtual object instead, built by Ni's 'new'. An array
        type's size must be known now and within the type's item limit,
        else CPython's function resumes here; its items are stored by the
        statements that follow (store_item)."""
        spec = self.tr.vspecs[name]
        vals = [self.ev(a, st, out, frames) for a in args]
        if spec['array']:
            if len(vals) != 1 or vals[0].bt != 'ct':
                raise ResumeHere('%s of a size not known now' % name)
            n = self.new_tmp('int')
            out('%s = %s;' % (n, vals[0].c))
            if spec['max_items'] and not vals[0].c.isdigit():
                out('if (%s > %s) {' % (n, spec['max_items']))
                sub = Out(out.depth + 1)
                self.resume(st.copy(), sub, frames,
                            'a %s of more than %s items' % (spec['type'], spec['max_items']))
                out.extend(sub)
                out('}')
            r = self.new_tmp('vinfo_t *')
            out('%s = %s(%s, NULL);' % (r, spec['new'], n))
            return Val('rt', r, 'PyObject *', owned=True, virt=dict(spec, size_c=n))
        if any(v.bt != 'rt' or v.owned for v in vals):
            raise ResumeHere('%s of a value that is not an object variable' % name)
        r = self.new_tmp('vinfo_t *')
        out('%s = %s(%s);' % (r, spec['new'], ', '.join(v.c for v in vals)))
        return Val('rt', r, 'PyObject *', owned=True, virt=dict(spec))

    def is_virtual(self, x, st):
        while x[0] == 'cast':
            x = x[2]
        return x[0] == 'id' and x[1] in st.vars and st.vars[x[1]].bt == 'rt' \
            and st.vars[x[1]].virt is not None and st.vars[x[1]].owned == 'yes'

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
            if self.s.is_obj(ctype):
                return 'null', False
            raise Refuse('returns a call of type %s' % ctype)
        if dest is None:
            return ('null' if self.s.is_obj(ctype) else 'neg'), False
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
                if s[0] == 'break':
                    break       # on after the switch or loop it leaves
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
                    self.late_promo(v.var.name)
                elif gf.params[k][1] in g.entry and out.depth == 1 and not st.early \
                        and v.var.name not in self.must:
                    self.must.append(v.var.name)
            elif v.var is not None and not v.var.isparam and self.s.is_obj(v.ctype):
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
        nulls = {i[2] for key, items in self.tr.policy.items() if key.endswith(':' + name)
                 for i in items if tuple(i[:2]) == ('ref', 'null_arg')}
        for (ptype, pname), a in zip(decl.params[1:], args):
            v = self.ev(a, st, out, frames)
            if ptype == 'vinfo_t *' and v.bt == 'ct' and pname in nulls and \
                    v.c.replace(' ', '') in ('NULL', '((PyObject*)NULL)'):
                # policy ('ref', 'null_arg', p): Ni's meta takes a NULL vinfo
                vals.append(Val('ct', 'NULL', 'void *'))
                continue
            if ptype == 'vinfo_t *' and v.bt != 'rt':
                raise ResumeHere('%s: a compile-time argument' % decl.name)
            if ptype != 'vinfo_t *' and v.bt != 'ct':
                raise ResumeHere('%s: a run-time argument' % decl.name)
            if v.bt == 'rt' and v.owned:
                raise ResumeHere('a call as an argument')
            if v.bt == 'rt' and self.s.is_obj(v.ctype):
                if v.item_of is not None:
                    # an item of a loaded object is in the frame (F24)
                    if 'items:' + v.item_of not in st.known and st.called:
                        self.late_promo(('items', v.item_of))
                elif v.var is None or not v.var.isparam:
                    raise ResumeHere('%s of a value that is not a parameter' % decl.name)
                elif v.var.name not in st.known and st.called:
                    self.late_promo(v.var.name)
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

    # ---- loops (rule loops, D78) ----

    def force_rt(self, name):
        """An integer local that a loop makes run-time: from the start."""
        fr = self.tr.force_rt.setdefault(self.f.name, set())
        if name in fr:
            raise Refuse('%s is compile-time and run-time at once' % name)
        fr.add(name)
        raise Rewalk()

    def late_promo(self, p):
        """A promotion needed after a call: at the entry of the outermost
        loop being translated if nothing was called before it, else at the
        function's entry (rule 1). p is a parameter, or ('items', name)."""
        if self.loop_ctx and not self.loop_ctx[0].entry_called:
            lp = self.tr.loop_promos.setdefault(self.f.name, {}).setdefault(
                self.loop_ctx[0].sid, [])
            if p not in lp:
                lp.append(p)
                raise Rewalk()
            return
        if isinstance(p, tuple):
            raise Refuse('the types of the items of %s are needed after a call' % p[1])
        if p not in self.late:
            self.late.append(p)

    def known_length(self, e, st):
        """(variable, representation) when e is a parameter (through casts)
        of an object type whose length and items Ni can know at compile
        time (policy_table.TYPES 'known_length')."""
        casts = []
        while e[0] == 'cast':
            casts.append(ctype_of(ctokens.tokenize(e[1])[0]))
            e = e[2]
        if e[0] != 'id' or e[1] not in st.vars:
            return None
        v = st.vars[e[1]]
        if v.alias is not None and v.alias in st.vars:
            casts.append(v.ctype)
            v = st.vars[v.alias]      # a local copy of a parameter is it
        if not v.isparam or v.bt != 'rt':
            return None
        for ctype in casts + [v.ctype]:
            r = known_length_of(ctype)
            if r is not None:
                self.kl_rep[v.name] = r
                return v.name, r
        return None

    def loads_in(self, e, st, out=None):
        """The objects whose length the expression e reads (the size field
        of a type with a known length), macros expanded."""
        if out is None:
            out = []
        if isinstance(e, tuple) and e:
            if e[0] == 'call' and e[1][0] == 'id' and e[1][1] in self.s.macros \
                    and e[1][1] not in KEEP and self.s.macros[e[1][1]].params is not None:
                try:
                    return self.loads_in(self.s.expand_call(e[1][1], e[2]), st, out)
                except Resume:
                    return out
            if e[0] == 'arrow':
                kl = self.known_length(e[1], st)
                if kl is not None and e[2] == kl[1]['size'] and \
                        kl[0] not in [n for n, _ in out]:
                    out.append(kl)
            for x in e[1:]:
                self.loads_in(x, st, out)
        elif isinstance(e, list):
            for x in e:
                self.loads_in(x, st, out)
        return out

    def ev_index(self, e, st, out, frames):
        arr, idx = e[1], e[2]
        r = self.field_array_read(e, st, out, frames)
        if r is not None:
            return r
        a = self.ev(arr, st, out, frames)
        if a.bt == 'ptr':
            i = self.ev(idx, st, out, frames)
            if i.bt == 'ct':
                return self.item_val(st, a, i.c)
        raise ResumeHere('an index %s' % cparse.c_expr(e))

    def item_access(self, st, a, off):
        """The C lvalue of item off (a meta expression) of the object the
        pointer a points into: its vinfo in the object's array."""
        v = st.vars[a.base]
        if v.virt is not None:
            acc = v.virt['item']
        else:
            if a.base not in self.kl_rep or a.base not in st.loaded:
                raise ResumeHere('the items of %s' % a.base)
            acc = ni_short(self.kl_rep[a.base]['item'])
        if off == '0':
            o = a.c
        elif a.c == '0':
            o = off
        else:
            o = '%s + %s' % (paren(a.c), paren(off))
        return '%s(%s, %s)' % (acc, v.cname, o)

    def item_val(self, st, a, off):
        """An item read through a pointer: its vinfo, borrowed. An item
        of a loaded parameter is in the frame (the object's array)."""
        v = st.vars[a.base]
        return Val('rt', self.item_access(st, a, off), 'PyObject *',
                   item_of=None if v.virt is not None else a.base)

    def store_item(self, lhs, rhs, st, out, frames):
        """'P[i] = x' or '*P = x' into a virtual the meta builds (D80):
        the item's vinfo is x. The object takes the reference x owns; a
        borrowed x is increfed now, and the C must incref it in its next
        statement (the reference it stores)."""
        if lhs[0] == 'index':
            a = self.ev(lhs[1], st, out, frames)
            i = self.ev(lhs[2], st, out, frames)
            off = i.c if i.bt == 'ct' else None
        else:
            a = self.ev(lhs[2], st, out, frames)
            off = '0'
        if a.bt != 'ptr' or off is None or st.vars[a.base].virt is None:
            raise ResumeHere('a store into %s' % cparse.c_expr(lhs))
        val = self.ev(rhs, st, out, frames)
        if val.bt != 'rt' or not self.s.is_obj(val.ctype):
            raise ResumeHere('a store of %s' % cparse.c_expr(rhs))
        item = self.item_access(st, a, off)
        out('%s = %s;' % (item, val.c))
        if val.owned:
            return
        if val.var is not None and val.var.owned == 'yes':
            val.var.owned = 'no'          # the object holds it now
            return
        if val.var is not None and val.var.owned == 'shadow':
            raise Refuse('%s stored while owned on some paths only' % val.var.name)
        out('vinfo_incref(%s);' % item)
        st.pending = (cparse.c_expr(lhs), val.var.name if val.var is not None else None)

    def ensure_loaded(self, kl, st, out, frames):
        """The length of the object kl names, from Ni's representation
        (PsycoTuple_Load): where it is unknown the meta resumes CPython's
        algorithm at the statement that reads it."""
        name, how = kl
        if name in st.loaded:
            return
        if self.loop_ctx:
            raise ResumeHere('the length of %s read first inside a loop' % name)
        if self.ncalls > self.stmt_calls:
            raise Refuse('the length of %s read after a call in its statement' % name)
        n = self.new_tmp('int')
        out('%s = %s(%s);' % (n, ni_short(how['load']), st.vars[name].cname))
        out('if (%s < 0) {' % n)
        sub = Out(out.depth + 1)
        self.resume(st.copy(), sub, frames,
                    'the length of %s is not known at compile time' % name)
        out.extend(sub)
        out('}')
        st.loaded[name] = n

    def loop_stmt(self, s, st, out, frames):
        """A loop with a compile-time bound is a loop of the meta: it runs
        at compile time, emitting its body once per turn (unrolled). Any
        other loop is CPython's: the meta resumes at it."""
        k = s[0]
        if k == 'for':
            init, cond, step, body = s[1], s[2], s[3], s[4]
        elif k == 'while':
            init, cond, step, body = None, s[1], [], s[2]
        else:
            init, cond, step, body = None, None, [], s[1]
        counted = self.tr.counted(s)
        if counted is None:
            raise ResumeHere('a loop with a run-time bound')
        # a length the bound reads: known at compile time, or the loop is
        # CPython's (rule loops, the known case and the fallback)
        for name, kl in self.loads_in(counted[1], st):
            if name in st.loaded:
                continue
            n = self.new_tmp('int')
            out('%s = %s(%s);' % (n, ni_short(kl['load']), st.vars[name].cname))
            out('if (%s < 0) {' % n)
            sub = Out(out.depth + 1)
            self.resume(st.copy(), sub, frames,
                        'the length of %s is not known at compile time' % name)
            out.extend(sub)
            out('}')
            st.loaded[name] = n
        if init is not None:
            if init[0] == 'expr':
                self.expr_stmt(init[1], st, out, frames)
            else:
                self.stmt(init, st, out, frames)
        if not self.loop_ctx:
            for p in self.tr.loop_promos.get(self.f.name, {}).get(id(s), []):
                self.loop_promote(st, out, p)
        entry_called = st.called if not self.loop_ctx else self.loop_ctx[0].entry_called
        head = st.copy()
        saved = (dict(self.decls), [list(x) for x in (self.must, self.late, self.returns,
                                                      self.resumed)],
                 set(self.used_likely), self.notimpl, self.any_call, self.ncalls)
        for _ in range(6):
            self.decls = dict(saved[0])
            self.must, self.late, self.returns, self.resumed = [list(x) for x in saved[1]]
            self.used_likely = set(saved[2])
            self.notimpl, self.any_call, self.ncalls = saved[3:]
            ctx = LoopCtx(id(s), entry_called, step, cond, frames)
            self.loop_ctx.append(ctx)
            exit_st = None
            try:
                bo = Out(out.depth + 1)
                sb = head.copy()
                c = True
                if cond is not None:
                    c = self.cond(cond, sb, bo, frames)
                if c is False:
                    return            # the loop never runs
                if c is not True:
                    bo('if (!(%s)) break;' % c)
                    exit_st = sb.copy()
                self.branch(body, sb, bo, frames)
                if not sb.ended:
                    self.walk_step(sb, bo, step, frames)
                    ctx.backs.append(sb)
            finally:
                self.loop_ctx.pop()
            new = self.loop_head(head, ctx.backs)
            if new is None:
                break
            head = new
        else:
            raise Refuse('the loop has no fixed point')
        out('for (;;) {')
        out.extend(bo)
        out('}')
        exits = ([exit_st] if exit_st is not None else []) + ctx.breaks
        if not exits:
            raise Refuse('a loop that only returns')
        final = exits[0]
        for x in exits[1:]:
            for name, v in final.vars.items():
                w = x.vars.get(name)
                if w is not None and v.bt == 'rt' and w.owned != v.owned and \
                        {v.owned, w.owned} != {'dead', 'no'}:
                    raise Refuse('%s is owned differently where the loop ends' % name)
            final.called = final.called or x.called
            final.known &= x.known
            final.slots = {a: b for a, b in final.slots.items() if x.slots.get(a) == b}
        st.__dict__.update(final.copy().__dict__)
        st.ended = False

    def loop_promote(self, st, out, p):
        if isinstance(p, tuple):
            name = p[1]
            kl = self.known_length(('id', name), st)
            if kl is None and name in self.kl_rep:
                kl = (name, self.kl_rep[name])     # loaded through a local copy
            if name not in st.loaded or kl is None:
                raise Refuse('the items of %s are needed before their length' % name)
            j = self.new_tmp('int')
            out('for (%s = 0; %s < %s; %s++) {' % (j, j, st.loaded[name], j))
            sub = Out(out.depth + 1)
            self.error_exit(st, sub, 'Psyco_NeedType(po, %s(%s, %s)) == NULL' % (
                ni_short(kl[1]['item']), st.vars[name].cname, j))
            out.extend(sub)
            out('}')
            st.known.add('items:' + name)
        else:
            self.promote(st, out, p, entry=True)

    def walk_step(self, st, out, step, frames):
        for x in step:
            self.expr_stmt(x, st, out, frames)

    def loop_head(self, head, backs):
        """The loop's entry state merged with its back edges; None when
        that is the entry state (the fixed point)."""
        new = head.copy()
        changed = False
        for b in backs:
            if b.called and not new.called:
                new.called = changed = True
            if not new.known <= b.known:
                new.known &= b.known
                changed = True
            sl = {a: c for a, c in new.slots.items() if b.slots.get(a) == c}
            if sl != new.slots:
                new.slots, changed = sl, True
            ld = {a: c for a, c in new.loaded.items() if b.loaded.get(a) == c}
            if ld != new.loaded:
                new.loaded, changed = ld, True
            for name, v in new.vars.items():
                w = b.vars.get(name)
                if w is None:
                    continue
                if v.bt is None:
                    continue      # unassigned at the head: the body assigns it first
                if v.bt != w.bt:
                    if v.ctype in INT_TYPES and not v.isparam:
                        self.force_rt(name)
                    raise Refuse('%s has another binding time after the loop body' % name)
                if v.bt == 'rt' and v.owned != w.owned and \
                        {v.owned, w.owned} != {'dead', 'no'}:
                    raise Refuse('%s is owned differently around the loop' % name)
                if v.conv != w.conv and v.conv is not None:
                    v.conv, changed = None, True
                if v.bt == 'ct' and v.tp != w.tp and v.tp is not None:
                    v.tp, changed = None, True
        return new if changed else None

    def const_return(self, st, out, name):
        """Py_RETURN_TRUE, Py_RETURN_FALSE, Py_RETURN_NONE: the object,
        a constant."""
        if self.sm.kind != 'obj':
            raise Refuse('%s from a non-object function' % name)
        self.release_all(st, out)
        out('return %s();' % CONST_RETURNS[name])
        st.ended = True
        self.returns.append('value')

    def switch_stmt(self, s, st, out, frames):
        """A switch on a compile-time value is a switch of the meta (D84):
        each case's statements are translated on their own path, which
        must end (return, or Py_UNREACHABLE's abort); a case running on
        into the next, or a break out of the switch, is refused."""
        _, c, body, span = s
        v = self.ev(c, st, out, frames)
        if v.bt == 'g':
            v = Val('ct', '1' if v.const else '0', 'int')
        if v.bt != 'ct':
            raise ResumeHere('a switch on a run-time value %s' % cparse.c_expr(c))
        stmts = block_of(body)
        groups, cur = [], None
        for i, x in enumerate(stmts):
            if x[0] in ('case', 'default'):
                if cur is None or cur[1]:
                    cur = ([], [])
                    groups.append(cur)
                cur[0].append(x)
            elif cur is None:
                raise Refuse('a statement before the first case of a switch')
            else:
                cur[1].append(i)
        out('switch (%s) {' % v.c)
        self.switch_depth += 1
        try:
            for labels, idxs in groups:
                for lab in labels:
                    if lab[0] == 'default':
                        out('default:')
                    else:
                        k = self.ev(lab[1], st, out, frames)
                        if k.bt != 'ct':
                            raise Refuse('a case label not known at compile time')
                        out('case %s:' % k.c)
                gst = st.copy()
                sub = Out(out.depth + 1)
                sub('{')
                inner = Out(out.depth + 2)
                for i in idxs:
                    if gst.ended:
                        break
                    x = stmts[i]
                    if x[0] == 'break':
                        # rule switch_break (D89): the statements after the
                        # switch, on this case's path; they must end it
                        self.switch_tail(gst, inner, frames)
                        break
                    self.statement(x, gst, inner, frames + [(stmts, i)])
                    if not gst.ended:
                        self.release_dead(gst, inner, self.tr.live.after[id(x)])
                if not gst.ended and idxs is groups[-1][1]:
                    self.switch_tail(gst, inner, frames)    # the last case ends the switch
                if not gst.ended:
                    raise Refuse('a case of a switch runs on into the next (%s)'
                                 % ', '.join(cparse.c_expr(lab[1]) if lab[0] == 'case'
                                             else 'default' for lab in labels))
                sub.extend(inner)
                sub('}')
                out.extend(sub)
        finally:
            self.switch_depth -= 1
        if not any(lab[0] == 'default' for labels, _ in groups for lab in labels):
            raise Refuse('a switch without a default')
        out('}')
        st.ended = True

    def raw_null_test(self, c, st):
        """(variable, True if NULL where c holds) when c compares with
        NULL an object read from memory (pointers.py)."""
        if c[0] == 'bin' and c[1] in ('==', '!=') and c[3] == ('id', 'NULL') \
                and c[2][0] == 'id' and getattr(st.vars.get(c[2][1]), 'noref', False):
            return c[2][1], c[1] == '=='
        return None

    def rest_of_path(self, st, out, frames):
        """The statements after the current one up to the function's end,
        translated on this path; a 'break' leaves the switch it is in."""
        if self.loop_ctx:
            raise Refuse('a path translated to its end inside a loop')
        fr = list(frames)
        while fr and not st.ended:
            outer, j = fr.pop()
            k = j + 1
            while k < len(outer) and not st.ended:
                x = outer[k]
                if x[0] == 'break':
                    while fr and fr[-1][0][fr[-1][1]][0] != 'switch':
                        fr.pop()
                    if not fr:
                        raise Refuse('a break outside a switch')
                    break
                if x[0] in ('label', 'continue'):
                    raise Refuse('a path to its end through %s' % x[0])
                self.statement(x, st, out, fr + [(outer, k)])
                if not st.ended:
                    self.release_dead(st, out, self.tr.live.after[id(x)])
                k += 1
        if not st.ended:
            raise Refuse('a path translated to its end that does not end')

    def switch_tail(self, st, out, frames):
        """A case's 'break': the rest of the block the switch is in,
        translated on the case's path, as if copied there."""
        if not frames:
            raise Refuse('a break out of a switch')
        outer, j = frames[-1]
        for k in range(j + 1, len(outer)):
            if st.ended:
                break
            x = outer[k]
            if x[0] in ('label', 'break', 'continue'):
                raise Refuse('a break out of a switch to %s' % x[0])
            self.statement(x, st, out, frames[:-1] + [(outer, k)])
            if not st.ended:
                self.release_dead(st, out, self.tr.live.after[id(x)])
        if not st.ended:
            raise Refuse('a break out of a switch that does not end the function')

    def jump(self, k, st, out):
        if not self.loop_ctx:
            raise Refuse('%s outside a loop' % k)
        ctx = self.loop_ctx[-1]
        if k == 'break':
            self.release_dead(st, out, self.tr.live.after[ctx.sid])
            ctx.breaks.append(st.copy())
            out('break;')
        else:
            self.walk_step(st, out, ctx.step, ctx.frames)
            ctx.backs.append(st.copy())
            out('continue;')
        st.ended = True

    def errors_leave_loop(self, name, frames):
        """A call's result in name, read next by the loop's condition with
        no error test: does the C's error value (-1) end the loop and reach
        'return name' with nothing run in between? Then the meta returns
        its error at once."""
        ctx = self.loop_ctx[-1]
        if self.sm.kind not in ('val', 'obj'):
            return False
        for stmts, i in frames[len(ctx.frames):]:
            for x in stmts[i + 1:]:
                if name in cparse.names_in(list(x[1:-1])):
                    return False
        if ctx.cond is None or not false_at(ctx.cond, name, -1):
            return False
        stmts, i = ctx.frames[-1]
        for x in stmts[i + 1:]:
            if name in cparse.names_in(list(x[1:-1])):
                return x[0] == 'return' and x[1] == ('id', name)
            if self.tr.find_any_call(x):
                return False
        return False

    def goto(self, s, st, out, frames):
        """A forward goto to a label of the function's top level: the meta
        goes on with the statements from the label (each goto gets its own
        copy of them, as a meta path does)."""
        if self.loop_ctx:
            raise Refuse('goto inside a loop')
        top = self.f.ast
        j = next((k for k, x in enumerate(top) if x[0] == 'label' and x[1] == s[1]), None)
        if j is None or frames[0][0] is not top or j <= frames[0][1]:
            raise Refuse('goto %s: not a forward jump to the top level' % s[1])
        if self.goto_depth > 8:
            raise Refuse('gotos nest too deep')
        self.release_dead(st, out, self.tr.live.before[id(top[j])])
        self.goto_depth += 1
        try:
            for idx in range(j, len(top)):
                if st.ended:
                    break
                self.statement(top[idx], st, out, [(top, idx)])
                if not st.ended:
                    self.release_dead(st, out, self.tr.live.after[id(top[idx])])
        finally:
            self.goto_depth -= 1
        if not st.ended:
            raise Refuse('the statements at %s do not return' % s[1])


def psyco_name(name):
    """Ni's name for the meta of a CPython function: PyX -> PsycoX,
    _PyX -> _PsycoX; None for other names."""
    if name.startswith('Py'):
        return 'Psyco' + name[2:]
    if name.startswith('_Py'):
        return '_Psyco' + name[3:]
    return None


def block_of(s):
    return s[1] if s[0] == 'block' else [s]


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
    loops=True) has a bound fixed by the C: a 'for' whose condition has a conjunct
    'V < E', 'V <= E' or 'V != E' with V stepped up by one in the loop's
    head, or 'V > E', 'V >= E' or 'V != E' with V stepped down by one, V
    assigned nowhere else in the loop and E assigned nowhere in it. Any
    other loop runs as long as run-time values say: None. start is what
    the loop's head assigns V, or None. Whether E and the start are known
    at compile time is the caller's to find (loop_rule)."""
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
    items = []
    if init[0] == 'expr':
        e = init[1]
        items = e[1] if e[0] == 'comma' else [e]
        for x in items:
            if x[0] == 'assign' and x[1] == '=' and x[2] == ('id', name):
                return x[3]
    elif init[0] == 'decl':
        for d in init[1]:
            if d[1] == name:
                return d[2]
    return None


def loop_rule(s, bound_known):
    """The loops rule (D78, policy_table.RULES['loops']) for the cparse
    loop s, shared with gen_ceval.py. bound_known(e) says whether the C
    expression e is known at compile time. Returns ('unroll', variable,
    bound, start), when the bound and the start (if the head sets one)
    are known: the meta runs the loop at compile time and emits its body once
    per turn; or ('residual', why): the loop is CPython's, run by a call
    to the function or a cimpl_ copy of the rest of it, after any known
    case the meta handles first. A bound that reads the length of an
    object of a TYPES known_length type is known when that object's
    length is (the walk loads it, and falls back when it isn't)."""
    c = counted_loop(s)
    if c is None:
        return ('residual', 'a loop with a run-time bound')
    for e in (c[1], c[2]):
        if e is not None and not bound_known(e):
            return ('residual', '%s is not known at compile time'
                    % cparse.c_expr(e))
    return ('unroll',) + c


class LoopCtx:
    """A loop the meta is translating."""

    def __init__(self, sid, entry_called, step, cond, frames):
        self.sid = sid                    # id() of the loop statement
        self.entry_called = entry_called  # a call was made before the outermost loop
        self.step, self.cond, self.frames = step, cond, frames
        self.breaks = []                  # states at a break
        self.backs = []                   # states at the end of the body or a continue


def virtual_specs():
    """{CPython constructor: spec} for the virtual types gen_virtual.py
    generates (policy_table.TYPES, D80): a call of one in a meta builds the
    virtual instead (Ni's 'new'). An array type's spec has its size and
    items fields, the item accessor and the item limit."""
    import gen_virtual
    out = {}
    for vt in gen_virtual.virtual_types():
        kl = vt.entry.get('known_length') or {}
        out[vt.ctor] = {'type': vt.type, 'new': vt.new, 'array': vt.array,
                        'size': vt.size, 'items': vt.items,
                        'item': kl.get('item') and ni_short(kl['item']),
                        'max_items': vt.entry['virtual'].get('max_items')}
    return out


def known_length_of(ctype):
    """policy_table.TYPES' 'known_length' of an object struct pointer."""
    struct = ctype.replace('const ', '').rstrip(' *').replace('struct ', '').strip()
    for key, t in policy_table.TYPES.items():
        if key.split(':')[1] == struct and 'known_length' in t:
            kl = t['known_length']
            if kl['size'] in t.get('immutable', ()) and kl['items'] in t.get('immutable', ()):
                return kl
    return None


def field_descriptor(ctype, field):
    """policy_table.TYPES' descriptor for a field of an object struct
    pointer, or None."""
    struct = ctype.replace('const ', '').rstrip(' *').replace('struct ', '').strip()
    for key, t in policy_table.TYPES.items():
        if key.split(':')[1] == struct:
            d = t.get('descriptors', {}).get(field)
            if d is not None:
                return ni_short(d)
    return None


def items_descriptor(ctype, field):
    """The descriptor gen_virtual.py writes for the items of a virtual
    type with a known_length (TUPLE_ob_item), when field is them; None
    otherwise."""
    import gen_virtual
    struct = ctype.replace('const ', '').rstrip(' *').replace('struct ', '').strip()
    for vt in gen_virtual.virtual_types():
        if vt.struct == struct and vt.array and vt.items == field:
            return vt.field(field)
    return None


def ni_short(path_name):
    """'ni/Objects/x.h:Name' -> 'Name'."""
    return path_name.rpartition(':')[2]


def false_at(e, name, value):
    """Is the condition e false when name holds value? (an && chain with a
    conjunct comparing name with an integer constant)"""
    if e[0] == 'bin' and e[1] == '&&':
        return false_at(e[2], name, value) or false_at(e[3], name, value)
    if e[0] == 'bin' and e[1] in PY_OPS and e[2] == ('id', name):
        b = e[3]
        neg = b[0] == 'un' and b[1] == '-'
        if neg:
            b = b[2]
        if b[0] != 'num' or not b[1].isdigit():
            return False
        n = -int(b[1]) if neg else int(b[1])
        return not {'<': value < n, '<=': value <= n, '==': value == n,
                    '!=': value != n, '>': value > n, '>=': value >= n}[e[1]]
    if e[0] == 'un' and e[1] == '!' and e[2] == ('id', name):
        return value != 0
    return False


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

class Unit:
    """One C file the translator reads: its metas are those Ni declares
    in 'decls' ('Psyco' + the name), and the functions filling the slots
    policy_table.TRANSLATED_SLOTS names for the file, which the header
    registers in gen_<file>_init()."""

    def __init__(self, cfile, decls, includer):
        self.cfile, self.decls, self.includer = cfile, decls, includer


UNITS = {
    'abstract': Unit(CFILE, NI_DECLS, 'pabstract.c'),
    'tuple': Unit('Objects/tupleobject.c', None, 'ptupleobject.c'),
    'list': Unit('Objects/listobject.c', None, 'plistobject.c'),
    'dict': Unit('Objects/dictobject.c', None, 'pdictobject.c'),
    'bytes': Unit('Objects/bytesobject.c', None, 'pbytesobject.c'),
    'unicode': Unit('Objects/unicodeobject.c', None, 'punicodeobject.c'),
    'structmember': Unit('Python/structmember.c', 'ni/Objects/pstructmember.h', 'pstructmember.c'),
    'descr': Unit('Objects/descrobject.c', None, 'pdescrobject.c'),
    'long': Unit('Objects/longobject.c', None, 'plongobject.c'),
    'float': Unit('Objects/floatobject.c', 'ni/Objects/pfloatobject.h', 'pfloatobject.c'),
    'object': Unit('Objects/object.c', None, 'pobject.c'),
}


def slot_specs(cfile):
    """The TRANSLATED_SLOTS specs for one C file: an entry is one spec
    ({'type': ..., 'slots': [...]}) or a list of them, one per type the
    file defines."""
    spec = policy_table.TRANSLATED_SLOTS.get(cfile)
    if spec is None:
        return []
    return spec if isinstance(spec, list) else [spec]


def slot_ref(tp, tfield, struct, slot):
    """The C expression for a type's slot: a field of one of its tables
    ('PyTuple_Type.tp_as_sequence->sq_contains'), or a function-pointer
    field of the type itself ('PyTuple_Type.tp_richcompare': struct
    None)."""
    if struct is None:
        return '%s.%s' % (tp, slot)
    return '%s.%s->%s' % (tp, tfield, slot)


def param_alias(var):
    """The parameter a value read from var stands for: var itself if a
    parameter, the one var copies, else None."""
    if var is None:
        return None
    if var.isparam:
        return var.name
    return var.alias


def slot_format(tr, fname):
    """Ni's argument format for the slot a translated slot function fills
    (policy_table.SLOT_ARGS), or None: vinfos only."""
    where = tr.slot_roots.get(fname)
    if where is None:
        return None
    spec = policy_table.SLOT_ARGS.get(where[2])
    return spec and spec['format']


def slot_binding(f, fmt):
    """The parameters' binding times from Ni's call format: 'l' a C value
    the meta is given (compile-time, objects included), 'v' a vinfo."""
    if len(fmt) != len(f.params):
        raise cparse.ParseError('%s takes %d parameters; Ni calls its slot with'
                                ' "%s"' % (f.name, len(f.params), fmt))
    return ['ct' if c == 'l' else 'rt' for c in fmt]


def type_tokens(source, tp):
    """The tokens of the file that holds type tp's initialiser: the unit's
    own, or else the one Objects/ file that defines it, for a slot of a
    type defined elsewhere that a function of this file fills
    (PySeqIter_Type.tp_iter is object.c's PyObject_SelfIter)."""
    toks = source.toks
    if any(t.text == tp and toks[i + 1].text == '=' for i, t in enumerate(toks[:-1])):
        return toks
    pat = re.compile(r'^PyTypeObject\s+%s\s*=' % re.escape(tp), re.M)
    found = []
    odir = os.path.join(source.src, 'Objects')
    for name in sorted(os.listdir(odir)):
        if name.endswith('.c'):
            with open(os.path.join(odir, name), encoding='latin-1') as f:
                text = f.read()
            if pat.search(text):
                found.append((name, text))
    if len(found) != 1:
        raise cparse.ParseError('type %s: %d initialisers in Objects/'
                                % (tp, len(found)))
    return ctokens.tokenize(found[0][1], 'Objects/' + found[0][0])[0]


def expanded_initializer(source, var_name):
    """The entries of 'T var_name = { ... };' with the file's macros
    expanded first, so a head macro holding its own comma
    (PyVarObject_HEAD_INIT) gives its fields; casts removed, as
    ctokens.initializer gives them."""
    toks = type_tokens(source, var_name)
    for i, t in enumerate(toks):
        if t.text == var_name and toks[i + 1].text == '=' and toks[i + 2].text == '{':
            end = ctokens.match_close(toks, i + 2)
            body = cparse.expand(toks[i + 3:end], source.macros, set())
            out = []
            for e in ctokens.split_top(body, ','):
                if e and e[0].text == '{':
                    # a nested struct (the head): one entry per field of it
                    # is not needed, it is the ob_base field
                    e = e[:ctokens.match_close(e, 0) + 1]
                while len(e) > 1 and e[0].text == '(':
                    e = e[ctokens.match_close(e, 0) + 1:]
                out.append(' '.join(x.text for x in e))
            return out
    raise cparse.ParseError('no initializer %s in %s' % (var_name, source.cfile))


def type_field_slot(source, spec, slot, tfields, entries):
    """A slot that is a function-pointer field of PyTypeObject itself
    (tp_richcompare, tp_iter, tp_iternext, tp_descr_get), read from the
    type's initialiser at that field's place: (function, (slot, None,
    slot)); None when the slot is not such a field."""
    if slot not in tfields or not source.is_fnptr(tfields[slot]):
        return None
    j = list(tfields).index(slot)
    entries = expanded_initializer(source, spec['type'])
    if j >= len(entries) or entries[j] not in source.funcs:
        raise cparse.ParseError('%s.%s is not a function of %s'
                                % (spec['type'], slot, source.cfile))
    return entries[j], (slot, None, slot)


def slot_functions(source, spec):
    """{function: (type field, table struct, slot)} for the slots spec
    names ({'type': 'PyTuple_Type', 'slots': [...]}): the file's table of
    that struct which the type's initialiser points to, read in the
    struct's field order; for a slot that is a field of the type itself,
    the initialiser's own entry (struct None)."""
    tfields = source.decls.structs['_typeobject']
    entries = ctokens.initializer(type_tokens(source, spec['type']), spec['type'])
    toks = source.toks
    out = {}
    for slot in spec['slots']:
        found = type_field_slot(source, spec, slot, tfields, entries)
        if found is not None:
            out[found[0]] = found[1]
            continue
        for tfield, ftype in tfields.items():
            if ftype not in TABLE_TYPES:
                continue
            struct = ftype.rstrip(' *')
            sfields = list(source.decls.structs.get(struct, {}))
            if slot not in sfields:
                continue
            tables = [toks[i + 1].text for i in range(len(toks) - 2)
                      if toks[i].text == struct and toks[i + 2].text == '='
                      and '& ' + toks[i + 1].text in entries]
            if len(tables) != 1:
                raise cparse.ParseError('%s: %d %s tables in %s' % (
                    spec['type'], len(tables), struct, source.cfile))
            table = ctokens.initializer(toks, tables[0])
            j = sfields.index(slot)
            if j >= len(table) or table[j] not in source.funcs:
                raise cparse.ParseError('%s->%s is not a function of %s'
                                        % (tables[0], slot, source.cfile))
            found = (table[j], (tfield, struct, slot))
        if found is None:
            raise cparse.ParseError("no slot %s in PyTypeObject's tables" % slot)
        out[found[0]] = found[1]
    return out


def generate(src, pyconfig, defines, ni_root=ROOT, effects=None, unit=None):
    unit = unit or UNITS['abstract']
    source = Source(src, pyconfig, defines, unit.cfile)
    ni = ni_decls(ni_root, NI_HEADERS)
    declared = set(ni_decls(ni_root, [unit.decls])) if unit.decls else set()
    policy = {k: e.get('policy', []) for k, e in policy_table.ENTRIES.items()}
    tr = Translator(source, ni, policy, effects)
    tr.slot_roots, tr.slot_types = {}, {}
    tr.header_asserts = set()     # rule raw_pointer's
    registered = {}               # one function filling several types' slots
    for spec in slot_specs(unit.cfile):
        for fname, where in slot_functions(source, spec).items():
            tr.slot_roots.setdefault(fname, where)
            tr.slot_types.setdefault(fname, spec['type'])
            registered.setdefault(fname, []).append(slot_ref(spec['type'], *where))
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
    used = {m for m in by_meta if m in declared or by_meta[m] in tr.slot_roots}
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
            '   %s: do not edit. Included by %s. */' % (unit.cfile, unit.includer), '']
    used = sorted({r for r in (getattr(tr, 'word_reps', None) or {}).values() if r.used},
                  key=lambda r: r.struct)
    if used:
        head += ['_Static_assert(sizeof(long) * 8 == %d, "words are longs (D83)");'
                 % WORD_BITS, '_Static_assert(sizeof(int) * 8 == 32, "C_INT_BITS");']
        head += [a for r in used for a in r.asserts()] + ['']
    if tr.header_asserts:
        head += sorted(tr.header_asserts) + ['']
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
    if tr.slot_roots:
        init = os.path.splitext(os.path.basename(unit.cfile))[0]
        body.append('/* the metas of the slots policy_table.TRANSLATED_SLOTS names */')
        body.append('static void gen_%s_init(void) {' % init)
        for n in sorted(tr.slot_roots):
            if n in tr.metas:
                for ref in registered[n]:
                    body.append('  Psyco_DefineMeta(%s, %s);' % (ref, tr.sums[n].meta))
        body.append('}')
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


scalars.GA = sys.modules[__name__]
conditions.GA = sys.modules[__name__]
pointers.GA = sys.modules[__name__]


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


def build_tuple(target):
    """ptupleobject_gen.h: the tuple slots the translator generates."""
    return build(target, UNITS['tuple'])


def build_list(target):
    """plistobject_gen.h: the list slots the translator generates."""
    return build(target, UNITS['list'])


def build_dict(target):
    """pdictobject_gen.h: the dict slots the translator generates."""
    return build(target, UNITS['dict'])


def build_bytes(target):
    """pbytesobject_gen.h: the bytes slots the translator generates."""
    return build(target, UNITS['bytes'])


def build_unicode(target):
    """punicodeobject_gen.h: the str slots the translator generates (D89)."""
    return build(target, UNITS['unicode'])


def build_structmember(target):
    """pstructmember_gen.h: PyMember_GetOne (D89)."""
    return build(target, UNITS['structmember'])


def build_descr(target):
    """pdescrobject_gen.h: the descriptors' __get__ slots (D89)."""
    return build(target, UNITS['descr'])


def build_long(target):
    """plongobject_gen.h: the int slots the translator generates (D83)."""
    return build(target, UNITS['long'])


def build_object(target):
    """pobject_gen.h: object.c's functions that fill other types' slots."""
    return build(target, UNITS['object'])


def build_float(target):
    """pfloatslots_gen.h: the float slots the translator generates (D83)."""
    return build(target, UNITS['float'])


def build(target, unit=None):
    try:
        import effects
        g = effects.analyse(target.src)
    except Exception as e:  # noqa: BLE001 - the build must say why
        raise cslots.GenError('effects.py: %s' % e)
    try:
        text, report = generate(target.src, target.pyconfig,
                                target_defines(target.python), effects=g, unit=unit)
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
    ap.add_argument('--unit', default='abstract', choices=sorted(UNITS))
    args = ap.parse_args(argv)
    g = None
    if not args.no_effects:
        import effects
        g = effects.analyse(args.src)
    defines = target_defines(args.python) if args.python else ['Py_BUILD_CORE', 'NDEBUG']
    text, report = generate(args.src, args.pyconfig, defines, effects=g,
                            unit=UNITS[args.unit])
    if args.report:
        print('\n'.join(report))
    else:
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
