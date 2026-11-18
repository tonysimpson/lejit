"""The known-length representation (types.length_spec): an immutable array
object whose length and items Ni can know when compiling (a tuple), read
as Ni holds it.

  length      x->ob_size where the C needs it as a meta value: Ni's `load`
              (the length, or -1 where Ni doesn't know it, when CPython's
              code runs from the statement instead); a length over the
              unroll budget is not known either, so every loop over it,
              and every promotion of its items, stays within the budget.
              Only returned, or compared with a run-time integer, it is
              the field read through its descriptor instead (the constant
              where Ni knows it), with no resume
  item        x->ob_item[k] with k meta, directly or through a pointer
              local: the item's vinfo in Ni's array, borrowed; with k
              run-time, the item read at run time (psyco_get_field_array)
  pointer     a PyObject ** local into an object's items: the object's
              variable and a meta offset (pointer arithmetic is the
              offset's)
  build       a call of the type's constructor with a meta size, at most
              max_items: Ni's virtual (`new`); its items are then stored
              by the C's own statements
  store       P[k] = x or *P = x into a virtual this meta builds: the
              item's vinfo is x, which gives the virtual its reference
              (a borrowed x is increfed, and the C's next statement must
              be its own Py_INCREF of the item)
  items       the types of every item of a loaded object, promoted in a
              loop of the meta (where a call needs them after another
              call: at the entry of the loop, or refused)
  bounds test 'i < 0 || i >= n', n a length: one unsigned compare, as C
              compilers make it, with a likely item for the branch
"""
from pycparser import c_ast

from . import cprint
from .ctyping import strip_casts, text
from .errors import Refuse, ResumeHere
from .values import Val


def lengths(w):
    r = w.tr.reps.get('known_length')
    return r.spec['lengths'] if r is not None else {}


def budget(w):
    return w.tr.budgets['unroll']


def struct_name(w, t):
    pt = w.u.pointee(t)
    if pt is None:
        return None
    name = text(pt).replace('const ', '').strip()
    return name.replace('struct ', '') if name else None


def object_spec(w, x, p):
    """(variable, spec) when expression x is an object variable of a type
    with a known length (by the outermost cast the C writes that names
    one, else by the variable's type or the parameter it copies); the
    variable is the parameter where x copies one."""
    specs = lengths(w)
    if not specs:
        return None
    casts = []
    while isinstance(x, (c_ast.Cast, c_ast.ExprList)):
        if isinstance(x, c_ast.ExprList):
            # (assert(...), (T *)(op)) from 3.8's _PyTuple_CAST, under NDEBUG
            if not all(isinstance(y, c_ast.Cast) and isinstance(y.expr, c_ast.Constant)
                       for y in x.exprs[:-1]):
                return None
            x = x.exprs[-1]
            continue
        casts.append(x.to_type.type)
        x = x.expr
    if not isinstance(x, c_ast.ID) or x.name not in p.vars:
        return None
    v = p.vars[x.name]
    if v.bt != 'dynamic' or not w.u.is_object(v.ctype):
        return None
    if v.virt is not None:
        return v, v.virt
    root = v
    if not v.isparam and v.alias and v.alias in p.vars and v.own == 'borrowed':
        root = p.vars[v.alias]
        casts.append(v.ctype)
    for t in casts + [root.ctype]:
        s = specs.get(struct_name(w, t))
        if s is not None:
            return root, s
    return None


def size_read(w, e, p):
    """(variable, spec) when e reads the size field of such an object."""
    e = strip_casts(e)
    if not (isinstance(e, c_ast.StructRef) and e.type == '->'):
        return None
    r = object_spec(w, e.name, p)
    if r is None or e.field.name != r[1]['size']:
        return None
    return r


def unloaded_size(w, e, p):
    r = size_read(w, e, p)
    return r is not None and r[0].virt is None and r[0].name not in p.loaded


def length(w, e, p, out, frames, as_value=False):
    """The value of a size read e, or None if e is none: the meta length
    (loaded if need be), or with as_value and the length not loaded yet,
    the field read through its descriptor."""
    r = size_read(w, e, p)
    if r is None:
        return None
    v, s = r
    if v.virt is not None:
        return Val('meta', v.virt['n'], w.type_of(e), within=True)
    if v.name not in p.loaded:
        if as_value:
            t = w.new_tmp('vinfo_t *')
            out.line('%s = psyco_get_field(po, %s, %s);' % (t, v.cname, s['size_desc']))
            w.error_exit(p, out, '%s == NULL' % t)
            return Val('dynamic', t, w.type_of(e), owned=True)
        load(w, v, s, p, out, frames)
    return Val('meta', p.loaded[v.name], w.type_of(e), within=True)


def load(w, v, s, p, out, frames):
    from . import resume
    n = w.new_tmp('int')
    out.line('%s = %s(%s);' % (n, s['load'], v.cname))
    back = out.sub()
    resume.resume(w, p.copy(), back, frames, 'the length of %s is not known at compile time'
                  ' or is over %d' % (v.name, budget(w)), inside=True)
    out.if_('%s < 0 || %s > %d' % (n, n, budget(w)), back)
    p.loaded[v.name] = n
    w.kl_specs[v.name] = s


def load_lengths(w, e, p, out, frames):
    """Load every length expression e reads (a loop's bound, before it)."""
    for x in walk_nodes(e):
        r = size_read(w, x, p)
        if r is not None and r[0].virt is None and r[0].name not in p.loaded:
            load(w, r[0], r[1], p, out, frames)


# ---- pointers and items ----

def pointer(w, e, p, out, frames):
    """The pointer value of e (x->ob_item, a pointer local, or one of
    these plus a meta offset), or None."""
    x = strip_casts(e)
    if isinstance(x, c_ast.StructRef) and x.type == '->':
        r = object_spec(w, x.name, p)
        if r is not None and x.field.name == r[1]['items']:
            return Val('pointer', '0', None, base=r[0].name)
        return None
    if isinstance(x, c_ast.ID) and x.name in p.vars and p.vars[x.name].bt == 'pointer':
        v = p.vars[x.name]
        if v.base is None:
            raise Refuse('the pointer %s read before it is set' % x.name)
        return Val('pointer', v.cname, None, base=v.base)
    if isinstance(x, c_ast.BinaryOp) and x.op == '+':
        a = pointer(w, x.left, p, out, frames)
        if a is None:
            return None
        k = w.ev(x.right, p, out, frames)
        if k.bt != 'meta':
            raise ResumeHere('a pointer moved by the run-time %s' % cprint.expr(x.right))
        return Val('pointer', '(%s + %s)' % (a.c, k.c), None, base=a.base)
    return None


def slot(w, e, p, out, frames):
    """(the item's C lvalue, its object variable) of e, an item P[k], *P
    or x->ob_item[k] with k meta; None if e is none, or k is run-time."""
    e = strip_casts(e)
    if isinstance(e, c_ast.ArrayRef):
        a = pointer(w, e.name, p, out, frames)
        if a is None:
            return None
        k = w.ev(e.subscript, p, out, frames)
        if k.bt != 'meta':
            return None
        off = k.c if a.c == '0' else '%s + %s' % (paren(a.c), paren(k.c))
    elif isinstance(e, c_ast.UnaryOp) and e.op == '*':
        a = pointer(w, e.expr, p, out, frames)
        if a is None:
            return None
        off = a.c
    else:
        return None
    base = p.vars[a.base]
    if base.virt is not None:
        acc = base.virt['item']
    else:
        if base.name not in p.loaded:
            raise ResumeHere('the items of %s, whose length is not loaded' % base.name)
        acc = w.kl_specs[base.name]['item']
    return '%s(%s, %s)' % (acc, base.cname, off), base


def read(w, e, p, out, frames):
    """An item's value, or None if e reads none."""
    x = strip_casts(e)
    r = slot(w, x, p, out, frames)
    if r is not None:
        lv, base = r
        v = Val('dynamic', lv, w.type_of(x))
        v.item_of = base.name if base.virt is None else None
        return v
    if isinstance(x, c_ast.ArrayRef) and isinstance(strip_casts(x.name), c_ast.StructRef):
        f = strip_casts(x.name)
        o = object_spec(w, f.name, p) if f.type == '->' else None
        if o is not None and f.field.name == o[1]['items'] and o[0].virt is None:
            k = w.ev(x.subscript, p, out, frames)
            if k.bt == 'dynamic' and w.u.is_int(k.ctype):
                t = w.new_tmp('vinfo_t *')
                out.line('%s = psyco_get_field_array(po, %s, FPYREF(%s), %s);'
                         % (t, o[0].cname, o[1]['items_desc'], k.c))
                w.error_exit(p, out, '%s == NULL' % t)
                return Val('dynamic', t, w.type_of(x), owned=True)
    return None


def store(w, lv, rhs, p, out, frames):
    """lv = rhs into an item of a virtual this meta builds; False if lv is
    no item."""
    r = slot(w, lv, p, out, frames)
    if r is None:
        return False
    acc, base = r
    if base.virt is None:
        raise ResumeHere('a store into %s, an item of a run-time object' % cprint.expr(lv))
    val = w.ev(rhs, p, out, frames)
    if val.bt != 'dynamic' or not w.u.is_object(val.ctype):
        raise ResumeHere('a store of %s into a virtual' % cprint.expr(rhs))
    out.line('%s = %s;' % (acc, val.c))
    if val.owned and val.var is None:
        return True                     # the virtual takes the call's reference
    if val.var is not None and val.var.own == 'new':
        val.var.own = 'borrowed'        # its reference is the virtual's now
        val.var.alias = None
        return True
    if val.var is not None and val.var.own == 'mixed':
        raise Refuse('%s stored while owned on some paths only' % val.var.name)
    out.line('vinfo_incref(%s);' % acc)
    w.pending_store = strip_casts(lv)
    return True


def increfs_store(w, x):
    """Py_INCREF(x) of the item the previous statement stored a borrowed
    reference into: its incref was made with the store."""
    from .policy import same
    if w.pending_store is not None and same(x, w.pending_store):
        w.pending_store = None
        return True
    return False


def construct(w, name, args, p, out, frames):
    """A constructor of a type with a known length: its virtual; else None."""
    s = next((s for s in lengths(w).values() if s['constructor'] == name), None)
    if s is None:
        return None
    if len(args) != 1:
        raise Refuse('%s with %d arguments' % (name, len(args)))
    size = w.ev(args[0], p, out, frames)
    if size.bt != 'meta':
        raise ResumeHere('%s of a size not known at compile time' % name)
    from . import resume
    # the C's Py_ssize_t up to the test: an int would wrap a huge
    # size into the limit; a negative one is CPython's error to raise
    n = w.new_tmp('Py_ssize_t')
    out.line('%s = %s;' % (n, size.c))
    if not size.c.isdigit():
        back = out.sub()
        resume.resume(w, p.copy(), back, frames, 'a %s of more than %s items'
                      % (s['type'], s['max_items']), inside=True)
        out.if_('%s < 0 || %s > %s' % (n, n, s['max_items']), back)
    t = w.new_tmp('vinfo_t *')
    out.line('%s = %s(%s, NULL);' % (t, s['new'], n))
    v = Val('dynamic', t, w.u.funcs[name].type if name in w.u.funcs else None, owned=True)
    v.made = s['type']
    v.virt = dict(s, n=n)
    return v


def promote_items(w, p, out, name):
    """Psyco_NeedType of every item of the loaded object `name`."""
    v = p.vars[name]
    if name not in p.loaded:
        raise Refuse('the types of the items of %s are needed before its length' % name)
    j = w.new_tmp('int')
    n = p.loaded[name]
    body = out.sub()
    w.error_exit(p, body, 'Psyco_NeedType(po, %s(%s, %s)) == NULL'
                 % (w.kl_specs[name]['item'], v.cname, j))
    out.block('for (%s = 0; %s < %s; %s++)' % (j, j, n, j), body)
    p.known.add('items:' + name)


def bounds_test(w, e, p, out, frames):
    """'x < 0 || x >= n', x a run-time integer variable and n a length:
    the meta's C truth of one unsigned compare; None for anything else."""
    from . import actions
    x = strip_casts(e)
    if not lengths(w) or not (isinstance(x, c_ast.BinaryOp) and x.op == '||'):
        return None
    lo, hi = strip_casts(x.left), strip_casts(x.right)
    if not (isinstance(lo, c_ast.BinaryOp) and lo.op == '<' and cprint.is_null(lo.right)
            and isinstance(hi, c_ast.BinaryOp) and hi.op == '>='
            and isinstance(strip_casts(lo.left), c_ast.ID)
            and isinstance(strip_casts(hi.left), c_ast.ID)
            and strip_casts(lo.left).name == strip_casts(hi.left).name):
        return None
    iv = p.vars.get(strip_casts(lo.left).name)
    if iv is None or iv.bt != 'dynamic' or not w.u.is_int(iv.ctype):
        return None
    if size_read(w, hi.right, p) is None:
        return None
    n = length(w, hi.right, p, out, frames, as_value=True)
    item = next((i for i in w.likely if actions.same(i.expr, x)), None)
    if item is None:
        if not p.called:
            raise ResumeHere("no 'likely' item for the run-time branch %s" % cprint.expr(x))
        raise Refuse("no 'likely' item for the run-time branch %s" % cprint.expr(x))
    cc = w.new_tmp('condition_code_t')
    out.line('%s = integer_cmp%s(po, %s, %s, Py_GE|COMPARE_UNSIGNED);'
             % (cc, '_i' if n.bt == 'meta' else '', iv.cname, n.c))
    if n.bt == 'dynamic':
        out.line('vinfo_decref(%s, po);' % n.c)
    w.error_exit(p, out, '%s == CC_ERROR' % cc)
    return actions.likely_branch(w, item, cc, out)


def paren(c):
    from .walk import paren as pa
    return pa(c)


def walk_nodes(n):
    yield n
    for _, c in n.children():
        yield from walk_nodes(c)
