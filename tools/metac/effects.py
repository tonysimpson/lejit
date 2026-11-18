#!/usr/bin/env python3
"""Effect rules (GENERATION step 3, D71): what a call may do, computed from a
token-level call graph over CPython's source, compared with the Cf* flags of
every residual call in Ni.

    effects.py --src DIR [-v]

For each C function (and each macro, as a node of its own) of Objects,
Python, Modules, Parser and Include, the walk finds:

  python    it may run Python code. Reasons: a call through a function
            pointer (a slot, a field, a parameter); a decref (SEEDS) of
            anything but a constant singleton or a variable declared with a
            no-__del__ type's struct (NO_DEL_TYPES, TYPES); a callee that may.
  blocks    it may release the GIL (SEEDS, EXTERNAL 'blocks').
  pure      no write through a pointer or to a file-scope variable (a
            refcount isn't counted), no read of a mutable file-scope
            variable (type objects and PyExc_* are taken as constant), no
            python, every callee pure. An
            allocator (SEEDS alloc) is impure: allocs records who reaches
            one, for rule allocator_pure.
  raises    it may set an exception: a call through a pointer, SEEDS
            sets_error, an ASSUMPTIONS 'raises'. A decref doesn't raise (an
            error in __del__ is printed). The error convention to check is
            then the return type's (Ni's CfPyErr*); a site whose result the
            caller tests itself (runtime_NON_NULL_f(...)) counts as checked.

The walk stops at SEEDS and at ASSUMPTIONS (the table's): what Ni accepts
without proof is taken as given, and said so. Garbage collection is ignored
(rule gc_ignored). A function outside the source (libc) runs no Python;
EXTERNAL says which are pure or block.

Each residual call site in Ni (psyco_generic_call, Psyco_META*) is resolved
to a C function (by name, through Ni's compat macros and a header's one-call
macros, or through a type's slot initialisers) or to Ni's own helper, read
the same way, and its flags compared: agree,
cautious (Ni assumes less than is proven), disagree (Ni assumes more, with
the path that shows it), or unproven (no function to read). NewRef is
compared with Doc/data/refcounts.dat. CANNOT_BLOCK is compared with blocks.
Not run by the build.
"""
import argparse
import collections
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import csource  # noqa: E402
import ctokens  # noqa: E402
import policy_table as T  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
# Include first: a header's macro shadows a function of the same name (one
# that the default configuration leaves out, as _Py_ForgetReference)
DIRS = ['Include', 'Objects', 'Python', 'Modules', 'Parser']
NOT_CALLS = {'if', 'while', 'for', 'switch', 'return', 'sizeof', 'defined',
             'case', 'do', 'else', '__attribute__', 'offsetof', 'Py_ARRAY_LENGTH',
             'assert', 'Py_UNUSED', '_Py_STATIC_ASSERT'}
SINGLETONS = {'Py_None', 'Py_True', 'Py_False', 'Py_NotImplemented',
              'Py_Ellipsis'}
REFCOUNT = {'ob_refcnt', '_Py_RefTotal'}
ASSIGN = {'=', '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=', '<<=', '>>=',
          '++', '--'}
C_TYPES = {'int', 'long', 'Py_ssize_t', 'Py_hash_t', 'double', 'size_t',
             'Py_UCS4', 'char', 'unsigned', 'short', 'float', 'digit',
             'sdigit', 'twodigits', 'stwodigits', 'PY_LONG_LONG', 'Py_UCS1',
             'Py_UCS2', 'wchar_t', 'Py_uhash_t', 'uint64_t', 'int64_t'}


class Node:
    def __init__(self, key, d, kind):
        self.key, self.d, self.kind = key, d, kind
        self.calls = []          # [(key or None, text)] resolved callees
        self.reasons = []        # [(reason, text)] direct reasons
        self.writes = []         # non-local writes (text)
        self.reads = []          # mutable file-scope variables read


def params_of(d):
    out = set()
    for p in ctokens.split_top(d.params or [], ','):
        ids = [x.text for x in p if x.kind == 'id']
        if ids:
            out.add(ids[-1])
        for j, x in enumerate(p):       # (*name)(...)
            if x.text == '*' and j + 1 < len(p) and p[j + 1].kind == 'id' \
                    and j > 0 and p[j - 1].text == '(':
                out.add(p[j + 1].text)
    return out


def declared(body):
    """{name: type word} for locals declared in body: TYPE [*]* NAME (=;,[)."""
    out = {}
    for j in range(1, len(body) - 1):
        x = body[j]
        if x.kind != 'id' or body[j + 1].text not in ('=', ';', ',', '[', ')'):
            continue
        k = j - 1
        while k >= 0 and body[k].text == '*':
            k -= 1
        if k >= 0 and body[k].kind == 'id' and body[k].text not in NOT_CALLS \
                and body[k].text != 'return' and (k == j - 1 or body[k + 1].text == '*'
                                                  or body[k].text in C_TYPES
                                                  or body[k].text[0].isupper()):
            if k == j - 1 and body[k].text not in C_TYPES and \
                    not re.match(r'(Py|_Py|struct|const|unsigned|static|register)', body[k].text) \
                    and not body[k].text.endswith('_t'):
                continue
            out[x.text] = body[k].text
    return out


class Graph:
    def __init__(self, src, ni_root=ROOT):
        self.src = src
        self.tree = csource.Tree(src)
        self.ni = csource.Tree(ni_root)
        self.ni_root = ni_root
        self.nodes = {}              # key -> Node
        self._globals = {}
        self.state = set()           # mutable file-scope variables, any file
        self.by_file = collections.defaultdict(dict)   # path -> name -> key
        self.globals = {}            # name -> key (non-static, or a header's)
        self.ni_macros = {}
        self.assume = {k.split(':')[1]: set(v['assume'])
                       for k, v in T.ASSUMPTIONS.items()}
        self.seeds = {k.split(':')[1]: v['reason'] for k, v in T.SEEDS.items()}
        struct_type = {k.split(':')[1]: v['type'] for k, v in T.TYPES.items()}
        self.no_del_structs = {s for s, t in struct_type.items()
                               if t in T.NO_DEL_TYPES}
        self.load()

    def files(self):
        for d in DIRS:
            for p in sorted(glob.glob(os.path.join(self.src, d, '**', '*.[ch]'),
                                      recursive=True)):
                yield os.path.relpath(p, self.src)

    def ni_files(self):
        for p in sorted(glob.glob(os.path.join(self.ni_root, 'ni', '**', '*.[ch]'),
                                  recursive=True)):
            rel = os.path.relpath(p, self.ni_root)
            if 'pstringobject' not in rel:      # not built
                yield rel

    def add(self, tree, path):
        try:
            f = tree.file(path)
        except SyntaxError:
            return
        if not f:
            return
        for name, d in f[1].items():
            if d.kind not in ('function', 'macro'):
                continue
            key = path + ':' + name
            self.nodes[key] = Node(key, d, d.kind)
            self.by_file[path][name] = key
            static = d.kind == 'function' and 'static' in [x.text for x in d.ret]
            if path.startswith('ni/'):
                if d.kind == 'macro':
                    self.ni_macros[name] = d
            elif path.startswith('Include/') or (not static and d.kind == 'function'):
                self.globals.setdefault(name, key)

    def load(self):
        for p in self.files():
            self.add(self.tree, p)
        for p in self.ni_files():
            self.add(self.ni, p)
        for p in self.files():
            if p.endswith('.c'):
                self.state |= {v for v in self.file_globals(p) if not (
                    v.endswith('_Type') or v.startswith('PyExc_'))}
        for key, n in self.nodes.items():
            self.read(n)

    def resolve(self, path, name):
        k = self.by_file.get(path, {}).get(name)
        if k:
            return k
        if path.startswith('ni/'):
            for p in self.by_file:          # ni.c includes everything
                if p.startswith('ni/') and name in self.by_file[p]:
                    return self.by_file[p][name]
        return self.globals.get(name)

    def read(self, n):
        d = n.d
        path = n.key.rsplit(':', 1)[0]
        if n.kind == 'macro':
            toks = d.toks[1:]
            if toks and toks[0].text == '(' and not toks[0].sp:
                toks = toks[ctokens.match_close(toks, 0) + 1:]
            body = toks
            params = set()
            if len(d.toks) > 1 and d.toks[1].text == '(' and not d.toks[1].sp:
                params = {x.text for x in d.toks[2:ctokens.match_close(d.toks, 1)]}
        else:
            body = d.body or []
            params = params_of(d)
        decl = declared(body)
        local = params | set(decl)
        n.globals = self.file_globals(path) if n.kind == 'function' else set()
        for j, x in enumerate(body):
            nxt = body[j + 1].text if j + 1 < len(body) else ''
            if x.kind == 'id' and nxt == '(' and x.text not in NOT_CALLS:
                prev = body[j - 1].text if j else ''
                if prev in ('->', '.'):
                    n.reasons.append(('indirect', '%s%s()' % (prev, x.text)))
                    continue
                if x.text in self.seeds:
                    self.seed(n, x.text, body, j, decl)
                    continue
                k = self.resolve(path, x.text)
                if k:
                    n.calls.append((k, x.text))
                elif x.text in local:
                    n.reasons.append(('indirect', x.text + '()'))
                elif x.text in T.EXTERNAL and T.EXTERNAL[x.text] == 'blocks':
                    n.reasons.append(('blocks', x.text))
                elif x.text not in T.EXTERNAL and not x.text[0].isupper() \
                        and x.text not in decl:
                    n.reasons.append(('external', x.text))
            elif x.text == ')' and nxt == '(' and j > 2:
                # (*fn)(...) or (cast)fn: a call through a pointer
                o = self.open_of(body, j)
                inner = [y.text for y in body[o + 1:j]]
                if inner and inner[0] == '*' or '->' in inner:
                    n.reasons.append(('indirect', '(%s)()' % ' '.join(inner)))
            elif x.kind == 'id' and nxt != '(' and x.text not in local:
                if x.text in self.seeds and self.seeds[x.text] != 'decref':
                    n.reasons.append((self.seeds[x.text], x.text))
                else:
                    k = self.resolve(path, x.text)
                    if k and self.nodes[k].kind == 'macro':
                        n.calls.append((k, x.text))
            if x.kind == 'id' and x.text in self.state and x.text not in local \
                    and nxt not in ('(', ':') and (not j or body[j - 1].text not in ('&', '.', '->', 'goto')):
                n.reads.append(x.text)
            if x.text in ASSIGN:
                self.write(n, body, j, local)

    def open_of(self, body, j):
        depth = 0
        for o in range(j, -1, -1):
            if body[o].text == ')':
                depth += 1
            elif body[o].text == '(':
                depth -= 1
                if depth == 0:
                    return o
        return 0

    def seed(self, n, name, body, j, decl):
        reason = self.seeds[name]
        if reason == 'decref':
            close = ctokens.match_close(body, j + 1)
            arg = [y.text for y in body[j + 2:close] if y.kind == 'id']
            if not arg or arg[-1] in SINGLETONS or \
                    decl.get(arg[-1]) in self.no_del_structs:
                return
            n.reasons.append(('decref', '%s(%s)' % (name, ' '.join(arg))))
        else:
            n.reasons.append((reason, name))

    def write(self, n, body, j, local):
        if body[j].text in ('++', '--'):
            if j + 1 < len(body) and body[j + 1].kind == 'id':
                lv = body[j + 1:j + 2]
                k = j + 2
                while k < len(body) and body[k].text in ('->', '.', '['):
                    k += 2
                lv = body[j + 1:k]
            else:
                lo = j - 1
                while lo > 0 and body[lo - 1].text in ('->', '.'):
                    lo -= 2
                lv = body[lo:j]
        else:
            lo = j
            depth = 0
            while lo > 0:
                t = body[lo - 1].text
                if t in (')', ']'):
                    depth += 1
                elif t in ('(', '['):
                    if depth == 0:
                        break
                    depth -= 1
                elif depth == 0 and t in {';', '{', '}', ',', '?', ':'} | ASSIGN:
                    break
                lo -= 1
            lv = body[lo:j]
        words = [x.text for x in lv]
        if not words or REFCOUNT & set(words):
            return
        ids = [w for w in words if re.match(r'^[A-Za-z_]\w*$', w)]
        if '->' in words or '[' in words or words[0] == '*' or \
                (words[0] == '(' and '*' in words[:3]):
            n.writes.append(' '.join(words))     # through a pointer
        elif ids and ids[-1] not in local and ids[-1] in n.globals:
            n.writes.append(' '.join(words))     # a file-scope variable

    def file_globals(self, path):
        """Names of a file's file-scope variables."""
        if path not in self._globals:
            tree = self.ni if path.startswith('ni/') else self.tree
            toks = tree.file(path)[0]
            out, depth, stmt = set(), 0, []
            for t in toks:
                if t.text == '{':
                    depth += 1
                elif t.text == '}':
                    depth -= 1
                    stmt = []
                    continue
                if depth:
                    continue
                if t.text == ';':
                    w = [x.text for x in stmt]
                    head = w[:w.index('=')] if '=' in w else w
                    if '(' not in head or head[:2] == ['(', '*']:
                        names = [x for x in head if re.match(r'^[A-Za-z_]\w*$', x)]
                        if names and 'typedef' not in w and 'const' not in head:
                            out.add(names[-1])
                    stmt = []
                else:
                    stmt.append(t)
            self._globals[path] = out
        return self._globals[path]

    # ---- the fixpoints ----

    def solve(self):
        self.python = self.reach(lambda n: [r for r in n.reasons
                                            if r[0] in ('indirect', 'decref')],
                                 'no_python')
        self.blocks = self.reach(lambda n: [r for r in n.reasons
                                            if r[0] == 'blocks'], 'no_block')
        self.impure = self.reach(lambda n: [('write', w) for w in n.writes]
                                 + [('reads', r) for r in n.reads]
                                 + [r for r in n.reasons if r[0] in
                                    ('indirect', 'decref', 'external', 'alloc')],
                                 'pure')
        # a decref doesn't raise: an error in __del__ is printed, not raised
        self.raises = self.reach(
            lambda n: [r for r in n.reasons if r[0] in ('indirect', 'sets_error')]
            + ([('assumed', 'raises')] if 'raises' in
               self.assume.get(n.key.rsplit(':', 1)[1], ()) else []),
            'no_raise')
        self.allocs = self.reach(lambda n: [r for r in n.reasons
                                            if r[0] == 'alloc'], 'pure')

    def reach(self, direct, assumption):
        """{key: witness path} for every node that reaches a direct reason;
        the walk does not go into an ASSUMPTIONS function with assumption."""
        out = {}
        for k, n in self.nodes.items():
            r = direct(n)
            if r and (r[0][0] == 'assumed' or not self.assumed(k, assumption)):
                out[k] = [(k, '%s %s' % r[0])]
        changed = True
        while changed:
            changed = False
            for k, n in self.nodes.items():
                if k in out or self.assumed(k, assumption):
                    continue
                for c, _ in n.calls:
                    if c in out and c != k:
                        out[k] = [(k, 'calls')] + out[c]
                        changed = True
                        break
        return out

    def assumed(self, key, what):
        name = key.rsplit(':', 1)[1]
        a = self.assume.get(name, set())
        return bool(a & {'no_python': {'no_python', 'pure'},
                         'pure': {'pure', 'raises'},
                         'no_raise': {'no_python', 'raises'},
                         'no_block': {'no_block'}}[what])

    def path_text(self, w):
        return ' -> '.join('%s (%s)' % (k.rsplit(':', 1)[1], why)
                           if why != 'calls' else k.rsplit(':', 1)[1]
                           for k, why in w)


# ---- Ni's call sites ----

CALLERS = {'psyco_generic_call', 'Psyco_META1', 'Psyco_META2', 'Psyco_META3',
           'Psyco_META4'}


def flag_values():
    toks, macros = ctokens.tokenize(open(os.path.join(ROOT, 'ni/cf_flags.h')).read())
    vals = {}
    for name, m in macros.items():
        if m.params is None and name.startswith('Cf'):
            vals[name] = m.body

    def ev(name):
        out = 0
        for t in vals[name]:
            if t.kind == 'num':
                out |= int(t.text, 0)
            elif t.text in vals:
                out |= ev(t.text)
        return out
    return {n: ev(n) for n in vals}


def sites(g):
    """[(where, callee words, flags value, path, result tested by the
    caller)] for Ni's residual calls."""
    fv = flag_values()
    out = []
    for path in g.ni_files():
        f = g.ni.file(path)
        if not f:
            continue
        seqs = [('', f[0])] + [(d.name, d.toks) for d in f[1].values() if d.kind == 'macro']
        for macro, ts in seqs:
            for i, t in enumerate(ts):
                if t.text in CALLERS and i + 1 < len(ts) and ts[i + 1].text == '(':
                    try:
                        close = ctokens.match_close(ts, i + 1)
                    except SyntaxError:
                        continue
                    args = ctokens.split_top(ts[i + 2:close], ',')
                    if len(args) < 3 or (macro and macro in CALLERS):
                        continue
                    flags = 0
                    ok = True
                    for x in args[2]:
                        if x.kind == 'id':
                            if x.text in fv:
                                flags |= fv[x.text]
                            else:
                                ok = False
                        elif x.kind == 'num':
                            flags |= int(x.text, 0)
                    before = [x.text for x in ts[max(0, i - 4):i]]
                    out.append(('%s:%d%s' % (path, t.line, ' #define ' + macro if macro else ''),
                                [x.text for x in args[1]], flags if ok else None, path,
                                any(b.startswith('runtime_') for b in before)))
    return out


def fields_of(toks, typedef_name):
    """ctokens.struct_fields, with 'T a, b;' as two fields."""
    for i, t in enumerate(toks):
        if t.text == '}' and i + 1 < len(toks) and toks[i + 1].text == typedef_name:
            depth, j = 0, i
            while True:
                depth += (toks[j].text == '}') - (toks[j].text == '{')
                if depth == 0:
                    break
                j -= 1
            out = []
            for d in ctokens.split_top(toks[j + 1:i], ';'):
                for part in ctokens.split_top(d, ','):
                    ids = [x.text for x in part if x.kind == 'id']
                    if ids:
                        out.append(ids[-1])
            return out
    raise LookupError('no struct ' + typedef_name)


def header_tokens(g, typedef_name):
    """Tokens of the Include/**/*.h that declares 'typedef struct {...}
    typedef_name;' (object.h until 3.7, cpython/object.h from 3.8), object.h
    first; the type slot tables are declared with it."""
    inc = os.path.join(g.tree.root, 'Include')
    paths = sorted(os.path.relpath(os.path.join(d, f), g.tree.root)
                   for d, _, fs in os.walk(inc) for f in fs if f.endswith('.h'))
    for p in ['Include/object.h'] + [p for p in paths if p != 'Include/object.h']:
        toks = g.tree.file(p)[0]
        if any(t.text == '}' and k + 1 < len(toks) and toks[k + 1].text == typedef_name
               for k, t in enumerate(toks)):
            return toks
    raise LookupError('no struct %s in Include/' % typedef_name)


def slot_function(g, words):
    """'PyList_Type . tp_as_sequence -> sq_concat' -> the C function key."""
    alias = {'PyString_Type': 'PyUnicode_Type', 'NiCompatStr_Type': 'PyUnicode_Type'}
    words = [w for w in words if w not in ('&', '(', ')')]
    tp = alias.get(words[0], words[0])
    var = None
    for p in g.files():
        if p.startswith('Objects/') and p.endswith('.c'):
            d = g.tree.find('%s:%s' % (p, tp))
            if d is not None and d.kind == 'var':
                var = (p, tp)
                break
    if var is None:
        return None
    path, name = var
    otoks = header_tokens(g, 'PyTypeObject')
    structs = {'PyTypeObject': None, 'tp_as_number': 'PyNumberMethods',
               'tp_as_sequence': 'PySequenceMethods',
               'tp_as_mapping': 'PyMappingMethods'}
    fields = fields_of(otoks, 'PyTypeObject')
    toks = g.tree.file(path)[0]
    entries = ctokens.initializer(toks, name)
    for w in words[1:]:
        if w in ('.', '->'):
            continue
        if w not in fields:
            return None
        e = entries[fields.index(w)] if fields.index(w) < len(entries) else '0'
        if w in structs:
            sub = e.lstrip('& ').strip()
            if sub == '0':
                return None
            fields = fields_of(otoks, structs[w])
            entries = ctokens.initializer(toks, sub)
            continue
        if e == '0' or not re.match(r'^\w+$', e):
            return None
        return g.resolve(path, e)
    return None


def callee(g, words, path):
    """The key of the function a site calls, or None."""
    if len(words) == 1:
        name = words[0]
        for _ in range(3):                 # Ni's compat macros
            m = g.ni_macros.get(name)
            body = [x.text for x in m.toks[1:]] if m else []
            if len(body) == 1 and re.match(r'^\w+$', body[0]):
                name = body[0]
            else:
                break
        k = g.resolve(path, name)
        for _ in range(3):       # a header's macro that is one call: follow it
            if not k or g.nodes[k].kind == 'function':
                break
            body = [x for x in g.nodes[k].d.toks[1:]]
            if body and body[0].text == '(' and not body[0].sp:
                body = body[ctokens.match_close(body, 0) + 1:]
            if len(body) < 2 or body[0].kind != 'id' or body[1].text != '(' or \
                    ctokens.match_close(body, 1) != len(body) - 1:
                return None
            k = g.resolve(k.rsplit(':', 1)[0], body[0].text)
        return k if k and g.nodes[k].kind == 'function' else None
    if '.' in words:
        return slot_function(g, words)
    return None


def refcounts(src):
    out = {}
    p = os.path.join(src, 'Doc', 'data', 'refcounts.dat')
    if os.path.exists(p):
        for line in open(p):
            f = line.rstrip('\n').split(':')
            if len(f) >= 4 and not line.startswith('#') and f[2] == '':
                out.setdefault(f[0], f[3])
    return out


def compare(g):
    fv = flag_values()
    rc = refcounts(g.src)
    cannot = {e['ni'] for e in T.CANNOT_BLOCK.values()}
    cb_keys = {g.resolve(k.split(':')[0], k.split(':')[1]) for k in T.CANNOT_BLOCK}
    tally = collections.Counter()
    notes = []
    for where, words, flags, path, tested in sites(g):
        text = ' '.join(words)
        k = callee(g, words, path)
        if flags is None:
            tally['flags', 'unproven'] += 1
            continue
        if k is None:
            # a pointer: nothing proven; Ni must assume the worst
            np = bool(flags & (fv['CfPure'] | fv['CfNoPython']))
            tally['no_python', 'disagree' if np else 'agree'] += 1
            tally['pure', 'disagree' if flags & fv['CfPure'] else 'agree'] += 1
            tally['callee', 'unresolved'] += 1
            if np:
                notes.append(('disagree', 'no_python', where, text, 'a call through a pointer'))
            continue
        tally['callee', 'resolved'] += 1
        name = k.rsplit(':', 1)[1]
        # python
        ni_np = bool(flags & (fv['CfPure'] | fv['CfNoPython']))
        py = g.python.get(k)
        if ni_np and py:
            tally['no_python', 'disagree'] += 1
            notes.append(('disagree', 'no_python', where, name, g.path_text(py)))
        elif ni_np or py:
            tally['no_python', 'agree'] += 1
        else:
            tally['no_python', 'cautious'] += 1
            notes.append(('cautious', 'no_python', where, name, 'no Python reached'))
        # pure
        ni_pure = bool(flags & fv['CfPure'])
        imp = g.impure.get(k)
        if ni_pure and imp:
            kind = 'allocator' if k in g.allocs and not py else 'disagree'
            tally['pure', kind] += 1
            notes.append((kind, 'pure', where, name, g.path_text(imp)))
        elif ni_pure or imp:
            tally['pure', 'agree'] += 1
        else:
            tally['pure', 'cautious'] += 1
            notes.append(('cautious', 'pure', where, name, 'nothing impure found'))
        # error convention: does Ni check for an error it may raise
        rtype = flags & fv['CfReturnTypeMask']
        checks = flags & fv['CfPyErrMask'] != fv['CfPyErrDontCheck']
        rs = g.raises.get(k)
        if rs and not checks and tested:
            tally['error', 'agree'] += 1        # the caller tests the result
        elif rs and not checks:
            tally['error', 'disagree'] += 1
            notes.append(('disagree', 'error', where, name,
                          'may raise, flags check nothing: ' + g.path_text(rs)))
        elif checks and not rs:
            tally['error', 'cautious'] += 1
            notes.append(('cautious', 'error', where, name, 'raises nothing'))
        else:
            tally['error', 'agree'] += 1
        # new reference
        if name in rc and rtype == fv['CfReturnTypePtr']:
            new = bool(flags & fv['CfNewRef'])
            if (rc[name] == '+1') == new:
                tally['newref', 'agree'] += 1
            else:
                tally['newref', 'disagree'] += 1
                notes.append(('disagree', 'newref', where, name,
                              'refcounts.dat says %s' % rc[name]))
        # blocking
        ni_nb = bool(flags & fv['CfNoBlock']) or k in cb_keys
        bl = g.blocks.get(k)
        if ni_nb and bl:
            tally['no_block', 'disagree'] += 1
            notes.append(('disagree', 'no_block', where, name, g.path_text(bl)))
        else:
            tally['no_block', 'agree' if (ni_nb or bl) else 'cautious'] += 1
    for key in T.CANNOT_BLOCK:
        k = g.resolve(key.split(':')[0], key.split(':')[1])
        if k is None:
            tally['cannot_block', 'unproven'] += 1
        elif k in g.blocks:
            tally['cannot_block', 'disagree'] += 1
            notes.append(('disagree', 'cannot_block', key, '', g.path_text(g.blocks[k])))
        else:
            tally['cannot_block', 'agree'] += 1
            if k in g.python:
                notes.append(('python', 'cannot_block', key, '', g.path_text(g.python[k])))
    return tally, notes


def analyse(src, ni_root=ROOT):
    g = Graph(src, ni_root)
    g.solve()
    return g


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', required=True)
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--why', nargs='*', help='print the effects of these functions')
    args = ap.parse_args(argv)
    g = analyse(args.src)
    if args.why:
        for name in args.why:
            k = g.globals.get(name) or next((k for k in g.nodes if k.endswith(':' + name)), None)
            if not k:
                print(name, 'not found')
                continue
            print(k)
            for what, m in (('python', g.python), ('blocks', g.blocks),
                            ('impure', g.impure), ('raises', g.raises)):
                print('  %-7s %s' % (what, g.path_text(m[k]) if k in m else 'no'))
        return 0
    tally, notes = compare(g)
    n = len(g.nodes)
    print('%d nodes: %d may run Python, %d may block, %d impure'
          % (n, len(g.python), len(g.blocks), len(g.impure)))
    for dim in ('callee', 'flags', 'no_python', 'pure', 'error', 'newref', 'no_block', 'cannot_block'):
        row = {r: c for (d, r), c in tally.items() if d == dim}
        print('%-12s %s' % (dim, ', '.join('%s %d' % kv for kv in sorted(row.items()))))
    for kind, dim, where, name, why in sorted(notes):
        if kind == 'disagree' or args.verbose:
            print('%-9s %-12s %s %s: %s' % (kind, dim, where, name, why))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
