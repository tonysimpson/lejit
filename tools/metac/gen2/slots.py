"""The functions filling a type's slots, read from the type's initialiser
in the family's file: a slot of one of its method tables is that table's
entry, in the order the table's struct declares its fields; a slot that is
a function-pointer field of the type itself is the type's own entry. And
a module's builtin functions, read from its PyModuleDef's method table."""
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


def flag_names(key):
    """'METH_FASTCALL | METH_KEYWORDS' -> ('METH_FASTCALL', 'METH_KEYWORDS')."""
    return tuple(sorted(w.strip() for w in key.split('|') if w.strip()))


# the ml_flags names (Include/methodobject.h), to name a function's flags
METH_NAMES = ('METH_VARARGS', 'METH_KEYWORDS', 'METH_NOARGS', 'METH_O', 'METH_CLASS',
              'METH_STATIC', 'METH_COEXIST', 'METH_FASTCALL')


def meth_names():
    import policy_table
    return sorted(set(METH_NAMES) | {n for k in policy_table.METH_ARGS for n in flag_names(k)})


def meth_text():
    """C naming the value of each ml_flags name, to be parsed after the
    file (the preprocessor knows the values)."""
    return 'enum { %s };\n' % ', '.join('__ni_meth_%s = %s' % (n, n) for n in meth_names())


def builtin_functions(ast, unit, names):
    """(module name, {C function: (Python name, flag names, METH_ARGS
    spec)}, {C function: (Python name, why not)}) for the module functions named by
    their Python names, from the file's PyModuleDef and the method table
    it names. A function whose ml_flags have no METH_ARGS entry is not
    attempted."""
    import policy_table
    from .types import const_value
    inits = initialisers(ast)
    defs = [d for d in ast.ext if isinstance(d, c_ast.Decl) and d.name in inits
            and unit.struct_name(d.type) == 'PyModuleDef']
    if len(defs) != 1:
        raise Refuse('%d PyModuleDef definitions with an initialiser' % len(defs))
    mfields = [n for n, _ in unit.fields('PyModuleDef')]
    head = inits[defs[0].name].exprs
    name = strip_casts(head[mfields.index('m_name')])
    table = strip_casts(head[mfields.index('m_methods')])
    if not (isinstance(name, c_ast.Constant) and isinstance(table, c_ast.ID)
            and table.name in inits):
        raise Refuse('the PyModuleDef names no module or no method table of the file')
    tfields = [n for n, _ in unit.fields('PyMethodDef')]
    value = {n: const_value(unit.enums['__ni_meth_' + n].value) for n in meth_names()}
    specs = []
    for key, spec in policy_table.METH_ARGS.items():
        bits = 0
        for n in flag_names(key):
            bits |= value[n]
        specs.append((bits, flag_names(key), spec))
    by_py = {}
    for e in inits[table.name].exprs:
        if not isinstance(e, c_ast.InitList) or len(e.exprs) < 3:
            continue
        py = strip_casts(e.exprs[tfields.index('ml_name')])
        fn = strip_casts(e.exprs[tfields.index('ml_meth')])
        if isinstance(py, c_ast.Constant) and isinstance(fn, c_ast.ID):
            by_py[py.value.strip('"')] = (fn.name,
                                          const_value(e.exprs[tfields.index('ml_flags')]))
    out, skipped = {}, {}
    for py in names:
        if py not in by_py:
            raise Refuse('no builtin %s in the method table %s' % (py, table.name))
        c, bits = by_py[py]
        match = [(fl, spec) for b, fl, spec in specs if b == bits]
        if not match:
            named = [n for n in sorted(value) if bits & value[n]]
            if sum(value[n] for n in named) != bits:
                named.append(str(bits & ~sum(value[n] for n in named)))
            skipped[c] = (py, 'ml_flags %s: no METH_ARGS entry' % ' | '.join(named))
            continue
        out[c] = (py, match[0][0], match[0][1])
    return name.value.strip('"'), out, skipped
