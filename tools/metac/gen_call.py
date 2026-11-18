#!/usr/bin/env python3
"""The argument binding of a Python call, generated from the C function
that makes and runs a frame (GENERATION step 7, DECISIONS D85).

A policy entry with ('other', 'frame_binding') names such a function
(3.7 and 3.8: ceval.c:_PyEval_EvalCodeWithName). What is generated is the
part between making the frame and running it: the statements after the
frame's locals array is taken (X = f->F, where F names a parameter of Ni's
declaration) up to the first statement that uses the frame otherwise.
Making the frame, the generator case and running it are the core's
(psyfunc.c). The output is one C function the compiler runs while it
compiles a call: it emits no code.

Binding times come from Ni's declaration of the binder (psyfunc.h,
by the usual name: _PyEval_EvalCodeWithName's is _PsycoEval_EvalCodeWithName): a parameter declared vinfo_t ** is an array of the values being
bound (the arguments, the defaults, the closure's cells, the frame's
locals); every other parameter is a C value of the compiler (the code
object, the counts, keyword names). A local takes the binding time of the
values assigned to it. Rules, none about one function:
  - a vinfo_t ** parameter that the C reads as a tuple
    ('((PyTupleObject *)P)->ob_item', the items field of TYPES'
    known_length) is the array of that tuple's items;
  - an expression over C values (fields of the code object, a type test of
    a keyword name, counts) is C in the binder, and so is a test of
    whether a slot of the locals holds a value (GETLOCAL(i) == NULL);
  - Py_INCREF/Py_DECREF of a value are vinfo_incref/vinfo_decref; a call
    of a virtual type's constructor (policy_table.TYPES, D80) is Ni's
    virtual object, within its item limit, its items stored by the C's
    stores into the items field;
  - anything with no form in the compiler (a call that needs the objects,
    such as PyDict_New, PyCell_New, PyObject_RichCompareBool, or an error
    being set) ends the binding there: the binder returns -1 and the
    caller makes the real call, which does the rest (or raises CPython's
    own message). Nothing has been emitted, so stopping anywhere is safe
    (D74's resume, the whole function). A goto to a label outside the
    binding (the C's 'fail') is the same;
  - a C parameter Ni's declaration leaves out may not be read.
Anything else is refused, with the construct, and the build stops.

Run by generate.py; 'python3 gen_call.py --src DIR --pyconfig H' prints
the header.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cparse  # noqa: E402
import ctokens  # noqa: E402
import cpython_src  # noqa: E402
import cslots  # noqa: E402
import gen_abstract  # noqa: E402
import policy_table  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(HERE))
HEADER = 'psyfunc_gen.h'
INCLUDER = 'ni/psyfunc.c'
# where Ni declares the binder, by the usual name (Psyco for Py)
NI_HEADERS = ['ni/psyfunc.h']
REFS = {'Py_INCREF': 'vinfo_incref(%s)', 'Py_XINCREF': 'vinfo_xincref(%s)',
        'Py_DECREF': 'vinfo_decref(%s, po)', 'Py_XDECREF': 'vinfo_xdecref(%s, po)'}
# markers the emitter puts on a declaration's and an assignment's line,
# for unused_dropped
DECL, SET = '\x00D', '\x00S'
INT_WORDS = {'int', 'long', 'Py_ssize_t', 'size_t', 'unsigned', 'short', 'char'}


class Refuse(Exception):
    pass


class NoForm(Exception):
    """A construct with no form in the compiler: the binding stops."""


class V:
    """A value in the binder: bt 'ct' (a C value of the compiler), 'vi' (a
    vinfo_t *), 'ctarr'/'viarr' (an array of either), with its C text; a
    virtual made here keeps its constructor's spec."""

    def __init__(self, bt, c, virt=None):
        self.bt, self.c, self.virt = bt, c, virt


def binding_entries():
    """The keys of the policy entries marked ('other', 'frame_binding')."""
    return [key for key, e in sorted(policy_table.ENTRIES.items())
            if any(it[:2] == ('other', 'frame_binding')
                   for it in e.get('policy', []))]


def is_int(ctype):
    words = ctype.replace('const', '').split()
    return '*' not in ctype and bool(words) and all(w in INT_WORDS for w in words)


class Binder:
    def __init__(self, source, func, decl):
        self.s = source
        self.f = func
        self.decl = decl
        self.vspecs = gen_abstract.virtual_specs()
        self.items_fields = {}     # struct -> its known_length items field
        for key, t in policy_table.TYPES.items():
            if 'known_length' in t:
                self.items_fields[key.split(':')[1]] = t['known_length']['items']
        self.params = {}           # Ni parameter -> V
        for ctype, name in decl.params:
            if name == 'po':
                continue
            if 'vinfo_t' in ctype:
                self.params[name] = V('viarr' if ctype.count('*') == 2 else 'vi', name)
            else:
                self.params[name] = V('ctarr' if ctype.count('*') == 2 else 'ct', name)
        self.cparams = {n for _, n in func.params}
        self.vars = {}             # local -> (bt, C type)
        self.labels = set()
        self.virtuals = {}         # local -> the spec of the virtual it holds
        self.decl_names = {name for node in self.decls_in(func.ast)
                           for _, name, *rest in node[1]}
        self.pre = []              # lines to emit before the next statement

    # ---- the slice ------------------------------------------------------------
    def frame_var(self):
        frames = [name for s in self.f.ast if s[0] == 'decl'
                  for ctype, name, _, *rest in s[1]
                  if ctype.replace('struct ', '') in ('PyFrameObject *', '_frame *')]
        if len(frames) != 1:
            raise Refuse('%s: no one frame variable (%r)' % (self.f.name, frames))
        return frames[0]

    def slice(self):
        """(prefix declarations, the binding's statements)."""
        fv = self.fv = self.frame_var()
        body = self.f.ast
        start = end = None
        for k, s in enumerate(body):
            uses = self.frame_uses(s, fv)
            if start is None:
                if uses and all(u in self.params for u in uses):
                    start = k
            elif uses and not all(u in self.params for u in uses):
                end = k
                break
        if start is None or end is None:
            raise Refuse('%s: no statements between the frame made and used'
                         % self.f.name)
        prefix = [s for s in body[:start] if s[0] == 'decl']
        return prefix, body[start:end]

    def frame_uses(self, node, fv):
        """The fields read of the frame variable in a node; '' for a use
        of the frame itself."""
        out = []

        def walk(e):
            if isinstance(e, tuple):
                if e[:1] == ('arrow',) and e[1] == ('id', fv):
                    out.append(e[2])
                    return
                if e == ('id', fv):
                    out.append('')
                    return
                for x in e[1:]:
                    walk(x)
            elif isinstance(e, list):
                for x in e:
                    walk(x)
        walk(node)
        return out

    # ---- generation -----------------------------------------------------------
    def run(self):
        prefix, stmts = self.slice()
        used = set(cparse.names_in(stmts))
        self.collect_labels(stmts)
        self.infer(prefix, stmts)
        out = []
        for s in prefix:
            for ctype, name, init, *rest in s[1]:
                if name not in used:
                    continue
                bt, ct = self.vars[name]
                if bt == 'none':
                    continue
                if init is None:
                    out.append('  %s %s;%s%s' % (ct, name, DECL, name))
                    continue
                try:
                    v = self.ev(init)
                except NoForm as e:
                    raise Refuse('%s: %s needed by the binding: %s'
                                 % (self.f.name, name, e))
                out.append('  %s %s = %s;%s%s' % (ct, name, v.c, DECL, name))
        self.out = out
        self.block(stmts, 1)
        params = ', '.join('%s %s' % (t, n) for t, n in self.decl.params)
        head = ['DEFINEFN', '%s %s(%s) {' % (self.decl.ret, self.decl.name, params)]
        return '\n'.join(head + self.unused_dropped(out) + ['  return 0;', '}']) + '\n'

    def unused_dropped(self, lines):
        """The lines without the locals nothing reads in their scope (those
        only the C's own work, now the real call's, used): their
        declarations and the assignments to them. Declarations are marked
        DECL + name, assignments SET + name, by the emitter."""
        ident = re.compile(r'[A-Za-z_]\w*')

        def indent(line):
            return len(line) - len(line.lstrip(' '))

        def mark(line, tag):
            if tag not in line:
                return None
            return line.split(tag)[1].split(DECL)[0].split(SET)[0]
        changed = True
        while changed:
            changed = False
            for k, line in enumerate(lines):
                name = mark(line, DECL)
                if name is None:
                    continue
                d = indent(line)
                end = k + 1
                while end < len(lines) and not (indent(lines[end]) < d
                                                and lines[end].strip().startswith('}')):
                    end += 1
                reads = [j for j in range(k + 1, end) if mark(lines[j], SET) != name
                         and name in ident.findall(lines[j].split('/*')[0].split(DECL)[0])]
                if not reads:
                    lines[k:end] = [x for j, x in enumerate(lines[k:end])
                                    if j > 0 and mark(x, SET) != name]
                    changed = True
                    break
        return [line.split(DECL)[0].split(SET)[0] for line in lines]

    def collect_labels(self, node):
        if isinstance(node, tuple):
            if node[0] == 'label':
                self.labels.add(node[1])
            for x in node[1:]:
                self.collect_labels(x)
        elif isinstance(node, list):
            for x in node:
                self.collect_labels(x)

    def ctype_for(self, bt, ctype):
        return {'vi': 'vinfo_t *', 'viarr': 'vinfo_t **'}.get(bt, ctype)

    def infer(self, prefix, stmts):
        """Each local's binding time: an integer is a C value; a pointer
        takes the binding time of what is assigned to it (repeated until
        nothing changes; a construct with no form doesn't count)."""
        decls = {}
        assigns = []
        for node in prefix + self.decls_in(stmts):
            for ctype, name, init, *rest in node[1]:
                if name in decls and decls[name][0] != ctype:
                    raise Refuse('%s: %s declared as %s and %s'
                                 % (self.f.name, name, decls[name][0], ctype))
                decls[name] = (ctype, init)
                if is_int(ctype):
                    self.vars[name] = ('ct', ctype)
                if init is not None:
                    assigns.append((name, init))
        self.assigns_in(stmts, assigns)
        for _ in range(8):
            changed = False
            for name, rhs in assigns:
                if name not in decls or name in self.vars and \
                        self.vars[name][0] != 'unknown':
                    continue
                try:
                    v = self.ev(rhs, infer=True)
                except (NoForm, KeyError):
                    continue
                except Refuse:
                    # unreadable (a GNU statement expression in 3.8's
                    # _PyThreadState_GET): no form either
                    continue
                self.vars[name] = (v.bt, self.ctype_for(v.bt, decls[name][0]))
                changed = True
            if not changed:
                break
        for name, (ctype, init) in decls.items():
            if name not in self.vars:
                # only ever assigned what has no form: reading it is the
                # real call (3.8's tstate, read only by error calls)
                self.vars[name] = ('none', ctype)

    def decls_in(self, node, out=None):
        if out is None:
            out = []
        if isinstance(node, tuple):
            if node[0] == 'decl':
                out.append(node)
            for x in node[1:]:
                self.decls_in(x, out)
        elif isinstance(node, list):
            for x in node:
                self.decls_in(x, out)
        return out

    def assigns_in(self, node, out):
        if isinstance(node, tuple):
            if node[0] == 'assign' and node[1] == '=' and node[2][0] == 'id':
                out.append((node[2][1], node[3]))
            for x in node[1:]:
                self.assigns_in(x, out)
        elif isinstance(node, list):
            for x in node:
                self.assigns_in(x, out)

    # ---- statements -------------------------------------------------------------
    def emit(self, depth, text):
        # a constructor's limit is tested before the statement making it
        for line in self.pre:
            self.out.append('  ' * depth + line)
        self.pre = []
        self.out.append('  ' * depth + text)

    def stop(self, depth, why):
        self.emit(depth, 'return -1; /* %s: the real call */' % why)

    def block(self, stmts, depth):
        """Statements in order; after one that always leaves, only a label
        is reached again."""
        dead = False
        for s in stmts:
            if dead and not self.has_label(s):
                continue
            dead = self.stmt(s, depth)

    def has_label(self, node):
        found = []
        self.collect_into(node, found)
        return bool(found)

    def collect_into(self, node, found):
        if isinstance(node, tuple):
            if node[0] == 'label':
                found.append(node[1])
            for x in node[1:]:
                self.collect_into(x, found)
        elif isinstance(node, list):
            for x in node:
                self.collect_into(x, found)

    def stmt(self, s, depth):
        """Emit one statement; True when it always leaves (no path falls
        through to the next)."""
        k = s[0]
        if k == 'block':
            self.emit(depth, '{')
            self.block(s[1], depth + 1)
            self.emit(depth, '}')
            return False
        if k == 'empty':
            return False
        if k == 'decl':
            for ctype, name, init, *rest in s[1]:
                bt, ct = self.vars[name]
                if bt == 'none':
                    continue
                if init is None:
                    self.emit(depth, '%s %s;%s%s' % (ct, name, DECL, name))
                    continue
                try:
                    v = self.ev(init)
                except NoForm as e:
                    self.emit(depth, '%s %s;%s%s' % (ct, name, DECL, name))
                    self.stop(depth, str(e))
                    return True
                self.check_bt(name, v)
                pure = SET + name if v.bt == 'ct' else ''
                self.emit(depth, '%s %s = %s;%s%s%s' % (ct, name, v.c, DECL, name, pure))
            return False
        if k == 'expr':
            return self.expr_stmt(s[1], depth)
        if k == 'if':
            try:
                c = self.cond(s[1])
            except NoForm as e:
                self.stop(depth, str(e))
                return True
            self.emit(depth, 'if (%s) {' % c)
            a = self.sub(s[2], depth + 1)
            if s[3] is not None:
                self.emit(depth, '} else {')
                b = self.sub(s[3], depth + 1)
                self.emit(depth, '}')
                return a and b
            self.emit(depth, '}')
            return False
        if k == 'for':
            init, cnd, steps, body = s[1], s[2], s[3], s[4]
            parts = []
            try:
                if init is None:
                    parts.append('')
                elif init[0] == 'expr':
                    parts.append(self.ev(init[1]).c)
                else:
                    raise Refuse('a declaration in a for head')
                parts.append(self.cond(cnd) if cnd is not None else '')
                parts.append(', '.join(self.ev(x).c for x in steps))
            except NoForm as e:
                self.stop(depth, str(e))
                return True
            self.emit(depth, 'for (%s; %s; %s) {' % tuple(parts))
            self.sub(s[4], depth + 1)
            self.emit(depth, '}')
            return False
        if k == 'do':
            try:
                c = self.cond(s[2])
            except NoForm as e:
                raise Refuse('a do loop whose condition has no form: %s' % e)
            self.emit(depth, 'do {')
            self.sub(s[1], depth + 1)
            self.emit(depth, '} while (%s);' % c)
            return False
        if k == 'while':
            try:
                c = self.cond(s[1])
            except NoForm as e:
                self.stop(depth, str(e))
                return True
            self.emit(depth, 'while (%s) {' % c)
            self.sub(s[2], depth + 1)
            self.emit(depth, '}')
            return False
        if k == 'goto':
            if s[1] in self.labels:
                self.emit(depth, 'goto %s;' % s[1])
            else:
                self.stop(depth, 'goto %s' % s[1])
            return True
        if k == 'label':
            self.emit(depth - 1, '%s:' % s[1])
            return self.stmt(s[2], depth)
        if k in ('continue', 'break'):
            self.emit(depth, '%s;' % k)
            return True
        if k == 'return':
            raise Refuse('%s: a return inside the binding' % self.f.name)
        raise Refuse('%s: statement %s' % (self.f.name, k))

    def sub(self, s, depth):
        stmts = s[1] if s[0] == 'block' else [s]
        dead = False
        for x in stmts:
            if dead and not self.has_label(x):
                continue
            dead = self.stmt(x, depth)
        return dead

    def check_bt(self, name, v):
        bt = self.vars[name][0]
        if bt != v.bt:
            raise Refuse('%s: %s is %s but assigned a %s value'
                         % (self.f.name, name, bt, v.bt))

    def expr_stmt(self, e, depth):
        try:
            if e[0] == 'call' and e[1][0] == 'id':
                name = e[1][1]
                if name == 'assert':
                    return False
                if name in REFS:
                    v = self.ev(e[2][0])
                    if v.bt != 'vi':
                        raise Refuse('%s of a C value %s' % (name, v.c))
                    self.emit(depth, REFS[name] % v.c + ';')
                    return False
                if self.is_macro(name):
                    return self.expr_stmt(self.expand(name, e[2]), depth)
                raise NoForm(name)
            if e[0] == 'assign':
                lhs, rhs = e[2], e[3]
                if lhs[0] == 'id' and lhs[1] in self.vars:
                    v = self.ev(rhs)
                    if e[1] == '=':
                        self.check_bt(lhs[1], v)
                    elif v.bt != 'ct' or self.vars[lhs[1]][0] != 'ct':
                        raise Refuse('%s on a value' % e[1])
                    if v.virt is not None:
                        self.virtuals[lhs[1]] = v.virt
                    # an assignment of a C value can go with its variable
                    pure = SET + lhs[1] if v.bt == 'ct' and e[1] == '=' else ''
                    self.emit(depth, '%s %s %s;%s' % (lhs[1], e[1], v.c, pure))
                    return False
                if lhs[0] == 'id' and lhs[1] in self.params and e[1] != '=':
                    v = self.ev(rhs)
                    if v.bt != 'ct' or self.params[lhs[1]].bt != 'ct':
                        raise Refuse('%s on a value' % e[1])
                    self.emit(depth, '%s %s %s;' % (lhs[1], e[1], v.c))
                    return False
                if e[1] != '=':
                    raise Refuse('%s into %s' % (e[1], cparse.c_expr(lhs)))
                place = self.place(lhs)
                v = self.ev(rhs)
                if v.bt != 'vi':
                    raise Refuse('a store of a C value into %s' % place)
                self.emit(depth, '%s = %s;' % (place, v.c))
                return False
            if e[0] in ('post', 'pre'):
                v = self.ev(e[2])
                if v.bt != 'ct':
                    raise Refuse('%s of a value' % e[1])
                self.emit(depth, cparse.c_expr(e) + ';')
                return False
        except NoForm as x:
            self.stop(depth, str(x))
            return True
        raise Refuse('%s: expression statement %s' % (self.f.name, cparse.c_expr(e)))

    def place(self, lhs):
        """The vinfo_t * slot an object store writes: an item of an array
        of values (the frame's locals) or of a virtual made here."""
        if lhs[0] != 'index':
            raise Refuse('a store into %s' % cparse.c_expr(lhs))
        a = self.ev(lhs[1])
        i = self.ev(lhs[2])
        if i.bt != 'ct':
            raise Refuse('an index that is a value')
        if a.bt == 'viarr':
            return '%s[%s]' % (a.c, i.c)
        if a.bt == 'items':
            return '%s(%s, %s)' % (a.virt['item'], a.c, i.c)
        raise Refuse('a store into %s' % cparse.c_expr(lhs))

    def cond(self, e):
        v = self.ev(e)
        if v.bt != 'ct':
            raise Refuse('a test of a value: %s' % cparse.c_expr(e))
        return v.c

    # ---- expressions ------------------------------------------------------------
    def is_macro(self, name):
        m = self.s.macros.get(name)
        return m is not None and m.params is not None \
            and name not in gen_abstract.KEEP

    def expand(self, name, args):
        """A header macro's call, expanded and read again."""
        m = self.s.macros[name]
        argtoks = [ctokens.tokenize(cparse.c_expr(a))[0] for a in args]
        if len(argtoks) != len(m.params):
            raise Refuse('macro %s with %d arguments' % (name, len(args)))
        env = dict(zip(m.params, argtoks))
        body = []
        for t in m.body:
            body.extend(env.get(t.text, [t]) if t.kind == 'id' else [t])
        body = cparse.expand(body, self.s.macros, gen_abstract.KEEP | {name})
        # 'assert(...),' inside an expression (3.7's PyTuple_GET_SIZE):
        # NDEBUG's assert is nothing
        k = 0
        while k < len(body):
            if body[k].text == 'assert' and k + 1 < len(body) \
                    and body[k + 1].text == '(':
                close = ctokens.match_close(body, k + 1)
                if close + 1 < len(body) and body[close + 1].text == ',':
                    del body[k:close + 2]
                    continue
            k += 1
        try:
            return cparse.parse_expr(body, self.s.decls.types, loops=True)
        except cparse.ParseError as e:
            raise Refuse('macro %s: %s' % (name, e))

    def ev(self, e, infer=False):
        k = e[0]
        if k == 'num':
            return V('ct', e[1])
        if k == 'str':
            return V('ct', ' '.join(e[1]))
        if k == 'id':
            name = e[1]
            if name == 'NULL':
                return V('ct', 'NULL')
            if name in self.vars:
                bt = self.vars[name][0]
                if bt == 'unknown':
                    raise KeyError(name)
                if bt == 'none':
                    raise NoForm('%s (no form)' % name)
                return V(bt, name, self.virtuals.get(name))
            if name in self.params:
                return self.params[name]
            if name in self.cparams:
                raise Refuse('%s reads %s, which Ni\'s %s does not pass'
                             % (self.f.name, name, self.decl.name))
            if infer and name in self.decl_names:
                raise KeyError(name)
            return V('ct', name)     # a constant or a global of the compiler
        if k == 'cast':
            v = self.ev(e[2], infer)
            if v.bt == 'ct':
                return V('ct', '((%s)%s)' % (e[1], v.c))
            return V(v.bt, v.c, v.virt)
        if k == 'arrow':
            if e[1] == ('id', self.fv):
                if e[2] not in self.params:
                    raise Refuse('the frame\'s %s, not a parameter of Ni\'s %s'
                                 % (e[2], self.decl.name))
                return self.params[e[2]]     # the frame's locals: Ni's array
            v = self.ev(e[1], infer)
            struct = self.struct_of(e[1])
            if v.bt == 'ct':
                return V('ct', '%s->%s' % (v.c, e[2]))
            if struct is not None and self.items_fields.get(struct) == e[2]:
                if v.bt == 'viarr':          # a tuple passed as its items
                    return V('viarr', v.c)
                if v.bt == 'vi' and v.virt is not None:
                    return V('items', v.c, v.virt)
            raise NoForm('the field %s of an object' % e[2])
        if k == 'index':
            a = self.ev(e[1], infer)
            i = self.ev(e[2], infer)
            if i.bt != 'ct':
                raise Refuse('an index that is a value')
            if a.bt == 'ctarr' or a.bt == 'ct':
                return V('ct', '%s[%s]' % (a.c, i.c))
            if a.bt == 'viarr':
                return V('vi', '%s[%s]' % (a.c, i.c))
            if a.bt == 'items':
                return V('vi', '%s(%s, %s)' % (a.virt['item'], a.c, i.c))
            raise Refuse('an index into %s' % a.bt)
        if k == 'un':
            v = self.ev(e[2], infer)
            if v.bt == 'ct':
                return V('ct', '%s(%s)' % (e[1], v.c))
            if e[1] == '!' and v.bt == 'vi':
                return V('ct', '!(%s)' % v.c)
            raise NoForm('%s of an object' % e[1])
        if k == 'bin':
            a = self.ev(e[2], infer)
            b = self.ev(e[3], infer)
            if a.bt == 'ct' and b.bt == 'ct':
                return V('ct', '(%s %s %s)' % (a.c, e[1], b.c))
            if e[1] in ('==', '!=') and 'vi' in (a.bt, b.bt) and \
                    'NULL' in (a.c, b.c):
                # whether a slot holds a value is known when Ni compiles
                return V('ct', '(%s %s %s)' % (a.c, e[1], b.c))
            if e[1] in ('+', '-') and a.bt == 'viarr' and b.bt == 'ct':
                return V('viarr', '(%s %s %s)' % (a.c, e[1], b.c))
            if e[1] in ('&&', '||') and a.bt == b.bt == 'ct':
                return V('ct', '(%s %s %s)' % (a.c, e[1], b.c))
            raise NoForm('%s on objects' % e[1])
        if k == 'cond':
            c = self.ev(e[1], infer)
            a = self.ev(e[2], infer)
            b = self.ev(e[3], infer)
            if c.bt == a.bt == b.bt == 'ct':
                return V('ct', '(%s ? %s : %s)' % (c.c, a.c, b.c))
            raise NoForm('a choice between objects')
        if k == 'call':
            if e[1][0] != 'id':
                raise NoForm('a call through a pointer')
            name = e[1][1]
            if name in self.vspecs:
                return self.construct(name, e[2], infer)
            if self.is_macro(name):
                return self.ev(self.expand(name, e[2]), infer)
            raise NoForm(name)
        if k in ('pre', 'post'):
            v = self.ev(e[2], infer)
            if v.bt != 'ct':
                raise Refuse('%s of a value' % e[1])
            return V('ct', cparse.c_expr(e))
        if k == 'assign':
            # a loop head's 'i = 0' or 'i += kwstep': C ints of the compiler
            lhs = self.ev(e[2], infer)
            rhs = self.ev(e[3], infer)
            if lhs.bt != 'ct' or rhs.bt != 'ct' or e[2][0] != 'id':
                raise Refuse('an assignment of a value inside an expression')
            return V('ct', '(%s %s %s)' % (lhs.c, e[1], rhs.c))
        if k == 'sizeof':
            return V('ct', 'sizeof(%s)' % e[1] if not e[1].startswith('sizeof')
                     else e[1])
        raise Refuse('%s: expression %s' % (self.f.name, k))

    def struct_of(self, e):
        """The struct a cast names: ((PyTupleObject *)x)->... ."""
        if e[0] == 'cast':
            return e[1].replace('const ', '').rstrip(' *').replace('struct ', '').strip()
        return None

    def construct(self, name, args, infer):
        """A virtual type's constructor (D80): Ni's 'new', for an array
        type of a size the compiler knows and within the type's limit (else
        the binding stops: the real call builds it)."""
        spec = self.vspecs[name]
        vals = [self.ev(a, infer) for a in args]
        if not spec['array'] or len(vals) != 1 or vals[0].bt != 'ct':
            raise NoForm('%s of a value' % name)
        virt = {'item': spec['item']}
        size = vals[0].c
        limit = spec.get('max_items')
        if not infer and limit and not size.isdigit():
            # the limit's value from the table: Ni's name is local to its file
            self.pre += ['if (%s > %d) /* %s */' % (size, policy_table.LIMITS[limit][1], limit),
                         '  return -1; /* a %s of more than that: the real'
                         ' call */' % spec['type']]
        return V('vi', '%s(%s, NULL)' % (spec['new'], size), virt)


def generate(src, pyconfig, defines, ni_root=ROOT):
    out, report = [], []
    decls = gen_abstract.ni_decls(ni_root, NI_HEADERS)
    for key in binding_entries():
        cfile, fname = key.split(':')
        source = gen_abstract.Source(src, pyconfig, defines, cfile)
        f = source.funcs.get(fname)
        if f is None or f.ast is None:
            raise Refuse('%s: not read (%s)' % (key, f.why if f else 'missing'))
        dname = gen_abstract.psyco_name(fname)
        if dname not in decls:
            raise Refuse('%s: Ni declares no %s (%s)' % (key, dname,
                                                        ', '.join(NI_HEADERS)))
        b = Binder(source, f, decls[dname])
        out.append('/* the argument binding of %s, between the frame made and'
                   ' run */' % key)
        out.append(b.run())
        report.append('generated  %-32s %s' % (fname, dname))
    head = ['/* GENERATED by tools/metac/gen_call.py from CPython %s:'
            % cpython_src.source_version(src),
            '   do not edit. Included by %s. */' % INCLUDER, '']
    return '\n'.join(head + out), report


def build(target):
    try:
        text, report = generate(target.src, target.pyconfig,
                                gen_abstract.target_defines(target.python))
    except (Refuse, cparse.ParseError) as e:
        raise cslots.GenError(str(e))
    return text, report


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--src', required=True)
    ap.add_argument('--pyconfig', required=True)
    ap.add_argument('--python', default=None)
    args = ap.parse_args(argv)
    defines = gen_abstract.target_defines(args.python) if args.python \
        else ['Py_BUILD_CORE', 'NDEBUG']
    text, report = generate(args.src, args.pyconfig, defines)
    print(text)
    print('\n'.join(report), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
