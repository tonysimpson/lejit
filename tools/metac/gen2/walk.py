"""One function's specialisation: its statements and expressions walked
once per path, each construct given its action (actions.py), the meta's
C built as it goes.

A statement that has no compile-time form raises ResumeHere; the
statement it is in is then taken back whole (its code, the path and the
walk's record) and resume.resume decides whether CPython's code may run
from there instead.
"""
import copy

from pycparser import c_ast

from . import actions, cprint, items, loops, reps, resume, scalar
from .cbuild import Code
from .ctyping import INT, LONG, VOID_P, strip_casts, text
from .errors import Refuse, ResumeHere
from .liveness import body
from .types import NOT_IMPLEMENTED, SINGLETONS, SINGLETON_NAMES
from .policy import FoldMayFail, IfKnown, Whole
from .values import Conv, Path, Val, Var

COMPARE = actions.COMPARE
FLIP = {'<': '>', '>': '<', '<=': '>=', '>=': '<=', '==': '==', '!=': '!='}
FOLD = {'<': int.__lt__, '>': int.__gt__, '<=': int.__le__, '>=': int.__ge__,
        '==': int.__eq__, '!=': int.__ne__}


class Record:
    """What one walk accumulates; a statement taken back restores it."""

    def __init__(self):
        self.decls = {}           # meta variable -> its declaration
        self.tmp = 0
        self.must, self.late = [], []     # promotions found at entry, after a call
        self.needs = set()        # parameter indexes whose type is read
        self.returns = []         # 'error' or 'value' per return
        self.resumed = []         # (statement text, why, at once)
        self.loop_late = []       # (loop, promotion) needed after a call in its turns
        self.notimpl = False      # may return NotImplemented
        self.used_likely = []     # policy Likely items used
        self.any_call = False
        self.ncalls = 0
        self.reps = set()         # representations used
        self.budgets = []         # budgets hit

    def snapshot(self):
        return copy.deepcopy(self.__dict__)

    def restore(self, snap):
        self.__dict__.update(copy.deepcopy(snap))


class Walk:
    def __init__(self, tr, fn, sm, hoist):
        self.tr, self.fn, self.sm, self.hoist = tr, fn, sm, hoist
        self.u = tr.unit
        self.name = fn.decl.name
        self.params = [(p.name, p.type) for p in param_decls(fn)]
        self.scope = dict(self.params)
        for n in walk_nodes(fn.body):
            if isinstance(n, c_ast.Decl) and n.name and not isinstance(n.type, c_ast.FuncDecl):
                self.scope.setdefault(n.name, n.type)
        self.bts = tr.local_bts[self.name]
        self.live = tr.liveness[self.name]
        self.items = tr.policy.get(self.name, [])
        self.likely = [i for i in self.items if type(i).__name__ == 'Likely']
        self.restype = [i.tp for i in self.items if type(i).__name__ == 'Restype']
        self.rec = Record()
        self.switch_depth = 0
        self.goto_depth = 0
        self.stmt_ncalls = 0
        self.stmt_frames = None
        self.incref_return = None
        self.word_counts = {}     # (statement, x) -> [reads so far, reads]
        self.chosen = {}          # a likely branch's C truth -> the arm assumed
        self.loops = []           # the meta loops being walked, outermost first
        self.loop_nodes = [n for n in walk_nodes(fn.body) if isinstance(n, c_ast.For)]
        self.loop_hoist = tr.loop_hoists.get(self.name, {})
        self.pending_store = None     # an item stored borrowed, to be increfed next
        self.sc_slots = None
        self.kl_specs = {}        # an object variable whose length is loaded -> its spec
        self.array = self.count = None    # a METH_FASTCALL root's C array and count
        self.status_err = None    # (the compare, True if its truth is the error)

    # ---- the walk's record ----

    def loop_key(self, s):
        """A loop's key: its place among the function's loops."""
        return next(k for k, n in enumerate(self.loop_nodes) if n is s)

    def late(self, p, what):
        """A promotion needed after a call: at the entry of the outermost
        meta loop if nothing was called before it, else at the function's
        entry (rule 1). what: a parameter, or 'items:NAME'."""
        if self.loops and not self.loops[0].entry_called:
            key = self.loops[0].key
            if what not in self.loop_hoist.get(key, []) \
                    and (key, what) not in self.rec.loop_late:
                self.rec.loop_late.append((key, what))
            return
        if what.startswith('items:'):
            if self.rec.loop_late:
                return          # the walk is made again with those promotions
            raise Refuse('the types of the items of %s are needed after a call'
                         % what[len('items:'):])
        if what not in self.rec.late:
            self.rec.late.append(what)

    @property
    def always_error(self):
        return bool(self.rec.returns) and all(r == 'error' for r in self.rec.returns)

    def err(self):
        return {'obj': 'NULL', 'val': 'NULL', 'ok': 'false', 'ct': '-1'}[self.sm.kind]

    def new_tmp(self, ctext):
        self.rec.tmp += 1
        n = '_t%d' % self.rec.tmp
        self.rec.decls[n] = declaration(ctext, n)
        return n

    def type_of(self, e):
        return self.u.type_of(e, self.scope)

    # ---- the function ----

    def run(self):
        p = Path()
        self.meta_params = []
        for k, (name, t) in enumerate(self.params):
            bt = self.sm.pbt[k]
            v = Var(name, t, bt, name, isparam=True)
            p.vars[name] = v
            if bt == 'array':
                # the C array and its count are the one tuple Ni passes
                v.cname = ARGS_TUPLE
                self.array = name
                self.meta_params.append(('vinfo_t *', ARGS_TUPLE))
            elif bt == 'count':
                self.count = name
            elif bt == 'dynamic':
                self.meta_params.append(('vinfo_t *', name))
            else:
                self.meta_params.append((text(unconst(t)), name))
        out = Code()
        self.out = out
        if any(isinstance(i, FoldMayFail) for i in self.items):
            actions.fold_may_fail(self, p, out)
        whole = next((i for i in self.items if isinstance(i, Whole)), None)
        if whole is not None:
            self.body_start = len(out.items)
            resume.resume(self, p, out, [(body(self.fn.body), 0)], 'policy whole: ' + whole.why)
            return
        for i in self.items:
            if isinstance(i, IfKnown):
                self.if_known(p, out, i.name)
        for name in self.hoist:
            if name in dict(self.params):
                actions.promote(self, p, out, name, entry=True)
        self.body_start = len(out.items)
        self.block(body(self.fn.body), p, out, [])
        if not p.ended:
            raise Refuse('the end of the function is reachable without a return')
        if self.sm.kind == 'ct' and self.rec.any_call:
            raise Refuse('a compile-time result after a run-time call')
        for item in self.likely:
            if item.text not in self.rec.used_likely:
                raise Refuse('policy %r matches no run-time branch' % item)

    def if_known(self, p, out, name):
        """Policy IfKnown: the integer parameter is its compile-time value
        where Ni knows it; any other value runs CPython's function (no
        promotion)."""
        v = p.vars[name]
        back = out.sub()
        resume.resume(self, p.copy(), back, [(body(self.fn.body), 0)],
                      '%s is not known at compile time' % name, inside=True)
        out.if_('!is_compiletime(%s->source)' % v.cname, back)
        k = Var(name, v.ctype, 'meta', name + '_k', isparam=True)
        self.rec.decls[k.cname] = declaration(text(unconst(v.ctype)), k.cname)
        out.line('%s = (%s)CompileTime_Get(%s->source)->value;'
                 % (k.cname, text(unconst(v.ctype)), v.cname))
        p.vars[name] = k

    # ---- a METH_FASTCALL root's arguments ----

    def count_split(self, stmts, i, p, out):
        """At the top-level statement that checks the count of arguments
        (conventions.ARGUMENT_CHECKS): Ni's length of the tuple, CPython's
        function where it is outside the check's bounds, and the rest of
        the function walked once per count within them (polyvariance),
        the count a constant on each path. False if the statement holds
        no check (the count is then read before it is known)."""
        bounds = set()
        for x in walk_nodes(stmts[i]):
            c = argument_check(self, x)
            if c is not None:
                bounds.add(c[1:3])
        if not bounds:
            return False
        if len(bounds) > 1:
            raise Refuse('the count of arguments checked against %s' % sorted(bounds))
        lo, hi = bounds.pop()
        here = [(stmts, i)]
        if hi - lo + 1 > self.tr.budgets['polyvariance']:
            self.rec.budgets.append('polyvariance')
            resume.resume(self, p, out, here, 'budget polyvariance: counts %d..%d' % (lo, hi))
            return True
        n = self.new_tmp('int')
        out.line('%s = %s(%s);' % (n, tuple_spec()['load'], ARGS_TUPLE))
        back = out.sub()
        resume.resume(self, p.copy(), back, here, 'a count of arguments outside %d..%d'
                      % (lo, hi), inside=True)
        out.if_('%s < %d || %s > %d' % (n, max(lo, 0), n, hi), back)
        paths = []
        for k in range(lo, hi + 1):
            pk = p.copy()
            pk.nargs = k
            code = out if lo == hi else out.sub()
            self.block(stmts, pk, code, [], start=i)
            if not pk.ended:
                raise Refuse('the end of the function is reachable without a return')
            paths.append((k, pk, code))
        if lo != hi:
            rest = paths[-1][2]
            for k, _, code in reversed(paths[:-1]):
                outer = out.sub()
                outer.if_('%s == %d' % (n, k), code, rest)
                rest = outer
            out.extend(rest)
        p.take(paths[-1][1])
        p.ended = p.early = True
        return True

    def bind_item(self, p, out, name, k):
        """The C variable `name` is item k of the root's arguments: a
        frame value, borrowed, its type promotable as a parameter's."""
        v = p.vars[name]
        if v.owned():
            self.release(v, out)
        out.line('%s = %s(%s, %d);' % (v.cname, tuple_spec()['item'], ARGS_TUPLE, k))
        v.own, v.isparam, v.conv, v.virt, v.alias = 'borrowed', True, None, None, None
        v.singleton, v.constant, v.known_null, v.rng = None, None, False, None
        p.known.discard(name)
        if name in self.hoist:
            if p.called:
                raise Refuse('the type of the argument %s is needed after a call' % name)
            actions.promote(self, p, out, name, entry=True)

    def args_item(self, e, p, out, frames):
        """The index of args[k], an item of the root's arguments read
        once their count is known; None if e is none."""
        if self.array is None or not isinstance(e, c_ast.ArrayRef):
            return None
        a = strip_casts(e.name)
        if not (isinstance(a, c_ast.ID) and a.name == self.array):
            return None
        k = self.ev(e.subscript, p, out, frames)
        j = int(k.c) if k.bt == 'meta' and k.c.isdigit() else None
        if p.nargs is None or j is None or not 0 <= j < p.nargs:
            raise Refuse('the argument %s read outside the checked count' % cprint.expr(e))
        return j

    def block(self, stmts, p, out, frames, start=0):
        i = start
        while i < len(stmts) and not p.ended:
            s = stmts[i]
            here = frames + [(stmts, i)]
            if self.count is not None and p.nargs is None and not frames \
                    and self.count in names_in(s) and self.count_split(stmts, i, p, out):
                break
            if (not p.called and self.sm.kind != 'ct'
                    and not isinstance(s, (c_ast.Decl, c_ast.EmptyStatement))
                    and self.tr.every_path_loops(self.fn, here)):
                resume.resume(self, p, out, here, 'every path from here runs a loop'
                              ' with a run-time bound')
                break
            n = scalar.run(self, stmts, i, p, out, here)
            if n:
                s = stmts[i + n - 1]
            else:
                pending = self.pending_store
                self.statement(s, p, out, here)
                if pending is not None and self.pending_store is pending:
                    raise Refuse('%s stored a borrowed reference that the next statement'
                                 ' does not incref' % cprint.expr(pending))
                n = 1
            if not p.ended:
                self.release_dead(p, out, self.live.after[id(s)])
            i += n

    def statement(self, s, p, out, frames):
        mark = len(out.items)
        saved_p, saved_r = p.copy(), self.rec.snapshot()
        outer = self.stmt_ncalls, self.stmt_frames
        self.stmt_ncalls, self.stmt_frames = self.rec.ncalls, frames
        try:
            self.stmt(s, p, out, frames)
        except ResumeHere as e:
            del out.items[mark:]
            self.rec.restore(saved_r)
            p.take(saved_p)
            self.stmt_ncalls = self.rec.ncalls
            resume.resume(self, p, out, frames, str(e))
        finally:
            self.stmt_ncalls, self.stmt_frames = outer

    # ---- ownership ----

    def release_dead(self, p, out, live):
        for v in list(p.vars.values()):
            if v.bt == 'dynamic' and v.owned() and (v.word_of or v.name) not in live:
                self.release(v, out)

    def release(self, v, out):
        if v.own == 'new':
            out.line('vinfo_decref(%s, po);' % v.cname)
        elif v.own == 'mixed':
            out.line('vinfo_xdecref(own_%s, po);' % v.cname)
        v.own = 'borrowed' if v.isparam else 'none'

    def release_all(self, p, out, keep=None):
        for v in p.vars.values():
            if v.bt == 'dynamic' and v.owned() and v.name != keep:
                if v.own == 'new':
                    out.line('vinfo_decref(%s, po);' % v.cname)
                else:
                    out.line('vinfo_xdecref(own_%s, po);' % v.cname)

    def error_exit(self, p, out, test):
        sub = out.sub()
        self.release_all(p, sub)
        sub.line('return %s;' % self.err())
        out.if_(test, sub)

    # ---- statements ----

    def stmt(self, s, p, out, frames):
        if isinstance(s, c_ast.Compound):
            self.block(body(s), p, out, frames)
        elif isinstance(s, c_ast.EmptyStatement):
            return
        elif once_do(s):
            self.stmt(s.stmt, p, out, frames)
        elif isinstance(s, c_ast.For):
            loops.loop(self, s, p, out, frames)
        elif isinstance(s, (c_ast.While, c_ast.DoWhile)):
            raise ResumeHere('a loop with a run-time bound')
        elif isinstance(s, c_ast.Switch):
            self.switch_stmt(s, p, out, frames)
        elif isinstance(s, (c_ast.Case, c_ast.Default)):
            raise Refuse('a case label outside a switch')
        elif isinstance(s, c_ast.Break) and self.loops:
            loops.break_(self, p, out)
        elif isinstance(s, (c_ast.Break, c_ast.Continue)):
            raise Refuse('%s outside a loop' % type(s).__name__.lower())
        elif isinstance(s, c_ast.Goto):
            self.goto(s, p, out, frames)
        elif isinstance(s, c_ast.Label):
            self.stmt(s.stmt, p, out, frames)
        elif isinstance(s, c_ast.Decl):
            if isinstance(s.type, c_ast.FuncDecl) or 'static' in s.storage \
                    or 'extern' in s.storage:
                raise ResumeHere('a declaration of %s' % s.name)
            if isinstance(s.type, c_ast.ArrayDecl):
                raise ResumeHere('an array %s' % s.name)
            self.declare(p, s.name, s.type)
            if s.init is not None:
                self.assign(p, out, s.name, s.init, frames)
        elif isinstance(s, c_ast.Return):
            self.ret(s.expr, p, out, frames)
        elif isinstance(s, c_ast.If):
            self.if_stmt(s, p, out, frames)
        else:
            self.expr_stmt(s, p, out, frames)

    def declare(self, p, name, t):
        bt = self.bts.get(name)
        if bt is None:
            raise ResumeHere('a local of type %s' % text(t))
        if bt == 'array' or (bt == 'pointer' and not items.lengths(self)):
            raise ResumeHere('a %s local %s' % (bt, name))
        old = p.vars.get(name)
        if old is not None and not old.isparam and text(old.ctype) != text(t):
            raise Refuse('%s declared twice with different types' % name)
        v = Var(name, t, bt, name)
        p.vars[name] = v
        if bt == 'pointer':
            self.rec.decls[name] = 'Py_ssize_t %s;' % name     # its offset
        elif bt == 'meta':
            self.rec.decls[name] = declaration(text(unconst(t)), name)
        else:
            self.rec.decls[name] = 'vinfo_t *%s;' % name

    def expr_stmt(self, e, p, out, frames):
        if isinstance(e, c_ast.ExprList):
            for x in e.exprs:
                self.expr_stmt(x, p, out, frames)
            return
        if isinstance(e, c_ast.Cast) and isinstance(e.expr, c_ast.Constant):
            return      # assert() under NDEBUG
        if isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID):
            fn = e.name.name
            args = e.args.exprs if e.args else []
            if (fn == 'abort' and not args) or fn in fatal_functions():
                # Py_UNREACHABLE() (abort(), or Py_FatalError from 3.8): a
                # path the C says never runs
                self.release_all(p, out)
                out.line('abort();')
                p.ended = True
                return
            if actions.refcount(self, p, out, fn, args, frames):
                return
        if isinstance(e, c_ast.UnaryOp) and e.op in ('++', '--', 'p++', 'p--'):
            out.line('%s;' % self.ev(e, p, out, frames).c)
            return
        if isinstance(e, c_ast.Assignment):
            lv = strip_casts(e.lvalue)
            if not isinstance(lv, c_ast.ID):
                if e.op == '=' and items.store(self, e.lvalue, e.rvalue, p, out, frames):
                    return
                raise ResumeHere('a store into %s' % cprint.expr(e.lvalue))
            name = lv.name
            v = p.vars.get(name)
            if e.op == '=':
                self.assign(p, out, name, e.rvalue, frames)
            elif e.op in ('+=', '-=') and v is not None and v.bt == 'meta':
                val = self.ev(e.rvalue, p, out, frames)
                if val.bt != 'meta':
                    raise Refuse('%s is meta, given a dynamic value' % name)
                out.line('%s %s %s;' % (v.cname, e.op, val.c))
            elif e.op in ('+=', '-=') and v is not None and v.bt == 'dynamic':
                self.assign(p, out, name, c_ast.BinaryOp(e.op[0], lv, e.rvalue, e.coord), frames)
            else:
                raise ResumeHere('operator %s' % e.op)
            return
        val = self.ev(e, p, out, frames, dest=None)
        if val.bt == 'call':
            out.line('%s;' % val.c)
        elif val.bt == 'dynamic' and val.owned and not (val.conv and val.conv.always_error):
            out.line('vinfo_xdecref(%s, po);' % val.c)

    def assign(self, p, out, name, e, frames):
        v = p.vars.get(name)
        if v is None:
            raise Refuse('assignment to %s' % name)
        for x in p.vars.values():
            if x.alias == name:
                x.alias = None      # a copy of the old value, no longer the parameter's
        if v.bt == 'pointer':
            val = items.pointer(self, e, p, out, frames)
            if val is None:
                raise ResumeHere('%s assigned no pointer into an object\'s items' % name)
            out.line('%s = %s;' % (v.cname, val.c))
            v.base = val.base
            return
        k = self.args_item(strip_casts(e), p, out, frames)
        if k is not None and v.bt == 'dynamic' and self.u.is_object(v.ctype):
            self.bind_item(p, out, name, k)
            return
        st = singleton(e)
        if st is not None and v.bt == 'dynamic' and self.u.is_object(v.ctype):
            # an immortal object: Ni's constant, made where it is used
            if v.owned():
                self.release(v, out)
            v.own, v.singleton, v.constant, v.known_null, v.conv = 'none', st, None, False, None
            p.known.discard(name)
            return
        val = self.ev(e, p, out, frames, dest=name)
        v.known_null = False
        v.singleton = v.constant = None
        if not v.isparam:
            p.known.discard(name)       # a known type was the old value's
        if val.bt == 'gen':
            val = Val('meta', '1' if val.const else '0', INT)
        if val.bt == 'call':
            val = self.held_call(val, out)
        if v.bt == 'meta':
            if val.bt != 'meta':
                raise Refuse('%s is meta, given a dynamic value' % name)
            out.line('%s = %s;' % (v.cname, val.c))
            v.tp = val.tp
            v.within = val.within
            return
        if val.bt != 'dynamic':
            if (cprint.is_null(e) and not self.u.is_int(v.ctype)) or val.c == 'NULL':
                # no value: what the C reads next it assigns first
                if v.owned():
                    self.release(v, out)
                v.own = 'borrowed' if v.isparam else 'none'
                v.known_null = True     # returned, it is the error
                return
            if self.u.is_int(v.ctype):
                val = Val('dynamic', 'vinfo_new(CompileTime_New((long)(%s)))' % val.c,
                          v.ctype, owned=True)
            else:
                raise ResumeHere('%s assigned a compile-time value' % name)
        if val.var is v:
            return
        if val.var is not None and val.var.word_of and not val.owned:
            # the variable's own reference to the word
            if val.last and val.var.own == 'new':
                val.var.own = 'none'
            else:
                out.line('vinfo_incref(%s);' % val.c)
            val = Val('dynamic', val.c, val.ctype, owned=True, rng=val.rng)
        if val.var is not None and val.var.owned() and not val.owned:
            raise Refuse('%s aliases an owned value' % name)
        if v.owned():
            # the new value may read the old one: release after
            tmp = self.new_tmp('vinfo_t *')
            out.line('%s = %s;' % (tmp, val.c))
            self.release(v, out)
            out.line('%s = %s;' % (v.cname, tmp))
        else:
            out.line('%s = %s;' % (v.cname, val.c))
        v.own = 'new' if val.owned else 'borrowed'
        v.alias = param_alias(val.var) if not val.owned else None
        v.conv = val.conv
        v.virt = val.virt
        v.rng = None
        if val.rng is not None:
            bs = reps.int_type(self.u, v.ctype)
            if bs is None or not reps.fits(val.rng[0], val.rng[1], *bs):
                raise ResumeHere('%s may not hold the value' % name)
            v.rng = val.rng
        if val.conv and val.conv.check_now:
            own, v.own = v.own, 'borrowed'      # NULL: nothing to release
            self.error_exit(p, out, '%s == NULL' % v.cname)
            v.own = own
            v.conv = val.conv.but(check_now=False)

    def held_call(self, val, out):
        """A call whose result is a C int status, kept in a variable: a
        residual or slot call's is a vinfo Ni holds no reference for
        (CfNoReturnValue), NULL for the error; a meta's bool has none."""
        if val.conv.kind != 'zero_ok':
            raise ResumeHere('the status of %s kept in a variable' % val.c.split('(')[0])
        return Val('dynamic', val.c, INT, conv=Conv('zero_ok', checkable=True))

    # ---- return ----

    def ret(self, e, p, out, frames):
        kind = self.sm.kind
        if e is None:
            raise Refuse('return without a value')
        x = strip_casts(e)
        if isinstance(x, c_ast.ID) and getattr(p.vars.get(x.name), 'known_null', False) \
                and (p.vars[x.name].own == 'none' or p.vars[x.name].isparam):
            e = x = c_ast.Constant('int', '0')      # 'v = NULL; ... return v;'
            return self.ret_error(p, out)
        if isinstance(x, c_ast.ID) and getattr(p.vars.get(x.name), 'singleton', None):
            return actions.const_return(self, p, out, p.vars[x.name].singleton)
        ref = new_reference(x)
        if ref is not None:
            # 'return Py_INCREF(x), x': Py_RETURN_NONE and the like
            s = singleton(ref)
            if s is not None:
                return actions.const_return(self, p, out, s)
            val = self.ev(ref, p, out, frames, dest='<return>')
            if val.bt != 'dynamic' or val.var is None or val.var.own != 'borrowed':
                raise Refuse('returns a new reference to %s' % cprint.expr(ref))
            out.line('vinfo_incref(%s);' % val.c)
            return self.ret_value_owned(p, out, Val('dynamic', val.c, val.ctype, owned=True,
                                                    conv=val.conv))
        if items.unloaded_size(self, x, p):
            val = items.length(self, x, p, out, frames, as_value=True)    # only returned
        else:
            val = self.ev(e, p, out, frames, dest='<return>')
        if self.incref_return is not None:
            if not same_expr(x, self.incref_return) or val.bt != 'dynamic':
                raise Refuse('an incref of %s not returned' % cprint.expr(self.incref_return))
            self.incref_return = None
            if not val.owned and val.var is None:
                out.line('vinfo_incref(%s);' % val.c)
                val = Val('dynamic', val.c, val.ctype, owned=True, conv=val.conv)
        if val.bt == 'gen':
            raise Refuse('returns a constant known now')
        if kind in ('obj', 'val'):
            if val.bt == 'meta':
                if val.c == 'NULL' or (kind == 'val' and (
                        val.c in ('-1', '(-1)', '-1.0', '(-1.0)')
                        or self.tr.error_value(self.fn, e))):
                    return self.ret_error(p, out)
                if kind == 'val':
                    return self.ret_value_owned(p, out, Val(
                        'dynamic', 'vinfo_new(CompileTime_New((long)(%s)))' % val.c,
                        LONG, owned=True))
                raise ResumeHere('returns the constant %s' % val.c)
            if val.bt == 'call':
                raise Refuse('returns the status of %s' % val.c)
            if val.var is not None and val.var.own == 'borrowed':
                raise Refuse('returns %s, which it does not own' % val.var.name)
            if val.conv and val.conv.always_error:
                self.release_all(p, out, keep=val.var.name if val.var else None)
                out.line('return %s;' % val.c)
                p.ended = True
                self.rec.returns.append('error')
                return
            if val.conv and val.conv.notimpl:
                self.rec.notimpl = True
            return self.ret_value_owned(p, out, val)
        if kind == 'ok':
            if val.bt == 'meta':
                if val.c in ('-1', '(-1)'):
                    return self.ret_error(p, out)
                if val.c == '0':
                    self.release_all(p, out)
                    out.line('return true;')
                    p.ended = True
                    self.rec.returns.append('value')
                    return
                raise Refuse('returns %s from a 0/-1 function' % val.c)
            if val.bt == 'call' and val.conv.kind == 'ok':
                return self.ret_value(p, out, val.c)
            return self.ret_value(p, out, val.c, as_ok=True)
        raise Refuse('kind %s' % kind)

    def ret_error(self, p, out):
        self.release_all(p, out)
        out.line('return %s;' % self.err())
        p.ended = True
        self.rec.returns.append('error')

    def ret_value(self, p, out, cexpr, as_ok=False, notimpl=False):
        """Return a value the meta owns, after releasing the rest."""
        expr = '%s != NULL' % cexpr if as_ok else cexpr
        owned = [v for v in p.vars.values() if v.bt == 'dynamic' and v.owned()]
        if self.restype and not as_ok:
            r = self.new_tmp('vinfo_t *')
            out.line('%s = %s;' % (r, expr))
            self.assert_restype(out, r, notimpl)
            self.release_all(p, out)
            out.line('return %s;' % r)
        elif owned:
            r = self.new_tmp('bool' if self.sm.kind == 'ok' else 'vinfo_t *')
            out.line('%s = %s;' % (r, expr))
            self.release_all(p, out)
            out.line('return %s;' % r)
        else:
            out.line('return %s;' % expr)
        p.ended = True
        self.rec.returns.append('value')

    def ret_value_owned(self, p, out, val):
        keep = val.var.name if val.var is not None else None
        if self.restype and val.var is None and val.made not in self.restype:
            r = val.c
            if not (r.startswith('_t') and r[2:].isdigit()):
                r = self.new_tmp('vinfo_t *')
                out.line('%s = %s;' % (r, val.c))
            self.assert_restype(out, r)
            val = Val('dynamic', r, val.ctype, owned=True, conv=val.conv)
        if val.var is not None and val.var.own == 'mixed':
            raise Refuse('returns a value owned on some paths only')
        owned = [v for v in p.vars.values() if v.bt == 'dynamic' and v.owned()
                 and v.name != keep]
        if owned:
            if val.var is None:
                r = self.new_tmp('vinfo_t *')
                out.line('%s = %s;' % (r, val.c))
            else:
                r = val.c
            self.release_all(p, out, keep=keep)
            out.line('return %s;' % r)
        else:
            out.line('return %s;' % val.c)
        if val.var is not None:
            val.var.own = 'none'
        p.ended = True
        self.rec.returns.append('value')

    def assert_restype(self, out, r, notimpl=False):
        for t in self.restype:
            out.unbraced_if(('%s != NULL && !IS_NOTIMPLEMENTED(%s)' % (r, r)) if notimpl
                            else '%s != NULL' % r,
                            'Psyco_AssertType(po, %s, &%s);' % (r, t))

    # ---- if ----

    def if_stmt(self, s, p, out, frames):
        facts_t, facts_f = [], []
        self.sc_slots = None
        self.status_err = None
        c = s.cond
        cexpr = self.cond(c, p, out, frames, facts_t, facts_f)
        sc = self.sc_slots if is_logic(c) else None
        if isinstance(cexpr, bool):
            branch = s.iftrue if cexpr else s.iffalse
            if branch is not None:
                self.block(body(branch), p, out, frames)
            return
        pt, pf = p.copy(), p.copy()
        nt = self.null_test(c, p)
        if nt is not None:
            # where the error test held, the value is NULL: nothing to release
            name, when = nt
            x = (pt if when else pf).vars[name]
            x.own = 'borrowed' if x.isparam else 'none'
            (pt if when else pf).failed = True
            (pf if when else pt).vars[name].conv = None
        st = self.status_test(c)
        if st is not None:
            (pt if st else pf).failed = True
        it = self.impl_test(c, p)
        if it is not None:
            name, when = it
            x = (pt if when else pf).vars[name]
            if x.conv:
                x.conv = x.conv.but(notimpl=False)
        if sc is not None and sc[0] == strip_casts(c).op:
            (pt if sc[0] == '&&' else pf).slots.update(sc[1])
        out_t, out_f = out.sub(), out.sub()
        for line in facts_t:
            out_t.line(line)
        for line in facts_f:
            out_f.line(line)
        self.release_dead(pt, out_t, self.live.before[id(s.iftrue)])
        self.block(body(s.iftrue), pt, out_t, frames)
        if s.iffalse is not None:
            self.release_dead(pf, out_f, self.live.before[id(s.iffalse)])
            self.block(body(s.iffalse), pf, out_f, frames)
        else:
            self.release_dead(pf, out_f, self.live.after[id(s)])
        self.merge(p, pt, pf, out_t, out_f)
        out.if_(cexpr, out_t, out_f)

    def merge(self, p, a, b, out_a, out_b):
        if a.ended and b.ended:
            p.take(a)
            p.ended = p.early = True
            return
        if a.ended or b.ended:
            p.take(b if a.ended else a)
            p.early = True
            return
        p.known = a.known & b.known
        p.called = a.called or b.called
        p.early = a.early or b.early
        p.failed = a.failed and b.failed
        p.slots = {k: v for k, v in a.slots.items() if b.slots.get(k) == v}
        p.loaded = {k: v for k, v in a.loaded.items() if b.loaded.get(k) == v}
        p.budgeted = a.budgeted & b.budgeted
        for name in sorted(set(a.vars) | set(b.vars)):
            va, vb = a.vars.get(name), b.vars.get(name)
            if va is None or vb is None:
                p.vars.pop(name, None)
                continue
            if va.bt != vb.bt:
                raise Refuse('%s has a different binding time on two paths' % name)
            if va.bt == 'pointer' and va.base != vb.base:
                raise Refuse('the pointer %s points into two objects' % name)
            if va.virt is not vb.virt and va.owned() and vb.owned():
                raise Refuse('%s holds a virtual on one path only' % name)
            v = va
            v.within = va.within and vb.within
            if va.bt == 'dynamic' and va.own != vb.own:
                states = {va.own, vb.own}
                if states <= {'borrowed', 'new', 'mixed'}:
                    self.rec.decls['own_' + name] = 'vinfo_t *own_%s;' % name
                    for o, x in ((out_a, va), (out_b, vb)):
                        if x.own == 'new':
                            o.line('own_%s = %s;' % (name, name))
                        elif x.own == 'borrowed':
                            o.line('own_%s = NULL;' % name)
                    v.own = 'mixed'
                elif states - {'none'} <= {'borrowed'}:
                    v.own = 'borrowed' if v.isparam else 'none'
                else:
                    raise Refuse('%s owned on one path, released on the other' % name)
            if va.bt == 'meta' and va.tp != vb.tp:
                v.tp = None
            if va.conv != vb.conv:
                v.conv = None
            v.known_null = va.known_null and vb.known_null
            p.vars[name] = v
        p.ended = False

    # ---- conditions: a meta C boolean, or a Python bool if known now ----

    def cond(self, e, p, out, frames, facts_t=None, facts_f=None, value=False):
        x = strip_casts(e)
        if reps.test(self, x, p, out, frames):
            return True
        t = actions.bit_test(self, x, p, out, frames)
        if t is not None:
            return t
        if isinstance(x, c_ast.UnaryOp) and x.op == '!':
            if self.is_obj_param(x.expr, p):
                return False
            v = self.checkable(x.expr, p)
            if v is not None:
                return '%s == NULL' % v.cname
        v = self.checkable(x, p)
        if v is not None:
            return '%s != NULL' % v.cname
        if isinstance(x, c_ast.BinaryOp) and x.op == '&&' and is_err_occurred(x.right) \
                and isinstance(strip_casts(x.left), c_ast.BinaryOp) \
                and self.error_test(strip_casts(x.left), p) is not None:
            # 'x == -1 && PyErr_Occurred()': a meta's error is the NULL vinfo
            return self.error_test(strip_casts(x.left), p)
        if self.is_obj_param(x, p):
            return True
        if isinstance(x, c_ast.UnaryOp) and x.op == '!':
            r = self.cond(x.expr, p, out, frames, facts_f, facts_t)
            return (not r) if isinstance(r, bool) else '!(%s)' % r
        t = items.bounds_test(self, x, p, out, frames)
        if t is not None:
            return t
        if is_logic(x):
            return self.short_circuit(x, p, out, frames)
        if isinstance(x, c_ast.BinaryOp) and x.op in COMPARE:
            r = self.compare(x, p, out, frames, facts_t, facts_f, value)
            if r is not None:
                return r
        if isinstance(x, c_ast.ID) and x.name in p.vars and p.vars[x.name].bt == 'dynamic' \
                and self.u.is_int(p.vars[x.name].ctype):
            # the truth of a run-time integer is 'x != 0' (P05)
            return self.cond(c_ast.BinaryOp('!=', x, c_ast.Constant('int', '0')), p, out,
                             frames, facts_t, facts_f)
        if self.is_virtual(x, p) or self.is_obj_param(x, p):
            return True
        val = self.ev(e, p, out, frames)
        if val.bt == 'gen':
            return bool(val.const)
        if val.bt == 'meta':
            return '%s != 0' % paren(val.c) if not val.c.startswith('(') else val.c
        raise ResumeHere('a test of the run-time value %s' % cprint.expr(e))

    def short_circuit(self, e, p, out, frames):
        op = e.op
        lc = self.cond(e.left, p, out, frames)
        if isinstance(lc, bool):
            if op == '&&' and not lc:
                return False
            if op == '||' and lc:
                return True
            return self.cond(e.right, p, out, frames)
        sub_p = p.copy()
        left = self.sc_slots
        if left is not None and left[0] == op and is_logic(e.left) \
                and strip_casts(e.left).op == op:
            sub_p.slots.update(left[1])
        sub = out.sub()
        try:
            rc = self.cond(e.right, sub_p, sub, frames)
        except ResumeHere as r:
            # the right operand runs only when the left allows: resume
            # there, on that path
            sub, sub_p = out.sub(), p.copy()
            resume.resume(self, sub_p, sub, frames, str(r), inside=True)
            rc = '0' if op == '&&' else '1'
        if isinstance(rc, bool):
            rc = '1' if rc else '0'
        # the slots read on the right are known where the whole test
        # holds (&&) or fails (||)
        self.sc_slots = (op, dict(sub_p.slots)) if not sub_p.ended else None
        if not sub:
            self.merge_cond(p, sub_p)
            return '(%s %s %s)' % (lc, op, rc)
        t = self.new_tmp('int')
        out.line('%s = %s;' % (t, lc))
        if not sub_p.ended:
            sub.line('%s = %s;' % (t, rc))
        out.if_(('' if op == '&&' else '!') + t, sub)
        self.merge_cond(p, sub_p)
        return t

    def merge_cond(self, p, sub):
        """The path after 'if (c) { sub }': what sub learnt is not known."""
        p.called = p.called or sub.called
        p.early = p.early or sub.ended
        p.slots = {k: v for k, v in p.slots.items() if sub.slots.get(k) == v}

    def compare(self, e, p, out, frames, facts_t, facts_f, value=False):
        op, a, b = e.op, e.left, e.right
        r = self.error_test(e, p)
        if r is not None:
            return r
        for x, y in ((a, b), (b, a)):
            if singleton(y) == NOT_IMPLEMENTED and op in ('==', '!='):
                v = self.ev(x, p, out, frames)
                if v.bt != 'dynamic' or not v.conv or not v.conv.notimpl:
                    raise ResumeHere('a compare with NotImplemented of %s' % cprint.expr(x))
                return ('IS_IMPLEMENTED(%s)' if op == '!=' else '!IS_IMPLEMENTED(%s)') % v.c
        for x, y in ((a, b), (b, a)):
            if cprint.is_null(y) and op in ('==', '!=') and (self.is_obj_param(x, p)
                                                           or self.is_virtual(x, p)):
                return op == '!='       # a meta's object parameter is never NULL, nor a virtual
            if cprint.is_null(y) and op in ('==', '!=') and self.known_null(x, p):
                return op == '=='
        va = vb = None
        if reps.is_zero(b):
            va = reps.sign(self, a, p, out)
        elif reps.is_zero(a):
            vb = reps.sign(self, b, p, out)
        # a length compared with a run-time integer is its field, read
        if items.unloaded_size(self, a, p) and not items.unloaded_size(self, b, p):
            vb = vb or self.ev(b, p, out, frames)
            va = items.length(self, a, p, out, frames, as_value=vb.bt == 'dynamic')
        elif items.unloaded_size(self, b, p) and not items.unloaded_size(self, a, p):
            va = va or self.ev(a, p, out, frames)
            vb = items.length(self, b, p, out, frames, as_value=va.bt == 'dynamic')
        va = va or self.ev(a, p, out, frames)
        vb = vb or self.ev(b, p, out, frames)
        if va.bt == 'call' or vb.bt == 'call':
            return self.status_compare(e, op, va, vb)
        if va.bt == 'gen' or vb.bt == 'gen':
            if va.bt == 'gen' and vb.bt == 'gen':
                return (va.const == vb.const) == (op == '==')
            return None
        if va.bt == 'meta' and vb.bt == 'meta':
            if va.c.isdigit() and vb.c.isdigit():
                return FOLD[op](int(va.c), int(vb.c))     # two constants: folded
            return '%s %s %s' % (paren(va.c), op, paren(vb.c))
        if va.bt == 'meta':
            va, vb, op = vb, va, FLIP[op]
            a, b = b, a
        if va.bt == 'dynamic' and vb.c == 'NULL' and op in ('==', '!=') \
                and self.u.pointee(va.ctype) is not None and not self.u.is_object(va.ctype):
            vb = Val('meta', '0', LONG)     # a C pointer: a word
        if self.u.is_object(va.ctype) or (vb.bt == 'dynamic' and self.u.is_object(vb.ctype)):
            raise ResumeHere('a compare of the object %s' % cprint.expr(a))
        if va.bt != 'dynamic' or vb.bt not in ('meta', 'dynamic'):
            raise ResumeHere('a compare of %s' % cprint.expr(e))
        if self.u.is_double(va.ctype) or (vb.bt == 'dynamic' and self.u.is_double(vb.ctype)):
            raise ResumeHere('a compare of a double %s' % cprint.expr(e))
        return actions.runtime_compare(self, p, out, e, op, va, vb, facts_t, facts_f, value)

    def status_compare(self, e, op, va, vb):
        """A C status (0, or -1 with an exception set) compared as the C
        tests it for its error: the meta's test of Ni's result (a bool's
        false, a residual call's NULL)."""
        if vb.bt == 'call':
            va, vb, op = vb, va, FLIP[op]
        k = vb.c.strip('()') if vb.bt == 'meta' else None
        err = {('!=', '0'): True, ('<', '0'): True, ('==', '-1'): True,
               ('==', '0'): False, ('>=', '0'): False}.get((op, k))
        if err is None:
            raise ResumeHere('a compare of %s' % cprint.expr(e))
        test = '!(%s)' % va.c if va.conv.kind == 'ok' else '%s == NULL' % va.c
        self.status_err = (id(e), err)
        return test if err else ('(%s) != NULL' % va.c if va.conv.kind != 'ok' else va.c)

    def status_test(self, c):
        """True if an error is pending where c holds, False where it fails,
        when c is just a status_compare (or its negation); else None."""
        x, neg = strip_casts(c), False
        while isinstance(x, c_ast.UnaryOp) and x.op == '!':
            x, neg = strip_casts(x.expr), not neg
        if self.status_err is None or self.status_err[0] != id(x):
            return None
        return self.status_err[1] != neg

    def null_test(self, c, p):
        """(variable, True if NULL where c holds) when c is just the error
        test of a call's result."""
        neg = False
        c = strip_casts(c)
        while isinstance(c, c_ast.UnaryOp) and c.op == '!':
            v = self.checkable(c.expr, p)
            if v is not None:
                return (v.name, not neg)
            c, neg = strip_casts(c.expr), not neg
        if isinstance(c, c_ast.BinaryOp) and c.op == '&&' and is_err_occurred(c.right):
            c = strip_casts(c.left)
        if isinstance(c, c_ast.BinaryOp) and self.error_test(c, p) is not None:
            v = self.checkable(c.left, p)
            return (v.name, neg == (c.op == '!='))
        return None

    def impl_test(self, c, p):
        """(variable, True if implemented where c holds) when c compares a
        call's result with Py_NotImplemented."""
        neg = False
        c = strip_casts(c)
        while isinstance(c, c_ast.UnaryOp) and c.op == '!':
            c, neg = strip_casts(c.expr), not neg
        if isinstance(c, c_ast.BinaryOp) and c.op in ('==', '!=') \
                and singleton(c.right) == NOT_IMPLEMENTED:
            x = strip_casts(c.left)
            if isinstance(x, c_ast.ID) and x.name in p.vars:
                return (x.name, (c.op == '!=') != neg)
        return None

    def checkable(self, x, p):
        x = strip_casts(x)
        if isinstance(x, c_ast.ID) and x.name in p.vars:
            v = p.vars[x.name]
            if v.bt == 'dynamic' and v.conv and v.conv.checkable:
                return v
        return None

    def known_null(self, x, p):
        """x is an object variable assigned NULL on this path."""
        x = strip_casts(x)
        return isinstance(x, c_ast.ID) and x.name in p.vars and p.vars[x.name].known_null \
            and p.vars[x.name].own == 'none' and not p.vars[x.name].isparam

    def is_virtual(self, x, p):
        x = strip_casts(x)
        return isinstance(x, c_ast.ID) and x.name in p.vars and p.vars[x.name].virt is not None \
            and p.vars[x.name].own == 'new'

    def is_obj_param(self, x, p):
        x = strip_casts(x)
        if not isinstance(x, c_ast.ID) or x.name not in p.vars:
            return False
        v = p.vars[x.name]
        return v.isparam and v.bt == 'dynamic' and self.u.is_object(v.ctype) \
            and v.own != 'none'

    def error_test(self, e, p):
        """The C's test of a call's error result: in the meta, the vinfo
        is NULL (a meta signals every error that way)."""
        e = strip_casts(e)
        if not isinstance(e, c_ast.BinaryOp) or e.op == '&&':
            return None
        v = self.checkable(e.left, p)
        if v is None:
            return None
        b = strip_casts(e.right)
        if e.op in ('==', '!=') and cprint.is_null(e.right) and not is_minus_one(b):
            return '%s %s NULL' % (v.cname, e.op)
        if e.op == '<' and isinstance(b, c_ast.Constant) and b.value == '0':
            return '%s == NULL' % v.cname
        if e.op == '==' and is_minus_one(b):
            return '%s == NULL' % v.cname
        return None

    # ---- expressions ----

    def ev(self, e, p, out, frames, dest=None):
        v = reps.value(self, e, p, out, frames)
        if v is not None:
            return v
        v = items.length(self, e, p, out, frames)
        if v is not None:
            return v
        if isinstance(e, (c_ast.ArrayRef, c_ast.UnaryOp)):
            v = items.read(self, e, p, out, frames)
            if v is not None:
                return v
        if isinstance(e, c_ast.Constant):
            if e.type == 'string':
                return Val('meta', e.value, self.type_of(e))
            return Val('meta', e.value, self.type_of(e))
        if isinstance(e, c_ast.ID):
            return self.ev_id(e.name, p, out)
        if isinstance(e, c_ast.Cast):
            return self.ev_cast(e, p, out, frames, dest)
        if isinstance(e, c_ast.StructRef):
            return self.ev_field(e, p, out, frames)
        if isinstance(e, c_ast.UnaryOp):
            return self.ev_unary(e, p, out, frames)
        if isinstance(e, c_ast.FuncCall):
            return actions.call(self, e, p, out, frames, dest)
        if isinstance(e, c_ast.BinaryOp):
            if e.op in ('&&', '||') or e.op in COMPARE:
                c = self.cond(e, p, out, frames, value=True)
                if isinstance(c, bool):
                    return Val('gen', None, INT, const=c)
                if isinstance(c, Val):
                    return c
                return Val('meta', c, INT)
            r = reps.arith(self, e, p, out, frames)
            if r is not None:
                return r
            va = self.ev(e.left, p, out, frames)
            vb = self.ev(e.right, p, out, frames)
            if va.bt == 'meta' and vb.bt == 'meta':
                return Val('meta', '(%s %s %s)' % (va.c, e.op, vb.c), va.ctype)
            if e.op == '+' and va.bt == 'dynamic' and vb.bt == 'dynamic':
                return actions.integer_add(self, p, out, va, vb)
            raise ResumeHere('run-time arithmetic %s' % cprint.expr(e))
        if isinstance(e, c_ast.TernaryOp):
            c = self.cond(e.cond, p, out, frames)
            if isinstance(c, bool):
                return self.ev(e.iftrue if c else e.iffalse, p, out, frames, dest)
            if c in self.chosen:
                return self.choice(e, c, p, out, frames, dest)
            va = self.ev(e.iftrue, p, out, frames)
            vb = self.ev(e.iffalse, p, out, frames)
            if va.bt == 'meta' and vb.bt == 'meta':
                return Val('meta', '(%s ? %s : %s)' % (c, va.c, vb.c), va.ctype)
            raise ResumeHere('a run-time choice %s' % cprint.expr(e))
        if isinstance(e, c_ast.ExprList):
            for x in e.exprs[:-1]:
                self.expr_stmt(x, p, out, frames)
            return self.ev(e.exprs[-1], p, out, frames, dest)
        if isinstance(e, c_ast.ArrayRef):
            raise ResumeHere('an item %s' % cprint.expr(e))
        raise ResumeHere('expression %s' % type(e).__name__)

    def choice(self, e, t, p, out, frames, dest):
        """c ? A : B on a run-time test its likely item decides: the arm
        it favours; where the other is taken, CPython's code resumes from
        the statement."""
        how = self.chosen[t]
        back = out.sub()
        resume.resume(self, p.copy(), back, frames, 'the unlikely arm of %s' % cprint.expr(e),
                      inside=True)
        out.if_(('!%s' if how else '%s') % t, back)
        return self.ev(e.iftrue if how else e.iffalse, p, out, frames, dest)

    def ev_id(self, name, p, out):
        if name in p.vars:
            v = p.vars[name]
            if v.bt == 'count':
                if p.nargs is None:
                    raise ResumeHere('the count of arguments before it is checked')
                return Val('meta', str(p.nargs), v.ctype)
            if v.bt == 'array':
                raise ResumeHere('the argument array %s as a value' % name)
            if v.singleton is not None:
                # an immortal object as a value: Ni's constant, the
                # variable's own reference
                out.line('%s = %s();' % (v.cname, SINGLETONS[v.singleton]))
                v.own, v.constant, v.singleton = 'new', v.singleton, None
            if v.bt == 'dynamic':
                if v.own == 'none' and not v.isparam:
                    raise Refuse('%s read with no value' % name)
                r = Val('dynamic', v.cname, v.ctype, var=v, conv=v.conv, rng=v.rng)
                r.virt = v.virt
                return r
            if v.bt == 'pointer':
                return Val('pointer', v.cname, v.ctype, base=v.base)
            return Val('meta', v.cname, v.ctype, tp=v.tp, within=v.within)
        if name in self.u.vars or name in self.u.enums:
            return Val('meta', name, self.u.vars.get(name, INT))
        raise ResumeHere('the name %s' % name)

    def ev_cast(self, e, p, out, frames, dest):
        o = cprint.offsetof(e)
        if o is not None:
            return Val('meta', 'offsetof(%s, %s)' % o, LONG)
        if cprint.is_null(e) and isinstance(e.expr, c_ast.Constant):
            return Val('meta', 'NULL', VOID_P)
        t = e.to_type.type
        if self.u.is_int(t) and reps.ranged(self, e.expr, p):
            return reps.arith(self, e, p, out, frames)
        v = self.ev(e.expr, p, out, frames, dest)
        if v.bt == 'meta':
            return Val('meta', '((%s)%s)' % (text(t), v.c), t, tp=v.tp)
        if v.bt == 'dynamic':
            if self.u.is_object(t) and self.u.is_object(v.ctype):
                r = Val('dynamic', v.c, t, owned=v.owned, conv=v.conv, var=v.var)
                r.made, r.virt, r.item_of = v.made, v.virt, v.item_of
                return r
            if self.u.is_int(t) and self.u.is_int(v.ctype):
                return v
            raise ResumeHere('a cast of a run-time value to %s' % text(t))
        return v

    def ev_field(self, e, p, out, frames):
        b = self.ev(e.name, p, out, frames)
        field = e.field.name
        if e.type != '->':
            raise ResumeHere('a field .%s' % field)
        if b.bt == 'dynamic':
            if self.u.is_object(b.ctype) and field == 'ob_type' and b.var is not None:
                tpv = actions.promote(self, p, out, b.var.name)
                return Val('meta', tpv, self.u.field(self.u.pointee(b.ctype), field), tp=tpv)
            r = actions.field_read(self, e, b, p, out)
            if r is not None:
                return r
            raise ResumeHere('the field %s of a run-time value' % field)
        if b.bt != 'meta':
            raise ResumeHere('a field of %s' % cprint.expr(e.name))
        pt = self.u.pointee(b.ctype)
        ftype = self.u.field(pt, field) if pt is not None else None
        if b.tp is None and ftype is not None and not self.u.is_type_object(b.ctype):
            # a field of a C struct the compiler holds: such structs don't
            # change once made
            return Val('meta', '%s->%s' % (paren(b.c), field), ftype)
        if b.tp is None or ftype is None:
            raise ResumeHere('the field %s of %s' % (field, cprint.expr(e.name)))
        if self.u.is_fnptr(ftype):
            return actions.slot_guard(self, p, out, frames, b.tp,
                                      '&%s->%s' % (paren(b.c), field), ftype)
        tp = b.tp if self.u.is_table(ftype) else None
        return Val('meta', '%s->%s' % (paren(b.c), field), ftype, tp=tp)

    def ev_unary(self, e, p, out, frames):
        op, x = e.op, e.expr
        if op == '&':
            if isinstance(x, c_ast.ArrayRef):
                arr = self.ev(x.name, p, out, frames)
                idx = self.ev(x.subscript, p, out, frames)
                if arr.bt == 'meta' and idx.bt == 'meta' and arr.tp is not None:
                    return Val('meta', '&%s[%s]' % (paren(arr.c), idx.c), arr.ctype,
                               tp=arr.tp, lv=True)
            if isinstance(x, c_ast.ID) and x.name in self.u.vars:
                s = singleton(e)
                if s is not None:
                    raise ResumeHere('the object %s as a value' % SINGLETON_NAMES[s])
                return Val('meta', '&%s' % x.name, self.type_of(e))
            raise ResumeHere('an address %s' % cprint.expr(e))
        if op == '*':
            ctype = None
            q = x
            if isinstance(q, c_ast.Cast):
                ctype = q.to_type.type
                q = q.expr
            a = self.ev(q, p, out, frames)
            if a.bt == 'meta' and a.lv and a.tp is not None and ctype is not None:
                ftype = self.u.pointee(ctype)
                if ftype is not None and self.u.is_fnptr(ftype):
                    return actions.slot_guard(self, p, out, frames, a.tp,
                                              '(void *)%s' % a.c, ftype)
            if a.bt == 'meta' and self.u.is_fnptr(a.ctype):
                return a          # (*f)(args): f
            raise ResumeHere('a load through %s' % cprint.expr(x))
        if op in ('sizeof',):
            return Val('meta', 'sizeof(%s)' % cprint.expr(x), LONG)
        if op in ('++', '--', 'p++', 'p--'):
            y = strip_casts(x)
            if isinstance(y, c_ast.ID) and y.name in p.vars \
                    and p.vars[y.name].bt in ('meta', 'pointer'):
                v = p.vars[y.name]
                return Val('meta', (op + v.cname) if op in ('++', '--')
                           else (v.cname + op[1:]), v.ctype)
            raise ResumeHere('%s of a run-time value' % op.lstrip('p'))
        if op == '-' and reps.ranged(self, x, p):
            return reps.arith(self, e, p, out, frames)
        v = self.ev(x, p, out, frames)
        if v.bt == 'meta':
            return Val('meta', '%s%s' % (op, paren(v.c)), v.ctype)
        if v.bt == 'gen' and op == '!':
            return Val('gen', None, INT, const=not v.const)
        raise ResumeHere('%s of a run-time value' % op)

    # ---- switch and goto ----

    def switch_stmt(self, s, p, out, frames):
        """A switch on a meta value is a switch of the meta: each case's
        statements are translated on their own path, which must end; a
        case falling into the next is refused (none is lost)."""
        v = self.ev(s.cond, p, out, frames)
        if v.bt == 'gen':
            v = Val('meta', '1' if v.const else '0', INT)
        if v.bt != 'meta':
            raise ResumeHere('a switch on a run-time value %s' % cprint.expr(s.cond))
        cases = []
        self.switch_depth += 1
        try:
            items = body(s.stmt)
            for k, c in enumerate(items):
                if not isinstance(c, (c_ast.Case, c_ast.Default)):
                    raise Refuse('a statement before the first case of a switch')
                label = 'default' if isinstance(c, c_ast.Default) else None
                if label is None:
                    kv = self.ev(c.expr, p, out, frames)
                    if kv.bt != 'meta':
                        raise Refuse('a case label not known at compile time')
                    label = 'case %s' % kv.c
                stmts = c.stmts or []
                if not stmts and k + 1 < len(items):
                    cases.append((label, None))      # shares the next case's code
                    continue
                cp = p.copy()
                inner = out.sub()
                for i, x in enumerate(stmts):
                    if cp.ended:
                        break
                    if isinstance(x, c_ast.Break):
                        self.rest_after(frames, cp, inner)
                        break
                    self.statement(x, cp, inner, frames + [(stmts, i)])
                    if not cp.ended:
                        self.release_dead(cp, inner, self.live.after[id(x)])
                if not cp.ended and k + 1 == len(items):
                    self.rest_after(frames, cp, inner)
                if not cp.ended:
                    raise Refuse('a case of a switch runs on into the next (%s)' % label)
                cases.append((label, inner))
        finally:
            self.switch_depth -= 1
        if not any(lab == 'default' for lab, _ in cases):
            raise Refuse('a switch without a default')
        out.switch(v.c, [(lab, code if code is not None else out.sub()) for lab, code in cases])
        p.ended = True

    def rest_after(self, frames, p, out):
        """A case's break: the rest of the block the switch is in, on the
        case's path, as if copied there; it must end the function."""
        if not frames:
            raise Refuse('a break out of a switch')
        outer, j = frames[-1]
        for k in range(j + 1, len(outer)):
            if p.ended:
                break
            x = outer[k]
            if isinstance(x, (c_ast.Label, c_ast.Break, c_ast.Continue)):
                raise Refuse('a break out of a switch to %s' % type(x).__name__)
            self.statement(x, p, out, frames[:-1] + [(outer, k)])
            if not p.ended:
                self.release_dead(p, out, self.live.after[id(x)])
        if not p.ended:
            raise Refuse('a break out of a switch that does not end the function')

    def goto(self, s, p, out, frames):
        """A forward goto to a label of the function's top level: the meta
        goes on with the statements from the label (each goto gets its own
        copy of them, as a meta path does)."""
        top = body(self.fn.body)
        j = next((k for k, x in enumerate(top)
                  if isinstance(x, c_ast.Label) and x.name == s.name), None)
        if j is None or frames[0][0] is not top or j <= frames[0][1]:
            raise Refuse('goto %s: not a forward jump to the top level' % s.name)
        if self.goto_depth > 8:
            raise Refuse('gotos nest too deep')
        self.release_dead(p, out, self.live.before[id(top[j])])
        self.goto_depth += 1
        try:
            for idx in range(j, len(top)):
                if p.ended:
                    break
                self.statement(top[idx], p, out, [(top, idx)])
                if not p.ended:
                    self.release_dead(p, out, self.live.after[id(top[idx])])
        finally:
            self.goto_depth -= 1
        if not p.ended:
            raise Refuse('the statements at %s do not return' % s.name)


# ---- helpers on the AST ----

# the meta's name for the one tuple of a METH_FASTCALL root's arguments
ARGS_TUPLE = 'vargs'


def tuple_spec():
    """The tuple's known-length spec (types.length_spec): how Ni loads
    its length and reads its items."""
    from .types import length_spec
    return next(s for s in length_spec().values() if s['type'] == 'PyTuple_Type')


def argument_check(w, x):
    """(the rule, min, max) when x is a call of a conventions.ARGUMENT_CHECKS
    function on the root's arguments with constant bounds; else None."""
    from . import conventions
    from .types import const_value
    if not (isinstance(x, c_ast.FuncCall) and isinstance(x.name, c_ast.ID)
            and x.name.name in conventions.ARGUMENT_CHECKS):
        return None
    rule = conventions.ARGUMENT_CHECKS[x.name.name]
    args = x.args.exprs if x.args else []
    decl = w.u.funcs.get(x.name.name)
    pnames = [getattr(q, 'name', None) for q in (decl.args.params if decl is not None
                                                 and decl.args else ())]
    for part in ('array', 'count', 'min', 'max'):
        if rule[part] is None:
            continue
        k, pname = rule[part]
        if k >= len(args) or (k < len(pnames) and pnames[k] not in (None, pname)):
            raise Refuse('%s: its parameter %d is not %s' % (x.name.name, k + 1, pname))
    for part, var in (('array', w.array), ('count', w.count)):
        if rule[part] is not None:
            a = strip_casts(args[rule[part][0]])
            if not (isinstance(a, c_ast.ID) and a.name == var):
                raise Refuse("%s of something other than the root's arguments" % x.name.name)
    lo, hi = (const_value(args[rule[b][0]]) for b in ('min', 'max'))
    if lo is None or hi is None or lo > hi:
        raise Refuse('%s with bounds not constants' % x.name.name)
    return rule, lo, hi

def param_decls(fn):
    args = fn.decl.type.args
    if args is None:
        return []
    return [p for p in args.params if isinstance(p, c_ast.Decl) and p.name]


def fatal_functions():
    """The functions policy_table.SEEDS says never return."""
    import policy_table
    return {k.split(':')[1] for k, v in policy_table.SEEDS.items() if v['reason'] == 'fatal'}


def walk_nodes(n):
    yield n
    for _, c in n.children():
        yield from walk_nodes(c)


def names_in(s):
    return {x.name for x in walk_nodes(s) if isinstance(x, c_ast.ID)}


def unconst(t):
    """t without its own const (a meta variable is assigned after its
    declaration)."""
    if isinstance(t, c_ast.TypeDecl) and 'const' in t.quals:
        return c_ast.TypeDecl(t.declname, [q for q in t.quals if q != 'const'], t.align,
                              t.type)
    return t


def declaration(ctext, name):
    return '%s%s%s;' % (ctext, '' if ctext.endswith('*') else ' ', name)


def paren(c):
    c = c.strip()
    if all(ch.isalnum() or ch == '_' for ch in c):
        return c
    return '(%s)' % c


def is_logic(e):
    e = strip_casts(e)
    return isinstance(e, c_ast.BinaryOp) and e.op in ('&&', '||')


def is_err_occurred(e):
    e = strip_casts(e)
    return isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID) \
        and e.name.name == 'PyErr_Occurred' and not e.args


def is_minus_one(e):
    e = strip_casts(e)
    return isinstance(e, c_ast.UnaryOp) and e.op == '-' and isinstance(e.expr, c_ast.Constant) \
        and e.expr.value in ('1', '1.0')


def singleton(e):
    """The singleton struct e takes the address of ('&_Py_NoneStruct'),
    else None."""
    e = strip_casts(e)
    if isinstance(e, c_ast.UnaryOp) and e.op == '&' and isinstance(e.expr, c_ast.ID) \
            and e.expr.name in SINGLETONS:
        return e.expr.name
    return None


def new_reference(e):
    """X when e is 'Py_INCREF(X), X' (the return macros' form)."""
    e = strip_casts(e)
    if isinstance(e, c_ast.ExprList) and len(e.exprs) == 2:
        a, b = e.exprs
        a = strip_casts(a)
        if isinstance(a, c_ast.FuncCall) and isinstance(a.name, c_ast.ID) \
                and a.name.name == '__ni_vocab_Py_INCREF' and a.args \
                and same_expr(a.args.exprs[0], b):
            return strip_casts(b)
    return None


def same_expr(a, b):
    from .policy import same
    return same(a, b)


def once_do(s):
    """'do S while (0);' with no break or continue of its own: S, once."""
    if not isinstance(s, c_ast.DoWhile):
        return False
    c = strip_casts(s.cond)
    if not (isinstance(c, c_ast.Constant) and c.value == '0'):
        return False
    found = []

    def walk(n):
        if isinstance(n, (c_ast.Break, c_ast.Continue)):
            found.append(1)
        if isinstance(n, (c_ast.For, c_ast.While, c_ast.DoWhile, c_ast.Switch)):
            return
        for _, c in n.children():
            walk(c)
    walk(s.stmt)
    return not found


def param_alias(var):
    if var is None:
        return None
    return var.name if var.isparam else var.alias
