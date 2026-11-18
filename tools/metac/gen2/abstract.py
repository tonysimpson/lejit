"""pabstract_gen.h: the metas of Objects/abstract.c's functions that Ni
declares in ni/Objects/pabstract.h, and of the file's functions they call
(doc/GENERATOR_DESIGN.md, Migration step 3, the first family).

A function's meta is found by walking it (walk.py) until the promotions it
needs before any call are all at its entry; a callee is translated before
its callers, so a call of it is a call of its meta.
"""
import os

from pycparser import c_ast

import cpython_src
import cslots
import front
from . import conventions, policy, resume, scope
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
    def __init__(self, family, ast, ni, declared, items, effects, own=None):
        """own: the file whose functions are translated, if not the
        family's (a test's C after it)."""
        self.family = family
        self.unit = Unit(ast)
        self.funcs = {d.decl.name: d for d in ast.ext if isinstance(d, c_ast.FuncDef)
                      and d.coord.file.endswith(own or family.cfile)}
        self.ni, self.declared = ni, declared
        self.policy, self.effects = items, effects
        import policy_table
        self.pure = {k for k, v in policy_table.EXTERNAL.items() if v == 'pure'}
        self.sums, self.metas, self.fails = {}, {}, {}
        self.resumes = []
        self.local_bts, self.liveness = {}, {}
        self.copies = resume.Copies(self)

    # ---- which functions, in what order ----

    def roots(self):
        return [n for n in sorted(self.funcs)
                if conventions.psyco_name(n) in self.declared]

    def callees(self, fn):
        out = []
        for n in id_names(fn.body):
            if n in self.funcs and n != fn.decl.name and n not in out:
                out.append(n)
        return out

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
            for k, p in enumerate(resume.params_of(fn)):
                if self.unit.is_object(p.type):
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
        if not self.unit.is_int(ret):
            raise Refuse('returns %s' % text(ret))
        decl = self.ni.get(self.meta_name(fn))
        if decl is not None:
            if decl.ret == 'bool':
                return 'ok'
            if decl.ret == 'vinfo_t *':
                return 'val'
            raise Refuse('Ni declares %s returning %s' % (decl.name, decl.ret))
        if 'static' in fn.decl.storage:
            return 'val'
        raise Refuse('an int result needs its convention: declare %s in %s'
                     % (self.meta_name(fn), self.family.decls))

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
            self.liveness[name] = Liveness(fn.body)
            self.local_bts[name] = locals_bt(
                fn, self.unit, dict(zip(sm.params, sm.pbt)))
            hoist = []
            for _ in range(16):
                w = Walk(self, fn, sm, hoist)
                w.run()
                new = [p for p in w.rec.must + w.rec.late if p not in hoist]
                if not new:
                    break
                hoist = hoist + [p for i, p in enumerate(new) if p not in new[:i]]
            else:
                raise Refuse('promotions after calls remain: %s' % w.rec.late)
            sm.entry = hoist
            sm.needs = w.rec.needs
            sm.may_call = w.rec.any_call
            sm.notimpl = w.rec.notimpl
            sm.always_error = w.always_error
            self.metas[name] = self.text(w)
            self.resumes.extend((name,) + r for r in w.rec.resumed)
        except Refuse as e:
            self.fails[name] = str(e)

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
        types = ['vinfo_t *' if b == 'dynamic' else text(unconst(p.type))
                 for p, b in zip(resume.params_of(fn), sm.pbt)]
        return 'static %s %s(PsycoObject *po%s);' % (RET[sm.kind], sm.meta,
                                                     ''.join(', ' + t for t in types))

    # ---- what the C says ----

    def returns_notimpl(self, fn):
        return self.returns_notimpl_in(fn.body, gotos=False)

    def returns_notimpl_in(self, n, gotos=True):
        """A return in n may give NotImplemented (a goto may go anywhere)."""
        for x in walk_nodes(n):
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
    ast = front.parse(ft, family.cfile)
    ni = conventions.ni_decls(ROOT, scope.NI_HEADERS)
    declared = set(conventions.ni_decls(ROOT, [family.decls]))
    items = policy.load(ft, family)
    tr = Translator(family, ast, ni, declared, items, effects)
    order = tr.run()
    # keep what Ni's declarations reach
    by_meta = {tr.sums[n].meta: n for n in order if n in tr.metas}
    texts = {m: tr.metas[n] for m, n in by_meta.items()}
    texts.update(tr.copies.texts)
    used = {m for m in by_meta if m in declared}
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
    if copies:
        out.append('/* run-time copies: where a heap type\'s slot changed, or'
                   ' where the meta has no compile-time form, CPython\'s'
                   ' algorithm resumes in these */')
        out += [tr.copies.texts[c] for c in copies]
    out += [tr.forward(n) for n in order if n in metas and (
        'static' in tr.funcs[n].decl.storage or tr.sums[n].meta not in declared)]
    out.append('')
    out += [metas[n] for n in order if n in metas]
    report = []

    def loc(n):
        return '%s %s:%s' % (n, family.cfile, n)
    for n in order:
        if n in metas:
            report.append('generated  %-32s %s' % (loc(n), tr.sums[n].meta))
    for n in sorted(tr.fails):
        report.append('skipped    %-32s %s' % (loc(n), tr.fails[n]))
    for fname, stmt, why, at_once in tr.resumes:
        if fname in metas:
            report.append('%-10s %-32s %s: %s' % ('restarts' if at_once else 'resumes',
                                                  loc(fname), why, stmt))
    text_ = '\n'.join(head + out) + '\n/* not generated:\n' + '\n'.join(
        '   ' + r for r in report if not r.startswith('generated')) + '\n*/\n'
    return text_, report


def build(target):
    try:
        import effects
        g = effects.analyse(target.python, target.src)
    except Exception as e:  # noqa: BLE001 - the build must say why
        raise cslots.GenError('effects.py: %s' % e)
    try:
        return generate(target, scope.FAMILIES['abstract'], g)
    except (front.FrontError, policy.PolicyError) as e:
        raise cslots.GenError(str(e))
