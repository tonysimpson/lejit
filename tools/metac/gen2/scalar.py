"""The scalar helper (doc/GENERATOR_DESIGN.md "The specialiser's
actions"): Ni's backend has no floating-point instructions, so C code on
doubles is copied into one C helper that the meta calls.

The run is the longest run of statements on C scalars, starting at the
current statement, that reads or writes a double and leaves at most one
variable live (the helper's result); or the rest of the block, when every
path through it returns (an error, a constructor given a scalar, or a
scalar). A run that only copies (a variable or a field into a variable, or
returned) is the walk's.

C scalars are: doubles and integers of the function (a vinfo, or a meta
value passed as a word), a float's double field (loaded by Ni), constants,
the pure functions of policy_table.EXTERNAL and those effects.py proves
pure, a function-pointer parameter every caller gives a pure function,
errno, and raising a built-in exception with a constant message.

The helper's call is always CfNoBlock and CfNoPython; CfPure only when it
raises nothing and reads and writes no C library state, so that Ni may
call it while compiling. errno is never read across a helper's boundary:
a run reads it only after setting it, and nothing after the run reads what
it set. A helper that raises returns an int status (0, or -1 with the
exception set) and its result through an out-parameter, so the run-time
test is of a register, not PyErr_Occurred().
"""
from pycparser import c_ast

from . import conventions, cprint
from .ctyping import strip_casts, text
from .errors import Refuse
from .values import Val

RAISES = ('PyErr_SetString', 'PyErr_Format', 'PyErr_SetFromErrno')
ERRNO = 'errno'


class Run:
    """What a run of scalar code reads, writes and calls."""

    def __init__(self):
        self.ins = []             # (key, kind, what): 'var' Var, 'field' Var
        self.local = {}           # declared inside the run: name -> type
        self.defs = []            # variables of the function it assigns
        self.raises = False
        self.doubles = False
        self.state = False        # reads or writes C library state
        self.errno_set = False
        self.errno_read = False
        self.ctor = None          # a tail: the representation its returns build
        self.fnptrs = []          # function-pointer parameters it calls
        self.copies = []          # static functions it calls, as their copies

    def add_in(self, key, kind, what):
        if all(k != key for k, _, _ in self.ins):
            self.ins.append((key, kind, what))

    def copy(self):
        import copy
        r = copy.copy(self)
        r.ins, r.local, r.defs = list(self.ins), dict(self.local), list(self.defs)
        r.fnptrs = list(self.fnptrs)
        r.copies = list(self.copies)
        return r


class Scan:
    """Is C scalar code? (a statement, an expression), for one walk."""

    def __init__(self, w, p):
        self.w, self.p, self.u = w, p, w.u
        self.inside = frozenset()
        from . import reps
        self.double_rep = reps.reps_of(w, 'double')

    def scalar_type(self, t):
        return t is not None and (self.u.is_int(t) or self.u.is_double(t))

    def expr(self, e, run):
        u = self.u
        if isinstance(e, c_ast.Constant):
            if e.type in ('double', 'float'):
                run.doubles = True
            return e.type != 'string'
        if is_errno(e):
            if not run.errno_set:
                return False        # set before the helper: it would cross over
            run.errno_read = run.state = True
            return True
        if isinstance(e, c_ast.ID):
            return self.name(e.name, run)
        if isinstance(e, c_ast.StructRef):
            if self.double_rep is None:
                return False
            from . import reps
            xv = reps.double_field(self.w, self.double_rep, e, self.p)
            if xv is None:
                return False
            run.add_in('%s_%s' % (xv.name, e.field.name), 'field', xv)
            run.doubles = True
            return True
        if isinstance(e, c_ast.UnaryOp):
            if e.op in ('&', '*', '++', '--', 'p++', 'p--'):
                return False
            if e.op == 'sizeof':
                return True
            return self.expr(e.expr, run)
        if isinstance(e, c_ast.BinaryOp):
            return self.expr(e.left, run) and self.expr(e.right, run)
        if isinstance(e, c_ast.TernaryOp):
            return all(self.expr(x, run) for x in (e.cond, e.iftrue, e.iffalse))
        if isinstance(e, c_ast.Cast):
            if isinstance(e.expr, c_ast.Constant) and u.is_void(e.to_type.type):
                return True         # assert() under NDEBUG
            if u.is_double(e.to_type.type):
                run.doubles = True
            return self.scalar_type(e.to_type.type) and self.expr(e.expr, run)
        if isinstance(e, c_ast.ExprList):
            return all(self.expr(x, run) for x in e.exprs)
        if isinstance(e, c_ast.FuncCall):
            return self.call(e, run)
        return False

    def name(self, n, run):
        if n in run.local:
            if self.u.is_double(run.local[n]):
                run.doubles = True
            return True
        v = self.p.vars.get(n)
        if v is not None:
            if v.singleton is not None or v.word_of or not self.scalar_type(v.ctype):
                return False
            if self.u.is_double(v.ctype):
                run.doubles = True
            if n not in run.defs or any(k == n for k, _, _ in run.ins):
                run.add_in(n, 'var', v)
            return True
        return n in self.u.enums

    def call(self, e, run):
        f = callee(e)
        args = e.args.exprs if e.args else []
        if f is None:
            return False
        n = f.name
        v = self.p.vars.get(n)
        if v is not None:
            # a function-pointer parameter: pure if every caller gives a
            # pure function
            if not (v.isparam and v.bt == 'meta' and self.u.is_fnptr(v.ctype)
                    and self.w.tr.fnptr_targets_pure(self.w.name, n)):
                return False
            run.add_in(n, 'var', v)
            run.fnptrs.append(n)
            return all(self.expr(a, run) for a in args)
        if n in self.w.tr.funcs:
            if not self.copied(n, run):
                return False
            return all(self.expr(a, run) for a in args)
        if not self.pure(n):
            return False
        return all(self.expr(a, run) for a in args)

    def copied(self, n, run):
        """A static function of the file that is itself scalar code (its
        parameters its locals, its returns scalars) is called in the helper
        as its run-time copy. It counts as C library state; it raises if
        its code may, and reads errno only after the run set it."""
        tr, u = self.w.tr, self.u
        g = tr.funcs[n]
        if 'static' not in g.decl.storage or g is self.w.fn or n in self.inside:
            return False
        args = g.decl.type.args
        ps = [x for x in (args.params if args else []) if isinstance(x, c_ast.Decl)]
        if ps and ps[0].name is None and u.is_void(ps[0].type):
            ps = []
        if not self.scalar_type(g.decl.type.type) or not all(
                x.name and self.scalar_type(x.type) for x in ps):
            return False
        sub = Run()
        sub.local = {x.name: x.type for x in ps}
        sub.errno_set = run.errno_set
        inner = StaticScan(self, n)
        if not all(inner.stmt(x, sub, True) for x in g.body.block_items or []):
            return False
        run.state = True
        run.raises = run.raises or sub.raises
        run.errno_read = run.errno_read or sub.errno_read
        for c in sub.copies + [n]:
            if c not in run.copies:
                run.copies.append(c)
        return True

    def pure(self, n):
        tr = self.w.tr
        if n in tr.pure or n.startswith('__builtin_'):
            return True
        if n in tr.funcs:
            return False
        key = tr.effects_key(n)
        return 'CfPure' in tr.effect_flags(key)

    def stmt(self, s, run, tail):
        u = self.u
        if isinstance(s, c_ast.EmptyStatement):
            return True
        if isinstance(s, c_ast.Compound):
            return all(self.stmt(x, run, tail) for x in s.block_items or [])
        if isinstance(s, c_ast.Decl):
            if isinstance(s.type, (c_ast.ArrayDecl, c_ast.FuncDecl)) \
                    or not self.scalar_type(s.type) or 'static' in s.storage:
                return False
            if s.init is not None and not self.expr(s.init, run):
                return False
            run.local[s.name] = s.type
            if u.is_double(s.type):
                run.doubles = True
            return True
        if isinstance(s, c_ast.Assignment):
            lv = strip_casts(s.lvalue)
            if is_errno(lv) and s.op == '=':
                if not self.expr(s.rvalue, run):
                    return False
                run.errno_set = run.state = True
                return True
            if not isinstance(lv, c_ast.ID):
                return False
            n = lv.name
            if n not in run.local:
                v = self.p.vars.get(n)
                if v is None or v.bt != 'dynamic' or not self.scalar_type(v.ctype) \
                        or v.word_of:
                    return False
            if s.op != '=' and not self.name(n, run):
                return False
            if not self.expr(s.rvalue, run):
                return False
            if n not in run.local and n not in run.defs:
                run.defs.append(n)
            if self.u.is_double(self.p.vars[n].ctype if n in self.p.vars else run.local[n]):
                run.doubles = True
            return True
        if isinstance(s, c_ast.Cast):
            return isinstance(s.expr, c_ast.Constant)       # assert()
        if isinstance(s, c_ast.FuncCall):
            f = strip_casts(s.name)
            if isinstance(f, c_ast.ID) and f.name in RAISES and all(
                    constant_arg(a) for a in (s.args.exprs if s.args else [])):
                run.raises = True
                return True
            return False
        if isinstance(s, c_ast.If):
            return self.expr(s.cond, run) and self.stmt(s.iftrue, run, tail) \
                and (s.iffalse is None or self.stmt(s.iffalse, run, tail))
        if isinstance(s, c_ast.Return):
            return tail and self.ret(s.expr, run)
        return False

    def ret(self, e, run):
        w = self.w
        if e is None:
            return False
        if self.error_return(e):
            return True
        if w.sm.kind == 'obj':
            e = strip_casts(e)
            if not (isinstance(e, c_ast.FuncCall) and callee(e) is not None
                    and e.args and len(e.args.exprs) == 1):
                return False
            r = self.double_rep
            if r is None or callee(e).name not in r.spec['constructors']:
                return False
            run.ctor = r
            before = run.doubles
            run.doubles = False
            ok = self.expr(e.args.exprs[0], run)
            run.doubles = before or run.doubles
            return ok
        if w.sm.kind == 'val' and self.scalar_type(w.fn.decl.type.type):
            return self.expr(e, run)
        return False

    def error_return(self, e):
        e = strip_casts(e)
        if self.w.sm.kind == 'obj':
            return cprint.is_null(e)
        return isinstance(e, c_ast.UnaryOp) and e.op == '-' \
            and isinstance(e.expr, c_ast.Constant) and e.expr.value in ('1', '1.0')


class StaticScan(Scan):
    """A static function's body, as scalar code of its own: only its
    parameters and locals are its variables, and it returns scalars."""

    def __init__(self, outer, name):
        import types
        self.w, self.p, self.u = outer.w, types.SimpleNamespace(vars={}), outer.u
        self.double_rep = None
        self.inside = outer.inside | {name}

    def name(self, n, run):
        if n in run.local:
            return True
        return n in self.u.enums

    def ret(self, e, run):
        return e is not None and self.expr(e, run)

    def error_return(self, e):
        return False


def callee(e):
    """The function call e names: 'f(...)' or '(*f)(...)', f an ID (casts
    ignored); else None."""
    f = strip_casts(e.name)
    if isinstance(f, c_ast.UnaryOp) and f.op == '*':
        f = strip_casts(f.expr)
    return f if isinstance(f, c_ast.ID) else None


def is_errno(e):
    """errno, as glibc's errno.h defines it: (*__errno_location ())."""
    e = strip_casts(e)
    if isinstance(e, c_ast.ID):
        return e.name == ERRNO
    return isinstance(e, c_ast.UnaryOp) and e.op == '*' \
        and isinstance(strip_casts(e.expr), c_ast.FuncCall) \
        and isinstance(strip_casts(e.expr).name, c_ast.ID) \
        and strip_casts(e.expr).name.name == '__errno_location'


def constant_arg(a):
    a = strip_casts(a)
    if isinstance(a, c_ast.Constant):
        return True
    return isinstance(a, c_ast.ID) and a.name.startswith('PyExc_')


def ends(stmts):
    """Does every path through the statements return?"""
    for s in stmts:
        if returns(s):
            return True
    return False


def returns(s):
    if isinstance(s, c_ast.Return):
        return True
    if isinstance(s, c_ast.Compound):
        return ends(s.block_items or [])
    if isinstance(s, c_ast.If):
        return s.iffalse is not None and returns(s.iftrue) and returns(s.iffalse)
    return False


def trivial(scan, stmts):
    """Copies only: a variable or a field into a variable, or returned
    (given to a constructor), which the walk does without a helper."""
    def copy(e):
        e = strip_casts(e)
        return isinstance(e, c_ast.ID) or (scan.double_rep is not None and isinstance(
            e, c_ast.StructRef) and e.field.name == scan.double_rep.spec['field'])
    for s in stmts:
        if isinstance(s, c_ast.EmptyStatement):
            continue
        if isinstance(s, c_ast.Assignment) and s.op == '=' and copy(s.rvalue):
            continue
        if isinstance(s, c_ast.Return) and s.expr is not None:
            e = strip_casts(s.expr)
            if copy(e) or scan.error_return(e) or (
                    isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID)
                    and scan.w.tr.constructor(e.name.name) and e.args
                    and len(e.args.exprs) == 1 and copy(e.args.exprs[0])):
                continue
        return False
    return True


def errno_after(w, frames, run_len):
    """Is errno read after the run (in the rest of the function)?"""
    rest = []
    for level, (stmts, i) in enumerate(reversed(frames)):
        rest.extend(stmts[i + run_len:] if level == 0 else stmts[i + 1:])
    from .abstract import walk_nodes
    return any(is_errno(n) for s in rest for n in walk_nodes(s))


def run(w, stmts, i, p, out, frames):
    """The scalar helper at statement i of a block: the number of
    statements it took (0: none)."""
    if not w.tr.reps or 'double' not in w.tr.reps:
        return 0
    s = stmts[i]
    if isinstance(s, c_ast.EmptyStatement) or (isinstance(s, c_ast.Decl) and s.init is None):
        return 0
    scan = Scan(w, p)
    rest = stmts[i:]
    tail = Run()
    if all(scan.stmt(x, tail, True) for x in rest) and ends(rest) and tail.doubles \
            and not trivial(scan, rest) and not (tail.errno_set and errno_after(
                w, frames, len(rest))):
        if tail.raises or not any(scan.error_return(x.expr) for x in walk_returns(rest)):
            emit(w, scan, rest, tail, p, out, frames, True)
            return len(rest)
    runs, r = [], Run()
    for j in range(i, len(stmts)):
        trial = r.copy()
        if not scan.stmt(stmts[j], trial, False):
            break
        r = trial
        runs.append(r)
    for j in range(len(runs), 0, -1):
        r = runs[j - 1]
        part = stmts[i:i + j]
        if not r.doubles or trivial(scan, part) or (r.errno_set and errno_after(w, frames, j)):
            continue
        outs = [n for n in r.defs if n in w.live.after[id(part[-1])]]
        if len(outs) > 1 or (not outs and not r.raises):
            continue
        emit(w, scan, part, r, p, out, frames, False, outs)
        return j
    return 0


def walk_returns(stmts):
    from .abstract import walk_nodes
    return [n for s in stmts for n in walk_nodes(s) if isinstance(n, c_ast.Return)]


class HelperPrinter(cprint.Printer):
    """The run's C in the helper: a field read is the parameter it was
    loaded into; a return is the helper's."""

    def __init__(self, scan, status, tail, result, copies):
        super().__init__()
        self.scan, self.status, self.tail, self.result = scan, status, tail, result
        self.copies = copies

    def visit_StructRef(self, n):
        from . import reps
        if self.scan.double_rep is not None:
            xv = reps.double_field(self.scan.w, self.scan.double_rep, n, self.scan.p)
            if xv is not None:
                return '%s_%s' % (xv.name, n.field.name)
        return super().visit_StructRef(n)

    def visit_FuncCall(self, n):
        f = callee(n)
        if f is not None and f.name in self.copies:
            args = n.args.exprs if n.args else []
            return '%s(%s)' % (self.copies[f.name], ', '.join(self.visit(a) for a in args))
        return super().visit_FuncCall(n)

    def visit_Return(self, n):
        if self.scan.error_return(n.expr):
            return 'return -1;'
        e = strip_casts(n.expr)
        if self.scan.w.sm.kind == 'obj':
            e = e.args.exprs[0]
        if self.status:
            return '{ *_r = %s; return 0; }' % self.visit(e)
        return 'return %s;' % self.visit(e)


def emit(w, scan, stmts, r, p, out, frames, tail, outs=()):
    from . import actions
    fn = w.fn
    if tail:
        if w.sm.kind == 'obj':
            rtype = c_ast.TypeDecl(None, [], None, c_ast.IdentifierType(['double']))
        else:
            rtype = fn.decl.type.type
    elif outs:
        rtype = p.vars[outs[0]].ctype
    else:
        rtype = None
    status = r.raises
    if not status and rtype is None:
        raise Refuse('a scalar run with no result')
    actions.no_error_pending(p)
    # the helper's parameters, the call's format and arguments
    params, fmt, args, loads = [], '', [], []
    order = sorted(r.ins, key=lambda x: (x[1] == 'field',
                                         list(p.vars).index(x[0]) if x[0] in p.vars else 0))
    from . import reps
    for key, kind, what in order:
        if kind == 'field':
            val = reps.load(w, scan.double_rep, what, p, out)
            loads.append(val)
            params.append('double %s' % key)
            fmt += 'd'
            args.append(val.c)
            continue
        v = what
        if v.bt == 'dynamic' and v.own == 'none' and not v.isparam:
            raise Refuse('%s read with no value' % key)
        params.append(text(unconst(v.ctype), key))
        if v.bt == 'meta':
            fmt += 'l'
            args.append('(long)(%s)' % v.cname)
        else:
            fmt += 'd' if w.u.is_double(v.ctype) else 'v'
            args.append(v.cname)
    if status and rtype is not None:
        params.append(text(c_ast.PtrDecl([], unconst(rtype)), '_r'))
    lines = ['  %s;' % text(unconst(p.vars[n].ctype), n)
             for n in r.defs if n not in [k for k, _, _ in r.ins]]
    pr = HelperPrinter(scan, status, tail, rtype,
                       {n: w.tr.copies.whole(n) for n in r.copies})
    for s in stmts:
        for ln in pr._generate_stmt(s).rstrip('\n').split('\n'):
            lines.append('  ' + ln)
    if not tail:
        if status:
            if outs:
                lines.append('  *_r = %s;' % outs[0])
            lines.append('  return 0;')
        else:
            lines.append('  return %s;' % outs[0])
    ret = 'int' if status else text(unconst(rtype))
    name = w.tr.helper(fn.decl.name, (stmts[0], len(stmts)), ret, params, lines)
    # the call
    w.rec.ncalls += 1
    p.called = True
    w.rec.any_call = True
    if status:
        arr = None
        if rtype is not None:
            arr = w.new_tmp('vinfo_array_t *')
            out.line('%s = array_new(1);' % arr)
            fmt += 'a'
            args.append(arr)
        t = w.new_tmp('vinfo_t *')
        out.line('%s = psyco_generic_call(po, %s, %s, "%s"%s);' % (
            t, name, 'CfCommonIntZeroOk | CfNoPython | CfNoBlock', fmt,
            ''.join(', ' + a for a in args)))
        for val in loads:
            out.line('vinfo_decref(%s, po);' % val.c)
        sub = out.sub()
        if arr:
            sub.line('array_release(%s);' % arr)
        w.release_all(p, sub)
        sub.line('return %s;' % w.err())
        out.if_('%s == NULL' % t, sub)
        if arr is None:
            return
        res = w.new_tmp('vinfo_t *')
        out.line('%s = %s->items[0];' % (res, arr))
        out.line('array_release(%s);' % arr)
    else:
        flags = conventions.ret_flag(w.u, rtype)
        if flags is None:
            raise Refuse('a scalar helper returning %s' % text(rtype))
        flags = ('CfPure | ' if not r.state else '') + flags + ' | CfNoPython | CfNoBlock'
        direct = direct_callee(w, scan, stmts, r, tail)
        res = w.new_tmp('vinfo_t *')
        if direct is not None:
            dflags = conventions.ret_flag(w.u, rtype)
            dflags += ''.join(' | ' + x for x in w.tr.effect_flags(w.tr.effects_key(direct)))
            out.line('%s = psyco_generic_call(po, %s, %s, "%s"%s);' % (
                res, direct, dflags, fmt, ''.join(', ' + a for a in args)))
            w.tr.unhelper(name)
        else:
            out.line('%s = psyco_generic_call(po, %s, %s, "%s"%s);' % (
                res, name, flags, fmt, ''.join(', ' + a for a in args)))
        for val in loads:
            out.line('vinfo_decref(%s, po);' % val.c)
        w.error_exit(p, out, '%s == NULL' % res)
    if tail:
        if w.sm.kind == 'obj':
            o = w.new_tmp('vinfo_t *')
            out.line('%s = %s(%s);' % (o, r.ctor.spec['new'], res))
            v = Val('dynamic', o, None, owned=True)
            v.made = r.ctor.spec['type']
            w.ret_value_owned(p, out, v)
        else:
            w.ret_value_owned(p, out, Val('dynamic', res, rtype, owned=True))
        return
    v = p.vars[outs[0]]
    if v.owned():
        w.release(v, out)
    out.line('%s = %s;' % (v.cname, res))
    v.own, v.conv, v.rng, v.alias = 'new', None, None, None


def direct_callee(w, scan, stmts, r, tail):
    """A helper that would only return one pure function of another file
    given its parameters in order: that function, called directly."""
    if not tail or r.raises or r.state or r.fnptrs or len(stmts) != 1 \
            or not isinstance(stmts[0], c_ast.Return) or w.sm.kind == 'obj':
        return None
    e = strip_casts(stmts[0].expr)
    if not (isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID)):
        return None
    n = e.name.name
    if n in w.tr.funcs or n in w.tr.pure or n.startswith('__builtin_') \
            or 'CfPure' not in w.tr.effect_flags(w.tr.effects_key(n)):
        return None
    keys = [k for k, _, _ in sorted(r.ins, key=lambda x: x[1] == 'field')]
    got = []
    for a in (e.args.exprs if e.args else []):
        a = strip_casts(a)
        if isinstance(a, c_ast.StructRef):
            b = strip_casts(a.name)
            got.append('%s_%s' % (getattr(b, 'name', ''), a.field.name))
        elif isinstance(a, c_ast.ID):
            got.append(a.name)
        else:
            return None
    return n if got == keys else None


def unconst(t):
    from .walk import unconst as u
    return u(t)


