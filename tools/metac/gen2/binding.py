"""Argument binding (doc/GENERATOR_DESIGN.md "One translator, four entry
points"): the part of the C function that makes and runs a frame
(scope.BINDING: ceval.c's _PyEval_EvalCodeWithName) between making the
frame and running it, as one C function the compiler runs while it
compiles a call. It emits no code.

The slice runs from the first statement whose reads of the frame are all
fields Ni's declaration passes (f->f_localsplus) to the first that uses
the frame otherwise. Binding times come from Ni's declaration of the
binder (conventions.psyco_name of the C name):
  array    a vinfo_t ** parameter: its length and which slots are NULL
           are known, its contents are vinfos; a local assigned one, or one
           plus a meta offset, is one. A tuple the C reads through its items
           field (the known_length spec's) is the array of its items;
  meta     any other parameter, the integer locals and what is computed
           from meta values: C in the binder;
  dynamic  an item of an array, and an object local assigned one: a vinfo;
  gen      a C parameter Ni's declaration leaves out has the value
           conventions.BINDING gives (Ni's one call passes it), folded with
           C's semantics (G5) into the tests that read it; a loop whose test
           folds false on entry is not generated, so code that would read
           what Ni doesn't pass never runs. A read of a parameter it leaves
           out with no value is refused.
A local's binding time is the one of the values assigned to it (to a
fixed point); one assigned only what has no form, or set before the slice
by a statement the binder doesn't replay, has none, and reading it is a
restart.

Actions: fold (meta C, and gen constants folded), the vocabulary's
refcounts as Ni's (vinfo_incref, vinfo_xdecref), a store into an array
slot or a virtual's item, a known_length constructor of a meta size as
Ni's virtual (behind its item limit; never NULL), and restart: anything
with no form when Ni compiles (a call needing the objects, an error being
set, a goto out of the slice) is 'return -1', and the caller makes the real
call. Nothing has been emitted, so stopping anywhere is safe, but a
reference the binder holds would leak: ownership is checked. A reference
is held from an incref of a local or a constructor's result until it is
stored or released; a restart, an overwrite or the end while one is held,
a store of a local holding none, or paths merging with different holdings,
is refused.

Declarations are hoisted (cbuild.Function), and a local whose value
nothing reads is dropped with its assignments.
"""
import os
import re

from pycparser import c_ast

import cpython_src
import cslots
import front
from . import conventions, scope, types
from .cbuild import Code, Function
from .ctyping import Unit, strip_casts, text
from .cprint import is_null
from .errors import Refuse
from .vocabulary import of_marker

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
# the vocabulary's refcounts, as the binder makes them
REFS = {'Py_INCREF': 'vinfo_incref(%s);', 'Py_XINCREF': 'vinfo_xincref(%s);',
        'Py_DECREF': 'vinfo_decref(%s, po);', 'Py_XDECREF': 'vinfo_xdecref(%s, po);'}


class NoForm(Exception):
    """No form when Ni compiles: the binding restarts here."""


class Val:
    """bt: gen (a constant: `const`), meta, array, dynamic, items (a
    virtual's items: `virt`); c: its C text."""

    def __init__(self, bt, c, const=None, virt=None, var=None, null=False):
        self.bt, self.c, self.const, self.virt, self.var = bt, c, const, virt, var
        self.null = null


class State:
    """One path: gen constants of meta locals and absent parameters, the
    locals holding a reference, the virtuals made (never NULL), and
    whether the path has left."""

    def __init__(self):
        self.consts, self.held, self.virt = {}, set(), {}
        self.ended = False

    def copy(self):
        s = State()
        s.consts, s.held, s.virt = dict(self.consts), set(self.held), dict(self.virt)
        s.ended = self.ended
        return s


def merge(states, where):
    live = [s for s in states if not s.ended]
    out = State()
    if not live:
        out.ended = True
        return out
    if any(s.held != live[0].held for s in live):
        raise Refuse('paths meet at %s holding different references (%s)'
                     % (where, ' / '.join(', '.join(sorted(s.held)) or 'none' for s in live)))
    out.held = set(live[0].held)
    out.consts = {k: v for k, v in live[0].consts.items()
                  if all(s.consts.get(k, object()) == v for s in live)}
    out.virt = {k: v for k, v in live[0].virt.items() if all(s.virt.get(k) is v for s in live)}
    return out


class Binder:
    def __init__(self, unit, fn, decl, absent):
        """unit: the file's declarations; fn: the C function's FuncDef;
        decl: Ni's NiDecl of the binder; absent: {C parameter: its value
        as C} for the parameters Ni's declaration leaves out."""
        self.u, self.fn, self.decl = unit, fn, decl
        self.name = fn.decl.name
        self.cparams = {p.name: p.type for p in params_of(fn)}
        for n in absent:
            if n not in self.cparams:
                raise Refuse('%s has no parameter %s (conventions.BINDING)' % (self.name, n))
            if any(n == pn for _, pn in decl.params):
                raise Refuse('%s passes %s, which conventions.BINDING gives a value'
                             % (decl.name, n))
        self.absent = {n: const_of(c) for n, c in absent.items()}
        self.params = {}            # Ni parameter -> Val
        for ctype, pname in decl.params:
            if pname == 'po':
                continue
            if 'vinfo_t' in ctype and ctype.count('*') == 2:
                self.params[pname] = Val('array', pname)
            elif 'vinfo_t' in ctype:
                self.params[pname] = Val('dynamic', pname)
            else:
                self.params[pname] = Val('meta', pname)
        self.items_fields = {s: spec['items'] for s, spec in types.length_spec().items()}
        self.ctors = {spec['constructor']: spec for spec in types.length_spec().values()}
        self.bts, self.ctypes = {}, {}
        self.decls = {}
        self.assigns = []           # (Code, index, local) of each assignment line
        self.labels = {}            # label -> [State] of the gotos to it
        self.tmp = 0

    # ---- the slice ----

    def frame_var(self):
        frames = [d.name for d in body(self.fn.body) if isinstance(d, c_ast.Decl)
                  and text(d.type).replace('struct ', '') in ('PyFrameObject *', '_frame *')]
        if len(frames) != 1:
            raise Refuse('%s: no one frame variable (%s)' % (self.name, frames))
        return frames[0]

    def frame_uses(self, node):
        """The fields of the frame read in node; '' for a use of the
        frame itself."""
        out = []

        def walk(n):
            if isinstance(n, c_ast.StructRef) and n.type == '->' \
                    and isinstance(n.name, c_ast.ID) and n.name.name == self.fv:
                out.append(n.field.name)
                return
            if isinstance(n, c_ast.ID) and n.name == self.fv:
                out.append('')
                return
            for _, c in n.children():
                walk(c)
        walk(node)
        return out

    def slice(self):
        self.fv = self.frame_var()
        stmts = body(self.fn.body)
        start = end = None
        for k, s in enumerate(stmts):
            uses = self.frame_uses(s)
            if start is None:
                if uses and all(u in self.params for u in uses):
                    start = k
            elif uses and not all(u in self.params for u in uses):
                end = k
                break
        if start is None or end is None:
            raise Refuse('%s: no statements between the frame made and used' % self.name)
        return stmts[:start], stmts[start:end]

    # ---- binding times of locals ----

    def infer(self, prefix, stmts):
        decls = {}
        for d in [s for s in prefix if isinstance(s, c_ast.Decl)] + decls_in(stmts):
            if d.name is None or isinstance(d.type, c_ast.FuncDecl):
                continue
            if d.name in decls and text(decls[d.name].type) != text(d.type):
                raise Refuse('%s: %s declared as %s and %s' % (
                    self.name, d.name, text(decls[d.name].type), text(d.type)))
            decls[d.name] = d
        for n in set(decls) & set(self.cparams):
            if read_outside(n, stmts):
                raise Refuse('%s: a local %s hides the parameter, which is read too'
                             % (self.name, n))
        unknown = set()
        for s in prefix:
            if not isinstance(s, c_ast.Decl):
                unknown |= {n for n, _ in assignments(s)}
        rhs = {}
        for d in decls.values():
            if d.init is not None:
                rhs.setdefault(d.name, []).append(d.init)
        for n, e in assignments(stmts):
            rhs.setdefault(n, []).append(e)
        for n in decls:
            self.ctypes[n] = decls[n].type
            if self.u.is_int(decls[n].type):
                self.bts[n] = 'meta'
        st = State()
        st.consts.update(self.absent)
        for _ in range(10):
            changed = False
            for n in decls:
                if n in self.bts or n in unknown:
                    continue
                for e in rhs.get(n, ()):
                    if is_null(e):
                        continue
                    try:
                        bt = self.ev(e, st.copy(), Code(), infer=True).bt
                    except (NoForm, KeyError, Refuse):
                        continue        # no form, or not read on any path walked
                    self.bts[n] = 'meta' if bt == 'gen' else 'dynamic' if bt == 'items' else bt
                    changed = True
                    break
            if not changed:
                break
        for n, d in decls.items():
            if n in self.bts:
                continue
            if n in unknown or rhs.get(n) and not any(is_null(e) for e in rhs[n]):
                self.bts[n] = 'none'
            elif self.u.is_object(d.type):
                self.bts[n] = 'dynamic'
            elif self.u.is_object_array_ptr(d.type):
                self.bts[n] = 'array'
            else:
                self.bts[n] = 'none'
        for n, bt in self.bts.items():
            if bt == 'meta':
                self.decls[n] = '%s;' % text(unconst(self.ctypes[n]), n)
            elif bt == 'dynamic':
                self.decls[n] = 'vinfo_t *%s;' % n
            elif bt == 'array':
                self.decls[n] = 'vinfo_t **%s;' % n

    # ---- the binder ----

    def run(self):
        prefix, stmts = self.slice()
        self.infer(prefix, stmts)
        out = Code()
        st = State()
        st.consts.update(self.absent)
        used = names_in(stmts)
        for d in prefix:
            if isinstance(d, c_ast.Decl) and d.init is not None and d.name in used \
                    and self.bts.get(d.name) != 'none':
                try:
                    self.assign(d.name, d.init, st, out)
                except NoForm as e:
                    raise Refuse('%s: %s, which the binding reads, has no form: %s'
                                 % (self.name, d.name, e))
        self.block(stmts, st, out)
        if self.labels:
            raise Refuse('%s: goto %s, a label not after it in its block'
                         % (self.name, ', '.join(sorted(self.labels))))
        if not st.ended:
            self.check_held(st, 'the end of the binding')
            out.line('return 0;')
        drop_unread(out, self.assigns, self.decls)
        params = ', '.join('%s%s%s' % (t, '' if t.endswith('*') else ' ', n)
                           for t, n in self.decl.params)
        head = 'DEFINEFN\n%s %s(%s)' % (self.decl.ret, self.decl.name, params)
        return Function(head, self.decls, out).render()

    def check_held(self, st, where):
        if st.held:
            raise Refuse('%s: %s while %s holds a reference' % (
                self.name, where, ', '.join(sorted(st.held))))

    def restart(self, st, out, why):
        self.check_held(st, 'a restart (%s)' % why)
        out.line('return -1; /* %s: the real call */' % why)
        st.ended = True

    # ---- statements ----

    def block(self, stmts, st, out):
        for s in stmts:
            if st.ended and not has_label(s):
                continue
            if st.ended and not isinstance(s, c_ast.Label):
                raise Refuse('%s: a label inside a statement no path reaches but by a goto (%s)' % (self.name, s.coord))
            self.stmt(s, st, out)

    def stmt(self, s, st, out):
        if isinstance(s, c_ast.Compound):
            self.block(body(s), st, out)
        elif isinstance(s, c_ast.EmptyStatement):
            return
        elif once_do(s):
            self.stmt(s.stmt, st, out)
        elif isinstance(s, c_ast.Decl):
            if isinstance(s.type, c_ast.FuncDecl) or s.name is None:
                raise Refuse('%s: a declaration of %s' % (self.name, s.name))
            if s.init is not None and self.bts.get(s.name) != 'none':
                self.restartable(lambda: self.assign(s.name, s.init, st, out), st, out)
        elif isinstance(s, c_ast.If):
            self.if_(s, st, out)
        elif isinstance(s, c_ast.For):
            self.for_(s, st, out)
        elif isinstance(s, (c_ast.While, c_ast.DoWhile)):
            self.restart(st, out, 'a loop with a run-time test')
        elif isinstance(s, c_ast.Goto):
            self.goto(s, st, out)
        elif isinstance(s, c_ast.Label):
            self.label(s, st, out)
        elif isinstance(s, (c_ast.Break, c_ast.Continue)):
            if not self.loops:
                raise Refuse('%s: %s outside a loop' % (self.name, type(s).__name__.lower()))
            if st.held != self.loops[-1]:
                raise Refuse('%s: a %s holding %s' % (self.name, type(s).__name__.lower(),
                                                      ', '.join(sorted(st.held))))
            out.line('%s;' % type(s).__name__.lower())
            st.ended = True
        elif isinstance(s, c_ast.Return):
            raise Refuse('%s: a return inside the binding' % self.name)
        elif isinstance(s, c_ast.Switch):
            raise Refuse('%s: a switch inside the binding' % self.name)
        else:
            self.restartable(lambda: self.expr_stmt(s, st, out), st, out)

    loops = ()

    def restartable(self, f, st, out):
        try:
            f()
        except NoForm as e:
            self.restart(st, out, str(e))

    def if_(self, s, st, out):
        try:
            c = self.cond(s.cond, st, out)
        except NoForm as e:
            self.restart(st, out, str(e))
            return
        if c.bt == 'gen':
            branch = s.iftrue if c.const else s.iffalse
            if branch is not None:
                self.stmt(branch, st, out)
            return
        a, b = st.copy(), st.copy()
        then, other = out.sub(), out.sub()
        self.stmt(s.iftrue, a, then)
        if s.iffalse is not None:
            self.stmt(s.iffalse, b, other)
        out.if_(c.c, then, other)
        m = merge([a, b], 'the end of an if')
        st.consts, st.held, st.virt, st.ended = m.consts, m.held, m.virt, m.ended

    def for_(self, s, st, out):
        try:
            if isinstance(s.init, c_ast.DeclList):
                raise Refuse('%s: a declaration in a for head' % self.name)
            init = self.ev(s.init, st, out).c if s.init is not None else ''
            if s.cond is not None:
                first = self.cond(s.cond, st, out)
                if first.bt == 'gen' and not first.const:
                    if init:
                        out.line('%s;' % (init[1:-1] if isinstance(s.init, c_ast.Assignment)
                                          else init))
                    return              # never entered
        except NoForm as e:
            self.restart(st, out, str(e))
            return
        assigned = {n for n, _ in assignments(s.stmt)} | {n for n, _ in assignments(s.next)}
        assigned |= set(stepped(s.stmt)) | set(stepped(s.next))
        try:
            for n in assigned:
                st.consts.pop(n, None)
            entry = st.copy()
            cond = self.cond(s.cond, entry, out).c if s.cond is not None else ''
            nxt = self.ev(s.next, entry, out).c if s.next is not None else ''
        except NoForm as e:
            raise Refuse('%s: a loop whose test or step has no form: %s' % (self.name, e))
        inner = out.sub()
        b = entry.copy()
        outer, self.loops = self.loops, tuple(self.loops) + (set(entry.held),)
        try:
            self.stmt(s.stmt, b, inner)
        finally:
            self.loops = outer
        if not b.ended and b.held != entry.held:
            raise Refuse('%s: a loop turn changes what is held (%s)'
                         % (self.name, ', '.join(sorted(b.held ^ entry.held))))
        for n in assigned:
            entry.consts.pop(n, None)
        out.block('for (%s; %s; %s)' % (init, cond, nxt), inner)
        st.consts, st.held, st.virt = entry.consts, entry.held, entry.virt

    def goto(self, s, st, out):
        if s.name not in labels_in(self.fn.body) or s.name not in self.slice_labels():
            self.restart(st, out, 'goto %s' % s.name)
            return
        self.labels.setdefault(s.name, []).append(st.copy())
        out.line('goto %s;' % s.name)
        st.ended = True

    def slice_labels(self):
        if not hasattr(self, '_slice_labels'):
            self._slice_labels = labels_in(c_ast.Compound(self.slice()[1]))
        return self._slice_labels

    def label(self, s, st, out):
        m = merge([st] + self.labels.pop(s.name, []), 'label %s' % s.name)
        st.consts, st.held, st.virt, st.ended = m.consts, m.held, m.virt, m.ended
        out.line('%s:;' % s.name)
        if not st.ended or has_label(s.stmt):
            self.stmt(s.stmt, st, out)

    def expr_stmt(self, e, st, out):
        if isinstance(e, c_ast.Cast) and isinstance(e.expr, c_ast.Constant):
            return                      # assert() under NDEBUG
        if isinstance(e, c_ast.ExprList):
            for x in e.exprs:
                self.expr_stmt(x, st, out)
                if st.ended:
                    return
            return
        if isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID):
            v = of_marker(e.name.name)
            if v is not None and v.name in REFS:
                self.refcount(v.name, e.args.exprs, st, out)
                return
        if isinstance(e, c_ast.Assignment):
            lv = strip_casts(e.lvalue)
            if isinstance(lv, c_ast.ID) and e.op == '=':
                self.assign(lv.name, e.rvalue, st, out)
                return
            if isinstance(lv, c_ast.ID):
                self.update(lv.name, e.op, e.rvalue, st, out)
                return
            if e.op != '=':
                raise Refuse('%s: %s into %s' % (self.name, e.op, cexpr(lv)))
            self.store(lv, e.rvalue, st, out)
            return
        if isinstance(e, c_ast.UnaryOp) and e.op in ('++', '--', 'p++', 'p--'):
            out.line('%s;' % self.ev(e, st, out).c)
            return
        v = self.ev(e, st, out)
        raise Refuse('%s: an expression statement of a %s value: %s' % (self.name, v.bt, v.c))

    def refcount(self, name, args, st, out):
        v = self.ev(args[0], st, out)
        if v.bt != 'dynamic':
            raise Refuse('%s: %s of a %s value %s' % (self.name, name, v.bt, v.c))
        if name in ('Py_INCREF', 'Py_XINCREF'):
            if v.var is None:
                raise Refuse('%s: %s of %s, which no local holds' % (self.name, name, v.c))
            if v.var in st.held:
                raise Refuse('%s: %s of %s, which holds a reference already'
                             % (self.name, name, v.var))
            st.held.add(v.var)
        elif v.var is not None:
            st.held.discard(v.var)
        out.line(REFS[name] % v.c)

    def assign(self, name, e, st, out):
        bt = self.bts.get(name)
        if name in self.cparams and name not in self.bts:
            raise Refuse('%s: an assignment to its parameter %s' % (self.name, name))
        if bt is None:
            raise Refuse('%s: an assignment to %s, no local' % (self.name, name))
        if bt == 'none':
            self.ev(e, st, out)         # names the call with no form
            raise NoForm('%s (no form)' % name)
        if name in st.held:
            raise Refuse('%s: %s assigned while it holds a reference' % (self.name, name))
        v = self.ev(e, st, out)
        if v.null and bt == 'dynamic':
            v = Val('dynamic', 'NULL', null=True)
        elif v.bt == 'gen':
            v = Val('meta', v.c, const=v.const)
        elif v.bt == 'items':
            raise Refuse('%s: %s assigned the items of %s' % (self.name, name, v.c))
        if v.bt != bt:
            raise Refuse('%s: %s is %s but assigned a %s value %s'
                         % (self.name, name, bt, v.bt, v.c))
        st.consts.pop(name, None)
        st.virt.pop(name, None)
        if v.const is not None and bt == 'meta':
            st.consts[name] = v.const
        if v.virt is not None:
            st.virt[name] = v.virt
            if v.var is None:
                st.held.add(name)       # the constructor's reference
        self.line(out, '%s = %s;' % (name, v.c), name)

    def update(self, name, op, e, st, out):
        """name op= e: on an absent parameter only when it folds."""
        v = self.ev(e, st, out)
        if name in self.absent:
            if v.bt != 'gen' or name not in st.consts:
                raise Refuse('%s: %s %s %s does not fold' % (self.name, name, op, v.c))
            st.consts[name] = fold(op[:-1], st.consts[name], v.const)
            return
        if self.bts.get(name) != 'meta' or v.bt not in ('meta', 'gen'):
            raise Refuse('%s: %s %s a %s value' % (self.name, name, op, v.bt))
        st.consts.pop(name, None)
        out.line('%s %s %s;' % (name, op, v.c))

    def store(self, lv, e, st, out):
        """An object store: into a slot of an array, or an item of a
        virtual made here; the value is a local holding a reference, which
        the slot takes."""
        if not isinstance(lv, c_ast.ArrayRef):
            raise Refuse('%s: a store into %s' % (self.name, cexpr(lv)))
        a = self.ev(lv.name, st, out)
        i = self.ev(lv.subscript, st, out)
        if i.bt not in ('meta', 'gen'):
            raise Refuse('%s: an index that is a value' % self.name)
        if a.bt == 'array':
            place = '%s[%s]' % (a.c, i.c)
        elif a.bt == 'items':
            place = '%s(%s, %s)' % (a.virt['item'], a.c, i.c)
        else:
            raise Refuse('%s: a store into %s' % (self.name, cexpr(lv)))
        v = self.ev(e, st, out)
        if v.bt != 'dynamic':
            raise Refuse('%s: a store of a %s value into %s' % (self.name, v.bt, place))
        if v.var is not None and v.var in st.held:
            st.held.discard(v.var)
        elif not v.null:
            raise Refuse('%s: %s stores %s, which holds no reference'
                         % (self.name, place, v.c))
        out.line('%s = %s;' % (place, v.c))

    def cond(self, e, st, out):
        v = self.ev(e, st, out)
        if v.bt == 'gen':
            return v
        if v.bt == 'dynamic':
            return Val('meta', '%s != NULL' % v.c)
        if v.bt != 'meta':
            raise Refuse('%s: a test of a %s value: %s' % (self.name, v.bt, cexpr(e)))
        return v

    def line(self, out, s, name):
        out.line(s)
        self.assigns.append((out, len(out.items) - 1, name))

    # ---- expressions ----

    def ev(self, e, st, out, infer=False):
        if isinstance(e, c_ast.Constant):
            k = const_value(e)
            if k is not None:
                return Val('gen', e.value, const=k)
            return Val('meta', e.value)
        if isinstance(e, c_ast.Cast):
            if is_null(e):
                return Val('gen', 'NULL', const=0, null=True)
            v = self.ev(e.expr, st, out, infer)
            if v.bt in ('meta', 'gen'):
                return Val('meta', '((%s)%s)' % (text(e.to_type), v.c))
            return v
        if isinstance(e, c_ast.ID):
            return self.ev_id(e.name, st, infer)
        if isinstance(e, c_ast.StructRef):
            return self.ev_field(e, st, out, infer)
        if isinstance(e, c_ast.ArrayRef):
            a = self.ev(e.name, st, out, infer)
            i = self.ev(e.subscript, st, out, infer)
            if i.bt not in ('meta', 'gen'):
                raise Refuse('%s: an index that is a value' % self.name)
            if a.bt == 'meta':
                return Val('meta', '%s[%s]' % (a.c, i.c))
            if a.bt == 'array':
                return Val('dynamic', '%s[%s]' % (a.c, i.c))
            if a.bt == 'items':
                return Val('dynamic', '%s(%s, %s)' % (a.virt['item'], a.c, i.c))
            if a.bt == 'gen':
                raise Refuse("%s reads an item of %s, which Ni's %s does not pass"
                             % (self.name, cexpr(e.name), self.decl.name))
            raise Refuse('%s: an index into a %s value' % (self.name, a.bt))
        if isinstance(e, c_ast.UnaryOp):
            return self.ev_unary(e, st, out, infer)
        if isinstance(e, c_ast.BinaryOp):
            return self.ev_binary(e, st, out, infer)
        if isinstance(e, c_ast.TernaryOp):
            c = self.ev(e.cond, st, out, infer)
            if c.bt == 'gen':
                return self.ev(e.iftrue if c.const else e.iffalse, st, out, infer)
            a = self.ev(e.iftrue, st, out, infer)
            b = self.ev(e.iffalse, st, out, infer)
            if c.bt == 'meta' and a.bt in ('meta', 'gen') and b.bt in ('meta', 'gen'):
                return Val('meta', '(%s ? %s : %s)' % (c.c, a.c, b.c))
            raise NoForm('a choice between objects')
        if isinstance(e, c_ast.FuncCall):
            if not isinstance(e.name, c_ast.ID):
                raise NoForm('a call through a pointer')
            name = e.name.name
            if name in self.ctors:
                return self.construct(name, e.args.exprs if e.args else [], st, out, infer)
            v = of_marker(name)
            raise NoForm(v.name if v else name)
        if isinstance(e, c_ast.Assignment):
            lv = strip_casts(e.lvalue)
            if not isinstance(lv, c_ast.ID) or self.bts.get(lv.name) != 'meta':
                raise Refuse('%s: an assignment of a value inside an expression' % self.name)
            v = self.ev(e.rvalue, st, out, infer)
            if v.bt not in ('meta', 'gen'):
                raise Refuse('%s: %s is meta but assigned a %s value' % (self.name, lv.name,
                                                                         v.bt))
            st.consts.pop(lv.name, None)
            if e.op == '=' and v.bt == 'gen':
                st.consts[lv.name] = v.const
            return Val('meta', '(%s %s %s)' % (lv.name, e.op, v.c))
        if isinstance(e, c_ast.ExprList):
            # '(assert(...), x)' under NDEBUG: '((void) 0, x)'
            for x in e.exprs[:-1]:
                if not (isinstance(x, c_ast.Cast) and isinstance(x.expr, c_ast.Constant)):
                    raise Refuse('%s: a comma expression' % self.name)
            return self.ev(e.exprs[-1], st, out, infer)
        raise Refuse('%s: the expression %s' % (self.name, type(e).__name__))

    def ev_id(self, name, st, infer):
        if name in self.bts and name not in self.params:
            bt = self.bts[name]
            if bt == 'none':
                raise NoForm('%s (no form)' % name)
            if name in st.consts:
                return Val('gen', name, const=st.consts[name])
            return Val(bt, name, var=name, virt=st.virt.get(name))
        if name in self.params:
            return self.params[name]
        if name in self.absent:
            k = st.consts[name]
            return Val('gen', c_const(k, self.cparams[name], self.u), const=k)
        if name in self.cparams:
            raise Refuse('%s reads %s, which Ni\'s %s does not pass'
                         % (self.name, name, self.decl.name))
        if infer and name in self.ctypes:
            raise KeyError(name)
        return Val('meta', name)        # an enumerator or a global of the compiler

    def ev_field(self, e, st, out, infer):
        if e.type == '->' and isinstance(e.name, c_ast.ID) and e.name.name == self.fv:
            if e.field.name not in self.params:
                raise Refuse("%s: the frame's %s, which Ni's %s does not pass"
                             % (self.name, e.field.name, self.decl.name))
            return self.params[e.field.name]
        v = self.ev(e.name, st, out, infer)
        if v.bt in ('meta', 'gen'):
            return Val('meta', '%s%s%s' % (v.c, e.type, e.field.name))
        struct = cast_struct(e.name)
        if e.type == '->' and struct is not None \
                and self.items_fields.get(struct) == e.field.name:
            if v.bt == 'array':
                return Val('array', v.c)        # a tuple passed as its items
            if v.bt == 'dynamic' and v.virt is not None:
                return Val('items', v.c, virt=v.virt)
        raise NoForm('the field %s of an object' % e.field.name)

    def ev_unary(self, e, st, out, infer):
        if e.op == 'sizeof':
            return Val('meta', 'sizeof(%s)' % (text(e.expr) if isinstance(e.expr, c_ast.Typename)
                                                else cexpr(e.expr)))
        v = self.ev(e.expr, st, out, infer)
        if e.op in ('++', '--', 'p++', 'p--'):
            if v.var is None or self.bts.get(v.var) != 'meta':
                raise Refuse('%s: %s of a %s value' % (self.name, e.op, v.bt))
            st.consts.pop(v.var, None)
            return Val('meta', ('%s%s' % (v.var, e.op[1:])) if e.op.startswith('p')
                       else '%s%s' % (e.op, v.var))
        if v.bt == 'gen' and e.op in ('-', '~', '!', '+'):
            k = {'-': -v.const, '~': ~v.const, '!': int(not v.const), '+': v.const}[e.op]
            return Val('gen', ('%s%s' if v.c.isdigit() else '%s(%s)') % (e.op, v.c), const=k)
        if v.bt in ('meta', 'gen') and e.op != '*':
            return Val('meta', '%s(%s)' % (e.op, v.c))
        if v.bt == 'dynamic' and e.op == '!':
            if v.var in st.virt:
                return Val('gen', '0', const=0)
            return Val('meta', '!(%s)' % v.c)
        raise NoForm('%s of an object' % e.op)

    def ev_binary(self, e, st, out, infer):
        a = self.ev(e.left, st, out, infer)
        if e.op in ('&&', '||') and a.bt == 'gen' and bool(a.const) == (e.op == '||'):
            return Val('gen', str(int(bool(a.const))), const=int(bool(a.const)))
        b = self.ev(e.right, st, out, infer)
        if a.bt == 'gen' and b.bt == 'gen':
            k = fold(e.op, a.const, b.const)
            if k is not None:
                return Val('gen', '(%s %s %s)' % (a.c, e.op, b.c), const=k)
        if e.op in ('&&', '||') and b.bt == 'gen' and bool(b.const) == (e.op == '||') \
                and a.bt in ('meta', 'gen'):
            # C's 'a || 1' is 1, 'a && 0' is 0, once a has been evaluated
            return Val('meta', '(%s %s %s)' % (a.c, e.op, b.c))
        sc = {'meta', 'gen'}
        if a.bt in sc and b.bt in sc:
            return Val('meta', '(%s %s %s)' % (a.c, e.op, b.c))
        if e.op in ('==', '!=') and 'dynamic' in (a.bt, b.bt) and (a.null or b.null):
            # whether a slot holds a value is known when Ni compiles; a
            # virtual made here is never NULL
            x = a if b.null else b
            if x.var in st.virt:
                k = int(e.op == '!=')
                return Val('gen', str(k), const=k)
            return Val('meta', '(%s %s NULL)' % (x.c, e.op))
        if e.op in ('+', '-') and a.bt == 'array' and b.bt in sc:
            return Val('array', '(%s %s %s)' % (a.c, e.op, b.c))
        if e.op in ('&&', '||'):
            parts = []
            for x in (a, b):
                if x.bt == 'dynamic':
                    parts.append('(%s != NULL)' % x.c)
                elif x.bt in sc:
                    parts.append(x.c)
                else:
                    raise NoForm('%s on objects' % e.op)
            return Val('meta', '(%s %s %s)' % (parts[0], e.op, parts[1]))
        raise NoForm('%s on objects' % e.op)

    def construct(self, name, args, st, out, infer):
        """A known_length constructor: Ni's virtual, for a meta size
        within the type's item limit (else the real call builds it)."""
        spec = self.ctors[name]
        if len(args) != 1:
            raise Refuse('%s: %s with %d arguments' % (self.name, name, len(args)))
        size = self.ev(args[0], st, out, infer)
        if size.bt not in ('meta', 'gen'):
            raise NoForm('%s of a size not known when compiling' % name)
        if infer:
            return Val('dynamic', '', virt=spec)
        import policy_table
        limit = policy_table.LIMITS[spec['max_items']][1]
        if size.bt == 'gen' and not 0 <= size.const <= limit:
            raise NoForm('a %s of %d items' % (spec['type'], size.const))
        n = size.c
        if size.bt != 'gen':
            # the C's Py_ssize_t up to the test, as items.construct (G21);
            # the limit's value from the table: Ni's name is local to its file
            self.tmp += 1
            n = '_t%d' % self.tmp
            self.decls[n] = 'Py_ssize_t %s;' % n
            out.line('%s = %s;' % (n, size.c))
            out.unbraced_if('%s < 0 || %s > %d' % (n, n, limit),
                            'return -1; /* a %s of more than %s items: the real call */'
                            % (spec['type'], spec['max_items']))
        return Val('dynamic', '%s(%s, NULL)' % (spec['new'], n), virt=spec)


# ---- the AST ----

def params_of(fn):
    args = fn.decl.type.args
    return [p for p in (args.params if args else []) if isinstance(p, c_ast.Decl)]


def body(s):
    if isinstance(s, c_ast.Compound):
        return s.block_items or []
    return [s]


def once_do(s):
    """'do S while (0);' with no break or continue of its own."""
    if not isinstance(s, c_ast.DoWhile):
        return False
    c = strip_casts(s.cond)
    if not (isinstance(c, c_ast.Constant) and c.value == '0'):
        return False
    return not any(isinstance(n, (c_ast.Break, c_ast.Continue)) for n in loop_free(s.stmt))


def loop_free(n):
    """The nodes of n outside any loop or switch nested in it."""
    yield n
    for _, c in n.children():
        if not isinstance(c, (c_ast.For, c_ast.While, c_ast.DoWhile, c_ast.Switch)):
            yield from loop_free(c)


def walk_nodes(n):
    if n is None:
        return
    yield n
    for _, c in n.children():
        yield from walk_nodes(c)


def names_in(nodes):
    return {x.name for s in nodes for x in walk_nodes(s) if isinstance(x, c_ast.ID)}


def decls_in(nodes):
    return [x for s in nodes for x in walk_nodes(s) if isinstance(x, c_ast.Decl)]


def assignments(n):
    """(local, value) of each plain assignment and initialised
    declaration in n."""
    nodes = n if isinstance(n, list) else [n]
    out = []
    for x in (y for s in nodes for y in walk_nodes(s)):
        if isinstance(x, c_ast.Assignment) and isinstance(strip_casts(x.lvalue), c_ast.ID):
            out.append((strip_casts(x.lvalue).name, x.rvalue))
        elif isinstance(x, c_ast.Decl) and x.init is not None and x.name:
            out.append((x.name, x.init))
    return out


def read_outside(name, stmts):
    """An ID `name` in stmts outside the blocks that declare a local of
    that name."""
    def walk(n, hidden):
        if isinstance(n, c_ast.Compound):
            hidden = hidden or any(isinstance(x, c_ast.Decl) and x.name == name
                                   for x in n.block_items or ())
        if isinstance(n, c_ast.ID) and n.name == name and not hidden:
            return True
        return any(walk(c, hidden) for _, c in n.children())
    return any(walk(s, False) for s in stmts)


def stepped(n):
    return [x.expr.name for x in walk_nodes(n) if isinstance(x, c_ast.UnaryOp)
            and x.op in ('++', '--', 'p++', 'p--') and isinstance(x.expr, c_ast.ID)]


def labels_in(n):
    return {x.name for x in walk_nodes(n) if isinstance(x, c_ast.Label)}


def has_label(n):
    return bool(labels_in(n))


def cast_struct(e):
    """The struct a cast names: ((PyTupleObject *)x)->... (3.8's
    _PyTuple_CAST: '((void) 0, (PyTupleObject *)x)->...')."""
    if isinstance(e, c_ast.ExprList):
        e = e.exprs[-1]
    if isinstance(e, c_ast.Cast):
        return text(e.to_type).replace('const ', '').replace('struct ', '').rstrip(' *').strip()
    return None


def cexpr(e):
    from .cprint import expr
    return expr(e)


def unconst(t):
    if isinstance(t, c_ast.TypeDecl) and 'const' in t.quals:
        return c_ast.TypeDecl(t.declname, [q for q in t.quals if q != 'const'], t.align, t.type)
    return t


def const_value(e):
    """An integer constant's value (C's suffixes and bases), else None."""
    if e.type == 'char':
        body_ = e.value[1:-1]
        return ord(body_) if len(body_) == 1 else None
    if e.type not in ('int', 'long', 'unsigned int', 'unsigned long', 'long long',
                      'unsigned long long'):
        return None
    v = e.value.rstrip('uUlL')
    try:
        return int(v, 8) if len(v) > 1 and v[0] == '0' and v[1] not in 'xX' else int(v, 0)
    except ValueError:
        return None


def const_of(c):
    """conventions.BINDING's C text of a value."""
    if c == 'NULL':
        return 0
    return int(c, 0)


def c_const(k, t, unit):
    return 'NULL' if k == 0 and not unit.is_int(t) else str(k)


def fold(op, a, b):
    """C's value of a op b on integers (G5: comparisons and logic give 0
    or 1, division truncates towards zero); None where C's is undefined."""
    if op in ('/', '%'):
        if b == 0:
            return None
        q = abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1)
        return q if op == '/' else a - q * b
    if op in ('<<', '>>') and b < 0:
        return None
    ops = {'+': lambda: a + b, '-': lambda: a - b, '*': lambda: a * b,
           '<<': lambda: a << b, '>>': lambda: a >> b, '&': lambda: a & b,
           '|': lambda: a | b, '^': lambda: a ^ b,
           '<': lambda: int(a < b), '>': lambda: int(a > b), '<=': lambda: int(a <= b),
           '>=': lambda: int(a >= b), '==': lambda: int(a == b), '!=': lambda: int(a != b),
           '&&': lambda: int(bool(a) and bool(b)), '||': lambda: int(bool(a) or bool(b))}
    return ops[op]() if op in ops else None


def drop_unread(out, assigns, decls):
    """Remove the assignments to locals nothing else reads, to a fixed
    point; their declarations go with them (Function declares only what
    the body names). An if left with no arm is dropped too."""
    ident = re.compile(r'\b\w+\b')
    while True:
        lines = [(code, k) for code, k, _ in assigns if code.items[k] is not None]
        mine = {(id(code), k): n for code, k, n in assigns if code.items[k] is not None}
        reads = {}
        for text_ in rendered(out, mine):
            for w in ident.findall(text_):
                reads[w] = reads.get(w, 0) + 1
        dead = [(code, k) for code, k in lines if not reads.get(mine[(id(code), k)])]
        if not dead:
            break
        for code, k in dead:
            code.items[k] = None
    prune(out)


def rendered(code, mine):
    """The text of code's lines, without the left side of the lines in
    `mine` (an assignment to the local it names)."""
    for k, it in enumerate(code.items):
        if it is None:
            continue
        if it[0] == 'line':
            s = it[1]
            if (id(code), k) in mine:
                s = s.split('=', 1)[1]
            yield s
        elif it[0] == 'if':
            yield it[1]
            yield from rendered(it[2], mine)
            if it[3] is not None:
                yield from rendered(it[3], mine)
        elif it[0] == 'uif':
            yield it[1]
            yield it[2]
        elif it[0] == 'block':
            yield it[1]
            yield from rendered(it[2], mine)


def prune(code):
    items = []
    for it in code.items:
        if it is None:
            continue
        if it[0] == 'if':
            prune(it[2])
            if it[3] is not None:
                prune(it[3])
            other = it[3] if it[3] else None
            if not it[2] and other is None:
                continue
            if not it[2]:
                it = ('if', '!(%s)' % it[1], other, None)
            else:
                it = ('if', it[1], it[2], other)
        elif it[0] == 'block':
            prune(it[2])
        items.append(it)
    code.items = items


# ---- the header ----

def generate(target):
    b = scope.BINDING
    ft = front.target(target.python, target.src)
    ast = front.parse(ft, b.cfile)
    unit = Unit(ast)
    fn = unit.defs.get(b.function)
    if fn is None:
        raise Refuse('%s: no %s' % (b.cfile, b.function))
    dname = conventions.psyco_name(b.function)
    decls = conventions.ni_decls(ROOT, [b.decls])
    if dname not in decls:
        raise Refuse('%s: Ni declares no %s (%s)' % (b.function, dname, b.decls))
    text_ = Binder(unit, fn, decls[dname], conventions.BINDING).run()
    key = '%s:%s' % (b.cfile, b.function)
    head = ['/* GENERATED by tools/metac/gen2/binding.py from CPython %s\'s %s:'
            % (cpython_src.source_version(target.src), b.cfile),
            '   do not edit. Included by %s. */' % b.includer, '',
            '/* the argument binding of %s, between the frame made and run */' % key]
    return '\n'.join(head) + '\n' + text_, ['generated  %s %s %s' % (b.function, key, dname)]


def build(target):
    try:
        return generate(target)
    except (Refuse, front.FrontError) as e:
        raise cslots.GenError(str(e))
