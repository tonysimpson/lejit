"""Meta loops (doc/GENERATOR_DESIGN.md "Analyses" 3, "Budgets"): a C loop
counting a meta variable by one to a meta bound (abstract.counted_loop)
is a loop of the meta, run when Ni compiles: its body is emitted once per
turn, unrolled. Any other loop is CPython's (ResumeHere).

  bound     the lengths it reads are loaded before the loop; a bound that
            is not a loaded length (at most the unroll budget already) is
            checked against the budget there, and CPython's code runs from
            the loop beyond it
  body      walked from the loop's head state, merged with the state at
            the end of a turn until it no longer changes (a call made in
            one turn is a call before the next); the walk's record is
            taken back before each walk
  exits     the test failing, and each break: the dead variables released
            there, then the paths merged (ownership must agree)
  entry     promotions a turn needs after a call are made at the entry of
            the outermost loop, when nothing was called before it (rule 1;
            Translator re-walks to place them): a parameter's type, or the
            types of every item of a loaded object

Nothing resumes inside a meta loop: a resume there would lose the turns
still to run (KNOWN_BUGS G6), so CPython's code runs from the outermost
loop instead (resume.resume raises again until the loop is left).
"""
from pycparser import c_ast

from . import cprint, items
from .errors import Refuse, ResumeHere
from .liveness import body


class Loop:
    def __init__(self, key, entry_called, switch_depth):
        self.key, self.entry_called, self.switch_depth = key, entry_called, switch_depth
        self.breaks = []


def counted(s):
    """(variable, bound, counting up) of a counted loop, or None."""
    from .abstract import counted_loop
    r = counted_loop(s)
    if r is None:
        return None
    step = s.next
    while isinstance(step, c_ast.Cast):
        step = step.expr
    return r[0], r[1], '+' in step.op


def start_of(init, name):
    """The constant a loop's init gives its variable, or None."""
    xs = init.exprs if isinstance(init, c_ast.ExprList) else [init]
    if isinstance(init, c_ast.DeclList):
        xs = [c_ast.Assignment('=', c_ast.ID(d.name), d.init) for d in init.decls if d.init]
    found = None
    for x in xs:
        if isinstance(x, c_ast.Assignment) and x.op == '=' \
                and isinstance(x.lvalue, c_ast.ID) and x.lvalue.name == name:
            r = x.rvalue
            while isinstance(r, c_ast.Cast):
                r = r.expr
            found = r.value if isinstance(r, c_ast.Constant) and r.value.isdigit() else None
    return found


def loop(w, s, p, out, frames):
    cl = counted(s)
    if cl is None:
        raise ResumeHere('a loop with a run-time bound')
    name, bound, up = cl
    lv = p.vars.get(name)
    if lv is None or lv.bt != 'meta':
        raise ResumeHere('a loop counting the run-time %s' % name)
    key = w.loop_key(s)
    items.load_lengths(w, bound, p, out, frames)
    start = start_of(s.init, name) if s.init is not None else None
    if start is not None:
        check_budget(w, s, key, bound, start, up, p, out, frames)
    init(w, s, p, out, frames)
    if start is None:
        check_budget(w, s, key, bound, lv.cname, up, p, out, frames)
    outermost = not w.loops
    for what in w.loop_hoist.get(key, []) if outermost else ():
        if what.startswith('items:'):
            items.promote_items(w, p, out, what[len('items:'):])
        else:
            from . import actions
            actions.promote(w, p, out, what, entry=True)
    entry_called = p.called if outermost else w.loops[0].entry_called
    head = p.copy()
    snap = w.rec.snapshot()
    for turn in range(w.tr.budgets['loop_walks']):
        if turn:
            w.rec.restore(snap)
        ctx = Loop(key, entry_called, w.switch_depth)
        ctx.depth = len(frames)
        w.loops.append(ctx)
        code = out.sub()
        bp = head.copy()
        backs, exit_p = [], None
        try:
            w.sc_slots = None
            c = w.cond(s.cond, bp, code, frames)
            if c is False:
                return          # it never runs
            if c is not True:
                exit_p = bp.copy()
                ex = code.sub()
                w.release_dead(exit_p, ex, w.live.after[id(s)])
                if ex:
                    ex.line('break;')
                    code.if_('!(%s)' % c, ex)
                else:
                    code.unbraced_if('!(%s)' % c, 'break;')
            w.block(body(s.stmt), bp, code, frames)
            if not bp.ended and s.next is not None:
                w.expr_stmt(s.next, bp, code, frames)
            if not bp.ended:
                backs.append(bp)
        finally:
            w.loops.pop()
        new = head_merge(head, backs)
        if new is None:
            break
        head = new
    else:
        raise Refuse('budget loop_walks: the loop %s has no fixed point'
                     % cprint.stmt_text(s))
    out.block('for (;;)', code)
    exits = ([exit_p] if exit_p is not None else []) + ctx.breaks
    if not exits:
        p.ended = True
        return
    exits_merge(p, exits)


def init(w, s, p, out, frames):
    i = s.init
    if i is None:
        return
    if isinstance(i, c_ast.DeclList):
        for d in i.decls:
            w.stmt(d, p, out, frames)
    else:
        w.expr_stmt(i, p, out, frames)


def check_budget(w, s, key, bound, start, up, p, out, frames):
    """The loop's turns at most the unroll budget, or CPython's code from
    the loop."""
    if key in p.budgeted:
        return
    b = w.ev(bound, p, out, frames)
    if b.bt != 'meta':
        raise ResumeHere('a loop with a run-time bound')
    p.budgeted.add(key)
    if b.within and start in ('0', None) and up:
        return
    turns = (b.c if start == '0' else '%s - %s' % (b.c, start)) if up \
        else '%s - %s' % (start, b.c)
    from . import resume
    back = out.sub()
    resume.resume(w, p.copy(), back, frames, 'a loop of more than %d turns' % items.budget(w),
                  inside=True)
    out.if_('%s > %d' % (turns, items.budget(w)), back)


def break_(w, p, out):
    ctx = w.loops[-1] if w.loops else None
    if ctx is None or ctx.switch_depth != w.switch_depth:
        raise Refuse('break outside a loop of the meta')
    ctx.breaks.append(p.copy())
    out.line('break;')
    p.ended = True


def same_own(a, b):
    def norm(v):
        return 'none' if v.own == 'borrowed' and not v.isparam else v.own
    return norm(a) == norm(b)


def head_merge(head, backs):
    """The loop's head state merged with the ends of its turns; None when
    that is the head state (the fixed point)."""
    new = head.copy()
    changed = False
    for b in backs:
        if b.called and not new.called:
            new.called = changed = True
        if not new.known <= b.known:
            new.known &= b.known
            changed = True
        sl = {k: v for k, v in new.slots.items() if b.slots.get(k) == v}
        if sl != new.slots:
            new.slots, changed = sl, True
        ld = {k: v for k, v in new.loaded.items() if b.loaded.get(k) == v}
        if ld != new.loaded:
            new.loaded, changed = ld, True
        for name, v in new.vars.items():
            x = b.vars.get(name)
            if x is None:
                continue
            if v.bt != x.bt:
                raise Refuse('%s has another binding time after a turn of the loop' % name)
            if v.bt == 'dynamic' and not same_own(v, x):
                raise Refuse('%s is owned differently around the loop' % name)
            if v.bt == 'pointer' and v.base != x.base:
                raise Refuse('the pointer %s moves to another object in the loop' % name)
            for attr in ('conv', 'tp', 'rng', 'alias', 'singleton'):
                if getattr(v, attr) is not None and getattr(v, attr) != getattr(x, attr):
                    setattr(v, attr, None)
                    changed = True
            for attr in ('within', 'known_null'):
                if getattr(v, attr) and not getattr(x, attr):
                    setattr(v, attr, False)
                    changed = True
    return new if changed else None


def exits_merge(p, exits):
    first = exits[0].copy()
    for x in exits[1:]:
        for name in list(first.vars):
            a, b = first.vars[name], x.vars.get(name)
            if b is None:
                del first.vars[name]
                continue
            if a.bt == 'dynamic' and not same_own(a, b):
                raise Refuse('%s is owned differently where the loop ends' % name)
            for attr in ('conv', 'tp', 'rng', 'alias', 'singleton'):
                if getattr(a, attr) != getattr(b, attr):
                    setattr(a, attr, None)
            for attr in ('within', 'known_null'):
                setattr(a, attr, getattr(a, attr) and getattr(b, attr))
        first.called = first.called or x.called
        first.known &= x.known
        first.slots = {k: v for k, v in first.slots.items() if x.slots.get(k) == v}
        first.loaded = {k: v for k, v in first.loaded.items() if x.loaded.get(k) == v}
        first.budgeted &= x.budgeted
        first.failed = first.failed and x.failed
        first.early = first.early or x.early
    first.ended = False
    p.take(first)
