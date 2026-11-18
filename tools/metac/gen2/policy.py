"""The judgement calls (doc/GENERATOR_DESIGN.md "Rules"), as typed items
keyed by the function's name. An item's C is preprocessed and parsed in
the scope of the function's file (load), and its precondition is checked
against the function's AST before translating it (check); a failure
names the item.

  Likely(cond, how)   the run-time branch on cond is taken (how True) or
                      not; it must match exactly one run-time branch
  Restype(T)          every result is of type T (Psyco_AssertType); no
                      return path may contradict it
  NotTranslation(why) Ni's meta of that name is not a translation of the
                      function; the function is not generated
  HandOnly(what)      an item only Ni's hand translation implements: the
                      function is left to it (a transitional reason)

Where these are, and where a generic rule decides instead, is the design's
"Rules" table; each item here has the source's own reason as its note.
"""
from pycparser import c_ast

from .ctyping import strip_casts


class PolicyError(Exception):
    pass


class Likely:
    def __init__(self, cond, how):
        self.text, self.how, self.expr = cond, how, None

    def __repr__(self):
        return 'likely(%r, %s)' % (self.text, self.how)

    def check(self, fn, unit):
        n = sum(1 for c in branch_conditions(fn.body) if same(c, self.expr))
        if n != 1:
            return '%r matches %d branches of %s, not one' % (self, n, fn.decl.name)
        return None


class Restype:
    def __init__(self, tp):
        self.tp, self.text, self.expr = tp, None, None

    def __repr__(self):
        return 'restype(%s)' % self.tp

    def check(self, fn, unit):
        params = {p.name for p in (fn.decl.type.args.params if fn.decl.type.args else ())}
        for ret, tests in returns(fn.body):
            e = strip_casts(ret.expr) if ret.expr is not None else None
            if isinstance(e, c_ast.ID) and e.name in params and \
                    not any(exact_test(t, e.name, self.tp) for t in tests):
                return '%r: %s returns its parameter %s with no test that it is a %s' % (
                    self, fn.decl.name, e.name, self.tp)
        return None


class NotTranslation:
    def __init__(self, why):
        self.why, self.text, self.expr = why, None, None

    def __repr__(self):
        return 'not_translation'

    def check(self, fn, unit):
        return None


class HandOnly:
    def __init__(self, what):
        self.what, self.text, self.expr = what, None, None

    def __repr__(self):
        return 'hand_only(%s)' % self.what

    def check(self, fn, unit):
        return None


ITEMS = {
    'abstract': {
        'PySequence_GetItem': [Likely('i < 0', False)],
        'PySequence_SetItem': [Likely('i < 0', False)],
        'PySequence_DelItem': [Likely('i < 0', False)],
        'PySequence_Tuple': [Restype('PyTuple_Type')],
        'PySequence_List': [Restype('PyList_Type')],
        'PyIter_Next': [NotTranslation("FOR_ITER's call: StopIteration is left set")],
        'PyNumber_Power': [HandOnly('shortcut type_whitelist')],
        'PyObject_GetIter': [HandOnly('shortcut skip_check'), HandOnly('virtual result')],
        'PyNumber_AsSsize_t': [HandOnly('shortcut const_fold'), HandOnly('shortcut one_digit')],
    },
}


KINDS = (Likely, Restype, NotTranslation, HandOnly)


def load(target, family, items=None, extra=''):
    """{function: [items]} for the family (or `items`), each item's C
    parsed in the scope of the family's file (and `extra`, C after it)."""
    import front
    items = ITEMS.get(family.name, {}) if items is None else items
    for name, its in items.items():
        for i in its:
            if not isinstance(i, KINDS):
                raise PolicyError('%s: unknown policy item %r' % (name, i))
    texts = sorted({i.text for its in items.values() for i in its if i.text})
    if texts:
        extra += ''.join('void __ni_rule_%d(void) { (%s); }\n' % (k, t)
                         for k, t in enumerate(texts))
        try:
            ast = front.parse(target, family.cfile, extra)
        except Exception as e:  # noqa: BLE001 - the item must be named
            raise PolicyError('policy items of %s do not parse: %s' % (family.name, e))
        exprs = {}
        for d in ast.ext:
            if isinstance(d, c_ast.FuncDef) and d.decl.name.startswith('__ni_rule_'):
                k = int(d.decl.name[len('__ni_rule_'):])
                exprs[texts[k]] = d.body.block_items[0]
        for its in items.values():
            for i in its:
                if i.text:
                    i.expr = exprs[i.text]
    return items


def same(a, b):
    """Structural equality of two expressions, casts ignored."""
    a, b = strip_casts(a), strip_casts(b)
    if type(a) is not type(b):
        return False
    for attr in a.attr_names:
        if getattr(a, attr) != getattr(b, attr) and attr != 'coord':
            return False
    ca, cb = a.children(), b.children()
    return len(ca) == len(cb) and all(same(x[1], y[1]) for x, y in zip(ca, cb))


def branch_conditions(n):
    """Every condition a branch tests, inside n: if, ?:, loops, and the
    operands of && and || there."""
    out = []

    def conds(c):
        out.append(c)
        c = strip_casts(c)
        if isinstance(c, c_ast.BinaryOp) and c.op in ('&&', '||'):
            conds(c.left)
            conds(c.right)
        elif isinstance(c, c_ast.UnaryOp) and c.op == '!':
            conds(c.expr)

    def walk(x):
        if isinstance(x, (c_ast.If, c_ast.TernaryOp, c_ast.While, c_ast.DoWhile)) \
                and x.cond is not None:
            conds(x.cond)
        elif isinstance(x, c_ast.For) and x.cond is not None:
            conds(x.cond)
        for _, c in x.children():
            walk(c)
    walk(n)
    return out


def returns(n, tests=()):
    """[(return statement, [conditions that hold there])] inside n."""
    out = []
    if isinstance(n, c_ast.Return):
        out.append((n, list(tests)))
    elif isinstance(n, c_ast.If):
        out += returns(n.iftrue, tests + (n.cond,))
        if n.iffalse is not None:
            out += returns(n.iffalse, tests)
    else:
        for _, c in n.children():
            out += returns(c, tests)
    return out


def exact_test(c, name, tp):
    """c is 'Py_TYPE(name) == &tp' (PyX_CheckExact) as preprocessed."""
    c = strip_casts(c)
    if not (isinstance(c, c_ast.BinaryOp) and c.op == '=='):
        return False
    a, b = strip_casts(c.left), strip_casts(c.right)
    return isinstance(a, c_ast.StructRef) and a.field.name == 'ob_type' \
        and isinstance(strip_casts(a.name), c_ast.ID) and strip_casts(a.name).name == name \
        and isinstance(b, c_ast.UnaryOp) and b.op == '&' and isinstance(b.expr, c_ast.ID) \
        and b.expr.name == tp
