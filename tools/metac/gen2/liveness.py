"""Liveness (doc/GENERATOR_DESIGN.md "Analyses", 2): for each statement,
the variables read after it (or before it) on some path before being
assigned again. Backwards over the AST; loops to a fixed point; a goto is
live as its label (the labels are found on a first pass)."""
from pycparser import c_ast

LOOPS = (c_ast.For, c_ast.While, c_ast.DoWhile)


def names(e):
    """The names an expression reads (a field name is not one)."""
    out = []

    def walk(n):
        if n is None:
            return
        if isinstance(n, c_ast.ID):
            out.append(n.name)
        elif isinstance(n, c_ast.StructRef):
            walk(n.name)
        elif isinstance(n, c_ast.Typename):
            return
        elif isinstance(n, c_ast.Decl):
            walk(n.init)
        else:
            for _, c in n.children():
                walk(c)
    walk(e)
    return out


def uses_defs(e):
    """(names read, name assigned) of an expression statement."""
    if isinstance(e, c_ast.Assignment) and isinstance(e.lvalue, c_ast.ID):
        reads = names(e.rvalue)
        if e.op != '=':
            reads = [e.lvalue.name] + reads
        return reads, e.lvalue.name
    return names(e), None


def expr_live(e, after):
    if isinstance(e, c_ast.ExprList):
        live = after
        for x in reversed(e.exprs):
            live = expr_live(x, live)
        return live
    reads, d = uses_defs(e)
    return (after - {d}) | set(reads)


def body(s):
    """The statements of a branch or block."""
    if s is None:
        return []
    if isinstance(s, c_ast.Compound):
        return s.block_items or []
    return [s]


class Liveness:
    def __init__(self, fn_body):
        self.after, self.before = {}, {}
        self.labels = {}
        self.jumps = []           # (live at break, live at continue)
        for _ in range(2):
            self.block(body(fn_body), frozenset())

    def block(self, stmts, after):
        live = after
        for s in reversed(stmts):
            live = self.stmt(s, live)
        return live

    def loop(self, s, after):
        if isinstance(s, c_ast.For):
            init, cond, step = s.init, s.cond, s.next
        else:
            init, cond, step = None, s.cond, None
        cnames = set(names(cond))
        do = isinstance(s, c_ast.DoWhile)
        head = frozenset(after | cnames)
        b = head
        for _ in range(20):
            cont = head if step is None else frozenset(expr_live(step, head))
            self.jumps.append((after, cont))
            b = self.stmt(s.stmt, cont)
            self.jumps.pop()
            new = frozenset(head | b) if not do else frozenset(after | cnames | b)
            if new == head:
                break
            head = new
        top = head if not do else b
        if isinstance(init, c_ast.DeclList):
            for d in reversed(init.decls):
                top = self.stmt(d, top)
        elif init is not None:
            top = expr_live(init, top)
        return frozenset(top)

    def stmt(self, s, after):
        self.after[id(s)] = after
        if isinstance(s, c_ast.Compound):
            live = self.block(body(s), after)
        elif isinstance(s, c_ast.Decl):
            live = (set(after) - {s.name}) | set(names(s.init))
        elif isinstance(s, c_ast.If):
            t = self.stmt(s.iftrue, after)
            e = self.stmt(s.iffalse, after) if s.iffalse is not None else after
            live = t | e | set(names(s.cond))
        elif isinstance(s, c_ast.Return):
            live = set(names(s.expr))
        elif isinstance(s, c_ast.Label):
            live = self.stmt(s.stmt, after)
            self.labels[s.name] = live
        elif isinstance(s, LOOPS):
            live = self.loop(s, after)
        elif isinstance(s, c_ast.Switch):
            cont = self.jumps[-1][1] if self.jumps else after
            self.jumps.append((after, cont))
            entries = []
            nxt = after
            for c in reversed(body(s.stmt)):
                if isinstance(c, (c_ast.Case, c_ast.Default)):
                    self.after[id(c)] = nxt
                    nxt = self.block(c.stmts or [], nxt)
                    entries.append(nxt)
                    self.before[id(c)] = nxt
                else:
                    nxt = self.stmt(c, nxt)
            self.jumps.pop()
            live = set(after).union(*entries) | set(names(s.cond))
        elif isinstance(s, c_ast.Break) and self.jumps:
            live = self.jumps[-1][0]
        elif isinstance(s, c_ast.Continue) and self.jumps:
            live = self.jumps[-1][1]
        elif isinstance(s, c_ast.Goto) and s.name in self.labels:
            live = self.labels[s.name]
        elif isinstance(s, c_ast.Goto):
            live = set(after).union(*self.labels.values())
        elif isinstance(s, c_ast.EmptyStatement) or s is None:
            live = after
        else:
            live = expr_live(s, after)
        live = frozenset(live)
        self.before[id(s)] = live
        return live
