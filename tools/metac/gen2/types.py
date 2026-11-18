"""Types as Ni represents them (doc/GENERATOR_DESIGN.md "Rules"), as much
as the families moved so far need.

SINGLETONS: CPython's immortal objects, by the struct the preprocessed C
takes the address of, with Ni's function giving a new reference to its
constant vinfo. Their identity is known when compiling, and a decref of
one runs no Python.

REPRESENTATIONS: an object held as a C scalar, as policy_table.TYPES'
'word' and 'double' sections say (policy.py checks their names). The C
texts are read as the family's file reads them (its macros expanded) and
matched against its code structurally, casts ignored; x stands for any
variable.
  word    an int that is one machine word where `test` holds: `load`
          (Ni's) gives the word, 1, or 0 where the int has more digits
          (CPython's code runs from there instead), -1 on an error;
          `value` is the word, in [-bound, bound]; `sign` compared with 0
          is the word compared with 0 (`sign_load`: a borrowed vinfo); a
          `constructors` call given a word is `new`, which takes the
          word's reference.
  double  a float holds a C double in `field`: a read of it is `load` (a
          borrowed vinfo), a `constructors` call given a double is `new`,
          which takes the reference (the math unit's scalars.py reads the
          section too).
"""
import collections

from pycparser import c_ast

from .ctyping import strip_casts

SINGLETONS = {
    '_Py_NoneStruct': 'psyco_vi_None',
    '_Py_TrueStruct': 'psyco_vi_Py_True',
    '_Py_FalseStruct': 'psyco_vi_Py_False',
    '_Py_NotImplementedStruct': 'psyco_vi_NotImplemented',
}
NOT_IMPLEMENTED = '_Py_NotImplementedStruct'
# the names the source writes them by, for messages
SINGLETON_NAMES = {'_Py_NoneStruct': 'Py_None', '_Py_TrueStruct': 'Py_True',
                   '_Py_FalseStruct': 'Py_False',
                   '_Py_NotImplementedStruct': 'Py_NotImplemented'}

def word_spec():
    import policy_table
    t = policy_table.TYPES['Include/longintrepr.h:_longobject']
    d = t['word']
    return {'type': t['type'], 'struct': 'struct _longobject', 'test': d['test'],
            'value': d['value'], 'sign': d['sign'], 'bound': d['bound'],
            'load': d['load'].split(':')[1], 'sign_load': d['sign_load'].split(':')[1],
            'new': d['new'].split(':')[1],
            'constructors': tuple(c.split(':')[1] for c in d['constructors'])}


def double_spec():
    import policy_table
    t = policy_table.TYPES['Include/floatobject.h:PyFloatObject']
    d = t['double']
    return {'type': t['type'], 'struct': 'PyFloatObject', 'field': d['field'],
            'load': d['load'].split(':')[1], 'new': d['new'].split(':')[1],
            'constructors': tuple(c.split(':')[1] for c in d['constructors'])}


Rep = collections.namedtuple('Rep', 'name spec exprs bound')
TEXTS = ('test', 'value', 'sign', 'bound')


def spec(name):
    return word_spec() if name == 'word' else double_spec()


def rule_text(names):
    """C defining one function per representation, its texts as
    statements, to be parsed after the family's file."""
    out = []
    for n in names:
        s = spec(n)
        texts = [s[k] for k in TEXTS if k in s]
        if texts:
            out.append('void __ni_rep_%s(%s *x) { %s }\n' % (
                n, s['struct'], ' '.join('(%s);' % t for t in texts)))
    return ''.join(out)


def load(ast, names):
    """{name: Rep} of the representations, their texts parsed (in ast,
    parsed with rule_text(names) after the file)."""
    bodies = {d.decl.name[len('__ni_rep_'):]: d.body.block_items for d in ast.ext
              if isinstance(d, c_ast.FuncDef) and d.decl.name.startswith('__ni_rep_')}
    out = {}
    for n in names:
        s = spec(n)
        keys = [k for k in TEXTS if k in s]
        exprs = dict(zip(keys, bodies.get(n, ())))
        bound = const_value(exprs['bound']) if 'bound' in exprs else None
        out[n] = Rep(n, s, exprs, bound)
    return out


def const_value(e):
    """The value of an integer constant expression (casts ignored), or
    None."""
    e = strip_casts(e)
    if isinstance(e, c_ast.Constant) and e.type in ('int', 'long', 'unsigned int',
                                                     'unsigned long'):
        return int(e.value.rstrip('uUlL'), 0)
    if isinstance(e, c_ast.UnaryOp) and e.op == '-':
        v = const_value(e.expr)
        return None if v is None else -v
    if isinstance(e, c_ast.BinaryOp):
        a, b = const_value(e.left), const_value(e.right)
        if a is None or b is None:
            return None
        ops = {'+': lambda: a + b, '-': lambda: a - b, '*': lambda: a * b,
               '<<': lambda: a << b, '>>': lambda: a >> b, '|': lambda: a | b,
               '&': lambda: a & b}
        return ops[e.op]() if e.op in ops else None
    return None


def bind(pattern, e, env=None):
    """The variable x stands for when expression e has the pattern's
    shape (casts ignored), else None."""
    env = {} if env is None else env
    return env.get('x') if _unify(pattern, e, env) else None


def _unify(p, e, env):
    p, e = strip_casts(p), strip_casts(e)
    if isinstance(p, c_ast.ID) and p.name == 'x':
        if not isinstance(e, c_ast.ID):
            return False
        return env.setdefault('x', e.name) == e.name
    if type(p) is not type(e):
        return False
    for attr in p.attr_names:
        if attr != 'coord' and getattr(p, attr) != getattr(e, attr):
            return False
    cp, ce = p.children(), e.children()
    return len(cp) == len(ce) and all(_unify(a[1], b[1], env) for a, b in zip(cp, ce))
