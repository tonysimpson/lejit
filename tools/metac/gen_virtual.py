#!/usr/bin/env python3
"""Virtual objects' support, generated from CPython's struct declarations,
its constructors and policy_table.TYPES.

Input: each TYPES entry whose 'virtual' names a 'constructor' (a CPython
function) and whose fields are all immutable. A mutable type (a list) keeps
its hand-written support: what a virtual of it may become is more than its
constructor's arguments. From the constructor's body:
  - a fixed type: each statement 'X->F = P;' (header macros expanded)
    stores the parameter P in the field F; 'if (P == NULL) P = Py_None;'
    gives P the default None. Every object parameter must reach a field.
    The virtual's slots are those fields, in the struct's order;
  - an array type (a 'known_length' entry: the size and items fields): the
    constructor's one parameter must be stored in the size field, and the
    C's setter macro (constructor prefix + _SET_ITEM) must store into the
    items field: PATTERNS E's 'PyT_New(n)' + 'PyT_SET_ITEM'.
What is written:
  - virtual_fields_gen.h, included by pobject.h: the field descriptors
    (CPY_FIELD chained in the struct's order from OB_type, or FIX_size /
    VAR_size before an array; FARRAY for the items), their FIELD_INDEX and
    FIELDS_TOTAL, named after the type (PySlice_Type: SLICE_start,
    iSLICE_START, SLICE_TOTAL);
  - one <file>_virtual_gen.h per type, included by the Ni file holding its
    source_virtual_t ('ni'): the virtual constructor ('new': the C's
    arguments as vinfos, a NULL one its default), the force function
    (compute_*: every slot compile-time, the object is built now and is a
    constant, rule allocator_pure; else CPython's constructor at run time,
    with the effect rules' flags, then the items stored), the direct form
    (direct_compute_*, the same constructor on raw values) and
    gen_virtual_<name>_init() registering them with TYPES' weights.
What stays Ni's (representation, in TYPES or in the core): which types are
virtual, their weights, the item limit ('max_items') the translator uses,
'forced': 'drop_fields' (the slots dropped once the object is real, so
nothing reads them as its fields), and Ni-only slots (none here).

Run by generate.py, which writes every header build() returns.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cparse  # noqa: E402
import cpython_src  # noqa: E402
import cslots  # noqa: E402
import ctokens  # noqa: E402
import policy_table  # noqa: E402

FIELDS_HEADER = 'virtual_fields_gen.h'


class VType:
    """One virtual type as the generator reads it."""

    def __init__(self, key, entry):
        self.key = key
        self.path, _, self.struct = key.partition(':')
        self.entry = entry
        v = entry['virtual']
        self.type = entry['type']                       # PySlice_Type
        self.name = self.type[2:-len('_Type')]          # Slice
        self.prefix = self.name.upper()                 # SLICE
        self.lower = self.name.lower()
        self.var = v['ni'].rpartition(':')[2]           # psyco_computed_slice
        self.ni_file = v['ni'].partition(':')[0]
        self.cpath, _, self.ctor = v['constructor'].partition(':')
        self.new = v['new'].rpartition(':')[2]
        self.weights = (str(v['weight']), str(v['weight_at_calls']))
        self.forced = v.get('forced')
        kl = entry.get('known_length')
        self.array = kl is not None
        self.size = kl and kl['size']
        self.items = kl and kl['items']
        self.slots = []         # fixed: [(field, param)] in the struct's order
        self.params = []        # the constructor's parameter names
        self.defaults = {}      # param -> 'None'

    def header(self):
        return os.path.splitext(os.path.basename(self.ni_file))[0] + '_virtual_gen.h'

    def field(self, f):
        return '%s_%s' % (self.prefix, f)

    def index(self, f):
        return 'i%s_%s' % (self.prefix, f.upper())


def virtual_types():
    """The TYPES entries the generator writes support for: a constructor
    named, every field immutable."""
    out = []
    for key, e in policy_table.TYPES.items():
        v = e.get('virtual')
        if not v or 'constructor' not in v or e.get('mutable'):
            continue
        out.append(VType(key, e))
    return out


def constructors():
    """{CPython constructor: Ni's virtual constructor} for every TYPES entry
    whose virtual names one, generated or not (a list's is Ni's): the rule
    that a call of one builds a virtual (gen_ceval.py, gen_abstract.py)."""
    out = {}
    for e in policy_table.TYPES.values():
        v = e.get('virtual') or {}
        if 'constructor' in v:
            out[v['constructor'].rpartition(':')[2]] = v['new'].rpartition(':')[2]
    return out


def headers():
    return [FIELDS_HEADER] + [t.header() for t in virtual_types()]


def stores_through_call(source, f, param, field, effects, depth=0, outer=None):
    """Whether f, with the file's and the headers' macros expanded, stores
    its parameter 'param' in 'field' ('-> field ... = param'), itself or in
    a function it passes it to (found by effects.py's call graph, read the
    same way): PyTuple_New's size reaches ob_size through
    PyObject_GC_NewVar's PyObject_INIT_VAR. A function defined in a header
    (3.8's static inline _PyObject_INIT_VAR) is read with the macros of
    the file that calls it as well (outer): a header alone doesn't include
    the headers before it (objimpl.h's Py_SIZE is object.h's)."""
    import gen_abstract
    macros = dict(outer or {})
    macros.update(source.macros)
    macros.update(source.local_macros)
    for st in _statements(f.toks):
        toks = cparse.expand(st, macros, set())
        w = [t.text for t in toks]
        for i in range(len(w) - 1):
            if w[i] == '->' and w[i + 1] == field:
                rest = [x for x in w[i + 2:] if x not in ('(', ')')]
                if rest[:2] == ['=', param]:
                    return True
        if depth >= 3 or effects is None:
            continue
        for i, x in enumerate(w):
            if i + 1 >= len(w) or w[i + 1] != '(' or toks[i].kind != 'id':
                continue
            key = effects.globals.get(x)
            if key is None or effects.nodes[key].kind != 'function':
                continue
            close = ctokens.match_close(toks, i + 1)
            args = [[t.text for t in a if t.text not in ('(', ')')]
                    for a in ctokens.split_top(toks[i + 2:close], ',')]
            if [param] not in args:
                continue
            path = key.partition(':')[0]
            try:
                other = gen_abstract.Source(source.src, source.pyconfig,
                                            source.defines, path)
            except cparse.ParseError:
                continue
            g = other.funcs.get(x)
            if g is None or len(g.params) != len(args):
                continue
            if stores_through_call(other, g, g.params[args.index([param])][1],
                                   field, effects, depth + 1,
                                   macros if path.startswith('Include/') else None):
                return True
    return False


def _statements(toks):
    """The body's simple statements as token lists (split at ';', '{', '}')."""
    out, cur = [], []
    for t in toks:
        if t.text in ('{', '}'):
            cur = []
            continue
        if t.text == ';':
            out.append(cur)
            cur = []
            continue
        cur.append(t)
    return out


def read_constructor(source, vt, effects=None):
    """Fill vt's slots, params and defaults from the constructor's body."""
    f = source.funcs.get(vt.ctor)
    if f is None:
        raise cslots.GenError('%s: no function %s in %s' % (vt.key, vt.ctor, vt.cpath))
    if f.ret != 'PyObject *':
        raise cslots.GenError('%s returns %s, not an object' % (vt.ctor, f.ret))
    fields = source.decls.structs.get(vt.struct)
    if not fields:
        raise cslots.GenError('%s: no struct %s' % (vt.key, vt.struct))
    vt.params = [p for _, p in f.params]
    stored = {}
    for st in _statements(f.toks):
        w = [t.text for t in st]
        # if (P == NULL) P = Py_None
        if len(w) == 9 and w[:2] == ['if', '('] and w[2] in vt.params \
                and w[3:7] == ['==', 'NULL', ')', w[2]] and w[7:] == ['=', 'Py_None']:
            vt.defaults[w[2]] = 'None'
            continue
        if len(w) < 3 or w[-2] != '=' or w[-1] not in vt.params:
            continue
        lhs = cparse.expand(st[:-2], source.macros, set())
        lw = [t.text for t in lhs]
        arrows = [i for i, x in enumerate(lw) if x == '->' and i + 1 < len(lw)]
        if not arrows:
            continue
        fld = lw[arrows[-1] + 1]
        if fld in stored and stored[fld] != w[-1]:
            raise cslots.GenError('%s stores %s and %s in %s'
                                  % (vt.ctor, stored[fld], w[-1], fld))
        stored[fld] = w[-1]
    if vt.array:
        if len(f.params) == 1 and not stored and stores_through_call(
                source, f, vt.params[0], vt.size, effects):
            stored = {vt.size: vt.params[0]}
        if len(f.params) != 1 or stored != {vt.size: vt.params[0]}:
            raise cslots.GenError(
                '%s: an array constructor must store its one parameter in %s'
                ' (it stores %s)' % (vt.ctor, vt.size, stored))
        setter = vt.ctor[:-len('_New')] + '_SET_ITEM'
        m = source.macros.get(setter)
        body = [t.text for t in cparse.expand(m.body, source.macros, set())] if m else []
        if '->' not in body or body[body.index('->') + 1] != vt.items:
            raise cslots.GenError('%s: %s does not store into %s'
                                  % (vt.key, setter, vt.items))
        vt.setter = setter
        return
    for ctype, p in f.params:
        if ctype != 'PyObject *' or p not in stored.values():
            raise cslots.GenError('%s: the parameter %s (%s) is not stored in a'
                                  ' field' % (vt.ctor, p, ctype))
    for fld in fields:
        if fld in stored:
            vt.slots.append((fld, stored[fld]))
    layouts = dict((n, (c, r)) for n, c, r in
                   policy_table.LAYOUTS.get(vt.struct, {}).get('fields', []))
    for fld, _ in vt.slots:
        if fld not in layouts:
            raise cslots.GenError('%s.%s is a virtual slot but not in'
                                  ' policy_table.LAYOUTS' % (vt.struct, fld))


def fields_text(types):
    out = ['/* GENERATED by tools/metac/gen_virtual.py: do not edit. The field',
           '   descriptors of the virtual types policy_table.TYPES generates',
           '   (D80); included by pobject.h. */', '']
    for vt in types:
        if vt.array:
            prev = 'FIX_size' if vt.size in vt.entry.get('immutable', ()) else 'VAR_size'
            out.append('#define %s FARRAY(CPY_FIELD(%s, %s, %s))'
                       % (vt.field(vt.items), vt.struct, vt.items, prev))
            out.append('#define %s FIELD_INDEX(%s)' % (vt.index(vt.items), vt.field(vt.items)))
        else:
            prev = 'OB_type'
            for fld, _ in vt.slots:
                out.append('#define %s CPY_FIELD(%s, %s, %s)'
                           % (vt.field(fld), vt.struct, fld, prev))
                out.append('#define %s FIELD_INDEX(%s)' % (vt.index(fld), vt.field(fld)))
                prev = vt.field(fld)
            out.append('#define %s_TOTAL FIELDS_TOTAL(%s)' % (vt.prefix, prev))
        out.append('')
    return '\n'.join(out)


def ctor_flags(tr, vt):
    """The run-time call of the constructor: a new reference or NULL, and
    the effect rules' flags (rule effects)."""
    key = '%s:%s' % (vt.cpath, vt.ctor)
    return tr.flags('PyObject *', 'null', key if key in tr.effects.nodes else None)


def code_text(vt, flags, version):
    lo, sv = vt.lower, vt.var
    out = ['/* GENERATED by tools/metac/gen_virtual.py from CPython %s\'s' % version,
           '   %s and policy_table.TYPES: do not edit. Included by %s. */'
           % (vt.cpath, os.path.basename(vt.ni_file)), '']
    if vt.array:
        it, setter = vt.index(vt.items), vt.setter
        out += [
            '/* %s(n) and %s for each item, on raw values */' % (vt.ctor, setter),
            'static PyObject *direct_compute_%s(vinfo_t *v, char *data) {' % lo,
            '  int i, n = v->array->count - %s;' % it,
            '  PyObject *o = %s(n);' % vt.ctor,
            '  if (o == NULL)',
            '    return NULL;',
            '  for (i = 0; i < n; i++) {',
            '    PyObject *item = direct_xobj_vinfo(v->array->items[%s + i], data);' % it,
            '    if (item == NULL) {',
            '      Py_DECREF(o);',
            '      return NULL;',
            '    }',
            '    %s(o, i, item);' % setter,
            '  }',
            '  return o;',
            '}', '',
            '/* forced: built now if every item is a constant (rule allocator_pure),',
            '   else %s(n) at run time and the items stored */' % vt.ctor,
            'static bool compute_%s(PsycoObject *po, vinfo_t *v) {' % lo,
            '  int i, n = v->array->count - %s;' % it,
            '  vinfo_t *newobj;',
            '  extra_assert(n == CompileTime_Get(v->array->items[iFIX_SIZE]->source)->value);',
            '  for (i = 0; i < n; i++)',
            '    if (!is_compiletime(v->array->items[%s + i]->source))' % it,
            '      break;',
            '  if (i == n) {',
            '    PyObject *o = direct_compute_%s(v, NULL);' % lo,
            '    if (o == NULL)',
            '      OUT_OF_MEMORY();',
            '    newobj = vinfo_new(CompileTime_NewSk(sk_new((long)o, SkFlagPyObj)));',
            '  }',
            '  else {',
            '    newobj = psyco_generic_call(po, %s, %s, "l", (long)n);' % (vt.ctor, flags),
            '    if (newobj == NULL)',
            '      return false;',
            '    for (i = 0; i < n; i++)',
            '      if (!psyco_put_nth_field(po, newobj, FMUT(FPYREF(%s)), i,'
            % vt.field(vt.items),
            '                               v->array->items[%s + i])) {' % it,
            '        vinfo_decref(newobj, po);',
            '        return false;',
            '      }',
            '  }',
            '  vinfo_move(po, v, newobj);',
            '  return true;',
            '}', '',
            '/* a virtual %s of count items, taking new references to source[0..count)' % vt.lower,
            '   (or filled by the caller when source is NULL) */',
            'DEFINEFN',
            'vinfo_t *%s(int count, vinfo_t **source) {' % vt.new,
            '  int i;',
            '  vinfo_t *v = vinfo_new(VirtualTime_New(&%s));' % sv,
            '  v->array = array_new(%s + count);' % it,
            '  v->array->items[iOB_TYPE] = vinfo_new(CompileTime_New((long)&%s));' % vt.type,
            '  v->array->items[iFIX_SIZE] = vinfo_new(CompileTime_New(count));',
            '  if (source != NULL)',
            '    for (i = 0; i < count; i++) {',
            '      vinfo_incref(source[i]);',
            '      v->array->items[%s + i] = source[i];' % it,
            '    }',
            '  return v;',
            '}', '']
        mask = '(-1 << %s)' % it
    else:
        params = vt.params
        by_param = dict((p, f) for f, p in vt.slots)
        out.append('/* %s(%s) on raw values */' % (vt.ctor, ', '.join(params)))
        out.append('static PyObject *direct_compute_%s(vinfo_t *v, char *data) {' % lo)
        out.append('  PyObject *o = NULL;')
        for p in params:
            out.append('  PyObject *%s = direct_xobj_vinfo(v->array->items[%s], data);'
                       % (p, vt.index(by_param[p])))
        out.append('  if (%s)' % ' && '.join('%s != NULL' % p for p in params))
        out.append('    o = %s(%s);' % (vt.ctor, ', '.join(params)))
        for p in reversed(params):
            out.append('  Py_XDECREF(%s);' % p)
        out.append('  return o;')
        out.append('}')
        out.append('')
        out.append('/* forced: built now if every field is a constant (rule allocator_pure),')
        out.append('   else %s at run time */' % vt.ctor)
        out.append('static bool compute_%s(PsycoObject *po, vinfo_t *v) {' % lo)
        out.append('  vinfo_t *newobj;')
        out.append('  if (%s) {' % ' &&\n      '.join(
            'is_compiletime(v->array->items[%s]->source)' % vt.index(f) for f, _ in vt.slots))
        out.append('    PyObject *o = direct_compute_%s(v, NULL);' % lo)
        out.append('    if (o == NULL)')
        out.append('      OUT_OF_MEMORY();')
        out.append('    newobj = vinfo_new(CompileTime_NewSk(sk_new((long)o, SkFlagPyObj)));')
        out.append('  }')
        out.append('  else {')
        out.append('    newobj = psyco_generic_call(po, %s, %s, "%s", %s);' % (
            vt.ctor, flags, 'v' * len(params),
            ', '.join('v->array->items[%s]' % vt.index(by_param[p]) for p in params)))
        out.append('    if (newobj == NULL)')
        out.append('      return false;')
        if vt.forced == 'drop_fields':
            out.append('    vinfo_array_shrink(po, v, %s);' % vt.index(vt.slots[0][0]))
        out.append('  }')
        out.append('  vinfo_move(po, v, newobj);')
        out.append('  return true;')
        out.append('}')
        out.append('')
        out.append('/* a virtual %s: %s\'s arguments, each a new reference%s */' % (
            vt.lower, vt.ctor, '; NULL is None for %s' % ', '.join(
                p for p in params if p in vt.defaults) if vt.defaults else ''))
        out.append('DEFINEFN')
        out.append('vinfo_t *%s(%s) {' % (vt.new, ', '.join('vinfo_t *%s' % p for p in params)))
        out.append('  vinfo_t *v = vinfo_new(VirtualTime_New(&%s));' % sv)
        out.append('  v->array = array_new(%s_TOTAL);' % vt.prefix)
        out.append('  v->array->items[iOB_TYPE] = vinfo_new(CompileTime_New((long)&%s));'
                   % vt.type)
        for f, p in vt.slots:
            if p in vt.defaults:
                out.append('  if (%s == NULL)' % p)
                out.append('    %s = psyco_vi_None();' % p)
                out.append('  else')
                out.append('    vinfo_incref(%s);' % p)
            else:
                out.append('  extra_assert(%s != NULL);' % p)
                out.append('  vinfo_incref(%s);' % p)
            out.append('  v->array->items[%s] = %s;' % (vt.index(f), p))
        out.append('  return v;')
        out.append('}')
        out.append('')
        mask = ' | '.join('(1 << %s)' % vt.index(f) for f, _ in vt.slots)
    out += [
        '/* the force and direct functions, with policy_table.TYPES\' weights */',
        'static void gen_virtual_%s_init(void) {' % lo,
        '  INIT_SVIRTUAL(%s, compute_%s, direct_compute_%s, %s, %s, %s);'
        % (sv, lo, lo, mask, vt.weights[0], vt.weights[1]),
        '}', '']
    return '\n'.join(out)


def generate(src, pyconfig, defines, effects):
    import gen_abstract
    types = virtual_types()
    version = cpython_src.source_version(src)
    out, sources = [], {}
    for vt in types:
        if vt.cpath not in sources:
            try:
                sources[vt.cpath] = gen_abstract.Source(src, pyconfig, defines, vt.cpath)
            except cparse.ParseError as e:
                raise cslots.GenError('%s: %s' % (vt.cpath, e))
        read_constructor(sources[vt.cpath], vt, effects)
    out.append((FIELDS_HEADER, fields_text(types),
                ['generated  %s' % vt.key for vt in types]))
    for vt in types:
        s = sources[vt.cpath]
        tr = gen_abstract.Translator(s, {}, {}, effects)
        text = code_text(vt, ctor_flags(tr, vt), version)
        out.append((vt.header(), text,
                    ['generated  %-20s %s, compute_%s, direct_compute_%s'
                     % ('virtual.%s %s:%s' % (vt.ctor, vt.cpath, vt.ctor), vt.new, vt.lower, vt.lower)]))
    return out


def build(target):
    """[(header, text, report)] for generate.py."""
    import effects
    import gen_abstract
    try:
        g = effects.analyse(target.python, target.src)
    except Exception as e:  # noqa: BLE001 - the build must say why
        raise cslots.GenError('effects.py: %s' % e)
    return generate(target.src, target.pyconfig,
                    gen_abstract.target_defines(target.python), g)


def main(argv):
    import argparse
    import effects
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', required=True)
    ap.add_argument('--pyconfig', required=True)
    ap.add_argument('--python', required=True)
    args = ap.parse_args(argv)
    g = effects.analyse(args.python, args.src)
    for name, text, report in generate(args.src, args.pyconfig,
                                       ['Py_BUILD_CORE', 'NDEBUG'], g):
        print('/* ==== %s */' % name)
        print(text)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
