"""The translator's driver, one family (scope.FAMILIES) at a time
(doc/GENERATOR_DESIGN.md, Migration step 3): pabstract_gen.h, the metas
of Objects/abstract.c's functions that Ni declares in
ni/Objects/pabstract.h; plongobject_gen.h and pfloatslots_gen.h, the
metas of the int and float slots scope.py names (registered by the
header's gen_FILE_init) and of floatobject.c's functions Ni declares;
pmathmodule_gen.h and pbltinmodule_gen.h, the metas of the math functions
and builtins scope.py names (registered under their method table's flags
by gen_FILE_init: a METH_FASTCALL function's C array and count are the
one tuple Ni passes, walked once per count its check allows); and the
metas of the file's functions those call.

A function's meta is found by walking it (walk.py) until the promotions it
needs before any call are all at its entry; a callee is translated before
its callers, so a call of it is a call of its meta.
"""
import os

from pycparser import c_ast

import cpython_src
import cslots
import front
from . import conventions, inline, policy, resume, scope, slots, types
from .bta import locals_bt
from .cbuild import Function
from .ctyping import Unit, strip_casts, text
from .errors import Refuse
from .liveness import Liveness, body
from .types import NOT_IMPLEMENTED
from .walk import Walk, new_reference, once_do, singleton, unconst

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
RET = {'obj': 'vinfo_t *', 'val': 'vinfo_t *', 'ok': 'bool', 'ct': 'int'}
# a builtin's parameter binding times, by its part (policy_table.METH_ARGS)
ROLE_BT = {'self': 'dynamic', 'arg': 'dynamic', 'array': 'array', 'count': 'count'}


class Summary:
    """What a caller needs to know of a translated function."""

    def __init__(self, fn):
        self.params = [p.name for p in resume.params_of(fn)]
        self.ret = fn.decl.type.type
        self.kind = None
        self.entry = []           # parameters promoted at entry, in order
        self.needs = set()        # indexes of parameters whose type it reads
        self.may_call = False
        self.always_error = False
        self.notimpl = False
        self.pbt = []
        self.meta = None


class Translator:
    def __init__(self, family, ast, ni, declared, items, effects, own=None, slot_asts=None):
        """own: the file whose functions are translated, if not the
        family's (a test's C after it); slot_asts: {file: its AST} of the
        files defining types whose slots the family fills."""
        self.family = family
        self.unit = Unit(ast)
        self.reps = types.load(ast, family.reps)
        self.budgets = dict(policy.BUDGETS)
        self.slot_roots = {}      # function -> [(slot reference, slot)]
        self.slot_types = {}      # function -> its type
        for tp, names, *where in family.slots:
            a, u = (slot_asts[where[0]], Unit(slot_asts[where[0]])) if where else (ast, self.unit)
            for f, refs in slots.slot_functions(a, u, tp, names).items():
                self.slot_roots.setdefault(f, []).extend(refs)
                self.slot_types.setdefault(f, tp)
        # function -> (Python name, flag names, METH_ARGS spec)
        self.module, self.builtins, self.builtins_skipped = None, {}, {}
        if family.builtins:
            self.module, self.builtins, self.builtins_skipped = slots.builtin_functions(
                ast, self.unit, family.builtins)
        mine = (own or family.cfile, clinic_file(own or family.cfile))
        self.funcs = {d.decl.name: d for d in ast.ext if isinstance(d, c_ast.FuncDef)
                      and d.coord.file.endswith(mine)}
        self.ni, self.declared = ni, declared
        self.policy, self.effects = items, effects
        import policy_table
        self.pure = {k for k, v in policy_table.EXTERNAL.items() if v == 'pure'}
        self.sums, self.metas, self.fails = {}, {}, {}
        self.resumes = []
        self.local_bts, self.liveness, self.expanded = {}, {}, {}
        self.reps_used, self.budgets_hit = set(), []
        self.header_asserts = set()   # assertions the header makes (address.py)
        self.loop_hoists = {}     # function -> {loop: [promotions at its entry]}
        self.copies = resume.Copies(self)
        self._expansions = {}

    # ---- which functions, in what order ----

    def roots(self):
        return [n for n in sorted(self.funcs)
                if conventions.psyco_name(n) in self.declared or n in self.slot_roots
                or n in self.builtins]

    def expansion(self, name):
        """(the function with its inlinable calls expanded, the budgets
        hit), made once."""
        if name not in self._expansions:
            self._expansions[name] = inline.expand(self, self.funcs[name], self.budgets)
        return self._expansions[name]

    def callees(self, fn):
        """The functions of the file fn still calls once its inlinable
        calls are expanded."""
        out = []
        for n in id_names(self.expansion(fn.decl.name)[0].body):
            if n in self.funcs and n != fn.decl.name and n not in out \
                    and not self.ni_own(n) and not self.constructor(n):
                out.append(n)
        return out

    def constructor(self, n):
        """A representation's constructor: given a word or a double, the
        representation's virtual (reps.construct)."""
        return any(n in r.spec['constructors'] for r in self.reps.values())

    def ni_own(self, n):
        """Ni's own meta of n (declared in another of Ni's headers) is the
        one calls use."""
        m = conventions.psyco_name(n)
        return 'static' not in self.funcs[n].decl.storage and m in self.ni \
            and m not in self.declared

    def run(self):
        order, seen = [], set()

        def visit(n, stack):
            if n in seen:
                return
            if n in stack:
                self.fails[n] = 'recursive'
                return
            for c in self.callees(self.funcs[n]):
                visit(c, stack | {n})
            seen.add(n)
            order.append(n)
        for r in self.roots():
            visit(r, frozenset())
        self.param_binding(order)
        for n in order:
            self.translate(n)
        return order

    def meta_name(self, fn):
        name = fn.decl.name
        if 'static' not in fn.decl.storage and conventions.psyco_name(name):
            return conventions.psyco_name(name)
        return name

    # ---- binding times of parameters ----

    def param_binding(self, order):
        pbt = {}
        for n in order:
            fn = self.funcs[n]
            decl = self.ni.get(self.meta_name(fn))
            out = []
            fmt = conventions.slot_format(self.slot_roots.get(n))
            if fmt is not None and len(fmt) != len(resume.params_of(fn)):
                self.fails[n] = 'Ni calls its slot with "%s"' % fmt
                fmt = None
            for k, p in enumerate(resume.params_of(fn)):
                if fmt is not None:
                    out.append('meta' if fmt[k] == 'l' else 'dynamic')
                elif self.unit.is_object(p.type):
                    out.append('dynamic')
                elif decl is not None and k + 1 < len(decl.params):
                    out.append('dynamic' if decl.params[k + 1][0] == 'vinfo_t *' else 'meta')
                elif 'static' not in fn.decl.storage:
                    out.append('dynamic' if self.unit.is_int(p.type) else 'meta')
                else:
                    out.append(None)
            pbt[n] = out
        changed = True
        while changed:
            changed = False
            for n in order:
                for k, b in enumerate(pbt[n]):
                    if b is not None:
                        continue
                    sites = list(self.call_sites(n))
                    if sites and all(self.meta_arg(c, a, pbt) for c, a in
                                     ((c, args[k]) for c, args in sites if k < len(args))):
                        pbt[n][k] = 'meta'
                        changed = True
        for n in order:
            sm = self.sums.setdefault(n, Summary(self.funcs[n]))
            sm.pbt = [b or 'dynamic' for b in pbt[n]]
            if n in self.builtins:
                # Ni's arguments by their parts (METH_ARGS 'c'): a vinfo
                # each, or the one tuple of an array and its count
                spec = self.builtins[n][2]
                if len(spec['c']) != len(sm.params) or not set(spec['c']) <= set(ROLE_BT):
                    self.fails[n] = 'ml_flags %s: Ni passes %s' % (
                        ' | '.join(self.builtins[n][1]), ', '.join(spec['c']))
                else:
                    sm.pbt = [ROLE_BT[r] for r in spec['c']]

    def call_sites(self, name):
        for cname, fn in self.funcs.items():
            for x in walk_nodes(fn.body):
                if isinstance(x, c_ast.FuncCall) and isinstance(x.name, c_ast.ID) \
                        and x.name.name == name:
                    yield cname, (x.args.exprs if x.args else [])

    def meta_arg(self, caller, e, pbt):
        fn = self.funcs[caller]
        params = [p.name for p in resume.params_of(fn)]
        local = {d.name: d.type for d in resume.decls_in(fn.body)}
        for n in id_names(e):
            if n in params:
                if (pbt.get(caller) or [None] * len(params))[params.index(n)] != 'meta':
                    return False
            elif n in local:
                if not self.unit.is_meta_type(local[n]):
                    return False
            elif n in self.funcs:
                return False
        return True

    # ---- one function ----

    def kind_of(self, fn):
        ret = fn.decl.type.type
        if self.unit.is_object(ret):
            return 'obj'
        if not (self.unit.is_int(ret) or self.unit.is_double(ret)) \
                or conventions.ret_flag(self.unit, ret) is None:
            raise Refuse('returns %s' % text(ret))
        decl = self.ni.get(self.meta_name(fn))
        if decl is not None:
            if decl.ret == 'bool':
                return 'ok'
            if decl.ret == 'vinfo_t *':
                return 'val'
            raise Refuse('Ni declares %s returning %s' % (decl.name, decl.ret))
        if 'static' in fn.decl.storage or fn.decl.name in self.slot_roots:
            return 'val'        # a slot's meta gives a vinfo of its C result
        raise Refuse('an int result needs its convention: declare %s in %s'
                     % (self.meta_name(fn), self.family.decls or 'a header of Ni'))

    def translate(self, name):
        fn = self.funcs[name]
        sm = self.sums[name]
        sm.meta = self.meta_name(fn)
        if name in self.fails:
            return
        items = self.policy.get(name, [])
        for i in items:
            if isinstance(i, policy.NotTranslation):
                self.fails[name] = "Ni's %s is not a translation of it: %s" % (sm.meta, i.why)
                return
        hand = [i.what for i in items if isinstance(i, policy.HandOnly)]
        if hand:
            self.fails[name] = 'policy items left to the hand translation: %s' % ', '.join(hand)
            return
        for i in items:
            why = i.check(fn, self.unit)
            if why:
                self.fails[name] = 'policy item: %s' % why
                return
        try:
            sm.kind = self.kind_of(fn)
            fn, hit = self.expansion(name)
            self.expanded[name] = fn
            self.liveness[name] = Liveness(fn.body)
            pbt = dict(zip(sm.params, sm.pbt))
            for i in items:
                if isinstance(i, policy.IfKnown):
                    pbt[i.name] = 'meta'        # its value, where the meta goes on
            self.local_bts[name] = locals_bt(fn, self.unit, pbt,
                                             length=lengths_read(self, fn))
            hoist = []
            lh = self.loop_hoists[name] = {}
            for _ in range(self.budgets['rewalks']):
                w = Walk(self, fn, sm, hoist)
                w.run()
                new = [p for p in w.rec.must + w.rec.late if p not in hoist]
                for key, what in w.rec.loop_late:
                    lh.setdefault(key, []).append(what)
                if not new and not w.rec.loop_late:
                    break
                hoist = hoist + [p for i, p in enumerate(new) if p not in new[:i]]
            else:
                raise Refuse('budget rewalks: promotions after calls remain: %s' % w.rec.late)
            sm.entry = hoist
            sm.needs = w.rec.needs
            sm.may_call = w.rec.any_call
            sm.notimpl = w.rec.notimpl
            sm.always_error = w.always_error
            self.reps_used |= w.rec.reps
            self.budgets_hit.extend((name, b) for b in hit + w.rec.budgets)
            self.metas[name] = self.text(w)
            self.resumes.extend((name,) + r for r in w.rec.resumed)
        except Refuse as e:
            self.fails[name] = str(e)

    def fnptr_targets_pure(self, fname, param):
        """Every call of fname gives its function-pointer parameter a pure
        function (by name)."""
        names = [p.name for p in resume.params_of(self.funcs[fname])]
        k = names.index(param)
        sites = list(self.call_sites(fname))
        if not sites or fname in self.slot_roots or conventions.psyco_name(fname) \
                in self.declared:
            return False
        for _, args in sites:
            a = strip_casts(args[k]) if k < len(args) else None
            if not (isinstance(a, c_ast.ID) and (a.name in self.pure or 'CfPure' in
                                                 self.effect_flags(self.effects_key(a.name)))):
                return False
        return True

    def helper(self, fname, key, ret, params, lines):
        """A scalar helper's C (scalar.py), named and kept with the copies."""
        return self.copies.helper(fname, key, ret, params, lines)

    def unhelper(self, name):
        self.copies.drop(name)

    def slot_key(self, n):
        """A static slot function's key in the report: its type and slot;
        a builtin's: its module and Python name."""
        if n in self.builtins or n in self.builtins_skipped:
            return '%s.%s' % (self.module, (self.builtins.get(n) or self.builtins_skipped[n])[0])
        if n in self.slot_roots and 'static' in self.funcs[n].decl.storage:
            return '%s.%s' % (self.slot_types[n], self.slot_roots[n][0][1])
        return n

    def text(self, w):
        fn, sm = w.fn, w.sm
        static = 'static' in fn.decl.storage or sm.meta not in self.declared
        head = '%s%s %s(PsycoObject *po%s)' % (
            'static ' if static else 'DEFINEFN ', RET[sm.kind], sm.meta,
            ''.join(', %s%s%s' % (t, '' if t.endswith('*') else ' ', p)
                    for t, p in w.meta_params))
        return Function(head, w.rec.decls, w.out,
                        '%s:%s' % (self.family.cfile, fn.decl.name)).render()

    def forward(self, name):
        sm = self.sums[name]
        fn = self.funcs[name]
        types = ['vinfo_t *' if b in ('dynamic', 'array') else text(unconst(p.type))
                 for p, b in zip(resume.params_of(fn), sm.pbt) if b != 'count']
        return 'static %s %s(PsycoObject *po%s);' % (RET[sm.kind], sm.meta,
                                                     ''.join(', ' + t for t in types))

    # ---- what the C says ----

    def returns_notimpl(self, fn):
        return self.returns_notimpl_in(fn.body, gotos=False)

    def returns_notimpl_in(self, n, gotos=True):
        """A return in n may give NotImplemented (a goto may go anywhere,
        and a variable assigned it may be returned)."""
        for x in walk_nodes(n):
            if isinstance(x, c_ast.Assignment) and x.op == '=' \
                    and singleton(x.rvalue) == NOT_IMPLEMENTED:
                return True
            if gotos and isinstance(x, c_ast.Goto):
                return True
            if isinstance(x, c_ast.Return) and x.expr is not None:
                e = strip_casts(x.expr)
                ref = new_reference(e)
                if singleton(ref if ref is not None else e) == NOT_IMPLEMENTED:
                    return True
                if isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID) \
                        and e.name.name in self.sums and self.sums[e.name.name].notimpl:
                    return True
        return False

    def error_value(self, fn, e, depth=0):
        """'return e' is the function's error return."""
        if e is None or depth > 4:
            return False
        e = strip_casts(e)
        ret = fn.decl.type.type
        if isinstance(e, c_ast.Constant) and e.value == '0' and self.unit.is_object(ret):
            return True
        if isinstance(e, c_ast.UnaryOp) and e.op == '-' and isinstance(e.expr, c_ast.Constant) \
                and e.expr.value == '1' and self.unit.is_int(ret):
            return True
        if isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID) \
                and e.name.name in self.funcs:
            g = self.funcs[e.name.name]
            rets = [x.expr for x in walk_nodes(g.body) if isinstance(x, c_ast.Return)]
            return bool(rets) and g is not fn and all(
                r is not None and not isinstance(strip_casts(r), c_ast.FuncCall)
                and self.error_value(g, r, depth + 1) for r in rets)
        return False

    def every_path_loops(self, fn, frames):
        """Every path from the statement frames name runs a loop with a
        run-time bound, or fails, before it returns a value."""
        def outer(level):
            if level < 0:
                return lambda: False
            stmts, j = frames[level]
            return lambda: self.seq_loops(fn, stmts, j + 1, outer(level - 1), 0)
        stmts, i = frames[-1]
        return self.seq_loops(fn, stmts, i, outer(len(frames) - 2), 0) == 'loop'

    def seq_loops(self, fn, stmts, i, cont, depth):
        """'loop' when every path runs a run-time loop or fails and one
        runs a loop, 'error' when every path fails, else False."""
        if depth > 8:
            return False
        for k in range(i, len(stmts)):
            s = stmts[k]

            def rest(k=k):
                return self.seq_loops(fn, stmts, k + 1, cont, depth)
            if once_do(s):
                return self.seq_loops(fn, body(s.stmt), 0, rest, depth)
            if isinstance(s, (c_ast.For, c_ast.While, c_ast.DoWhile)):
                return 'loop' if counted_loop(s) is None else False
            if isinstance(s, c_ast.Return):
                return 'error' if self.error_value(fn, s.expr) else False
            if isinstance(s, c_ast.If):
                a = self.seq_loops(fn, body(s.iftrue), 0, rest, depth)
                if not a:
                    return False
                b = (self.seq_loops(fn, body(s.iffalse), 0, rest, depth)
                     if s.iffalse is not None else rest())
                if not b:
                    return False
                return 'loop' if 'loop' in (a, b) else 'error'
            if isinstance(s, c_ast.Compound):
                return self.seq_loops(fn, body(s), 0, rest, depth)
            if isinstance(s, c_ast.Label):
                return self.seq_loops(fn, [s.stmt], 0, rest, depth)
            if isinstance(s, c_ast.Goto):
                top = body(fn.body)
                j = next((n for n, x in enumerate(top)
                          if isinstance(x, c_ast.Label) and x.name == s.name), None)
                if j is None:
                    return False
                return self.seq_loops(fn, top, j, lambda: False, depth + 1)
            if isinstance(s, (c_ast.Break, c_ast.Continue, c_ast.Switch)):
                return False
        return cont()

    # ---- effects ----

    def effects_key(self, name):
        g = self.effects
        if g is None:
            return None
        return g.by_file.get(self.family.cfile, {}).get(name) or g.globals.get(name)

    def effect_flags(self, key):
        return self.effects.cf_flags(key) if self.effects is not None and key else []

    def cannot_raise(self, key):
        return self.effects is not None and key is not None and self.effects.cannot_raise(key)


def lengths_read(tr, fn):
    """A test of whether an expression of fn reads the size field of an
    object whose type has a known length (by a cast the C writes, or the
    declared type of the variable it reads): a meta value for binding
    times, the length Ni loads (items.py)."""
    specs = tr.reps['known_length'].spec['lengths'] if 'known_length' in tr.reps else {}
    if not specs:
        return None
    declared = {d.name: d.type for d in resume.decls_in(fn.body)}
    declared.update({p.name: p.type for p in resume.params_of(fn)})

    def struct(t):
        pt = tr.unit.pointee(t)
        return text(pt).replace('const ', '').replace('struct ', '').strip() if pt else None

    def test(e):
        if not (isinstance(e, c_ast.StructRef) and e.type == '->'):
            return False
        x, names = e.name, []
        while isinstance(x, c_ast.Cast):
            names.append(struct(x.to_type.type))
            x = x.expr
        if isinstance(x, c_ast.ID) and x.name in declared:
            names.append(struct(declared[x.name]))
        return any(n in specs and e.field.name == specs[n]['size'] for n in names)
    return test


def clinic_file(cfile):
    """The file Argument Clinic writes a C file's wrappers into, which the
    file includes: its functions are the file's."""
    d, base = os.path.split(cfile)
    return os.path.join(d, 'clinic', base + '.h')


def counted_loop(s):
    """(variable, bound) when loop s is a 'for' stepping V by one towards
    E (V < E, V <= E or V != E, V++; or V > E, V >= E, V != E, V--), V
    assigned nowhere else in it, E nowhere in it and calling nothing;
    else None."""
    if not isinstance(s, c_ast.For) or s.cond is None or s.next is None:
        return None
    step = strip_casts(s.next)
    if not (isinstance(step, c_ast.UnaryOp) and step.op in ('++', 'p++', '--', 'p--')
            and isinstance(step.expr, c_ast.ID)):
        return None
    v, up = step.expr.name, '+' in step.op
    from .bta import assignments
    from .liveness import names as rd
    assigned = {n for n, _ in assignments(s.stmt)} | {
        x.expr.name for x in walk_nodes(s.stmt) if isinstance(x, c_ast.UnaryOp)
        and x.op in ('++', 'p++', '--', 'p--') and isinstance(x.expr, c_ast.ID)}
    for c in conjuncts(s.cond):
        c = strip_casts(c)
        ok = ('<', '<=', '!=') if up else ('>', '>=', '!=')
        if isinstance(c, c_ast.BinaryOp) and c.op in ok and isinstance(strip_casts(c.left),
                                                                    c_ast.ID) \
                and strip_casts(c.left).name == v and v not in assigned \
                and not set(rd(c.right)) & assigned \
                and not any(isinstance(x, c_ast.FuncCall) for x in walk_nodes(c.right)):
            return v, c.right
    return None


def conjuncts(e):
    e = strip_casts(e)
    if isinstance(e, c_ast.BinaryOp) and e.op == '&&':
        return conjuncts(e.left) + conjuncts(e.right)
    return [e]


def walk_nodes(n):
    yield n
    for _, c in n.children():
        yield from walk_nodes(c)


def id_names(n):
    return [x.name for x in walk_nodes(n) if isinstance(x, c_ast.ID)]


def generate(target, family, effects=None):
    ft = front.target(target.python, target.src)
    ast = front.parse(ft, family.cfile, types.rule_text(family.reps)
                      + (slots.meth_text() if family.builtins else ''))
    ni = conventions.ni_decls(ROOT, scope.NI_HEADERS)
    declared = set(conventions.ni_decls(ROOT, [family.decls])) if family.decls else set()
    items = policy.load(ft, family)
    slot_asts = {s[2]: front.parse(ft, s[2]) for s in family.slots if len(s) > 2}
    tr = Translator(family, ast, ni, declared, items, effects, slot_asts=slot_asts)
    order = tr.run()
    # keep what Ni's declarations reach
    by_meta = {tr.sums[n].meta: n for n in order if n in tr.metas}
    texts = {m: tr.metas[n] for m, n in by_meta.items()}
    texts.update(tr.copies.texts)
    used = {m for m in by_meta if m in declared or by_meta[m] in tr.slot_roots
            or by_meta[m] in tr.builtins}
    work = list(used)
    import re
    while work:
        for m in set(re.findall(r'\b\w+\b', texts[work.pop()])) & set(texts):
            if m not in used:
                used.add(m)
                work.append(m)
    order = [n for n in order if n not in tr.metas or tr.sums[n].meta in used]
    metas = {n: t for n, t in tr.metas.items() if tr.sums[n].meta in used}
    copies = [c for c in tr.copies.order if c in used]
    head = ['/* GENERATED by tools/metac/gen2/abstract.py from CPython %s\'s'
            % cpython_src.source_version(target.src),
            '   %s: do not edit. Included by %s. */' % (family.cfile, family.includer), '']
    out = []
    done = [n for n in sorted(tr.builtins) if n in metas]
    if done:
        out.append('/* the C functions, for the fallbacks: set by the registration */')
        out += ['static PyCFunction %s;' % cfunc_name(n) for n in done]
        out.append('')
    if 'word' in tr.reps_used:
        r = tr.reps['word']
        out += ['_Static_assert(sizeof(long) * 8 == 64, "words are longs");',
                '_Static_assert(sizeof(int) * 8 == 32, "C ints are 32 bits");',
                '_Static_assert(%s == %d, "the word range of %s");' % (
                    r.spec['bound'], r.bound, r.spec['struct']), '']
    if tr.header_asserts:
        out += sorted(tr.header_asserts) + ['']
    if copies:
        out.append('/* run-time copies: where a heap type\'s slot changed, or'
                   ' where the meta has no compile-time form, CPython\'s'
                   ' algorithm resumes in these */')
        out += [tr.copies.texts[c] for c in copies]
    out += [tr.forward(n) for n in order if n in metas and (
        'static' in tr.funcs[n].decl.storage or tr.sums[n].meta not in declared)]
    out.append('')
    out += [metas[n] for n in order if n in metas]
    regs = sorted((tr.sums[n].meta, ref) for n in metas for ref, _ in tr.slot_roots.get(n, ()))
    if regs:
        base = os.path.splitext(os.path.basename(family.cfile))[0]
        out.append('/* the metas of the slots tools/metac/gen2/scope.py names */')
        out.append('static void gen_%s_init(void) {' % base)
        out += ['  Psyco_DefineMeta(%s, %s);' % (ref, m) for m, ref in regs]
        out.append('}')
    if done:
        base = os.path.splitext(os.path.basename(family.cfile))[0]
        out.append('/* the metas of the functions tools/metac/gen2/scope.py names, under the'
                   ' flags of the module\'s method table */')
        out.append('static void gen_%s_init(PyObject *md) {' % base)
        out += ['  %s = Psyco_DefineModuleFn(md, "%s", %s, &%s);' % (
            cfunc_name(n), tr.builtins[n][0], ' | '.join(tr.builtins[n][1]), tr.sums[n].meta)
            for n in done]
        out.append('}')
    report = []

    def loc(n):
        f = tr.funcs[n].coord.file if n in tr.funcs else ''
        return '%s %s:%s' % (tr.slot_key(n), clinic_file(family.cfile)
                             if f.endswith(clinic_file(family.cfile)) else family.cfile, n)
    for n in order:
        if n in metas:
            report.append('generated  %-32s %s' % (loc(n), tr.sums[n].meta))
    for n in sorted(set(tr.fails) | set(tr.builtins_skipped)):
        report.append('skipped    %-32s %s' % (loc(n), tr.fails.get(n)
                                                or tr.builtins_skipped[n][1]))
    notes = []
    for n, b in tr.budgets_hit:
        notes.append('budget     %-32s %s' % (loc(n), b))
    for fname, stmt, why, at_once in tr.resumes:
        if fname in metas:
            report.append('%-10s %-32s %s: %s' % ('restarts' if at_once else 'resumes',
                                                  loc(fname), why, stmt))
    text_ = '\n'.join(head + out) + '\n/* not generated:\n' + '\n'.join(
        '   ' + r for r in report + notes if not r.startswith('generated')) + '\n*/\n'
    return text_, report


def build(target, family='abstract'):
    try:
        import effects
        g = effects.analyse(target.python, target.src)
    except Exception as e:  # noqa: BLE001 - the build must say why
        raise cslots.GenError('effects.py: %s' % e)
    try:
        return generate(target, scope.FAMILIES[family], g)
    except (front.FrontError, policy.PolicyError) as e:
        raise cslots.GenError(str(e))


def cfunc_name(fname):
    """The variable holding a builtin's C function, for its fallback."""
    return 'cfunc_' + fname


def fallback(tr, fname, params):
    """The builtin's C function itself, as Ni calls it for its flags
    (METH_ARGS 'fallback'), with the meta's own parameters."""
    _, flags, spec = tr.builtins[fname]
    return spec['fallback'].format(cfunc=cfunc_name(fname), flags=' | '.join(flags),
                                   args=', '.join(params))


def build_long(target):
    return build(target, 'long')


def build_float(target):
    return build(target, 'float')


def build_math(target):
    return build(target, 'math')


def build_bltin(target):
    return build(target, 'bltin')


def build_tuple(target):
    return build(target, 'tuple')


def build_list(target):
    return build(target, 'list')


def build_dict(target):
    return build(target, 'dict')


def build_bytes(target):
    return build(target, 'bytes')


def build_unicode(target):
    return build(target, 'unicode')


def build_structmember(target):
    return build(target, 'structmember')


def build_descr(target):
    return build(target, 'descr')


def build_object(target):
    return build(target, 'object')
