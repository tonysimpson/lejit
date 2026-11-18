"""The actions (doc/GENERATOR_DESIGN.md "Actions"), each in one place: what
the meta does for a construct the walk has classified. The resume action
is resume.py's.

  fold          a C expression of meta values, computed when compiling
  map           a call of a CPython function Ni has its own meta of
  residual      psyco_generic_call of CPython's function (or a static's copy)
  meta call     a call of another function of the family, translated
  guard         a type's slot or mro read through Ni (a heap type's may change)
  field read    an object's field through Ni's descriptor (TYPES 'descriptors')
  promote       an object parameter's type, by Psyco_NeedType
  raise         PyErr_Format and PyErr_SetString, at compile time
Inline, scalar helper, build virtual and restart have no use in this
family; a construct needing one is refused by the walk.
"""
from pycparser import c_ast

from . import conventions, cprint
from .ctyping import INT, VOID_P, strip_casts, text
from .errors import Refuse, ResumeHere
from .types import SINGLETONS
from .values import Conv, Val


# ---- promote ----

def promote(w, p, out, name, entry=False):
    """The meta variable holding the type of object variable `name`."""
    v = p.vars.get(name)
    tpv = 'tp_' + name
    if name in p.known:
        return tpv
    if v is not None and not v.isparam and v.alias and v.own == 'borrowed' and not entry:
        return promote(w, p, out, v.alias)      # a copy of a parameter
    if v is not None and not v.isparam and v.bt == 'dynamic' and not entry:
        return known_type(w, p, out, v)
    if v is None or not v.isparam:
        raise ResumeHere('the type of %s, which is not a parameter' % name)
    pidx = [n for n, _ in w.params].index(name)
    w.rec.needs.add(pidx)
    if not entry:
        if p.called:
            w.late(p, name)
        elif out.depth == 1 and not p.early and name not in w.rec.must:
            w.rec.must.append(name)
    w.rec.decls[tpv] = 'PyTypeObject *%s;' % tpv
    out.line('%s = Psyco_NeedType(po, %s);' % (tpv, v.cname))
    w.error_exit(p, out, '%s == NULL' % tpv)
    p.known.add(name)
    return tpv


def known_type(w, p, out, v):
    """The type of a value that is not a parameter: the one Ni knows (a
    virtual's, a constant's), else CPython resumes from the statement."""
    from . import resume
    tpv = 'tp_' + v.name
    if w.rec.ncalls > w.stmt_ncalls:
        raise ResumeHere('the type of %s, which is not a parameter' % v.name)
    w.rec.decls[tpv] = 'PyTypeObject *%s;' % tpv
    out.line('%s = Psyco_KnownType(%s);' % (tpv, v.cname))
    sub = out.sub()
    resume.resume(w, p.copy(), sub, w.stmt_frames, 'the type of %s is not known' % v.name,
                  inside=True)
    out.if_('%s == NULL' % tpv, sub)
    p.known.add(v.name)
    return tpv


# ---- guard ----

def guard(w, p, out, frames, read, why):
    """switch (read) { case 1: on; case 0: resume; default: the error }"""
    from . import resume
    stay = out.sub()
    stay.line('break;')
    back = out.sub()
    resume.resume(w, p.copy(), back, frames, why, inside=True)
    fail = out.sub()
    w.release_all(p, fail)
    fail.line('return %s;' % w.err())
    out.switch(read, [('case 1', stay), ('case 0', back), ('default', fail)])


def slot_guard(w, p, out, frames, tpv, addr, ftype):
    """A slot of a type, read through Psyco_TypeSlot."""
    if addr in p.slots:
        return Val('meta', p.slots[addr], ftype)
    n = w.new_tmp(text(ftype))
    guard(w, p, out, frames, 'Psyco_TypeSlot(po, %s, %s, (void **)&%s)' % (tpv, addr, n),
          'a slot of a heap type changed')
    p.slots[addr] = n
    return Val('meta', n, ftype)


def subtype(w, p, out, frames, args):
    a = w.ev(args[0], p, out, frames)
    b = w.ev(args[1], p, out, frames)
    if a.bt != 'meta' or b.bt != 'meta':
        raise ResumeHere('PyType_IsSubtype of run-time types')
    r = w.new_tmp('bool')
    guard(w, p, out, frames, 'Psyco_TypeIsSubtype(po, %s, %s, &%s)' % (a.c, b.c, r),
          'the mro of a heap type changed')
    return Val('meta', r, INT)


# ---- field reads ----

def struct_of(w, t):
    """The TYPES struct a pointer type points to (its typedef or tag), or
    None."""
    from .types import descriptors
    pt = w.u.pointee(t)
    if pt is None:
        return None
    tag = w.u.struct_name(pt)
    for name in descriptors():
        named = c_ast.TypeDecl(None, [], None, c_ast.IdentifierType([name]))
        if name in (text(pt).replace('const ', '').strip(), tag) \
                or (tag is not None and w.u.struct_name(named) == tag):
            return name
    return None


def structs_of(w, x, b):
    """The TYPES structs the object x is read as, outermost cast first,
    then its variable's own type."""
    out = []
    while isinstance(x, c_ast.Cast):
        out.append(struct_of(w, x.to_type.type))
        x = x.expr
    out.append(struct_of(w, b.var.ctype))
    return out


def field_read(w, e, b, p, out):
    """x->f of an object, f a field TYPES gives Ni's descriptor of: the
    struct is the outermost the C casts x to that TYPES names, else x's
    own (a cast to PyVarObject or PyObject, which every object begins
    with, says nothing). None if no rule."""
    from .types import descriptors
    if b.bt != 'dynamic' or b.var is None:
        return None
    field = e.field.name
    for st in structs_of(w, e.name, b):
        d = descriptors().get(st, {}).get(field) if st else None
        if d is None:
            continue
        t = w.new_tmp('vinfo_t *')
        out.line('%s = psyco_get_field(po, %s, %s);' % (t, b.var.cname, d))
        w.error_exit(p, out, '%s == NULL' % t)
        ftype = w.type_of(e)
        return Val('dynamic', t, ftype, owned=True)
    return None


# ---- fold-may-fail ----

def fold_may_fail(w, p, out):
    """The function called while compiling when its object parameters
    are all constants (policy FoldMayFail); its C int result, if no
    error, is the meta's constant."""
    if w.sm.kind != 'val' or not w.u.is_int(w.fn.decl.type.type):
        raise Refuse('fold_may_fail of a function not returning a C int')
    if 'static' in w.fn.decl.storage:
        raise Refuse('fold_may_fail of a static function, which Ni cannot call')
    objs = [n for n, t in w.params if w.u.is_object(t)]
    args = []
    for n, t in w.params:
        v = p.vars[n]
        if v.bt != 'dynamic':
            args.append(v.cname)
        elif w.u.is_object(t):
            args.append('(PyObject *)CompileTime_Get(%s->source)->value' % v.cname)
        else:
            raise Refuse('fold_may_fail with the run-time integer %s' % n)
    sub = out.sub()
    sub.line('%s r = %s(%s);' % (text(w.fn.decl.type.type), w.name, ', '.join(args)))
    sub.unbraced_if('r >= 0', 'return vinfo_new(CompileTime_New(r));')
    sub.line('PyErr_Clear(); /* raised again at run time */')
    out.if_(' && '.join('is_compiletime(%s->source)' % p.vars[n].cname for n in objs), sub)


# ---- raise ----

def raise_(w, p, out, frames, name, args):
    vals = [w.ev(a, p, out, frames) for a in args]
    if any(v.bt != 'meta' for v in vals):
        raise ResumeHere('%s with a run-time argument' % name)
    fn = 'PycException_SetFormat' if name == 'PyErr_Format' else 'PycException_SetString'
    out.line('%s(po, %s);' % (fn, ', '.join(v.c for v in vals)))
    p.failed = True
    return Val('meta', 'NULL', VOID_P)


# ---- reference counts (the vocabulary's refs) ----

def refcount(w, p, out, fn, args, frames):
    """An incref or decref statement; True if it was one."""
    from . import vocabulary
    entry = vocabulary.of_marker(fn)
    if entry is None or not entry.refs:
        return False
    if entry.kind == 'writes_arg':
        raise ResumeHere('%s' % entry.name)
    refs = entry.refs[0]
    x = strip_casts(args[0])
    if refs in ('+1', 'x+1'):
        if singleton_of(x) is not None:
            return True     # Ni's constant for it is made where it is used
        from . import items
        if items.increfs_store(w, x):
            return True
        nxt = next_statement(frames)
        if not isinstance(x, c_ast.ID) and isinstance(nxt, c_ast.Return) \
                and nxt.expr is not None and same(strip_casts(nxt.expr), x):
            w.incref_return = x     # 'Py_INCREF(x); return x;'
            return True
        v = var_of(w, p, x)
        if refs == 'x+1' and v.known_null:
            return True
        if v.own != 'borrowed':
            raise Refuse('an incref of %s, which the meta owns' % v.name)
        out.line('vinfo_incref(%s);' % v.cname)
        v.own = 'new'
        return True
    if refs in ('-1', 'x-1'):
        v = var_of(w, p, x)
        if refs == 'x-1' and v.own == 'none':
            return True     # NULL on this path: nothing to release
        if not v.owned():
            raise Refuse('a decref of %s, which the meta does not own' % v.name)
        w.release(v, out)
        v.own = 'none'
        return True
    return False


def var_of(w, p, x):
    if isinstance(x, c_ast.ID) and x.name in p.vars and p.vars[x.name].bt == 'dynamic':
        return p.vars[x.name]
    raise ResumeHere('a reference count of %s' % cprint.expr(x))


def singleton_of(x):
    x = strip_casts(x)
    if isinstance(x, c_ast.UnaryOp) and x.op == '&' and isinstance(x.expr, c_ast.ID) \
            and x.expr.name in SINGLETONS:
        return x.expr.name
    return None


def const_return(w, p, out, struct):
    if w.sm.kind != 'obj':
        raise Refuse('returns an object from a non-object function')
    if struct == 'NotImplemented' or SINGLETONS[struct] == 'psyco_vi_NotImplemented':
        w.rec.notimpl = True
    w.release_all(p, out)
    out.line('return %s();' % SINGLETONS[struct])
    p.ended = True
    w.rec.returns.append('value')


def next_statement(frames):
    stmts, i = frames[-1]
    return stmts[i + 1] if i + 1 < len(stmts) else None


def same(a, b):
    from .policy import same as s
    return s(a, b)


# ---- run-time branches ----

def runtime_compare(w, p, out, e, op, va, vb, facts_t, facts_f, value=False):
    """A compare of a run-time integer: a condition code, and the branch
    the policy says is likely (P05, P06); as a value (the C's 0 or 1, not
    a branch), the condition's vinfo."""
    cc = w.new_tmp('condition_code_t')
    ct = vb.bt == 'meta'
    inverted = False
    if ct and vb.c == '0' and op in ('!=', '=='):
        call, inverted = 'integer_non_null(po, %s)' % va.c, op == '=='
    elif ct:
        call = 'integer_cmp_i(po, %s, %s, %s)' % (va.c, vb.c, COMPARE[op])
    else:
        call = 'integer_cmp(po, %s, %s, %s)' % (va.c, vb.c, COMPARE[op])
    out.line('%s = %s;' % (cc, call))
    for v in (va, vb):
        if v.bt == 'dynamic' and v.owned and v.var is None:
            out.line('vinfo_decref(%s, po);' % v.c)     # a call's result, compared
    w.error_exit(p, out, '%s == CC_ERROR' % cc)
    if inverted:
        out.line('%s = INVERT_CC(%s);' % (cc, cc))
    if value:
        r = w.new_tmp('vinfo_t *')
        out.line('%s = psyco_vinfo_condition(po, %s);' % (r, cc))
        return Val('dynamic', r, INT, owned=True)
    # (the truth of an integer x is 'x != 0': an item may name x)
    item = next((i for i in w.likely if same(i.expr, e) or (
        op == '!=' and vb.c == '0' and same(i.expr, e.left))), None)
    if item is None:
        if not p.called:
            # nothing run yet: CPython's code runs whole, its branches its own
            raise ResumeHere("no 'likely' item for the run-time branch %s" % cprint.expr(e))
        raise Refuse("no 'likely' item for the run-time branch %s" % cprint.expr(e))
    t = likely_branch(w, item, cc, out)
    if ct and vb.c == '0' and va.var is not None and facts_f is not None:
        if op == '<':
            facts_f.append('assert_nonneg(%s);' % va.c)
        elif op == '>=':
            facts_t.append('assert_nonneg(%s);' % va.c)
    return t


def likely_branch(w, item, cc, out):
    """The meta's C truth of condition code cc, the branch item favours
    assumed (the other is compiled when it is taken)."""
    if item.text not in w.rec.used_likely:
        w.rec.used_likely.append(item.text)
    t = w.new_tmp('int')
    out.line('%s = runtime_condition_%s(po, %s);' % (t, 't' if item.how else 'f', cc))
    w.chosen[t] = item.how
    return t


def bit_test(w, e, p, out, frames):
    """X->F.B as a test, B a one-bit field of F, where LAYOUTS lists F's
    bits and TYPES a descriptor of F: F read through it, and B, the top
    bit of the bytes Ni reads, tested as 'F >= B's mask', unsigned. None
    if e is no such read."""
    import policy_table
    from .types import descriptors
    if not (isinstance(e, c_ast.StructRef) and e.type == '.'
            and isinstance(e.name, c_ast.StructRef) and e.name.type == '->'):
        return None
    b = w.ev(e.name.name, p, out, frames)
    if b.bt != 'dynamic' or b.var is None:
        return None
    field, bit = e.name.field.name, e.field.name
    for st in structs_of(w, e.name.name, b):
        lay = policy_table.LAYOUTS.get(st or '', {})
        bits = lay.get('bits', {}).get(field)
        desc = descriptors().get(st or '', {}).get(field)
        if bits and desc:
            break
    else:
        return None
    read = {n: r for n, _, r in lay.get('fields', ())}.get(field)
    pos = 0
    for spec in bits:
        name, width = spec.split(':')
        if name == bit:
            break
        pos += int(width)
    else:
        raise ResumeHere('no bit %s in %s.%s' % (bit, st, field))
    if read is None or int(width) != 1 or pos + 1 != int(read.split('/')[0][1:]):
        raise ResumeHere('the bit-field %s.%s is not the top bit Ni reads' % (field, bit))
    item = next((i for i in w.likely if same(i.expr, e)), None)
    if item is None:
        if not p.called:
            raise ResumeHere("no 'likely' item for the run-time test %s" % cprint.expr(e))
        raise Refuse("no 'likely' item for the run-time test %s" % cprint.expr(e))
    fv = w.new_tmp('vinfo_t *')
    out.line('%s = psyco_get_field(po, %s, %s);' % (fv, b.var.cname, desc))
    w.error_exit(p, out, '%s == NULL' % fv)
    cc = w.new_tmp('condition_code_t')
    out.line('%s = integer_cmp_i(po, %s, NI_BIT_%s__%s__%s, Py_GE | COMPARE_UNSIGNED);'
             % (cc, fv, st, field, bit))
    out.line('vinfo_decref(%s, po);' % fv)
    w.error_exit(p, out, '%s == CC_ERROR' % cc)
    return likely_branch(w, item, cc, out)


COMPARE = {'<': 'Py_LT', '<=': 'Py_LE', '==': 'Py_EQ', '!=': 'Py_NE', '>': 'Py_GT',
           '>=': 'Py_GE'}


def integer_add(w, p, out, va, vb):
    r = w.new_tmp('vinfo_t *')
    out.line('%s = integer_add(po, %s, %s, false);' % (r, va.c, vb.c))
    return Val('dynamic', r, va.ctype, owned=True, conv=Conv(check_now=True))


# ---- calls ----

def call(w, e, p, out, frames, dest):
    """A call: the action by what is called (the design's dispatch order)."""
    tr = w.tr
    args = e.args.exprs if e.args else []
    f = strip_casts(e.name)
    if isinstance(f, c_ast.ID):
        name = f.name
        if name in p.vars:
            v = p.vars[name]
            if v.bt == 'meta' and w.u.is_fnptr(v.ctype):
                return slot_call(w, Val('meta', v.cname, v.ctype), args, p, out, frames, dest)
            raise ResumeHere('a call through %s' % name)
        if name in ('PyErr_Format', 'PyErr_SetString'):
            return raise_(w, p, out, frames, name, args)
        if name == 'PyType_IsSubtype':
            return subtype(w, p, out, frames, args)
        if name in tr.metas:
            return meta_call(w, name, args, p, out, frames, dest)
        from . import reps
        r = reps.construct(w, name, args, p, out, frames)
        if r is not None:
            return r
        from . import items
        r = items.construct(w, name, args, p, out, frames)
        if r is not None:
            return r
        pname = conventions.psyco_name(name)
        if pname in tr.ni and pname not in tr.declared:
            return ni_call(w, tr.ni[pname], name, args, p, out, frames, dest)
        if name in tr.funcs:
            g = tr.funcs[name]
            if 'static' in g.decl.storage:
                cname = tr.copies.whole(name)
                return residual(w, name, args, p, out, frames, dest, cname=cname,
                                rtype=g.decl.type.type)
            return residual(w, name, args, p, out, frames, dest)
        if name in tr.pure:
            vals = [w.ev(a, p, out, frames) for a in args]
            if all(v.bt == 'meta' for v in vals):
                return Val('meta', '%s(%s)' % (name, ', '.join(v.c for v in vals)), INT)
            raise ResumeHere('%s of a run-time value' % name)
        if name in w.u.funcs:
            return residual(w, name, args, p, out, frames, dest)
        raise ResumeHere('a call of %s' % name)
    callee = w.ev(f, p, out, frames)
    if callee.bt == 'meta' and w.u.is_fnptr(callee.ctype):
        return slot_call(w, callee, args, p, out, frames, dest)
    raise ResumeHere('a call through %s' % cprint.expr(e.name))


def args_of(w, args, p, out, frames):
    vals = []
    for a in args:
        v = w.ev(a, p, out, frames)
        if v.bt == 'gen':
            v = Val('meta', '1' if v.const else '0', INT)
        if v.bt == 'call':
            raise ResumeHere('a call as an argument')
        if v.bt == 'dynamic' and v.owned and not held_scalar(w, v):
            raise ResumeHere('a call as an argument')
        vals.append(v)
    return vals


def held_scalar(w, v):
    """The result of a call that cannot fail and returns a C scalar may be
    another call's argument; it is released after that call."""
    return v.conv is not None and v.conv.never and not w.u.is_object(v.ctype)


def release_held(w, vals, out):
    for v in vals:
        if v.bt == 'dynamic' and v.owned and held_scalar(w, v):
            out.line('vinfo_decref(%s, po);' % v.c)


def fmt(vals):
    f, a = '', []
    for v in vals:
        if v.bt == 'dynamic':
            f += 'v'
            a.append(v.c)
        else:
            f += 'l'
            a.append('(long)(%s)' % v.c if v.c != 'NULL' else '0')
    return f, a


def no_error_pending(p):
    """A run-time call on a path where an error is pending (raised now,
    or a call's error test held) would run with the exception set."""
    if p.failed:
        raise Refuse('a run-time call where an error is pending')


def note_call(w, p):
    w.rec.ncalls += 1
    p.called = True
    p.slots = {}
    w.rec.any_call = True


def slot_call(w, callee, args, p, out, frames, dest):
    no_error_pending(p)
    vals = args_of(w, args, p, out, frames)
    rtype = w.u.fn_type(callee.ctype).type
    conv, checkable = convention(w, dest, frames, rtype)
    flags = conventions.flags(w.u, rtype, conv)
    f, a = fmt(vals)
    if len(vals) > 3:
        raise Refuse('a slot call with %d arguments' % len(vals))
    note_call(w, p)
    c = 'Psyco_META%d(po, %s, %s, "%s"%s)' % (len(vals), callee.c, flags, f,
                                            ''.join(', ' + x for x in a))
    if conv == 'zero_ok':
        return Val('call', c, INT, conv=Conv('zero_ok'))
    r = w.new_tmp('vinfo_t *')
    out.line('%s = %s;' % (r, c))
    return Val('dynamic', r, rtype, owned=True,
               conv=Conv(conv, checkable=checkable, notimpl=conv == 'notimpl'))


def meta_call(w, name, args, p, out, frames, dest):
    tr = w.tr
    g = tr.sums[name]
    gparams = g.params
    vals = []
    for k, a in enumerate(args):
        v = w.ev(a, p, out, frames)
        if v.bt == 'gen':
            v = Val('meta', '1' if v.const else '0', INT)
        want = g.pbt[k]
        if want == 'meta' and v.bt != 'meta':
            raise Refuse('%s: argument %d is run-time' % (name, k + 1))
        if want == 'dynamic' and v.bt != 'dynamic':
            raise ResumeHere('%s: a compile-time argument %d' % (name, k + 1))
        if v.bt == 'dynamic' and v.owned:
            raise ResumeHere('a call as an argument')
        vals.append(v)
    # the callee reads these parameters' types
    for k in sorted(g.needs, key=lambda k: (g.entry.index(gparams[k])
                                            if gparams[k] in g.entry else 99, k)):
        v = vals[k]
        if v.item_of is not None:
            if 'items:' + v.item_of not in p.known and p.called:
                w.late(p, 'items:' + v.item_of)
        elif v.var is not None and v.var.isparam and v.var.name not in p.known:
            if p.called:
                w.late(p, v.var.name)
            elif gparams[k] in g.entry and out.depth == 1 and not p.early \
                    and v.var.name not in w.rec.must:
                w.rec.must.append(v.var.name)
        elif v.var is not None and not v.var.isparam and w.u.is_object(v.ctype):
            raise Refuse('%s reads the type of %s, which is not a parameter'
                         % (name, v.var.name))
    for k, v in enumerate(vals):
        if k in g.needs and gparams[k] in g.entry and v.var is not None:
            p.known.add(v.var.name)
    if g.may_call:
        no_error_pending(p)
        note_call(w, p)
    c = '%s(po%s)' % (g.meta, ''.join(', ' + v.c for v in vals))
    if g.kind == 'ok':
        return Val('call', c, INT, conv=Conv('ok'))
    if g.kind == 'ct':
        r = w.new_tmp('int')
        out.line('%s = %s;' % (r, c))
        w.error_exit(p, out, '%s == -1' % r)
        return Val('meta', r, INT)
    if g.always_error:
        p.failed = True
    if dest is None and g.always_error:
        out.line('%s;' % c)
        return Val('meta', 'NULL', VOID_P)
    r = w.new_tmp('vinfo_t *')
    out.line('%s = %s;' % (r, c))
    return Val('dynamic', r, g.ret, owned=True,
               conv=Conv('null', checkable=True, notimpl=g.notimpl,
                         always_error=g.always_error))


def ni_call(w, decl, name, args, p, out, frames, dest=None):
    if len(decl.params) != len(args) + 1:
        raise ResumeHere('%s has %d parameters' % (decl.name, len(decl.params)))
    no_error_pending(p)
    vals = []
    for (ptype, pname), a in zip(decl.params[1:], args):
        v = w.ev(a, p, out, frames)
        if v.bt == 'gen':
            v = Val('meta', '1' if v.const else '0', INT)
        if ptype == 'vinfo_t *' and v.bt != 'dynamic':
            raise ResumeHere('%s: a compile-time argument' % decl.name)
        if ptype != 'vinfo_t *' and v.bt != 'meta':
            raise ResumeHere('%s: a run-time argument' % decl.name)
        if v.bt == 'dynamic' and v.owned and not held_scalar(w, v):
            raise ResumeHere('a call as an argument')
        if v.bt == 'dynamic' and v.item_of is not None:
            if 'items:' + v.item_of not in p.known and p.called:
                w.late(p, 'items:' + v.item_of)
        elif v.bt == 'dynamic' and w.u.is_object(v.ctype):
            pv = v.var
            if pv is not None and not pv.isparam and pv.alias and pv.own == 'borrowed':
                pv = p.vars.get(pv.alias, pv)       # a copy of a parameter
            if pv is None or not pv.isparam:
                raise ResumeHere('%s of a value that is not a parameter' % decl.name)
            if pv.name not in p.known and p.called:
                w.late(p, pv.name)
            if pv is not v.var:
                v = Val('dynamic', pv.cname, v.ctype, var=pv, conv=v.conv)
        vals.append(v)
    note_call(w, p)
    c = '%s(po%s)' % (decl.name, ''.join(', ' + v.c for v in vals))
    held = any(v.bt == 'dynamic' and v.owned for v in vals)
    if decl.ret == 'bool' and not held:
        return Val('call', c, INT, conv=Conv('ok'))
    if decl.ret != 'vinfo_t *':
        raise ResumeHere('%s returns %s' % (decl.name, decl.ret))
    r = w.new_tmp('vinfo_t *')
    out.line('%s = %s;' % (r, c))
    release_held(w, vals, out)
    ctype = w.u.funcs[name].type if name in w.u.funcs else None
    if dest not in (None, '<return>') and ctype is not None and w.u.is_int(ctype) \
            and not error_tested(w, dest, frames):
        # the meta's NULL is the C's -1: tested now, as nothing else would
        return Val('dynamic', r, ctype, owned=True, conv=Conv('null', check_now=True))
    return Val('dynamic', r, ctype, owned=True, conv=Conv('null', checkable=True))


def error_tested(w, dest, frames):
    """The next statement reading dest (inside the meta loop being walked,
    whose test or step may read it first) tests it for the C's error."""
    scope = frames[w.loops[-1].depth:] if w.loops else frames
    nxt = next_use(dest, scope)
    if not isinstance(nxt, c_ast.If):
        return False
    k = test_kind(strip_casts(nxt.cond), dest)
    return k is not None and k[0] in ('neg', 'minus1')


def residual(w, name, args, p, out, frames, dest, cname=None, rtype=None):
    no_error_pending(p)
    vals = args_of(w, args, p, out, frames)
    if rtype is None:
        rtype = w.u.funcs[name].type
    conv, checkable = convention(w, dest, frames, rtype)
    key = w.tr.effects_key(name)
    if dest is None and cname is None and not w.u.is_object(rtype) \
            and w.tr.cannot_raise(key):
        conv = 'never'      # its result is an argument
    try:
        flags = conventions.flags(w.u, rtype, conv, w.tr.effect_flags(key))
    except conventions.NoConvention as e:
        raise Refuse(str(e))
    f, a = fmt(vals)
    note_call(w, p)
    c = 'psyco_generic_call(po, %s, %s, "%s"%s)' % (cname or name, flags, f,
                                                     ''.join(', ' + x for x in a))
    if conv == 'zero_ok':
        return Val('call', c, INT, conv=Conv('zero_ok'))
    r = w.new_tmp('vinfo_t *')
    out.line('%s = %s;' % (r, c))
    release_held(w, vals, out)
    return Val('dynamic', r, rtype, owned=True,
               conv=Conv(conv, checkable=checkable, notimpl=conv == 'notimpl',
                         never=conv == 'never'))


# ---- how the C tests a call's result ----

def convention(w, dest, frames, ctype):
    """(convention, checkable): from the next statement that reads the
    result, or from what the function returns."""
    if dest == '<return>':
        kind = w.sm.kind
        if kind == 'ok':
            return 'zero_ok', False
        if kind == 'val':
            return 'neg', False
        if w.u.is_object(ctype):
            return 'null', False
        raise Refuse('returns a call of type %s' % text(ctype))
    if dest is None:
        return ('null' if w.u.is_object(ctype) else 'neg'), False
    nxt = next_use(dest, frames)
    if nxt is None:
        raise Refuse('the result in %s is never read' % dest)
    if isinstance(nxt, c_ast.If):
        c = strip_casts(nxt.cond)
        r = test_kind(c, dest)
        if r is not None:
            return r
    if isinstance(nxt, c_ast.Return) and nxt.expr is not None:
        r = strip_casts(nxt.expr)
        if isinstance(r, c_ast.ID) and r.name == dest:
            return convention(w, '<return>', frames, ctype)
    raise Refuse('cannot tell how the C checks %s for an error (%s)'
                 % (dest, cprint.expr(nxt.cond) if isinstance(nxt, c_ast.If)
                    else type(nxt).__name__))


def test_kind(c, dest):
    def is_dest(x):
        x = strip_casts(x)
        return isinstance(x, c_ast.ID) and x.name == dest

    def null_like(x):
        if isinstance(x, c_ast.UnaryOp) and x.op == '!' and is_dest(x.expr):
            return True
        return isinstance(x, c_ast.BinaryOp) and x.op in ('==', '!=') and is_dest(x.left) \
            and cprint.is_null(x.right) and not minus_one(x.right)
    if isinstance(c, c_ast.BinaryOp) and c.op == '<' and is_dest(c.left) \
            and isinstance(strip_casts(c.right), c_ast.Constant) \
            and strip_casts(c.right).value == '0':
        return 'neg', True
    if isinstance(c, c_ast.BinaryOp) and c.op == '&&':
        x = strip_casts(c.left)
        if isinstance(x, c_ast.BinaryOp) and x.op == '==' and is_dest(x.left) \
                and minus_one(x.right) and is_err_occurred(c.right):
            return 'minus1', True
    if null_like(c):
        return 'null', True
    if isinstance(c, c_ast.BinaryOp) and c.op == '||' and null_like(strip_casts(c.left)):
        return 'null', True
    if isinstance(c, c_ast.BinaryOp) and c.op in ('==', '!=') and is_dest(c.left) \
            and singleton_of(c.right) == '_Py_NotImplementedStruct':
        return 'notimpl', False
    return None


def minus_one(e):
    e = strip_casts(e)
    return isinstance(e, c_ast.UnaryOp) and e.op == '-' and isinstance(e.expr, c_ast.Constant) \
        and e.expr.value == '1'


def is_err_occurred(e):
    e = strip_casts(e)
    return isinstance(e, c_ast.FuncCall) and isinstance(e.name, c_ast.ID) \
        and e.name.name == 'PyErr_Occurred'


def next_use(name, frames):
    """The first statement after the current one that reads name (an
    assert under NDEBUG is no statement)."""
    from .liveness import names
    for stmts, i in reversed(frames):
        for s in stmts[i + 1:]:
            if isinstance(s, c_ast.Break):
                break
            if name in names(s):
                return s
    return None
