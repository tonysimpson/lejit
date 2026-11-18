"""Resume (doc/GENERATOR_DESIGN.md "Resume"): CPython's code runs at run
time from a statement, as a call the meta emits, and the call's result is
the meta's. One checked function; the refusals are the design's.

At once (nothing called yet on the path), the call is of the whole
function: CPython's own if exported, a builtin's C function as Ni calls
one of its flags, else its copy (cimpl_NAME). Later,
it is of a copy of the rest of the function from the statement
(cimpl_NAME_k), taking the variables live there; the copy takes its own
reference to each object the meta owns, and the meta releases its own.
"""
from pycparser import c_ast

from . import conventions, cprint, inline
from .ctyping import text
from .errors import Refuse, ResumeHere
from .liveness import names


def resume(w, p, out, frames, why, inside=False):
    if w.loops:
        # a copy from inside a meta loop would lose the turns still to
        # run: CPython's code runs from the outermost loop instead
        raise ResumeHere(why)
    if inside and w.rec.ncalls > w.stmt_ncalls:
        # the copy would evaluate the statement's call again
        raise Refuse('resuming in a statement after its call: %s' % why)
    if p.failed:
        raise Refuse('resuming where a call has failed: %s' % why)
    if w.switch_depth and p.called:
        raise Refuse('resuming inside a switch: %s' % why)
    tr, fn = w.tr, w.fn
    stmt = frames[-1][0][frames[-1][1]]
    at_once = out is w.out and len(out.items) == w.body_start and not p.called
    w.rec.resumed.append((cprint.stmt_text(stmt), why, at_once))
    if not p.called and w.name in tr.builtins:
        # the C function itself, called as Ni calls one of its flags
        from .abstract import fallback
        p.called = True
        w.rec.any_call = True
        return w.ret_value(p, out, fallback(tr, w.name, [n for n, _ in w.params]))
    if not p.called:
        params = [(n, t) for n, t in w.params]
        if 'static' not in fn.decl.storage:
            name = w.name
        elif w.name in tr.slot_roots:
            name = tr.slot_roots[w.name][0][0]     # the type's slot is the function
        else:
            name = tr.copies.whole(w.name)
    else:
        if inlined_frames(frames):
            raise Refuse('resuming inside an inlined call: %s' % why)
        live = w.live.before[id(stmt)]
        owned = set()
        for vname in sorted(live):
            v = p.vars.get(vname)
            if v is not None and v.bt == 'pointer':
                raise Refuse('resuming with the pointer %s live: %s' % (vname, why))
            if v is not None and v.virt is not None and v.owned():
                raise Refuse('resuming while the virtual %s is being built: %s' % (vname, why))
            if v is not None and v.bt == 'dynamic' and v.owned() and w.u.is_object(v.ctype):
                if v.own == 'mixed':
                    raise Refuse('resuming with %s owned on some paths' % vname)
                owned.add(vname)
        name, params = tr.copies.rest(w, frames, live, owned)
    fmt, args = '', []
    for pname, _ in params:
        v = p.vars.get(pname)
        if v is None or (v.own == 'none' and v.bt == 'dynamic' and not v.isparam):
            raise Refuse('resuming needs %s, which has no value here' % pname)
        if v.bt == 'dynamic':
            fmt += 'd' if w.u.is_double(v.ctype) else 'v'
            args.append(v.cname)
        else:
            fmt += 'l'
            args.append('(long)(%s)' % v.cname)
    notimpl = w.sm.kind == 'obj' and notimpl_possible(w, frames)
    conv = conventions.resume_conv(w.sm.kind, notimpl)
    if conv is None:
        raise Refuse('resuming in a compile-time function')
    flags = conventions.flags(w.u, fn.decl.type.type, conv, tr.effect_flags(tr.effects_key(w.name)))
    call = 'psyco_generic_call(po, %s, %s, "%s"%s)' % (name, flags, fmt,
                                                     ''.join(', ' + a for a in args))
    p.called = True
    w.rec.any_call = True
    w.ret_value(p, out, call, w.sm.kind == 'ok', notimpl=conv == 'notimpl')


def inlined_frames(frames):
    """Is the statement inside a call's body inlined in place? (A copy is
    of the C as written, which calls the function instead.)"""
    return any(inline.expanded(stmts[i]) for stmts, i in frames[:-1])


def notimpl_possible(w, frames):
    """The resumed code may return NotImplemented: the function can, and
    (if its result type is asserted) the rest from here can."""
    if not w.tr.returns_notimpl(w.fn):
        return False
    if not w.restype:
        return True
    rest = []
    for level, (stmts, i) in enumerate(reversed(frames)):
        rest.extend(stmts[i:] if level == 0 else stmts[i + 1:])
    return any(w.tr.returns_notimpl_in(s) for s in rest)


class Copies:
    """Run-time copies of the family's functions, C text from the source,
    in the order made; a copy follows the copies it calls."""

    def __init__(self, tr):
        self.tr = tr
        self.keys = {}            # key -> (name, params)
        # the statement lists keys name by id(), kept so that no later
        # list (a branch's [stmt]) can take one's id and match its key
        self._pinned = []
        self.texts = {}           # name -> C
        self.order = []
        self.making = set()
        self._macros = None

    def whole(self, fname):
        fn = self.tr.funcs[fname]
        params = [(p.name, p.type) for p in params_of(fn)]
        return self._make(fn, [(fn.body.block_items or [], 0)], params, set(), whole=True)[0]

    def rest(self, w, frames, live, owned):
        fn = w.fn
        types = dict(w.scope)
        order = [n for n, _ in w.params] + sorted(n for n in types if n not in dict(w.params))
        params = [(v, types[v]) for v in order if v in live]
        return self._make(fn, frames, params, owned)

    def _make(self, fn, frames, params, owned, whole=False):
        fname = fn.decl.name
        key = (fname, tuple((id(s), i) for s, i in frames), tuple(sorted(owned)))
        if key in self.keys:
            return self.keys[key]
        if key in self.making:
            raise Refuse('copy: %s calls itself' % fname)
        n = sum(1 for k in self.keys if k[0] == fname and self.keys[k][0] != 'cimpl_' + fname)
        name = 'cimpl_%s' % fname if whole else 'cimpl_%s_%d' % (fname, n + 1)
        if whole and name in self.texts:
            name += '_whole'        # a scalar helper has the name
        for pn, t in params:
            if isinstance(t, c_ast.ArrayDecl):
                raise Refuse('copy: array %s is live' % pn)
        parts, used = [], []
        from .scalar import ends
        for stmts, i in reversed(frames):
            rest = [inline.orig(x) for x in (stmts[i:] if not parts else stmts[i + 1:])]
            used.extend(rest)
            parts.append(' '.join(join_decls(rest)))
            if ends(rest) and not leaves_block(rest):
                break           # what follows the block never runs
        body = ''
        for part in parts:
            body = ('{ %s } ' % body if body else '') + part
        declared_inside = {d.name for s in used for d in decls_in(s)}
        locs = {}
        for d in decls_in(self.tr.funcs[fname].body):
            locs.setdefault(d.name, d.type)
        pnames = {pn for pn, _ in params}
        decls = []
        for v in sorted(set(x for s in used for x in names(s))):
            if v in locs and v not in pnames and v not in declared_inside:
                if isinstance(locs[v], c_ast.ArrayDecl):
                    raise Refuse('copy: an array declared outside it')
                decls.append('%s %s;' % (text(locs[v]), v))
        self.making.add(key)
        try:
            body = self.rename_statics(body)
        finally:
            self.making.discard(key)
        body = ''.join('Py_INCREF(%s); ' % v for v in sorted(owned)) + body
        ret = text(fn.decl.type.type)
        c = 'static %s %s(%s) {\n  %s\n  %s\n}\n' % (
            ret, name,
            ', '.join(text(t, pn) for pn, t in params) or 'void', ' '.join(decls), body)
        self.keys[key] = (name, params)
        self._pinned.extend(s for s, _ in frames)
        self.texts[name] = c
        self.order.append(name)
        return name, params

    def helper(self, fname, key, ret, params, lines):
        """A scalar helper (scalar.py): cimpl_FUNCTION, then cimpl_FUNCTION_sN."""
        key = ('scalar', fname, key)
        if key in self.keys:
            name = self.keys[key][0]
            self.order.remove(name)
        else:
            n = sum(1 for k in self.keys if k[0] == 'scalar' and k[1] == fname)
            # the first is cimpl_FUNCTION, unless the whole copy has it
            name = 'cimpl_%s' % fname if n == 0 else 'cimpl_%s_s%d' % (fname, n + 1)
            if name in self.texts:
                name = 'cimpl_%s_s%d' % (fname, n + 1)
        self.keys[key] = (name, None)
        self.texts[name] = '/* %s:%s: scalar code */\nstatic %s %s(%s) {\n%s\n}\n' % (
            self.tr.family.cfile, fname, ret, name, ', '.join(params) or 'void',
            '\n'.join(lines))
        self.order.append(name)
        return name

    def drop(self, name):
        if name in self.texts:
            del self.texts[name]
            self.order.remove(name)
            for k in [k for k, v in self.keys.items() if v[0] == name]:
                del self.keys[k]

    def local_macros(self):
        """The function-like macros the family's file defines (Ni's file
        including the copies doesn't), to expand in a copy's text."""
        if self._macros is None:
            import ctokens
            files = [f.coord.file for f in self.tr.funcs.values()]
            path = next((f for f in files if f.endswith(self.tr.family.cfile)), files[0])
            try:
                with open(path, encoding='latin-1') as f:
                    macros = ctokens.tokenize(f.read(), path)[1]
            except OSError:
                macros = {}     # C given as text (a test's)
            self._macros = macros, {n for n, m in macros.items() if m.params is None}
        return self._macros

    def rename_statics(self, body):
        import ctokens
        import cparse
        import cslots
        macros, keep = self.local_macros()
        toks = cparse.expand(ctokens.tokenize(body)[0], macros, keep)
        out = []
        for i, t in enumerate(toks):
            g = self.tr.funcs.get(t.text) if t.kind == 'id' else None
            if g is not None and 'static' in g.decl.storage and i + 1 < len(toks) \
                    and toks[i + 1].text == '(':
                t = t._replace(text=self.whole(t.text))
            out.append(t)
        return cslots.text(out)


def leaves_block(stmts):
    """A goto, or a break or continue of no loop or switch inside stmts."""
    def walk(n, inner):
        if isinstance(n, (c_ast.Goto, c_ast.Label)):
            return True
        if isinstance(n, (c_ast.Break, c_ast.Continue)) and not inner:
            return True
        inner = inner or isinstance(n, (c_ast.For, c_ast.While, c_ast.DoWhile, c_ast.Switch))
        return any(walk(c, inner) for _, c in n.children())
    return any(walk(s, False) for s in stmts)


def params_of(fn):
    args = fn.decl.type.args
    return [p for p in (args.params if args else []) if isinstance(p, c_ast.Decl) and p.name]


def decls_in(n):
    out = []

    def walk(x):
        if isinstance(x, c_ast.Decl) and x.name and not isinstance(x.type, c_ast.FuncDecl):
            out.append(x)
        for _, c in x.children():
            walk(c)
    walk(n)
    return out


def join_decls(stmts):
    """The statements' source texts; the declarators of one declaration
    ('PyObject *a, *b;') are one text."""
    out, last = [], None
    for s in stmts:
        t = cprint.stmt_text(s, limit=None)
        if isinstance(s, c_ast.Decl) and last is not None and t == last[1] \
                and s.coord.line == last[0]:
            continue
        out.append(t)
        last = (s.coord.line, t) if isinstance(s, c_ast.Decl) else None
    return out
