"""The representations' actions (types.py REPRESENTATIONS): the C read as
what Ni holds for an int or a float.

  guard       the word test: the meta asks Ni for the word (case 1), and
              CPython's code runs from the statement where the int has
              more digits (case 0)
  value       the word, borrowed from the variable that holds it ('w_X'),
              with its range; released at its last read
  arithmetic  + - * and unary - on words and integer constants, and casts
              between integer types, where interval arithmetic proves that
              the C can't overflow its types (signed overflow is undefined
              in C, so the C itself must not): Ni's integer operations,
              with no overflow check
  sign        the sign field compared with 0: Ni's sign vinfo, compared
  load        a float's double field: Ni's vinfo of it, as a new reference
  new         a constructor given a word or a double: Ni's virtual, which
              takes the reference

C's integer types are LP64's (the header asserts the widths it relies on).
"""
from pycparser import c_ast

from . import cprint
from .ctyping import LONG, strip_casts, text
from .errors import ResumeHere
from .types import bind
from .values import Val, Var

WORD_BITS = 64
INT_BITS = {'char': 8, 'short': 16, 'int': 32, 'long': 64}
WORD_OPS = {'+': 'integer_add', '-': 'integer_sub', '*': 'integer_mul'}
DOUBLE = c_ast.TypeDecl(None, [], None, c_ast.IdentifierType(['double']))


def fits(lo, hi, bits, signed=True):
    if signed:
        return -2 ** (bits - 1) <= lo and hi < 2 ** (bits - 1)
    return 0 <= lo and hi < 2 ** bits


def int_type(u, t):
    """(bits, signed) of an integer C type through typedefs, or None."""
    t = u.resolve(t)
    if not (isinstance(t, c_ast.TypeDecl) and isinstance(t.type, c_ast.IdentifierType)):
        return None
    names = t.type.names
    if 'double' in names or 'float' in names:
        return None
    signed = 'unsigned' not in names and names != ['_Bool']
    if names == ['_Bool']:
        return 8, False
    bits = 32
    for n in ('char', 'short', 'long'):
        if n in names:
            bits = INT_BITS[n]
    if not set(names) & {'char', 'short', 'int', 'long', 'signed', 'unsigned'}:
        return None
    return bits, signed


def reps_of(w, name):
    return w.tr.reps.get(name)


def object_var(w, p, x):
    v = p.vars.get(x) if x is not None else None
    if v is not None and v.bt == 'dynamic' and w.u.is_object(v.ctype):
        return v
    return None


# ---- the word: its guard, its value, arithmetic on it ----

def test(w, e, p, out, frames):
    """True where e is the word test of an object variable (the guard
    made), else None."""
    r = reps_of(w, 'word')
    if r is None:
        return None
    x = bind(r.exprs['test'], e)
    xv = object_var(w, p, x)
    if xv is None:
        return None
    wname = '$w_' + x
    if wname in p.vars and p.vars[wname].owned():
        return True
    from . import resume
    cname = 'w_' + x
    w.rec.decls[cname] = 'vinfo_t *%s;' % cname
    w.rec.reps.add('word')
    stay = out.sub()
    stay.line('break;')
    back = out.sub()
    resume.resume(w, p.copy(), back, frames, '%s is not one word (%s)'
                  % (x, r.spec['test']), inside=True)
    fail = out.sub()
    w.release_all(p, fail)
    fail.line(w.fail_line(p))
    out.switch('%s(po, %s, &%s)' % (r.spec['load'], xv.cname, cname),
               [('case 1', stay), ('case 0', back), ('default', fail)])
    v = Var(wname, LONG, 'dynamic', cname)
    v.own = 'new'
    v.word_of = x
    v.rng = (-r.bound, r.bound)
    p.vars[wname] = v
    return True


def value(w, e, p, out, frames):
    """The word or a float's double when e reads one, else None."""
    r = reps_of(w, 'word')
    if r is not None:
        x = bind(r.exprs['value'], e)
        if object_var(w, p, x) is not None:
            wv = p.vars.get('$w_' + x)
            if wv is None or not wv.owned():
                raise ResumeHere('%s read where %s has no word' % (r.spec['value'], x))
            uses = word_uses(w, r, x)
            uses[0] += 1
            return Val('dynamic', wv.cname, LONG, var=wv, rng=wv.rng,
                       last=uses[0] == uses[1] and x not in after_statement(w))
    d = reps_of(w, 'double')
    if d is not None:
        xv = double_field(w, d, e, p)
        if xv is not None:
            return load(w, d, xv, p, out)
    return None


def double_field(w, d, e, p):
    """The object variable whose double field e reads, or None."""
    e = strip_casts(e)
    if isinstance(e, c_ast.StructRef) and e.type == '->' and e.field.name == d.spec['field']:
        b = strip_casts(e.name)
        if isinstance(b, c_ast.ID):
            return object_var(w, p, b.name)
    return None


def load(w, d, xv, p, out):
    t = w.new_tmp('vinfo_t *')
    out.line('%s = %s(po, %s);' % (t, d.spec['load'], xv.cname))
    w.error_exit(p, out, '%s == NULL' % t)
    out.line('vinfo_incref(%s);' % t)
    return Val('dynamic', t, DOUBLE, owned=True)


def after_statement(w):
    """The names live after the statement being translated."""
    stmts, i = w.stmt_frames[-1]
    s = stmts[i]
    if isinstance(s, c_ast.Return):
        return frozenset()
    return w.live.after.get(id(s), frozenset())


def word_uses(w, r, x):
    """[reads so far, reads in all] of x's word in the statement being
    translated."""
    stmts, i = w.stmt_frames[-1]
    s = stmts[i]
    key = (s, x)
    if key not in w.word_counts:
        own = s.cond if isinstance(s, c_ast.If) else s
        w.word_counts[key] = [0, reads(r.exprs['value'], own, x)]
    return w.word_counts[key]


def reads(pattern, n, x):
    """How many times n reads the pattern of x (a cast of a read is the
    read)."""
    if not isinstance(n, c_ast.Cast) and bind(pattern, n) == x:
        return 1
    return sum(reads(pattern, c, x) for _, c in n.children())


def nodes(n):
    yield n
    for _, c in n.children():
        yield from nodes(c)


def ranged(w, e, p):
    """Does e read a word or a variable holding a range?"""
    r = reps_of(w, 'word')
    if r is None:
        return False
    for n in nodes(e):
        if isinstance(n, c_ast.ID) and n.name in p.vars and p.vars[n.name].rng is not None:
            return True
        x = bind(r.exprs['value'], n)
        if x is not None and object_var(w, p, x) is not None:
            return True
    return False


def operand(w, e, p, out, frames):
    v = w.ev(e, p, out, frames)
    if v.bt == 'meta':
        from .types import const_value
        n = const_value(e)
        if n is None:
            raise ResumeHere('arithmetic with %s, not a constant' % cprint.expr(e))
        return Val('meta', str(n), v.ctype, rng=(n, n))
    if v.bt == 'dynamic' and v.rng is not None:
        return v
    raise ResumeHere('arithmetic on %s, which has no known range' % cprint.expr(e))


def arith(w, e, p, out, frames):
    """Word arithmetic for e (a BinaryOp, unary minus or Cast), or None
    when it reads no word."""
    if not ranged(w, e, p):
        return None
    if isinstance(e, c_ast.Cast):
        bs = int_type(w.u, e.to_type.type)
        a = operand(w, e.expr, p, out, frames)
        if bs is None:
            raise ResumeHere('a cast of a word to %s' % text(e.to_type))
        if not fits(a.rng[0], a.rng[1], *bs):
            raise ResumeHere('a cast to %s may truncate' % text(e.to_type))
        return Val(a.bt, a.c, e.to_type.type, owned=a.owned, var=a.var, rng=a.rng,
                   last=a.last)
    t = w.type_of(e)
    parts = [e.expr] if isinstance(e, c_ast.UnaryOp) else [e.left, e.right]
    bits = 32
    for x in parts:
        xt = w.type_of(x)
        bs = int_type(w.u, xt) if xt is not None else None
        if bs is None or not bs[1]:
            raise ResumeHere('arithmetic on %s, not of a signed integer type'
                             % cprint.expr(x))
        bits = max(bits, bs[0])
    if isinstance(e, c_ast.UnaryOp):
        a = operand(w, e.expr, p, out, frames)
        if a.bt != 'dynamic':
            raise ResumeHere('a constant expression')
        lo, hi = -a.rng[1], -a.rng[0]
        call = 'integer_neg(po, %s, false)' % a.c
        vals = [a]
    else:
        op = e.op
        if op not in WORD_OPS:
            raise ResumeHere('the operator %s on a word' % op)
        a = operand(w, e.left, p, out, frames)
        b = operand(w, e.right, p, out, frames)
        if op == '*':
            c = [x * y for x in a.rng for y in b.rng]
            lo, hi = min(c), max(c)
        elif op == '+':
            lo, hi = a.rng[0] + b.rng[0], a.rng[1] + b.rng[1]
        else:
            lo, hi = a.rng[0] - b.rng[1], a.rng[1] - b.rng[0]
        if a.bt == 'meta' and b.bt == 'meta':
            raise ResumeHere('a constant expression')
        if b.bt == 'meta' and op in '+-':
            call = 'integer_add_i(po, %s, %d, false)' % (a.c, b.rng[0] if op == '+'
                                                         else -b.rng[0])
        elif a.bt == 'meta' and op == '+':
            call = 'integer_add_i(po, %s, %d, false)' % (b.c, a.rng[0])
        elif 'meta' in (a.bt, b.bt):
            raise ResumeHere('%s with a constant' % op)
        else:
            call = '%s(po, %s, %s, false)' % (WORD_OPS[op], a.c, b.c)
        vals = [a, b]
    if not fits(lo, hi, bits):
        raise ResumeHere('%s may overflow its %d-bit C type' % (cprint.expr(e), bits))
    out.line('/* in [%d, %d]: no overflow */' % (lo, hi))
    r = w.new_tmp('vinfo_t *')
    out.line('%s = %s;' % (r, call))
    consumed(w, vals, out)
    w.error_exit(p, out, '%s == NULL' % r)
    return Val('dynamic', r, t if t is not None else LONG, owned=True, rng=(lo, hi))


def consumed(w, vals, out):
    """Release what an operation used up: temporaries, and words at their
    last read."""
    for v in vals:
        if v.bt != 'dynamic':
            continue
        if v.owned and v.var is None:
            out.line('vinfo_decref(%s, po);' % v.c)
        elif v.last and v.var is not None and v.var.word_of and v.var.own == 'new':
            w.release(v.var, out)
            v.var.own = 'none'


# ---- the sign ----

def sign(w, e, p, out):
    """Ni's sign vinfo (borrowed) when e is the sign field of an int, else
    None."""
    r = reps_of(w, 'word')
    if r is None or 'sign' not in r.exprs:
        return None
    x = bind(r.exprs['sign'], e)
    xv = object_var(w, p, x)
    if xv is None:
        return None
    w.rec.reps.add('word')
    t = w.new_tmp('vinfo_t *')
    out.line('%s = %s(po, %s);' % (t, r.spec['sign_load'], xv.cname))
    w.error_exit(p, out, '%s == NULL' % t)
    return Val('dynamic', t, LONG)


def is_zero(e):
    e = strip_casts(e)
    return isinstance(e, c_ast.Constant) and e.value == '0'


# ---- constructors ----

def construct(w, name, args, p, out, frames):
    """A constructor given a word or a double: Ni's virtual; else None."""
    if len(args) != 1:
        return None
    for rname in ('word', 'double'):
        r = reps_of(w, rname)
        if r is None or name not in r.spec['constructors']:
            continue
        if rname == 'word' and not ranged(w, args[0], p):
            continue
        a = w.ev(args[0], p, out, frames)
        if a.bt != 'dynamic':
            continue
        if rname == 'word' and (a.rng is None or not fits(a.rng[0], a.rng[1], WORD_BITS)):
            raise ResumeHere('%s of a value that may not fit a word' % name)
        if rname == 'double' and not w.u.is_double(a.ctype):
            continue
        if rname == 'word':
            w.rec.reps.add('word')
        if a.owned and a.var is None:
            pass                                # the virtual takes it
        elif a.last and a.var is not None and a.var.own == 'new':
            a.var.own = 'none'                  # its last read: taken
        else:
            out.line('vinfo_incref(%s);' % a.c)
        t = w.new_tmp('vinfo_t *')
        out.line('%s = %s(%s);' % (t, r.spec['new'], a.c))
        v = Val('dynamic', t, w.u.funcs[name].type if name in w.u.funcs else None,
                owned=True)
        v.made = r.spec['type']
        return v
    return None
