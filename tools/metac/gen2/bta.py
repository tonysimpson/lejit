"""Binding times (doc/GENERATOR_DESIGN.md "Analyses", 3), to a fixed point.

A local's binding time follows from its type: an object is dynamic, a
type, method table, function pointer or C string is meta, a PyObject ** is
a pointer, an array an array. An integer is meta unless some assignment
gives it a dynamic value (monovariant: one binding time per variable).

A parameter's: an object is dynamic; an exported function's other
parameters follow Ni's declaration of its meta (a vinfo_t * is dynamic);
a static's scalar is meta when every call in the file passes a meta
expression, else dynamic.
"""
from pycparser import c_ast

from .ctyping import strip_casts


def expr_bt(e, env, unit, meta_calls=(), length=None):
    """'meta' or 'dynamic' for expression e, its names' binding times in
    env; calls are dynamic but for names in meta_calls, and a length the
    meta loads (length(e)) is meta."""
    if e is None or isinstance(e, c_ast.Constant):
        return 'meta'
    if isinstance(e, c_ast.ID):
        if e.name in env:
            return 'dynamic' if env[e.name] in ('dynamic', None) else 'meta'
        return 'meta'
    if isinstance(e, c_ast.Cast):
        return expr_bt(e.expr, env, unit, meta_calls, length)
    if length is not None and length(e):
        return 'meta'
    if isinstance(e, c_ast.StructRef):
        b = expr_bt(e.name, env, unit, meta_calls, length)
        if b == 'meta':
            return 'meta'
        return 'meta' if e.field.name == 'ob_type' else 'dynamic'
    if isinstance(e, c_ast.FuncCall):
        name = e.name.name if isinstance(e.name, c_ast.ID) else None
        if name in meta_calls and all(expr_bt(a, env, unit, meta_calls, length) == 'meta'
                                      for a in (e.args.exprs if e.args else ())):
            return 'meta'
        return 'dynamic'
    if isinstance(e, c_ast.Typename):
        return 'meta'
    if isinstance(e, (c_ast.Assignment,)):
        return 'dynamic'
    parts = [c for _, c in e.children()]
    if not parts:
        return 'meta'
    return 'dynamic' if any(expr_bt(c, env, unit, meta_calls, length) == 'dynamic'
                            for c in parts) else 'meta'


def local_kind(unit, t):
    """The binding time a local's type gives it, or 'int' (decided by its
    assignments), or None (no rule)."""
    if isinstance(t, c_ast.ArrayDecl):
        return 'array'
    if unit.is_object(t):
        return 'dynamic'
    if unit.is_object_array_ptr(t):
        return 'pointer'
    if unit.is_meta_type(t):
        return 'meta'
    if unit.is_int(t):
        return 'int'
    if unit.is_double(t):
        return 'dynamic'
    return None


def assignments(body):
    """[(name, expression or None)] of every assignment to a plain
    variable in body: declarations' initialisers, '=', 'op=' and ++/--
    (None: the value depends on the old one and nothing else)."""
    out = []

    def walk(n):
        if isinstance(n, c_ast.Decl) and n.init is not None and n.name:
            out.append((n.name, n.init))
        elif isinstance(n, c_ast.Assignment) and isinstance(strip_casts(n.lvalue), c_ast.ID):
            out.append((strip_casts(n.lvalue).name, n.rvalue))
        for _, c in n.children():
            walk(c)
    walk(body)
    return out


def locals_bt(fn, unit, pbt, meta_calls=(), length=None):
    """{local: binding time} for function definition fn, its parameters'
    binding times pbt ({name: bt}); a local of no rule maps to None."""
    decls = {}

    def walk(n):
        if isinstance(n, c_ast.Decl) and n.name and not isinstance(n.type, c_ast.FuncDecl):
            decls.setdefault(n.name, local_kind(unit, n.type))
        for _, c in n.children():
            walk(c)
    walk(fn.body)
    env = dict(pbt)
    for name, kind in decls.items():
        if name not in env:
            env[name] = 'meta' if kind == 'int' else kind
    ints = {n for n, k in decls.items() if k == 'int' and n not in pbt}
    assigns = assignments(fn.body)
    changed = True
    while changed:
        changed = False
        for name, e in assigns:
            if name in ints and env[name] == 'meta' \
                    and expr_bt(e, env, unit, meta_calls, length) == 'dynamic':
                env[name] = 'dynamic'
                changed = True
    return {n: env[n] for n in decls if n not in pbt}
