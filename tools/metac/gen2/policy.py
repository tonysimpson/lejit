"""The judgement calls (doc/GENERATOR_DESIGN.md "Rules"), as typed items
keyed by the function's name. An item's C is preprocessed and parsed in
the scope of the function's file (load), and its precondition is checked
against the function's AST before translating it (check); a failure
names the item.

  Likely(cond, how[, branches])
                      the run-time branch on cond is taken (how True) or
                      not; it must match exactly `branches` (1) branches
  Restype(T)          every result is of type T (Psyco_AssertType); no
                      return path may contradict it (a parameter returned
                      unchanged must be tested exactly a T on its path)
  NotTranslation(why) Ni's meta of that name is not a translation of the
                      function; the function is not generated
  HandOnly(what)      an item only Ni's hand translation implements: the
                      function is left to it (a transitional reason)
  Whole(why)          the meta calls the function whole, with its effect
                      flags: what compiling it would gain is not worth
                      the guards it costs
  FoldMayFail(why)    when every object argument is a constant, the meta
                      calls CPython's function while compiling; an error
                      is cleared and left to the run-time code (the
                      arguments are immutable, so a respawn's replay of
                      the call is safe)
  Holds(cond, why)    the caller has made the test cond already (why): it
                      holds wherever the function, or a static it inlines,
                      tests it; it must match exactly one branch there
  IfKnown(n)          the integer parameter n is its value where Ni knows
                      it when compiling; any other value runs CPython's
                      function whole (no promotion: a count is rarely
                      worth one)

Where these are, and where a generic rule decides instead, is the design's
"Rules" table; each item here has the source's own reason as its note.
"""
from pycparser import c_ast

from .ctyping import strip_casts


class PolicyError(Exception):
    pass


class Likely:
    def __init__(self, cond, how, branches=1):
        self.text, self.how, self.expr = cond, how, None
        self.branches = branches

    def __repr__(self):
        return 'likely(%r, %s)' % (self.text, self.how)

    def check(self, fn, unit):
        n = sum(1 for c in branch_conditions(fn.body) if same(c, self.expr))
        if n != self.branches:
            return '%r matches %d branches of %s, not %d' % (self, n, fn.decl.name,
                                                             self.branches)
        return None


class Restype:
    def __init__(self, tp):
        self.tp, self.text, self.expr = tp, None, None

    def __repr__(self):
        return 'restype(%s)' % self.tp

    def check(self, fn, unit):
        params = [p.name for p in (fn.decl.type.args.params if fn.decl.type.args else ())
                  if getattr(p, 'name', None)]
        bad = unchecked_returns(fn.body, {n: 'param' for n in params}, self.tp)
        if bad:
            return '%r: %s returns its parameter %s with no test that it is a %s' % (
                self, fn.decl.name, bad, self.tp)
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


class Whole:
    def __init__(self, why):
        self.why, self.text, self.expr = why, None, None

    def __repr__(self):
        return 'whole'

    def check(self, fn, unit):
        return None


class Holds:
    def __init__(self, cond, why):
        self.text, self.why, self.expr = cond, why, None

    def __repr__(self):
        return 'holds(%r)' % self.text

    def check(self, fn, unit):
        bodies = [fn.body] + [unit.defs[n].body for n in called(fn.body)
                              if n in unit.defs and n != fn.decl.name]
        n = sum(1 for b in bodies for c in branch_conditions(b) if same(c, self.expr))
        if n != 1:
            return '%r matches %d branches of %s and its callees, not one' % (
                self, n, fn.decl.name)
        return None


class IfKnown:
    def __init__(self, name):
        self.name, self.text, self.expr = name, None, None

    def __repr__(self):
        return 'if_known(%s)' % self.name

    def check(self, fn, unit):
        params = [p for p in (fn.decl.type.args.params if fn.decl.type.args else ())
                  if getattr(p, 'name', None) == self.name]
        if not params or not unit.is_int(params[0].type):
            return '%r: %s has no integer parameter %s' % (self, fn.decl.name, self.name)
        return None


class FoldMayFail:
    def __init__(self, why):
        self.why, self.text, self.expr = why, None, None

    def __repr__(self):
        return 'fold_may_fail'

    def check(self, fn, unit):
        params = fn.decl.type.args.params if fn.decl.type.args else ()
        if not any(unit.is_object(getattr(p, 'type', None)) for p in params
                   if getattr(p, 'name', None)):
            return '%r: %s takes no object' % (self, fn.decl.name)
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
    },
}


ITEMS['long'] = {f: [Restype('PyLong_Type')] for f in (
    'long_add', 'long_sub', 'long_mul', 'long_neg', 'long_invert', 'long_long', 'long_abs')}
# CPython's long_abs negates only a negative int
ITEMS['long']['long_abs'].append(Likely('Py_SIZE(v) < 0', False))
ITEMS['long']['long_true_divide'] = [Restype('PyFloat_Type')]
ITEMS['float'] = {'float_float': [Restype('PyFloat_Type')]}
ITEMS['tuple'] = {
    'tupleitem': [Likely('i < 0 || i >= Py_SIZE(a)', False)],
    # the item is not found at each turn: the search goes on
    'tuplecontains': [Likely('cmp == 0', True)],
    # the items compared are expected equal: the loop goes on
    'tuplerichcompare': [Likely('k', True)],
    'tupleconcat': [Restype('PyTuple_Type')],
    # a repeat count is a constant where it matters ((x,) * 3)
    'tuplerepeat': [IfKnown('n'), Restype('PyTuple_Type')],
}
ITEMS['unicode'] = {
    # a legacy string (not ready) is rare
    'unicode_length': [Likely('PyUnicode_IS_READY(self)', True)],
    # strings are immutable
    'PyUnicode_Contains': [FoldMayFail('two constant strings')],
    # it builds a new string: compiled, it would test both strings' readiness
    # and types only to call CPython's copy of the rest
    'PyUnicode_Concat': [Whole('a new string built in C')],
    'PyUnicode_RichCompare': [HandOnly('promote known_only'), HandOnly('virtual result')],
}
ITEMS['structmember'] = {
    # a member holding NULL is rare
    'PyMember_GetOne': [Likely('*(char**)addr == NULL', False), Likely('v == NULL', False, branches=2)],
}
ITEMS['descr'] = {
    'member_get': [Holds('PyObject_TypeCheck(obj, descr->d_type)',
                         'pobject.c:generic_getattr calls it only on an instance of d_type')],
    'method_get': [HandOnly('virtual result')],
}

# The translator's budgets: past one, the function (or the call) is left to
# the run-time code, and the report names the budget.
BUDGETS = {
    'rewalks': 16,              # walks to move promotions before calls
    'inline_depth': 3,          # inlines inside inlined bodies
    'inline_statements': 40,    # a callee's statements, to inline it
    'loop_walks': 6,            # walks of a meta loop's body to its fixed point
    # turns of a meta loop, and items whose types are promoted, checked when
    # Ni compiles (beyond it, CPython's code from the loop or the load);
    # above CT_TUPLE_LOAD_SIZE_LIMIT, so no constant tuple Ni loads hits it
    'unroll': 16,
    'polyvariance': 5,          # counts of a builtin's arguments, each its own path
}

KINDS = (Likely, Restype, NotTranslation, HandOnly, FoldMayFail, Whole, IfKnown, Holds)


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


def called(n):
    """The names of the functions n calls directly."""
    out, stack = [], [n]
    while stack:
        x = stack.pop()
        if isinstance(x, c_ast.FuncCall) and isinstance(x.name, c_ast.ID):
            out.append(x.name.name)
        stack.extend(c for _, c in x.children())
    return out


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


def unchecked_returns(stmts, state, tp):
    """A parameter some path returns while it may still be the caller's
    object untested (state: name -> 'param', 'tested' or 'other'), or
    None."""
    if not isinstance(stmts, list):
        stmts = [stmts]
    for s in stmts:
        if s is None:
            continue
        if isinstance(s, c_ast.Return):
            e = strip_casts(s.expr) if s.expr is not None else None
            if isinstance(e, c_ast.ID) and state.get(e.name) == 'param':
                return e.name
            return None
        if isinstance(s, c_ast.Compound):
            bad = unchecked_returns(s.block_items or [], state, tp)
            if bad:
                return bad
            continue
        if isinstance(s, c_ast.If):
            st, sf = dict(state), dict(state)
            for n in state:
                if exact_test(s.cond, n, tp):
                    st[n] = 'tested'
            bad = unchecked_returns(s.iftrue, st, tp) or unchecked_returns(s.iffalse, sf, tp)
            if bad:
                return bad
            for n in state:
                if st[n] != 'param' and sf[n] != 'param':
                    state[n] = 'tested' if 'tested' in (st[n], sf[n]) else 'other'
            continue
        for n in assigned(s):
            if n in state:
                state[n] = 'other'
        for r, _ in returns(s):
            e = strip_casts(r.expr) if r.expr is not None else None
            if isinstance(e, c_ast.ID) and state.get(e.name) == 'param':
                return e.name
    return None


def assigned(n):
    out = []
    stack = [n]
    while stack:
        x = stack.pop()
        if isinstance(x, c_ast.Assignment) and isinstance(strip_casts(x.lvalue), c_ast.ID):
            out.append(strip_casts(x.lvalue).name)
        stack.extend(c for _, c in x.children())
    return out


def exact_test(c, name, tp):
    """c is 'Py_TYPE(name) == &tp' (PyX_CheckExact) as preprocessed, or
    an && of which one is."""
    c = strip_casts(c)
    if isinstance(c, c_ast.BinaryOp) and c.op == '&&':
        return exact_test(c.left, name, tp) or exact_test(c.right, name, tp)
    if not (isinstance(c, c_ast.BinaryOp) and c.op == '=='):
        return False
    a, b = strip_casts(c.left), strip_casts(c.right)
    return isinstance(a, c_ast.StructRef) and a.field.name == 'ob_type' \
        and isinstance(strip_casts(a.name), c_ast.ID) and strip_casts(a.name).name == name \
        and isinstance(b, c_ast.UnaryOp) and b.op == '&' and isinstance(b.expr, c_ast.ID) \
        and b.expr.name == tp
