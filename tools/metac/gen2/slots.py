"""The functions filling a type's slots, read from the type's initialiser
in the family's file: a slot of one of its method tables is that table's
entry, in the order the table's struct declares its fields; a slot that is
a function-pointer field of the type itself is the type's own entry."""
from pycparser import c_ast

from .ctyping import strip_casts
from .errors import Refuse


def initialisers(ast):
    """{name: InitList} of the file-scope variables defined with one."""
    return {d.name: d.init for d in ast.ext
            if isinstance(d, c_ast.Decl) and d.name and isinstance(d.init, c_ast.InitList)}


def entry(init, k):
    """The function named by entry k of an initialiser, or None."""
    if k >= len(init.exprs):
        return None
    e = strip_casts(init.exprs[k])
    return e.name if isinstance(e, c_ast.ID) else None


def slot_functions(ast, unit, tp, slots):
    """{function: [(slot reference as C, slot)]} for type tp's slots."""
    inits = initialisers(ast)
    if tp not in inits:
        raise Refuse('no initialiser of %s in the file' % tp)
    tfields = unit.fields('_typeobject')
    names = [n for n, _ in tfields]
    out = {}
    for slot in slots:
        if slot in names and unit.is_fnptr(dict(tfields)[slot]):
            f = entry(inits[tp], names.index(slot))
            ref = '%s.%s' % (tp, slot)
        else:
            f = ref = None
            for k, (tfield, t) in enumerate(tfields):
                if not unit.is_table(t):
                    continue
                sfields = [n for n, _ in type_fields(unit, unit.pointee(t))]
                if slot not in sfields:
                    continue
                e = strip_casts(inits[tp].exprs[k]) if k < len(inits[tp].exprs) else None
                if not (isinstance(e, c_ast.UnaryOp) and e.op == '&'
                        and isinstance(e.expr, c_ast.ID) and e.expr.name in inits):
                    raise Refuse('%s.%s is not a table of the file' % (tp, tfield))
                f = entry(inits[e.expr.name], sfields.index(slot))
                ref = '%s.%s->%s' % (tp, tfield, slot)
                break
        if f is None:
            raise Refuse('%s has no function in its slot %s' % (tp, slot))
        out.setdefault(f, []).append((ref, slot))
    return out


def type_fields(unit, t):
    """[(name, type)] of the struct type t names (an anonymous one through
    its typedef)."""
    r = unit.resolve(t)
    if isinstance(r, c_ast.TypeDecl) and isinstance(r.type, c_ast.Struct):
        if r.type.decls is not None:
            return [(d.name, d.type) for d in r.type.decls]
        return unit.fields(r.type.name) or []
    return []
