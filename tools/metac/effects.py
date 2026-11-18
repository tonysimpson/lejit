#!/usr/bin/env python3
"""Effects (doc/GENERATOR_DESIGN.md "Analyses", 1): what each C function of
the target's CPython may do, read from the front end's preprocessed AST of
every file linked into libpython and every file Ni mirrors, so only the
compiled #if arms count.

    effects.py --python PYTHON [--src DIR] NAME...    # why, per flag

Each function gets five flags, each holding only if nothing breaks it:
  pure       its result depends only on its arguments' values: it reads no
             memory but its own locals and const data
  readonly   it writes no memory but its own locals (refcounts aside)
  no_python  it runs no Python code
  no_block   it doesn't release the GIL
  no_raise   it sets no exception (the error conventions need it)

A call through anything but a function's name, or of a function declared in
CPython's headers but defined in no file read, breaks every flag. A function
named without being called is a callee. A function declared only by a
system header is libc's: it runs no Python and doesn't block, and only
EXTERNAL makes it pure; even then it reads what a pointer argument points
to, and writes it unless the pointer is to const. Running Python
breaks everything but no_raise for a decref, whose __del__'s errors are
printed, not raised.

What is taken on trust is policy_table's, each entry with its reason: SEEDS
(the walk stops at them: decref, gc, blocks, sets_error, alloc, fatal),
ASSUMPTIONS (flags held whatever the body does), EXTERNAL, and NO_DEL_TYPES
(a decref of a local declared with one of their structs, or of a singleton,
runs no Python). The decref seeds are vocabulary (tools/metac/shim/vocab.h),
so each use keeps its argument.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import front  # noqa: E402
from pycparser import c_ast  # noqa: E402
from pycparser.c_parser import ParseError  # noqa: E402

FLAGS = ('pure', 'readonly', 'no_python', 'no_block', 'no_raise')
ALL = frozenset(FLAGS)
PYTHON = ALL - {'no_raise'}
BREAKS = {
    'indirect': ALL, 'unscanned': ALL, 'asm': ALL, 'decref': PYTHON,
    'read': {'pure'}, 'write': {'readonly'},
    'alloc': {'pure', 'readonly'}, 'gc': {'pure', 'readonly'},
    'sets_error': {'pure', 'readonly', 'no_raise'},
    'blocks': {'pure', 'readonly', 'no_block'}, 'fatal': set(),
    'libc': {'pure', 'readonly'}, 'libc pure': set(),
    'raises': {'no_raise'},
}
ASSUMED = {'no_python': {'no_python'}, 'no_block': {'no_block'},
           'pure': {'pure', 'readonly'}, 'raises': set()}
SINGLETONS = {'_Py_NoneStruct', '_Py_TrueStruct', '_Py_FalseStruct',
              '_Py_NotImplementedStruct', '_Py_EllipsisObject'}
REFCOUNT = {'ob_refcnt'}
WRITES_ARG = {'Py_CLEAR', 'Py_SETREF', 'Py_XSETREF'}
VOCAB = '__ni_vocab_'


class EffectsError(Exception):
    pass


class Rules:
    """What is taken on trust: seeds {key: reason}, assumptions {key:
    names}, external {name: effect}, no_del struct and typedef names."""

    def __init__(self, seeds, assumptions, external, no_del):
        self.seeds, self.assumptions = seeds, assumptions
        self.external, self.no_del = external, set(no_del)
        self.decref = {k.split(':')[1] for k, r in seeds.items() if r == 'decref'}


def policy_rules():
    import policy_table as T
    struct_type = {k.split(':')[1]: v['type'] for k, v in T.TYPES.items()}
    return Rules({k: v['reason'] for k, v in T.SEEDS.items()},
                 {k: set(v['assume']) for k, v in T.ASSUMPTIONS.items()},
                 dict(T.EXTERNAL),
                 {s for s, t in struct_type.items() if t in T.NO_DEL_TYPES})


class Function:
    kind = 'function'

    def __init__(self, key):
        self.key = key
        self.reasons = []        # [(kind, text)]
        self.calls = []          # [(name, TU-local key or None, origin)]


class Unit:
    """One translation unit's file-scope names."""

    def __init__(self, ast, src):
        self.src = src
        self.defs, self.funcs, self.vars, self.typedefs = {}, {}, {}, {}
        self.params = {}
        self.enums = set()
        for d in ast.ext:
            if isinstance(d, c_ast.FuncDef):
                self.defs[d.decl.name] = self.key(d.decl)
                self.funcs[d.decl.name] = self.origin(d.decl)
            elif isinstance(d, c_ast.Typedef):
                self.typedefs[d.name] = d.type
            elif isinstance(d, c_ast.Decl) and d.name:
                if isinstance(d.type, c_ast.FuncDecl):
                    self.funcs.setdefault(d.name, self.origin(d))
                    args = d.type.args.params if d.type.args else []
                    self.params[d.name] = [getattr(a, 'type', None) for a in args]
                else:
                    self.vars[d.name] = d.type
        for n in _walk(ast):
            if isinstance(n, c_ast.Enumerator):
                self.enums.add(n.name)

    def key(self, decl):
        return '%s:%s' % (self.path(decl.coord.file), decl.name)

    def path(self, f):
        return os.path.relpath(f, self.src) if f.startswith(self.src + os.sep) else f

    def origin(self, decl):
        return 'src' if decl.coord.file.startswith(self.src + os.sep) else 'libc'

    def names(self, t):
        """The type names a declared type's pointee goes by, through
        typedefs: 'PyLongObject *' gives PyLongObject and _longobject."""
        out = set()
        while isinstance(t, (c_ast.PtrDecl, c_ast.ArrayDecl, c_ast.TypeDecl)):
            t = t.type
        while t is not None:
            if isinstance(t, c_ast.Struct):
                out.add(t.name)
                return out
            if not isinstance(t, c_ast.IdentifierType) or t.names[-1] in out:
                return out
            name = t.names[-1]
            out.add(name)
            t = self.typedefs.get(name)
            while isinstance(t, c_ast.TypeDecl):
                t = t.type
        return out


def _walk(n):
    yield n
    for _, c in n.children():
        yield from _walk(c)


def _const(t):
    if isinstance(t, c_ast.ArrayDecl):
        return _const(t.type)
    return isinstance(t, (c_ast.PtrDecl, c_ast.TypeDecl)) and 'const' in t.quals


def _strip(e):
    while isinstance(e, c_ast.Cast):
        e = e.expr
    return e


class Reader:
    """One function's direct reasons and calls."""

    def __init__(self, unit, rules, fn, d):
        self.u, self.rules, self.fn = unit, rules, fn
        self.scope = {}          # name -> (static, declared type)
        for p in (d.decl.type.args.params if d.decl.type.args else []):
            if isinstance(p, c_ast.Decl) and p.name:
                self.scope[p.name] = (False, p.type)
        self.stmt(d.body)

    def reason(self, kind, text):
        self.fn.reasons.append((kind, text))

    def edge(self, name):
        self.fn.calls.append((name, self.u.defs.get(name), self.u.funcs.get(name)))

    def stmt(self, n):
        if n is None:
            return
        if isinstance(n, c_ast.Decl):
            if isinstance(n.type, c_ast.FuncDecl) or 'extern' in n.storage:
                return
            self.scope[n.name] = ('static' in n.storage, n.type)
            if n.init is not None:
                self.expr(n.init)
        elif isinstance(n, (c_ast.Typedef, c_ast.StaticAssert, c_ast.Pragma)):
            return
        elif isinstance(n, c_ast.Node) and type(n).__name__ in _STATEMENTS:
            for _, c in n.children():
                self.stmt(c)
        else:
            self.expr(n)

    def array(self, e):
        if isinstance(e, c_ast.ID):
            t = self.scope[e.name][1] if e.name in self.scope else self.u.vars.get(e.name)
            return isinstance(t, c_ast.ArrayDecl)
        return False

    def expr(self, e, addr=False):
        """Visit e as evaluated; addr: only its address is taken."""
        if isinstance(e, c_ast.ID):
            self.name(e.name, addr)
        elif isinstance(e, c_ast.StructRef):
            if e.type == '->':
                if not addr:
                    self.reason('read', '->' + e.field.name)
                self.expr(e.name)
            else:
                self.expr(e.name, addr)
        elif isinstance(e, c_ast.ArrayRef):
            if self.array(e.name):
                self.expr(e.name, addr)
            else:
                if not addr:
                    self.reason('read', '[]')
                self.expr(e.name)
            self.expr(e.subscript)
        elif isinstance(e, c_ast.UnaryOp):
            if e.op in ('sizeof', '_Alignof'):
                return
            if e.op == '&':
                self.expr(e.expr, True)
            elif e.op == '*':
                if not addr:
                    self.reason('read', '*')
                self.expr(e.expr)
            elif e.op in ('++', '--', 'p++', 'p--'):
                r = _strip(e.expr)
                if isinstance(r, c_ast.StructRef) and r.field.name in REFCOUNT:
                    self.expr(r.name)       # an incref or decref, not a read
                else:
                    self.write(e.expr)
                    self.expr(e.expr)
            else:
                self.expr(e.expr)
        elif isinstance(e, c_ast.Assignment):
            self.write(e.lvalue)
            self.expr(e.lvalue, e.op == '=')
            self.expr(e.rvalue)
        elif isinstance(e, c_ast.FuncCall):
            self.call(e)
        elif isinstance(e, c_ast.Cast):
            self.expr(e.expr, addr)
        elif isinstance(e, (c_ast.Typename, c_ast.Constant)) or e is None:
            return
        elif isinstance(e, c_ast.Compound):     # a GNU statement expression
            self.stmt(e)
        else:
            for _, c in e.children():
                self.expr(c)

    def name(self, name, addr):
        if name in self.scope:
            static, t = self.scope[name]
            if static and not addr and not _const(t):
                self.reason('read', 'static ' + name)
        elif name == '__asm__':     # what the prelude leaves of a GNU asm
            self.reason('asm', '')
        elif name in self.u.enums:
            return
        elif name in self.u.funcs:
            self.edge(name)
        elif name in self.u.vars:
            if not addr and not _const(self.u.vars[name]):
                self.reason('read', name)

    def write(self, e):
        while True:
            e = _strip(e)
            if isinstance(e, c_ast.StructRef):
                if e.field.name in REFCOUNT:
                    return
                if e.type == '->':
                    break
                e = e.name
            elif isinstance(e, c_ast.ArrayRef) and self.array(e.name):
                e = e.name
            elif isinstance(e, c_ast.ID):
                if e.name in self.scope and not self.scope[e.name][0]:
                    return
                self.reason('write', e.name)
                return
            else:
                break
        self.reason('write', 'memory')

    def call(self, c):
        f = c.name
        while isinstance(f, c_ast.UnaryOp) and f.op == '*':
            f = f.expr
        args = c.args.exprs if c.args else []
        if isinstance(f, c_ast.ID) and f.name.startswith(VOCAB):
            self.vocab(f.name[len(VOCAB):], args)
        elif isinstance(f, c_ast.ID) and f.name == '__ni_va_arg':
            self.reason('read', 'va_arg')
        elif isinstance(f, c_ast.ID) and f.name not in self.scope \
                and f.name not in self.u.vars:
            self.edge(f.name)
            if self.u.funcs.get(f.name) == 'libc' and f.name not in self.u.defs:
                self.pointers(self.u.params.get(f.name, []), args)
        else:
            self.reason('indirect', 'line %d' % c.coord.line)
            self.expr(c.name)
        for a in args:
            self.expr(a)

    def pointers(self, params, args):
        """A libc function reads what its pointer arguments point to, and
        writes it unless the pointer is to const; a local's is its own."""
        for p, a in zip(params, args):
            if not isinstance(p, c_ast.PtrDecl):
                continue
            a = _strip(a)
            if isinstance(a, c_ast.UnaryOp) and a.op == '&' and isinstance(a.expr, c_ast.ID) \
                    and a.expr.name in self.scope and not self.scope[a.expr.name][0]:
                continue
            self.reason('read', 'through a pointer argument')
            if 'const' not in getattr(p.type, 'quals', ()):
                self.reason('write', 'through a pointer argument')

    def vocab(self, name, args):
        if name in self.rules.decref:
            e = _strip(args[0])
            if isinstance(e, c_ast.UnaryOp) and e.op == '&' \
                    and isinstance(e.expr, c_ast.ID) and e.expr.name in SINGLETONS:
                pass
            elif isinstance(e, c_ast.ID) and e.name in self.scope and \
                    self.u.names(self.scope[e.name][1]) & self.rules.no_del:
                pass
            else:
                self.reason('decref', name)
            if name in WRITES_ARG:
                self.write(args[0])
        elif name.startswith('_Py_atomic_load'):
            self.reason('read', name)
        elif name.startswith('_Py_atomic_store'):
            self.reason('write', name)
        elif not name.endswith('_fence'):
            raise EffectsError('vocabulary name %s has no effect rule' % name)


_STATEMENTS = {'Compound', 'If', 'For', 'While', 'DoWhile', 'Switch', 'Case',
               'Default', 'Label', 'Return', 'Goto', 'Break', 'Continue',
               'EmptyStatement', 'DeclList'}


class Effects:
    """The flags of every function read. nodes {key: Function}; globals
    {name: key} for functions visible from any file (not static, or a
    header's); by_file {path: {name: key}}, a file's own definitions and,
    for each file read, every definition it compiles."""

    def __init__(self, src, rules):
        self.src, self.rules = src, rules
        self.nodes, self.globals, self.by_file = {}, {}, {}
        self.unread = []         # files that don't preprocess for this target

    def add(self, path, ast):
        """Read the translation unit `path`."""
        u = Unit(ast, self.src)
        for d in ast.ext:
            if not isinstance(d, c_ast.FuncDef):
                continue
            key = u.key(d.decl)
            file, name = key.rsplit(':', 1)
            self.by_file.setdefault(path, {})[name] = key
            self.by_file.setdefault(file, {})[name] = key
            if key in self.nodes:
                continue
            fn = self.nodes[key] = Function(key)
            if not (file.endswith('.c') and 'static' in d.decl.storage):
                self.globals.setdefault(name, key)
            if key not in self.rules.seeds:
                Reader(u, self.rules, fn, d)

    def direct(self, fn):
        """[(kind, text)] of fn, with its calls resolved; [(key)] callees."""
        reasons, callees = list(fn.reasons), []
        for name, local, origin in fn.calls:
            key = local or self.globals.get(name)
            if key:
                callees.append(key)
            elif origin == 'libc' or (origin is None and name.startswith('__builtin_')):
                e = self.rules.external.get(name)
                kind = {'pure': 'libc pure', 'blocks': 'blocks'}.get(e, 'libc')
                reasons.append((kind, name))
            else:
                reasons.append(('unscanned', name))
        if fn.key in self.rules.seeds:
            callees = []
        return reasons, callees

    def solve(self):
        """self.broken {flag: {key: why}}: why is a reason or 'calls KEY'.
        An assumed flag is given: the walk doesn't go into its function."""
        vocabulary = front.vocabulary()
        for key, reason in self.rules.seeds.items():
            if key in self.nodes:
                self.nodes[key].reasons = [(reason, 'seed')]
            elif reason != 'decref':
                raise EffectsError('SEEDS %s: no such function was read' % key)
            elif key.split(':')[1] not in vocabulary:
                raise EffectsError('SEEDS %s: a decref must be vocabulary' % key)
        for key in self.rules.assumptions:
            if key not in self.nodes:
                raise EffectsError('ASSUMPTIONS %s: no such function was read' % key)
        self.broken = {f: {} for f in FLAGS}
        edges, given = {}, {}
        for key, fn in self.nodes.items():
            reasons, edges[key] = self.direct(fn)
            a = self.rules.assumptions.get(key, ())
            given[key] = set().union(*[ASSUMED[x] for x in a])
            if 'raises' in a:
                given[key].add('no_raise')
                self.broken['no_raise'][key] = 'raises assumed'
            for kind, text in reasons:
                for f in BREAKS[kind] - given[key]:
                    self.broken[f].setdefault(key, '%s %s' % (kind, text))
        for f in FLAGS:
            b = self.broken[f]
            changed = True
            while changed:
                changed = False
                for key, callees in edges.items():
                    if key in b or f in given[key]:
                        continue
                    for c in callees:
                        if c in b:
                            b[key] = 'calls ' + c
                            changed = True
                            break

    def flags(self, key):
        """The flags that hold for key, or None if no function has it."""
        if key not in self.nodes:
            return None
        return frozenset(f for f in FLAGS if key not in self.broken[f])

    def cf_flags(self, key):
        """Ni's Cf* effect flags for a residual call of key (ni/cf_flags.h):
        CfPure (called at compile time when every argument is known, and
        then also no Python and no exception), else CfNoPython; CfNoBlock."""
        f = self.flags(key)
        if f is None:
            return []
        out = []
        if {'pure', 'readonly', 'no_python', 'no_raise'} <= f:
            out.append('CfPure')
        elif 'no_python' in f:
            out.append('CfNoPython')
        if 'no_block' in f:
            out.append('CfNoBlock')
        return out

    def cannot_raise(self, key):
        f = self.flags(key)
        return f is not None and {'no_python', 'no_raise'} <= f

    def why(self, key, flag):
        """The chain of callees to what breaks flag for key."""
        out = []
        while key in self.broken[flag] and len(out) < 50:
            why = self.broken[flag][key]
            out.append('%s (%s)' % (key, why) if not why.startswith('calls ') else key)
            if not why.startswith('calls '):
                break
            key = why[len('calls '):]
        return ' -> '.join(out)


def scanned_files(src):
    return sorted(front.library_files(src) | set(front.mirrored_files()))


_CACHE = {}


def analyse(python, src=None, rules=None):
    """Effects for the CPython `python` runs (its source `src`, else
    cpython_src's), computed once per process. A file that isn't mirrored
    and doesn't preprocess (a pyconfig.h from another system can name
    headers this one lacks) is left out, so its functions are the worst
    case."""
    key = (python, src)
    if key not in _CACHE:
        t = front.target(python, src)
        g = Effects(t.src, rules or policy_rules())
        mirrored = set(front.mirrored_files())
        for cfile in scanned_files(t.src):
            try:
                g.add(cfile, front.parse(t, cfile))
            except (front.FrontError, ParseError) as e:
                if cfile in mirrored:
                    raise EffectsError('%s: %s' % (cfile, e))
                g.unread.append(cfile)
        g.solve()
        _CACHE[key] = g
    return _CACHE[key]


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--python', required=True)
    ap.add_argument('--src')
    ap.add_argument('names', nargs='+')
    args = ap.parse_args(argv)
    g = analyse(args.python, args.src)
    if g.unread:
        print('not read:', ' '.join(g.unread))
    for name in args.names:
        keys = [k for k in g.nodes if k == name or k.endswith(':' + name)]
        if not keys:
            print(name, 'not found')
        for k in keys:
            print(k, ' '.join(g.cf_flags(k)) or '(no Cf flags)')
            for f in FLAGS:
                print('  %-9s %s' % (f, g.why(k, f) or 'holds'))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
