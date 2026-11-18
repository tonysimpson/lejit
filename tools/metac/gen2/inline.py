"""Inline (doc/GENERATOR_DESIGN.md "The specialiser's actions"): a call of
a static function of the file given the address of a local (an
out-parameter) or a function's name is the function's body in its place,
by value: each parameter is a fresh local holding its argument, evaluated
once, and a parameter given '&x' is the caller's x itself where the body
reads or writes '*p' (the address is known when generating; any other use
of it refuses the inline). A parameter given a constant or a function's
name, and never assigned in the body, is that constant or function where
the body reads it (nothing to evaluate). The callee's locals are renamed.

The call is the statement's: 'return f(...)' (the body's returns are the
caller's), or 'if (f(...) OP K) S1 else S2' and 'if (f(...))', where
each of the body's returns gives a constant and so chooses S1 or S2; a
choice that doesn't leave the function must come last on its path.

The expanded statement (and each statement around it, copied) keeps the
source's in ORIG: a run-time copy is of the C as written. EXPANDED holds
the expanded bodies (AST nodes have slots, so these are side tables).
"""
import copy
import weakref

from pycparser import c_ast

from .ctyping import strip_casts, text
from .types import const_value

OPS = {'<': lambda a, b: a < b, '<=': lambda a, b: a <= b, '>': lambda a, b: a > b,
       '>=': lambda a, b: a >= b, '==': lambda a, b: a == b, '!=': lambda a, b: a != b}


ORIG = weakref.WeakKeyDictionary()
EXPANDED = weakref.WeakSet()


def orig(s):
    return ORIG.get(s, s)


def expanded(s):
    return s in EXPANDED


class NoInline(Exception):
    pass


def expand(tr, fn, budgets):
    """fn with its inlinable calls expanded (fn itself if none), and the
    budgets hit."""
    hit = []
    body = fn.body
    for _ in range(budgets['inline_depth']):
        new = Expander(tr, fn, budgets, hit).stmt(body)
        if new is body:
            break
        body = new
    else:
        if Expander(tr, fn, budgets, []).stmt(body) is not body:
            hit.append('inline_depth')
    if body is fn.body:
        return fn, hit
    out = c_ast.FuncDef(fn.decl, fn.param_decls, body, fn.coord)
    return out, hit


def names_in(n):
    out = set()
    stack = [n]
    while stack:
        x = stack.pop()
        if isinstance(x, c_ast.ID):
            out.add(x.name)
        elif isinstance(x, c_ast.Decl) and x.name:
            out.add(x.name)
        stack.extend(c for _, c in x.children())
    return out


class Expander:
    def __init__(self, tr, fn, budgets, hit):
        self.tr, self.fn, self.budgets, self.hit = tr, fn, budgets, hit
        self.taken = names_in(fn.body) | {p.name for p in params(fn)}
        self.local = {d.name for d in walk(fn.body) if isinstance(d, c_ast.Decl)} \
            | {p.name for p in params(fn)}

    def stmt(self, s):
        """s, or a copy of it with its calls expanded (orig: s)."""
        if s is None:
            return None
        r = self.call_stmt(s)
        if r is not None:
            return r
        if isinstance(s, c_ast.Compound):
            items = s.block_items or []
            new = [self.stmt(x) for x in items]
            if all(a is b for a, b in zip(new, items)):
                return s
            return keep(c_ast.Compound(new, s.coord), s)
        if isinstance(s, c_ast.If):
            t, f = self.stmt(s.iftrue), self.stmt(s.iffalse)
            if t is s.iftrue and f is s.iffalse:
                return s
            return keep(c_ast.If(s.cond, t, f, s.coord), s)
        if isinstance(s, c_ast.Label):
            x = self.stmt(s.stmt)
            return s if x is s.stmt else keep(c_ast.Label(s.name, x, s.coord), s)
        if isinstance(s, c_ast.DoWhile):
            x = self.stmt(s.stmt)
            return s if x is s.stmt else keep(c_ast.DoWhile(s.cond, x, s.coord), s)
        return s

    def call_stmt(self, s):
        try:
            if isinstance(s, c_ast.Return) and s.expr is not None:
                m = self.inlinable(s.expr)
                if m is not None:
                    g, args = m
                    if text(g.decl.type.type) != text(self.fn.decl.type.type):
                        return None
                    return keep(mark(self.body(g, args, None, s)), s)
            if isinstance(s, c_ast.If):
                c = strip_casts(s.cond)
                test = None
                if isinstance(c, c_ast.BinaryOp) and c.op in OPS:
                    k = const_value(c.right)
                    m = self.inlinable(c.left) if k is not None else None
                    test = (c.op, k)
                elif isinstance(c, c_ast.FuncCall):
                    m, test = self.inlinable(c), ('!=', 0)
                else:
                    m = None
                if m is not None:
                    g, args = m
                    return keep(mark(self.body(g, args, (test[0], test[1], s.iftrue,
                                                         s.iffalse), s)), s)
        except NoInline:
            return None
        return None

    def inlinable(self, e):
        e = strip_casts(e)
        if not (isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID)):
            return None
        g = self.tr.funcs.get(e.name.name)
        if g is None or 'static' not in g.decl.storage or g is self.fn \
                or g.decl.name == self.fn.decl.name:
            return None
        args = e.args.exprs if e.args else []
        ps = params(g)
        if len(args) != len(ps):
            return None
        if not any(out_arg(a) is not None or self.designator(a) for a in args):
            return None
        n = sum(1 for x in walk(g.body) if isinstance(x, c_ast.Node)
                and type(x).__name__ in STATEMENTS)
        if n > self.budgets['inline_statements']:
            self.hit.append('inline_statements (%s)' % g.decl.name)
            return None
        if any(isinstance(x, (c_ast.For, c_ast.While, c_ast.DoWhile, c_ast.Switch,
                              c_ast.Goto, c_ast.Label)) for x in walk(g.body)):
            return None
        return g, args

    def designator(self, a):
        """a names a function (not a variable of the caller)."""
        a = strip_casts(a)
        return isinstance(a, c_ast.ID) and a.name in self.tr.unit.funcs \
            and a.name not in self.local

    def fixed(self, g, p, a):
        """The parameter p may be replaced by its argument a: a constant or
        a function's name, and p never assigned or addressed in g."""
        if not (self.designator(a) or const_value(a) is not None
                or (isinstance(strip_casts(a), c_ast.Constant)
                    and strip_casts(a).type != 'string')):
            return False
        for x in walk(g.body):
            if isinstance(x, c_ast.Assignment) and isinstance(strip_casts(x.lvalue), c_ast.ID) \
                    and strip_casts(x.lvalue).name == p.name:
                return False
            if isinstance(x, c_ast.UnaryOp) and x.op in ('&', '++', '--', 'p++', 'p--') \
                    and isinstance(strip_casts(x.expr), c_ast.ID) \
                    and strip_casts(x.expr).name == p.name:
                return False
        return True

    def fresh(self, g, name):
        n = '%s_%s' % (g.decl.name, name)
        k = 1
        while n in self.taken:
            k += 1
            n = '%s_%s_%d' % (g.decl.name, name, k)
        self.taken.add(n)
        return n

    def body(self, g, args, test, at):
        ptr, rename, head, value = {}, {}, [], {}
        for p, a in zip(params(g), args):
            x = out_arg(a)
            if x is not None:
                ptr[p.name] = x
                continue
            if self.fixed(g, p, a):
                value[p.name] = a
                continue
            n = self.fresh(g, p.name)
            rename[p.name] = n
            d = copy.deepcopy(p)
            rename_decl(d, n)
            d.init = copy.deepcopy(a)
            d.coord = at.coord
            head.append(d)
        for d in walk(g.body):
            if isinstance(d, c_ast.Decl) and d.name and d.name not in rename \
                    and not isinstance(d.type, c_ast.FuncDecl):
                rename[d.name] = self.fresh(g, d.name)
        b = copy.deepcopy(g.body)
        b = Subst(ptr, rename, value).visit(b)
        items = b.block_items or []
        if test is not None:
            items = choose(items, test, True)
        return c_ast.Compound(head + items, at.coord)


STATEMENTS = {'Decl', 'Assignment', 'FuncCall', 'If', 'Return', 'Compound', 'UnaryOp'}


def keep(new, old):
    ORIG[new] = ORIG.get(old, old)
    return new


def mark(b):
    EXPANDED.add(b)
    return b


def params(fn):
    a = fn.decl.type.args
    return [p for p in (a.params if a else []) if isinstance(p, c_ast.Decl) and p.name]


def out_arg(a):
    """x when a is '&x' of a plain variable."""
    a = strip_casts(a)
    if isinstance(a, c_ast.UnaryOp) and a.op == '&':
        x = strip_casts(a.expr)
        if isinstance(x, c_ast.ID):
            return x.name
    return None


def walk(n):
    yield n
    for _, c in n.children():
        yield from walk(c)


def rename_decl(d, name):
    d.name = name
    t = d.type
    while not isinstance(t, c_ast.TypeDecl):
        t = t.type
    t.declname = name


class Subst:
    """The callee's body in the caller: '*p' is the caller's variable, a
    fixed parameter its argument, the callee's own names renamed."""

    def __init__(self, ptr, rename, value=None):
        self.ptr, self.rename, self.value = ptr, rename, value or {}

    def visit(self, n):
        if isinstance(n, c_ast.UnaryOp) and n.op == '*':
            x = strip_casts(n.expr)
            if isinstance(x, c_ast.ID) and x.name in self.ptr:
                return c_ast.ID(self.ptr[x.name], n.coord)
        if isinstance(n, c_ast.ID):
            if n.name in self.ptr:
                raise NoInline('the pointer %s is used as a value' % n.name)
            if n.name in self.value:
                return copy.deepcopy(self.value[n.name])
            if n.name in self.rename:
                return c_ast.ID(self.rename[n.name], n.coord)
            return n
        if isinstance(n, c_ast.Decl) and n.name in self.rename:
            rename_decl(n, self.rename[n.name])
        for attr in n.__slots__:
            if attr in ('coord', '__weakref__'):
                continue
            v = getattr(n, attr, None)
            if isinstance(v, c_ast.Node):
                setattr(n, attr, self.visit(v))
            elif isinstance(v, list):
                setattr(n, attr, [self.visit(x) if isinstance(x, c_ast.Node) else x
                                  for x in v])
        return n


def choose(stmts, test, tail):
    """The body's returns as the caller's test chooses: S1 or S2 in each
    one's place."""
    op, k, s1, s2 = test
    out = []
    last = max((j for j, s in enumerate(stmts)
                if not isinstance(s, c_ast.EmptyStatement)), default=-1)
    for j, s in enumerate(stmts):
        at_end = tail and j >= last
        if isinstance(s, c_ast.Return):
            c = const_value(s.expr) if s.expr is not None else None
            if c is None:
                raise NoInline('a return of a value not known when generating')
            chosen = s1 if OPS[op](c, k) else s2
            if chosen is None or not leaves(chosen):
                if not at_end:
                    raise NoInline('a choice that does not leave the function, not last')
                if chosen is not None:
                    out.append(copy.deepcopy(chosen))
                continue
            out.append(copy.deepcopy(chosen))
            continue
        if isinstance(s, c_ast.Compound):
            out.append(c_ast.Compound(choose(s.block_items or [], test, at_end), s.coord))
            continue
        if isinstance(s, c_ast.If):
            t = choose([s.iftrue], test, at_end)
            f = choose([s.iffalse], test, at_end) if s.iffalse is not None else None
            out.append(c_ast.If(s.cond, one(t, s), one(f, s) if f is not None else None,
                                s.coord))
            continue
        out.append(s)
    return out


def one(stmts, at):
    if len(stmts) == 1:
        return stmts[0]
    return c_ast.Compound(stmts, at.coord)


def leaves(s):
    if isinstance(s, c_ast.Return):
        return True
    if isinstance(s, c_ast.Compound):
        return any(leaves(x) for x in s.block_items or [])
    if isinstance(s, c_ast.If):
        return s.iffalse is not None and leaves(s.iftrue) and leaves(s.iffalse)
    return False
