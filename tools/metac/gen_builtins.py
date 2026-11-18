#!/usr/bin/env python3
"""Metas of a module's builtin functions, by the general translator; first
on Python/bltinmodule.c, written into pbltinmodule_gen.h, which
pbltinmodule.c includes.

What is translated: the functions policy_table.TRANSLATED_FUNCTIONS names
for the file, by their Python names. Which C function each name stands
for, and its calling convention, come from the source: the module's
PyModuleDef names its method table, whose entries (Argument Clinic's
*_METHODDEF macros expanded) give the name, the C function and its
ml_flags. The generated gen_<file>_init(md) registers each meta under
those flags (Psyco_DefineModuleFn).

How: gen_abstract.py's translator, with these rules
on top, each general:
  - a root's parameters follow how Ni calls a meta for its ml_flags
    (policy_table.METH_ARGS): METH_O's (self, arg) are vinfos; a
    METH_FASTCALL function's C array and count, (args, nargs), are the one
    tuple Ni passes, whose length is asked of Ni's representation
    (PsycoTuple_Load);
  - a call of a function whose policy entry has ('other',
    'unpack_arguments') binds its outputs to the array's items when the
    count is within the bounds the call passes; the count is a C value of
    the meta, so the rest of the function is translated once for each
    count in the bounds (outputs past the count keep their value). Any
    other count, or one not known, is CPython's function: the C
    function through Ni's convention for its flags (METH_ARGS 'fallback'),
    which raises CPython's own message;
  - Argument Clinic's wrappers need no rule of their own: a wrapper is a
    function that unpacks or converts and calls its _impl, which is a
    static function of the file and is translated as any other;
  - where a root resumes before anything was called, it is the C
    function called through that fallback; Py_None, Py_True and Py_False
    as values are Ni's constants; an integer typedef (Py_hash_t) is the
    type it names.
Everything else is gen_abstract's, and refuses as it does.

Run by generate.py; 'python3 gen_builtins.py --src DIR --pyconfig H'
prints the header and the report.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cparse  # noqa: E402
import csource  # noqa: E402
import ctokens  # noqa: E402
import cslots  # noqa: E402
import cpython_src  # noqa: E402
import gen_abstract as ga  # noqa: E402
import policy_table  # noqa: E402

ROOT = ga.ROOT
# Ni headers searched for a CPython function's meta ('Psyco' + name)
NI_HEADERS = ga.NI_HEADERS + ['ni/Objects/pboolobject.h',
                              'ni/Python/pbltinmodule.h']


class Unit:
    def __init__(self, cfile, includer):
        self.cfile, self.includer = cfile, includer


UNITS = {
    'bltin': Unit('Python/bltinmodule.c', 'pbltinmodule.c'),
    'math': Unit('Modules/mathmodule.c', 'pmath.c'),
}


# ---- reading the file and what it includes ----------------------------------

class Source(ga.Source):
    """A C file with the files it includes by a relative "name" spliced
    in (Argument Clinic's clinic/*.c.h), as the preprocessor reads it.
    files: [(path, first line, last line)] of the spliced text."""

    def read_text(self):
        self.files = self.line_files(self.cfile)
        return self.splice(self.cfile, 0)

    def includes(self, path, line, depth):
        """The file a line includes by a relative name, or None."""
        m = re.match(r'\s*#\s*include\s+"([^"]+)"', line)
        if not m or depth >= 4:
            return None
        sub = os.path.normpath(os.path.join(os.path.dirname(path), m.group(1)))
        return sub if os.path.exists(os.path.join(self.src, sub)) else None

    def splice(self, path, depth):
        with open(os.path.join(self.src, path), encoding='latin-1') as f:
            lines = f.read().split('\n')
        out = []
        for ln in lines:
            sub = self.includes(path, ln, depth)
            if sub:
                out.append('')       # the directive's line, empty
                out.extend(self.splice(sub, depth + 1).split('\n'))
            else:
                out.append(ln)
        return '\n'.join(out)

    def line_files(self, path):
        """[(path, first, last)] of the spliced text's lines."""
        spans = []

        def walk(p, start, depth):
            with open(os.path.join(self.src, p), encoding='latin-1') as f:
                lines = f.read().split('\n')
            ln = seg = start
            for text in lines:
                sub = self.includes(p, text, depth)
                if sub:
                    spans.append((p, seg, ln))
                    ln = walk(sub, ln + 1, depth + 1)
                    seg = ln
                else:
                    ln += 1
            spans.append((p, seg, ln - 1))
            return ln
        walk(path, 1, 0)
        return spans

    def file_of(self, name):
        """The file a function of the spliced text is defined in."""
        f = self.funcs.get(name)
        line = f.line if f is not None and hasattr(f, 'line') else None
        if line is None:
            return self.cfile
        for p, lo, hi in self.files:
            if lo <= line <= hi:
                return p
        return self.cfile

    def add_func(self, name, d, keep_local):
        super().add_func(name, d, keep_local)
        self.funcs[name].line = d.body[0].line if d.body else 0


def module_table(source):
    """(module name, [(Python name, C function, ml_flags text)]) from the
    file's PyModuleDef and the method table it names, macros expanded."""
    toks = source.toks
    defs = [toks[i + 1].text for i in range(len(toks) - 2)
            if toks[i].text == 'PyModuleDef' and toks[i + 2].text == '=']
    if len(defs) != 1:
        raise cparse.ParseError('%s: %d PyModuleDef definitions' % (source.cfile, len(defs)))
    head = ga.expanded_initializer(source, defs[0])
    # PyModuleDef_HEAD_INIT, m_name, m_doc, m_size, m_methods, ...
    fields = list(source.decls.structs.get('PyModuleDef', {}))
    if not fields or 'm_methods' not in fields or 'm_name' not in fields:
        raise cparse.ParseError('PyModuleDef has no m_name or m_methods')
    name = head[fields.index('m_name')].strip('"')
    table = head[fields.index('m_methods')]
    for i, t in enumerate(toks):
        if t.text == table and toks[i + 1].text == '[' and toks[i + 2].text == ']' \
                and toks[i + 3].text == '=' and toks[i + 4].text == '{':
            end = ctokens.match_close(toks, i + 4)
            body = cparse.expand(toks[i + 5:end], source.local_macros, set())
            break
    else:
        raise cparse.ParseError('no method table %s in %s' % (table, source.cfile))
    out = []
    for e in ctokens.split_top(body, ','):
        if not e or e[0].text != '{':
            continue
        parts = ctokens.split_top(e[1:ctokens.match_close(e, 0)], ',')
        if len(parts) < 3 or parts[0][0].text == 'NULL':
            continue
        fn = parts[1]
        while len(fn) > 1 and fn[0].text == '(':
            fn = fn[ctokens.match_close(fn, 0) + 1:]
        flags = ' '.join(t.text for t in parts[2])
        out.append((parts[0][0].text.strip('"'), fn[0].text, flags))
    return name, out


def flag_set(text):
    """'METH_FASTCALL | METH_KEYWORDS' -> frozenset of the names."""
    return frozenset(w.strip() for w in text.replace('(', ' ').replace(')', ' ').split('|')
                     if w.strip())


def meth_args(flags):
    """policy_table.METH_ARGS' entry for these ml_flags, or None."""
    for key, spec in policy_table.METH_ARGS.items():
        if flag_set(key) == flags:
            return spec
    return None


def unpackers(source):
    """{C function: (index of the array or None, of the count, of min, of
    max, of the first output or None)} for the functions whose policy
    entry has ('other', 'unpack_arguments'), read from their definitions'
    parameters: the bounds are the parameters named min and max, the count
    the Py_ssize_t after the array (or the one named nargs when there is no
    array), the outputs the '...' (the entry says what the call does; the
    source says where its parts are)."""
    out = {}
    for key, e in policy_table.ENTRIES.items():
        if not any(i[:2] == ('other', 'unpack_arguments') for i in e.get('policy', [])):
            continue
        path, name = key.split(':')
        with open(os.path.join(source.src, path), encoding='latin-1') as f:
            toks = ctokens.tokenize(f.read(), path)[0]
        d = csource.scan(toks, {}, path).get(name)
        if d is None or d.kind != 'function':
            continue            # not in this source: the policy check says so
        params = ga.split_params(d.params)
        names = [p for _, p in params]
        types = [t for t, _ in params]
        if 'min' not in names or 'max' not in names:
            raise cparse.ParseError('%s: no parameters min and max' % key)
        if 'PyObject const **' in types:
            a = types.index('PyObject const **')
            c = a + 1
            if c >= len(types) or types[c] != 'Py_ssize_t':
                raise cparse.ParseError('%s: the array is not followed by its count' % key)
            if not ''.join(t.text for t in d.params).endswith('...'):
                raise cparse.ParseError('%s: an array and no outputs' % key)
            first = len(params) - 1
        elif 'nargs' in names and types[names.index('nargs')] == 'Py_ssize_t':
            a, c, first = None, names.index('nargs'), None
        else:
            raise cparse.ParseError('%s: neither (array, count, ...) nor nargs' % key)
        out[name] = (a, c, names.index('min'), names.index('max'), first)
    return out


# ---- the translator ------------------------------------------------------------

class Translator(ga.Translator):
    def __init__(self, source, ni, policy, effects, roots, unpack):
        super().__init__(source, ni, policy, effects)
        self.builtin_roots = roots     # C function -> (Python name, flags, spec)
        self.unpack = unpack

    def roots(self):
        return sorted(self.builtin_roots)

    def new_walk(self, f, sm, hoist):
        return Walk(self, f, sm, hoist)

    def policy_items(self, name):
        return self.policy.get('%s:%s' % (self.s.file_of(name), name), [])

    def effects_key(self, name):
        g = self.effects
        if g is None:
            return None
        return g.by_file.get(self.s.file_of(name), {}).get(name) or g.globals.get(name)

    def param_binding(self, order):
        super().param_binding(order)
        for n, (_, _, spec) in self.builtin_roots.items():
            f = self.s.funcs[n]
            if len(spec['c']) != len(f.params):
                raise cparse.ParseError('%s takes %d parameters; its flags give %d'
                                        % (n, len(f.params), len(spec['c'])))
            self.sums[n].pbt = ['rt'] * len(f.params)


class Walk(ga.Walk):
    fast = None             # a METH_FASTCALL root's (array, count, tuple) names

    def run(self):
        root = self.tr.builtin_roots.get(self.f.name)
        if root is None:
            return super().run()
        spec = root[2]
        f = self.f
        st = ga.State()
        self.params = []
        self.fast = None            # (array C name, count C name, meta tuple)
        for k, (ctype, p) in enumerate(f.params):
            role = spec['c'][k]
            if role == 'array':
                self.fast = (p, f.params[k + 1][1], 'vargs')
                self.params.append(('vinfo_t *', 'vargs'))
                continue
            if role == 'count':
                continue
            v = ga.Var(p, ctype, 'rt', p)
            v.isparam = True
            st.vars[p] = v
            self.params.append(('vinfo_t *', p))
        out = ga.Out()
        self.out = out
        for p in self.hoist:
            if p in st.vars:
                self.promote(st, out, p, entry=True)
        self.body_start = len(out.lines)
        self.block(f.ast, st, out, [])
        if st.pending is not None:
            raise ga.Refuse('a borrowed reference stored into a new object, never increfed')
        if not st.ended:
            raise ga.Refuse('the end of the function is reachable without a return')
        for name in self.likely:
            if name not in self.used_likely:
                raise ga.Refuse("policy 'likely' %r matches no run-time branch" % name)

    # ---- the root's own fallback ----

    def resume(self, st, out, frames, why):
        root = self.tr.builtin_roots.get(self.f.name)
        if root is None or st.called:
            return super().resume(st, out, frames, why)
        if st.failed:
            raise ga.Refuse('resuming where a call has failed: %s' % why)
        stmt = frames[-1][0][frames[-1][1]]
        self.resumed.append((self.f.name, self.tr.stmt_text(self.f, stmt)[:70], why,
                             self.at_once(st, out)))
        self.release_all(st, out)
        out('return %s;' % fallback_call(self.f, root, [p for _, p in self.params]))
        st.ended = True
        st.called = True
        self.any_call = True
        self.returns.append('value')

    # ---- argument unpacking (rule unpack_arguments) ----

    def unpack_call(self, s):
        """The call when s is 'if (!U(...)) <exit>' for an unpacking U."""
        if s[0] != 'if' or s[3] is not None:
            return None
        c = s[1]
        if c[0] != 'un' or c[1] != '!' or c[2][0] != 'call' or c[2][1][0] != 'id':
            return None
        return c[2] if c[2][1][1] in self.tr.unpack else None

    def block(self, stmts, st, out, frames):
        for i, s in enumerate(stmts):
            if self.unpack_call(s) is not None:
                if frames or stmts is not self.f.ast:
                    raise ga.Refuse('argument unpacking not at the top level')
                self.unpack(self.unpack_call(s), stmts, i, st, out)
                return
        return super().block(stmts, st, out, frames)

    def unpack(self, call, top, i, st, out):
        # the statements before it, as usual
        if i:
            super().block(top[:i], st, out, [])
            if st.ended:
                return
        a, c, lo, hi, first = self.tr.unpack[call[1][1]]
        args = call[2]
        if self.fast is None or (a is not None and args[a] != ('id', self.fast[0])) \
                or args[c] != ('id', self.fast[1]):
            raise ga.Refuse('%s of something other than the root\'s arguments'
                            % call[1][1])
        bounds = []
        for k in (lo, hi):
            if args[k][0] != 'num':
                raise ga.Refuse('%s with a bound not a constant' % call[1][1])
            bounds.append(int(args[k][1]))
        outs = []
        for e in args[first:] if first is not None else []:
            if e[0] != 'un' or e[1] != '&' or e[2][0] != 'id' or e[2][1] not in st.vars:
                raise ga.Refuse('%s output %s' % (call[1][1], cparse.c_expr(e)))
            outs.append(e[2][1])
        if first is None:
            outs = [None] * bounds[1]
        if bounds[1] > len(outs) or bounds[0] > bounds[1] or bounds[1] - bounds[0] > 4:
            raise ga.Refuse('%s with bounds %s' % (call[1][1], bounds))
        n = self.new_tmp('int')
        out('%s = PsycoTuple_Load(%s);' % (n, self.fast[2]))
        out('if (%s < %d || %s > %d) {' % (n, max(bounds[0], 0), n, bounds[1]))
        sub = ga.Out(out.depth + 1)
        self.resume(st.copy(), sub, [(top, i)], 'a count of arguments outside %d..%d'
                    % tuple(bounds))
        out.extend(sub)
        out('}')
        counts = list(range(bounds[0], bounds[1] + 1))
        ends = []
        for j, k in enumerate(counts):
            sk = st.copy()
            last = j == len(counts) - 1
            sub = ga.Out(out.depth + (0 if len(counts) == 1 else 1))
            for m, name in enumerate(outs[:k]):
                if name is not None:
                    self.bind(sk, sub, name, m)
            self.count_now = k
            try:
                self.rest(top, i + 1, sk, sub)
            finally:
                self.count_now = None
            if not sk.ended:
                raise ga.Refuse('the end of the function is reachable without a return')
            if len(counts) == 1:
                out.extend(sub)
            else:
                out('if (%s == %d) {' % (n, k) if j == 0 else
                    'else {' if last else 'else if (%s == %d) {' % (n, k))
                out.extend(sub)
                out('}')
            ends.append(sk)
        st.__dict__.update(ends[-1].__dict__)
        st.ended = True

    def bind(self, st, out, name, m):
        """The C variable 'name' is item m of the root's arguments: a
        frame value, borrowed, promotable like a parameter."""
        v = st.vars[name]
        if v.owned in ('yes', 'shadow'):
            self.release(v, out)
        out('%s = PsycoTuple_GET_ITEM(%s, %d);' % (v.cname, self.fast[2], m))
        v.owned, v.isparam, v.conv, v.virt, v.alias = 'no', True, None, None, None
        if v.bt is None:
            v.bt = 'rt'
            self.decls[v.cname] = 'vinfo_t *%s;' % v.cname
        st.known.discard(name)
        if name in self.hoist:
            self.promote(st, out, name, entry=True)

    def rest(self, top, j, st, out):
        """The top-level statements from j, as gen_abstract's goto walks
        them."""
        for idx in range(j, len(top)):
            if st.ended:
                break
            self.statement(top[idx], st, out, [(top, idx)])
            if not st.ended:
                self.release_dead(st, out, self.tr.live.after[id(top[idx])])

    # ---- names and types ----

    def promote(self, st, out, name, entry=False):
        if name in [p for _, p in self.f.params]:
            return super().promote(st, out, name, entry)
        v = st.vars.get(name)
        if v is None or not v.isparam:
            raise ga.ResumeHere("the type of %s, which is not a parameter (F24)" % name)
        # an unpacked argument: a frame value, like a parameter
        tpv = 'tp_' + name
        if name in st.known:
            return tpv
        if not entry:
            if st.called:
                if name not in self.late:
                    self.late.append(name)
            elif not st.early and name not in self.must:
                self.must.append(name)
        self.decls[tpv] = 'PyTypeObject *%s;' % tpv
        out('%s = Psyco_NeedType(po, %s);' % (tpv, v.cname))
        self.error_exit(st, out, '%s == NULL' % tpv)
        st.known.add(name)
        return tpv

    count_now = None        # the count of arguments on this path, once checked

    def ev_id(self, name, st, out):
        if self.fast is not None and name == self.fast[1] and name not in st.vars:
            if self.count_now is None:
                raise ga.ResumeHere('the count of arguments before it is checked')
            return ga.Val('ct', str(self.count_now), 'Py_ssize_t')
        if name in CONSTANTS and name not in st.vars:
            r = self.new_tmp('vinfo_t *')
            out('%s = %s();' % (r, CONSTANTS[name]))
            return ga.Val('rt', r, 'PyObject *', owned=True)
        return super().ev_id(name, st, out)

    def declare(self, st, name, ctype):
        return super().declare(st, name, int_typedef(self.s, ctype))

    def assign(self, st, out, name, e, frames):
        if self.fast is not None and e[0] == 'index' and e[1] == ('id', self.fast[0]) \
                and name in st.vars and self.s.is_obj(st.vars[name].ctype):
            # an item of the arguments, read after their count is checked
            if self.count_now is None or e[2][0] != 'num' or \
                    not 0 <= int(e[2][1]) < self.count_now:
                raise ga.Refuse('the argument %s read outside the checked count'
                                % cparse.c_expr(e))
            self.bind(st, out, name, int(e[2][1]))
            return
        super().assign(st, out, name, e, frames)
        v = st.vars.get(name)
        x = e
        while x[0] == 'cast':
            x = x[2]
        if v is not None and v.bt == 'rt' and x == ('id', 'NULL'):
            v.conv = {'null': True}     # known to hold NULL on this path
        if v is not None and v.bt == 'rt' and x[0] == 'call' and x[1][0] == 'id' \
                and shortcut(x[1][1], 'zero_or_one') is not None:
            v.zero_or_one = True        # once its error test is passed

    def ev_call(self, e, st, out, frames, dest):
        fn, args = e[1], e[2]
        word = shortcut(fn[1], 'bool_word') if fn[0] == 'id' else None
        if word is not None and len(args) == 1 and args[0][0] == 'id' \
                and getattr(st.vars.get(args[0][1]), 'zero_or_one', False) \
                and st.vars[args[0][1]].bt == 'rt':
            v = st.vars[args[0][1]]
            r = self.new_tmp('vinfo_t *')
            out('%s = %s(po, %s);' % (r, word, v.cname))
            return ga.Val('rt', r, 'PyObject *', owned=True,
                          conv={'checkable': True, 'notimpl': False})
        return super().ev_call(e, st, out, frames, dest)

    def compare(self, e, st, out, frames, facts_t, facts_f):
        op, a, b = e[1], e[2], e[3]
        # an object variable known to hold NULL, compared with NULL: known now
        for x, y in ((a, b), (b, a)):
            if y == ('id', 'NULL') and op in ('==', '!=') and x[0] == 'id' \
                    and x[1] in st.vars and (st.vars[x[1]].conv or {}).get('null') \
                    and st.vars[x[1]].owned == 'dead':
                return op == '=='
        # the result of a Ni function returning bool (true: done, false: an
        # error) tested as C tests a 0/-1 result for an error
        if op in ('!=', '<') and b == ('num', '0') or op == '==' and b == ('un', '-', ('num', '1')):
            mark = len(out.lines)
            va = self.ev(a, st, out, frames)
            if va.bt == 'okv':
                return '!(%s)' % va.c
            del out.lines[mark:]
        return super().compare(e, st, out, frames, facts_t, facts_f)


# Py_None, Py_True, Py_False as values: Ni's constant vinfos (new references)
CONSTANTS = {'Py_None': 'psyco_vi_None', 'Py_True': 'psyco_vi_Py_True',
             'Py_False': 'psyco_vi_Py_False'}


def shortcut(name, kind):
    """The note of the ('shortcut', kind) item on the policy entry of the
    C function 'name' (whatever file defines it), or None."""
    for key, e in policy_table.ENTRIES.items():
        if key.endswith(':' + name):
            for item in e.get('policy', []):
                if tuple(item[:2]) == ('shortcut', kind):
                    return item[2] if len(item) > 2 else ''
    return None


def int_typedef(source, ctype):
    """An integer typedef (Py_hash_t) as the integer type it names."""
    seen = 0
    while ctype not in ga.INT_TYPES and seen < 4:
        base = source.decls.aliases.get(ctype)
        if base is None:
            break
        ctype, seen = base, seen + 1
    return ctype


def fallback_call(f, root, params):
    """The C function itself, as Ni calls it for its flags (METH_ARGS'
    'fallback'), with the meta's own parameters."""
    pyname, flags, spec = root
    return spec['fallback'].format(cfunc=cfunc_name(f.name),
                                   flags=' | '.join(sorted(flags)),
                                   args=', '.join(params))


def cfunc_name(fname):
    return 'cfunc_' + fname


# ---- the header ----------------------------------------------------------------

def roots_of(source):
    """{C function: (Python name, flags, METH_ARGS spec)} for the functions
    TRANSLATED_FUNCTIONS names, from the module's method table."""
    wanted = policy_table.TRANSLATED_FUNCTIONS.get(source.cfile, [])
    module, table = module_table(source)
    skipped = {}
    by_name = {py: (c, flag_set(fl)) for py, c, fl in table}
    out = {}
    for py in wanted:
        if py not in by_name:
            raise cparse.ParseError('%s: no builtin %s in its method table'
                                    % (source.cfile, py))
        c, flags = by_name[py]
        spec = meth_args(flags)
        if spec is None:
            # a convention Ni has no meta for: the C function is called
            skipped[c] = 'ml_flags %s: no METH_ARGS entry' % ' | '.join(sorted(flags))
            continue
        if c not in source.funcs:
            raise cparse.ParseError('%s.%s: %s is not a function of %s'
                                    % (module, py, c, source.cfile))
        out[c] = (py, flags, spec)
    return module, out, skipped


def generate(src, pyconfig, defines, ni_root=ROOT, effects=None, unit=None):
    unit = unit or UNITS['bltin']
    source = Source(src, pyconfig, defines, unit.cfile)
    ni = ga.ni_decls(ni_root, NI_HEADERS)
    policy = {k: e.get('policy', []) for k, e in policy_table.ENTRIES.items()}
    module, roots, skipped = roots_of(source)
    tr = Translator(source, ni, policy, effects, roots, unpackers(source))
    tr.copy_keys = {}
    tr.declared_generated = set()
    tr.declared = set()
    order = tr.run(set())
    used = {tr.sums[n].meta for n in roots if n in tr.metas}
    texts = dict((tr.sums[n].meta, tr.metas[n]) for n in order if n in tr.metas)
    texts.update(tr.copies)
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
    files = sorted({p for p, _, _ in source.files})
    head = ['/* GENERATED by tools/metac/gen_builtins.py from CPython %s\'s'
            % cpython_src.source_version(src),
            '   %s: do not edit. Included by %s. */' % (', '.join(files), unit.includer), '']
    body = []
    done = [n for n in sorted(roots) if n in tr.metas]
    if done:
        body.append('/* the C functions, for the fallbacks: set by the registration */')
        for n in done:
            body.append('static PyCFunction %s;' % cfunc_name(n))
        body.append('')
    if tr.copy_order:
        body.append('/* run-time copies: where the meta has no compile-time form,'
                    ' CPython\'s algorithm resumes in these */')
        for name in tr.copy_order:
            body.append(tr.copies[name])
    for n in order:
        if n in tr.metas and n not in roots:
            sm = tr.sums[n]
            f = source.funcs[n]
            ret = {'obj': 'vinfo_t *', 'val': 'vinfo_t *', 'ok': 'bool', 'ct': 'int'}[sm.kind]
            body.append('static %s %s(PsycoObject *po%s);' % (
                ret, sm.meta, ''.join(', %s' % ('vinfo_t *' if b == 'rt' else t)
                                      for (t, _), b in zip(f.params, sm.pbt))))
    body.append('')
    for n in order:
        if n in tr.metas:
            body.append(tr.metas[n])
    init = os.path.splitext(os.path.basename(unit.cfile))[0]
    body.append('/* the metas of the functions policy_table.TRANSLATED_FUNCTIONS'
                ' names, under the flags of the module\'s method table */')
    body.append('static void gen_%s_init(PyObject *md) {' % init)
    for n in done:
        py, flags, _ = roots[n]
        body.append('  %s = Psyco_DefineModuleFn(md, "%s", %s, &%s);'
                    % (cfunc_name(n), py, ' | '.join(sorted(flags)), tr.sums[n].meta))
    body.append('}')
    report = []

    pynames = {c: py for py, c, _ in module_table(source)[1]}

    def loc(n):
        """Its key (a builtin's module.name) and its location."""
        key = '%s.%s' % (module, pynames[n]) if n in pynames else n
        return '%s %s:%s' % (key, source.file_of(n), n)
    for n in order:
        if n in tr.metas:
            report.append('generated  %-32s %s' % (loc(n), tr.sums[n].meta))
    for n in sorted(set(tr.fails) | set(skipped)):
        report.append('skipped    %-32s %s' % (loc(n), tr.fails.get(n) or skipped[n]))
    for n in sorted(roots):
        if n not in tr.metas and n not in tr.fails:
            report.append('skipped    %-32s not translated' % loc(n))
    for fname, stmt, why, at_once in tr.resumes:
        report.append('%-10s %-32s %s: %s' % ('restarts' if at_once else 'resumes',
                                              loc(fname), why, stmt))
    text = '\n'.join(head + body) + '\n/* not generated:\n' + '\n'.join(
        '   ' + r for r in report if not r.startswith('generated')) + '\n*/\n'
    return text, report


def build_math(target):
    """pmathmodule_gen.h: the math functions."""
    return build(target, UNITS['math'])


def build(target, unit=None):
    try:
        import effects
        g = effects.analyse(target.python, target.src)
    except Exception as e:  # noqa: BLE001 - the build must say why
        raise cslots.GenError('effects.py: %s' % e)
    try:
        return generate(target.src, target.pyconfig,
                        ga.target_defines(target.python), effects=g, unit=unit)
    except cparse.ParseError as e:
        raise cslots.GenError(str(e))


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', required=True)
    ap.add_argument('--pyconfig', required=True)
    ap.add_argument('--python', default=None)
    ap.add_argument('--no-effects', action='store_true')
    ap.add_argument('--report', action='store_true')
    ap.add_argument('--unit', default='bltin', choices=sorted(UNITS))
    args = ap.parse_args(argv)
    g = None
    if args.python and not args.no_effects:
        import effects
        g = effects.analyse(args.python, args.src)
    defines = ga.target_defines(args.python) if args.python else ['Py_BUILD_CORE', 'NDEBUG']
    text, report = generate(args.src, args.pyconfig, defines, effects=g,
                            unit=UNITS[args.unit])
    print('\n'.join(report) if args.report else text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
