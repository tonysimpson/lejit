"""Scalar code in the general translator (doc/METACOMPILER.md "Scalar
code").

Ni's backend has no floating-point instructions, so a C double is a word
holding its bits: a vinfo the meta passes to C helpers. Three rules, used by
gen_abstract.py's Walk (ScalarRules is mixed into it), none of them about
one function:

  - representation (policy_table.TYPES, a 'double' section): an object of
    that struct holds a scalar in a field; a read of the field is Ni's
    'load', a call of one of the 'constructors' given a scalar is Ni's
    'new' (a virtual object holding it);
  - copy-through: a maximal run of statements
    that read and write only C scalars (locals of the representation's C
    type, integers, constants, the pure functions of
    policy_table.EXTERNAL, raising an exception) becomes one C helper the
    meta calls: CfPure, so it folds on constants, or CfPyErrCheck when it
    can raise. The run's inputs are its arguments, its one result is
    returned; where the run ends the function (every path returns a
    constructor of a scalar or an error) the helper returns the scalar and
    the meta builds the object;
  - inlining: a static function called with the address of a local (an
    out-parameter) or with a function's name is its body at the call, the
    parameters replaced by the arguments, so the out-parameter is the
    caller's variable and the function argument is the function called
    ('return f(...)', and 'if (f(...) OP K)' where f returns constants).

Anything else is left to the walk as before.
"""
import cparse
import ctokens
import policy_table


class Same:
    """An object in a cache key, equal only to itself. It holds the object,
    so its id can't be reused while the key lives: a key of a bare id() can
    match a temporary made later at the same address, which changes the
    copies a run makes and the names they get."""
    __slots__ = ('obj',)

    def __init__(self, obj):
        self.obj = obj

    def __eq__(self, other):
        return isinstance(other, Same) and other.obj is self.obj

    def __hash__(self):
        return id(self.obj)

STMT_HEADS = {'block', 'if', 'return', 'goto', 'empty', 'expr', 'decl',
              'label', 'unsupported', 'for', 'while', 'do', 'switch', 'case',
              'default', 'break', 'continue'}
# 'Py_INCREF(K); return K;' as object.h writes it for these
SINGLETON_RETURNS = {'Py_NotImplemented': 'Py_RETURN_NOTIMPLEMENTED',
                     'Py_None': 'Py_RETURN_NONE', 'Py_True': 'Py_RETURN_TRUE',
                     'Py_False': 'Py_RETURN_FALSE'}
INL = ('inl',)          # the span of a statement that came from another function


class NoInline(Exception):
    pass


# ---- inlining static functions (rule inline_static) -------------------------

def inline_calls(source):
    """Inline, in every function of the source, the calls rule
    inline_static names; f.ast is replaced."""
    for f in source.funcs.values():
        if f.ast is None:
            continue
        for _ in range(8):
            ast, changed = _stmts(source, f, f.ast)
            f.ast = ast
            if not changed:
                break


def _stmts(source, f, stmts):
    out, changed = [], False
    for s in stmts:
        r = _stmt(source, f, s)
        if r is not None:
            out.append(r)
            changed = True
            continue
        k = s[0]
        if k == 'block':
            b, c = _stmts(source, f, s[1])
            out.append(('block', b, s[2]))
            changed |= c
        elif k == 'if':
            t, c1 = _stmts(source, f, [s[2]])
            e, c2 = _stmts(source, f, [s[3]]) if s[3] is not None else ([None], False)
            out.append(('if', s[1], t[0], e[0], s[4]))
            changed |= c1 or c2
        else:
            out.append(s)
    return out, changed


def _inlinable(source, f, e):
    """(callee, args) when e calls a static function rule inline_static
    takes."""
    if not (isinstance(e, tuple) and e[0] == 'call' and e[1][0] == 'id'):
        return None
    g = source.funcs.get(e[1][1])
    if g is None or not g.static or g.ast is None or g is f:
        return None
    args = e[2]
    if len(args) != len(g.params):
        return None
    local = set(local_names(f.ast)) | {p for _, p in f.params}
    if not any(_out_arg(a) is not None or (a[0] == 'id' and a[1] not in local
                                           and is_function(source, a[1]))
               for a in args):
        return None
    return g, args


def is_function(source, name):
    return name in source.funcs or (name in source.decls.protos
                                    and name not in source.decls.vars)


def _out_arg(a):
    """x when a is '&x' (through parentheses)."""
    if a[0] == 'un' and a[1] == '&' and a[2][0] == 'id':
        return a[2]
    return None


def _stmt(source, f, s):
    """The replacement of statement s, or None."""
    try:
        if s[0] == 'return' and s[1] is not None:
            m = _inlinable(source, f, s[1])
            if m is not None:
                g, args = m
                if g.ret != f.ret:
                    return None
                body = _body(source, f, g, args, None)
                return ('block', body, INL)
        if s[0] == 'if' and s[1][0] == 'bin' and s[1][1] in cparse_ops():
            m = _inlinable(source, f, s[1][2])
            k = const_int(s[1][3])
            if m is not None and k is not None:
                g, args = m
                body = _body(source, f, g, args, (s[1][1], k, s[2], s[3]))
                return ('block', body, INL)
        if s[0] == 'if' and s[1][0] == 'call':
            # 'if (g(...))' is 'if (g(...) != 0)'
            m = _inlinable(source, f, s[1])
            if m is not None:
                g, args = m
                body = _body(source, f, g, args, ('!=', 0, s[2], s[3]))
                return ('block', body, INL)
    except NoInline:
        return None
    return None


def cparse_ops():
    return ('<', '<=', '>', '>=', '==', '!=')


def const_int(e):
    if e[0] == 'num' and e[1].isdigit():
        return int(e[1])
    if e[0] == 'un' and e[1] == '-' and e[2][0] == 'num' and e[2][1].isdigit():
        return -int(e[2][1])
    return None


def local_names(ast):
    out = []

    def walk(n):
        if isinstance(n, tuple) and n and n[0] == 'decl':
            out.extend(d[1] for d in n[1])
        if isinstance(n, (tuple, list)):
            for x in n:
                if isinstance(x, (tuple, list)):
                    walk(x)
    walk(ast)
    return out


def assigned(ast):
    out = set()

    def walk(n):
        if isinstance(n, tuple) and n:
            if n[0] == 'assign' and n[2][0] == 'id':
                out.add(n[2][1])
            if n[0] in ('pre', 'post') and n[2][0] == 'id':
                out.add(n[2][1])
        if isinstance(n, (tuple, list)):
            for x in n:
                if isinstance(x, (tuple, list)):
                    walk(x)
    walk(ast)
    return out


def _body(source, f, g, args, test):
    """g's statements with its parameters bound to args. test: None for
    'return g(...)', else (op, K, S1, S2) of 'if (g(...) op K) S1 else S2'."""
    ptr, env = {}, {}
    mine = assigned(g.ast)
    for (ctype, p), a in zip(g.params, args):
        x = _out_arg(a)
        if x is not None:
            ptr[p] = x
        elif p in mine:
            raise NoInline('%s assigns its parameter %s' % (g.name, p))
        elif a[0] in ('id', 'num', 'str') or (a[0] == 'un' and a[1] == '-'
                                              and a[2][0] == 'num') \
                or (a[0] == 'cast' and a[2][0] == 'id'):
            env[p] = a
        else:
            raise NoInline('%s: argument %s' % (g.name, cparse.c_expr(a)))
    rename = {n: '%s_%s' % (g.name, n) for n in local_names(g.ast)}
    body = list(g.ast)
    # a local that only copies a parameter's value is that value
    for k, s in enumerate(body):
        if s[0] == 'decl' and len(s[1]) == 1 and s[1][0][2] is not None \
                and not s[1][0][3]:
            name, init = s[1][0][1], s[1][0][2]
            if name in mine:
                continue
            if init[0] == 'un' and init[1] == '*' and init[2][0] == 'id' \
                    and init[2][1] in ptr:
                env[name] = ptr[init[2][1]]
            elif init[0] == 'id' and init[1] in env:
                env[name] = env[init[1]]
            else:
                continue
            rename.pop(name, None)
            body[k] = ('empty', s[2])

    def sub(n):
        if isinstance(n, list):
            return [sub(x) for x in n]
        if not isinstance(n, tuple) or not n or not isinstance(n[0], str):
            return n
        h = n[0]
        if h == 'un' and n[1] == '*' and n[2][0] == 'id' and n[2][1] in ptr:
            return ptr[n[2][1]]
        if h == 'id':
            if n[1] in ptr:
                raise NoInline('%s uses the pointer %s' % (g.name, n[1]))
            if n[1] in env:
                return env[n[1]]
            if n[1] in rename:
                return ('id', rename[n[1]])
            return n
        if h == 'call' and n[1][0] == 'un' and n[1][1] == '*':
            callee = sub(n[1][2])
            if callee[0] == 'id' and is_function(source, callee[1]):
                return ('call', callee, sub(n[2]))
        if h == 'decl':
            return ('decl', [(c, rename.get(nm, nm), sub(i) if i is not None else None, d)
                             for c, nm, i, d in n[1]], INL)
        if h in STMT_HEADS:
            return tuple([h] + [sub(x) for x in n[1:-1]] + [INL])
        return tuple([h] + [sub(x) for x in n[1:]])
    body = [sub(s) for s in body]
    if test is not None:
        body = _returns(body, test, ptr, True)
    return singletons(body)


def _returns(stmts, test, ptr, tail):
    """Each 'return C' of an inlined body as the caller's test chooses:
    S1 or S2 in its place. A choice that does not end the function must be
    the body's last statement on its path."""
    op, k, s1, s2 = test
    out = []
    last = max((j for j, s in enumerate(stmts) if s[0] != 'empty'), default=-1)
    for j, s in enumerate(stmts):
        at_end = tail and j >= last
        h = s[0]
        if h == 'return':
            c = const_int(s[1]) if s[1] is not None else None
            if c is None:
                raise NoInline('a return of %s' % cparse.c_expr(s[1]))
            chosen = s1 if _holds(c, op, k) else s2
            if chosen is not None and chosen[0] == 'return' and out \
                    and out[-1][0] == 'expr' and out[-1][1][0] == 'assign' \
                    and out[-1][1][1] == '=' and chosen[1] is not None \
                    and out[-1][1][2] == chosen[1] and chosen[1] in ptr.values():
                # '*p = E; return C;' answered 'return x;' (x the caller's
                # variable p points to): 'return E;'
                e = out.pop()[1][3]
                out.append(('return', e, INL))
                continue
            if chosen is None or not ends(chosen):
                if not at_end:
                    raise NoInline('a return before the end chooses a statement'
                                   ' that does not leave the function')
                if chosen is not None:
                    out.append(chosen)
                continue
            out.append(chosen)
            continue
        if h == 'block':
            out.append(('block', _returns(s[1], test, ptr, at_end), s[2]))
            continue
        if h == 'if':
            t = _returns([s[2]], test, ptr, at_end)
            e = _returns([s[3]], test, ptr, at_end) if s[3] is not None else None
            out.append(('if', s[1], _one(t), _one(e) if e is not None else None, s[4]))
            continue
        if h in ('for', 'while', 'do', 'switch', 'label', 'goto'):
            raise NoInline('a %s in an inlined test' % h)
        out.append(s)
    return out


def _one(stmts):
    return stmts[0] if len(stmts) == 1 else ('block', stmts, INL)


def _holds(c, op, k):
    return {'<': c < k, '<=': c <= k, '>': c > k, '>=': c >= k,
            '==': c == k, '!=': c != k}[op]


def ends(s):
    """Does every path through s leave the function?"""
    h = s[0]
    if h == 'return':
        return True
    if h == 'expr' and s[1][0] == 'id' and s[1][1] in SINGLETON_RETURNS.values():
        return True
    if h == 'block':
        return any(ends(x) for x in s[1])
    if h == 'if':
        return s[3] is not None and ends(s[2]) and ends(s[3])
    return False


def singletons(stmts):
    """'Py_INCREF(K); return K;' is object.h's Py_RETURN_ for K."""
    out = []
    for s in stmts:
        if s[0] == 'block':
            s = ('block', singletons(s[1]), s[2])
        elif s[0] == 'if':
            t = singletons([s[2]])
            e = singletons([s[3]]) if s[3] is not None else None
            s = ('if', s[1], _one(t), _one(e) if e is not None else None, s[4])
        if s[0] == 'return' and s[1] is not None and s[1][0] == 'id' \
                and s[1][1] in SINGLETON_RETURNS and out and out[-1][0] == 'expr' \
                and out[-1][1] == ('call', ('id', 'Py_INCREF'), [s[1]]):
            out[-1] = ('expr', ('id', SINGLETON_RETURNS[s[1][1]]), INL)
            continue
        out.append(s)
    return out


# ---- printing statements as C --------------------------------------------------

def declarator(ctype, name):
    """C declaration text of name with type ctype ('double (*)(double)' too)."""
    if '(*)' in ctype:
        return ctype.replace('(*)', '(*%s)' % name, 1)
    return '%s%s%s' % (ctype, '' if ctype.endswith('*') else ' ', name)


def c_stmt(s, expr=None, depth=1):
    """Statement s as C lines; expr prints an expression (cparse.c_expr by
    default), so a caller can replace parts of it."""
    expr = expr or cparse.c_expr
    ind = '  ' * depth
    h = s[0]
    if h == 'empty':
        return []
    if h == 'block':
        out = [ind + '{']
        for x in s[1]:
            out += c_stmt(x, expr, depth + 1)
        return out + [ind + '}']
    if h == 'expr':
        return [ind + expr(s[1]) + ';']
    if h == 'return':
        return [ind + 'return' + ('' if s[1] is None else ' ' + expr(s[1])) + ';']
    if h == 'decl':
        return [ind + declarator(c, n) + ('' if i is None else ' = ' + expr(i)) + ';'
                for c, n, i, d in s[1] if not d]
    if h == 'if':
        out = [ind + 'if (%s) {' % expr(s[1])]
        out += c_stmt(_unblock(s[2]), expr, depth + 1) if s[2][0] != 'block' else \
            [y for x in s[2][1] for y in c_stmt(x, expr, depth + 1)]
        if s[3] is not None:
            out.append(ind + '}')
            out.append(ind + 'else {')
            out += c_stmt(s[3], expr, depth + 1) if s[3][0] != 'block' else \
                [y for x in s[3][1] for y in c_stmt(x, expr, depth + 1)]
        return out + [ind + '}']
    raise ValueError('statement %s' % h)


def _unblock(s):
    return s


def stmt_text(s):
    """One line of C for a statement (a run-time copy's text)."""
    return ' '.join(x.strip() for x in c_stmt(s, depth=0))


# ---- the representation and copy-through (rules scalar_rep, copy_through) --------

# set by gen_abstract.py: the module, for its Val, Var, Refuse, Resume...
GA = None
# C types of integers a helper may compute with, besides the
# representations' own
SCALAR_INT = ('int', 'long', 'Py_ssize_t', 'size_t')
RET_FLAGS = {'double': 'CfReturnTypeDouble', 'int': 'CfReturnTypeInt',
             'long': 'CfReturnTypeLong', 'Py_ssize_t': 'CfReturnTypeNative'}
RAISES = {k.split(':')[1] for k in policy_table.SCALAR_CODE['raises']}
PURE = {n for n, e in policy_table.EXTERNAL.items() if e == 'pure'}
CMP = ('==', '!=', '<', '<=', '>', '>=', '&&', '||')


class ScalarRep:
    """One TYPES 'double' section: an object of the struct holds a C
    scalar in a field."""

    def __init__(self, struct, spec, type_name):
        self.struct, self.spec, self.type = struct, spec, type_name
        self.field = spec['field']
        self.ctype = spec['value_ctype']
        self.load = spec['load'].split(':')[1]
        self.new = spec['new'].split(':')[1]
        self.ctors = {c.split(':')[1] for c in spec['constructors']}


def scalar_reps(source):
    """{struct or typedef name: ScalarRep} for the TYPES entries with a
    'double' section whose struct the unit's source declares."""
    out = {}
    for key, t in policy_table.TYPES.items():
        if 'double' not in t:
            continue
        struct = key.split(':')[1]
        if struct not in source.decls.structs:
            continue
        r = ScalarRep(struct, t['double'], t['type'])
        out[struct] = r
        for name, what in source.decls.aliases.items():
            if what.replace('struct ', '').strip() == struct:
                out[name] = r
    return out


class Run:
    """What scalar code reads, writes and calls."""

    def __init__(self):
        self.ins = {}          # name -> ('var', Var) | ('value', Var, rep) | ('ct', Var)
        self.order = []        # input names, in the order first read
        self.defs = set()      # variables it assigns
        self.local = {}        # variables declared inside it -> ctype
        self.raises = False
        self.doubles = False   # reads a representation's scalar
        self.ctor = None       # a tail: the constructor its returns call
        self.statics = set()   # static functions of the file it calls

    def copy(self):
        r = Run()
        r.ins, r.order, r.defs = dict(self.ins), list(self.order), set(self.defs)
        r.local, r.statics = dict(self.local), set(self.statics)
        r.raises, r.doubles, r.ctor = self.raises, self.doubles, self.ctor
        return r


class ScalarRules:
    """Walk's rules for C scalars. Each hook answers None (or 0)
    when the code is not scalar, and the walk goes on as before."""

    def sreps(self):
        tr = self.tr
        if getattr(tr, 'scalar_reps', None) is None:
            tr.scalar_reps = scalar_reps(tr.s)
        return tr.scalar_reps

    def scalar_ctypes(self):
        return {r.ctype for r in self.sreps().values()}

    # -- declarations --

    def scalar_declare(self, st, name, ctype):
        """A local of a representation's scalar type is a run-time value."""
        if ctype.replace('const ', '') not in self.scalar_ctypes():
            return False
        if name in st.vars and not st.vars[name].isparam and st.vars[name].ctype != ctype:
            raise GA.Refuse('%s declared twice with different types' % name)
        v = GA.Var(name, ctype, 'rt', name)
        v.owned = 'dead'
        st.vars[name] = v
        self.decls[name] = 'vinfo_t *%s;' % name
        return True

    # -- field reads and constructors --

    def value_read(self, e, st):
        """(variable, rep) when e reads a representation's field of an
        object variable (through casts and header macros)."""
        if not self.sreps() or not isinstance(e, tuple):
            return None
        if e[0] == 'call' and e[1][0] == 'id' and e[1][1] in self.s.macros \
                and e[1][1] not in GA.KEEP and self.s.macros[e[1][1]].params is not None:
            try:
                return self.value_read(self.s.expand_call(e[1][1], e[2]), st)
            except GA.Resume:
                return None
        if e[0] != 'arrow':
            return None
        base, casts = e[1], []
        while base[0] == 'cast':
            casts.append(base[1])
            base = base[2]
        if base[0] != 'id' or base[1] not in st.vars:
            return None
        v = st.vars[base[1]]
        if v.bt != 'rt' or not self.s.is_obj(v.ctype):
            return None
        for ctype in casts[:1] or [v.ctype]:
            struct = ctype.replace('const ', '').rstrip(' *').replace('struct ', '').strip()
            r = self.sreps().get(struct)
            if r is not None and r.field == e[2]:
                return v, r
        return None

    def load_value(self, v, r, st, out):
        """Ni's load of the scalar: a new reference to its vinfo."""
        t = self.new_tmp('vinfo_t *')
        out('%s = %s(po, %s);' % (t, r.load, v.cname))
        self.error_exit(st, out, '%s == NULL' % t)
        out('vinfo_incref(%s);' % t)
        return GA.Val('rt', t, r.ctype, owned=True)

    def scalar_ev(self, e, st, out, frames):
        """A field read, a constructor given a scalar, or an expression on
        scalars (a helper of its own); None for anything else."""
        if not self.sreps() or not isinstance(e, tuple):
            return None
        m = self.value_read(e, st)
        if m is not None:
            return self.load_value(m[0], m[1], st, out)
        if e[0] == 'call' and e[1][0] == 'id' and len(e[2]) == 1:
            for r in dict.fromkeys(self.sreps().values()):
                if e[1][1] in r.ctors:
                    a = self.scalar_value(e[2][0], st, out)
                    if a is None:
                        return None
                    if not a.owned:
                        out('vinfo_incref(%s);' % a.c)
                    t = self.new_tmp('vinfo_t *')
                    out('%s = %s(%s);' % (t, r.new, a.c))
                    return GA.Val('rt', t, 'PyObject *', owned=True,
                                  virt={'array': False, 'type': r.type})
        if e[0] in ('bin', 'un', 'cond', 'call', 'cast'):
            run = Run()
            if self.scalar_expr(e, st, run) and run.doubles:
                return self.emit_expr(e, run, st, out)
        return None

    def scalar_value(self, e, st, out):
        """The scalar e as a vinfo: a variable's, a load, or a helper's
        result; None if e is not scalar."""
        if e[0] == 'id' and e[1] in st.vars and st.vars[e[1]].ctype in self.scalar_ctypes():
            v = st.vars[e[1]]
            if v.owned == 'dead' and not v.isparam:
                raise GA.Refuse('%s read with no value' % e[1])
            return GA.Val('rt', v.cname, v.ctype, var=v)
        m = self.value_read(e, st)
        if m is not None:
            return self.load_value(m[0], m[1], st, out)
        r = self.raw_load(e, st, out, owned=True, scalars=self.scalar_ctypes())
        if r is not None:
            return r        # '*(double *)p' (pointers.py)
        run = Run()
        if self.scalar_expr(e, st, run) and (run.doubles or e[0] == 'num'):
            return self.emit_expr(e, run, st, out, sorted(self.scalar_ctypes())[0])
        return None

    # -- what is scalar code --

    def scalar_type(self, ctype):
        ctype = ctype.replace('const ', '').strip()
        return ctype in self.scalar_ctypes() or ctype in SCALAR_INT

    def scalar_expr(self, e, st, run, local=frozenset()):
        """Is e an expression on C scalars only? Its inputs go to run."""
        k = e[0]
        if k == 'num':
            return True
        if k == 'id':
            return self.scalar_name(e[1], st, run, local)
        if k == 'un':
            return e[1] not in ('&', '*') and self.scalar_expr(e[2], st, run, local)
        if k == 'bin':
            return self.scalar_expr(e[2], st, run, local) \
                and self.scalar_expr(e[3], st, run, local)
        if k == 'cond':
            return all(self.scalar_expr(x, st, run, local) for x in e[1:])
        if k == 'cast':
            return self.scalar_type(e[1]) and self.scalar_expr(e[2], st, run, local)
        if k == 'arrow':
            m = self.value_read(e, st)
            if m is None:
                return False
            name = '%s_%s' % (m[0].name, m[1].field)
            if name not in run.ins:
                run.ins[name] = ('value', m[0], m[1])
                run.order.append(name)
            run.doubles = True
            return True
        if k == 'call':
            return self.scalar_call(e, st, run, local)
        return False

    def scalar_name(self, name, st, run, local):
        if name in local or name in run.local:
            return True
        if name in policy_table.SCALAR_CODE['variables']:
            return True
        v = st.vars.get(name)
        if v is not None:
            if v.ctype in self.scalar_ctypes():
                run.doubles = True
                kind = 'var'
            elif v.ctype.replace('const ', '') in SCALAR_INT or self.s.is_fnptr(v.ctype):
                if v.bt is None:
                    kind = None         # assigned in the run first
                else:
                    kind = 'var' if v.bt == 'rt' else 'ct'
            else:
                return False
            if kind is not None and name not in run.ins and name not in run.defs:
                run.ins[name] = (kind, v)
                run.order.append(name)
            return True
        m = self.s.macros.get(name)
        if m is not None and m.params is None:
            # a constant (EDOM, HUGE_VAL): an expression with no variable
            try:
                e = cparse.parse_expr(cparse.expand(ctokens.tokenize(name)[0], self.s.macros,
                                                    set()), self.s.decls.types)
            except cparse.ParseError:
                return False
            sub = Run()
            return self.scalar_expr(e, st, sub) and not sub.order
        return False

    def scalar_call(self, e, st, run, local):
        fn, args = e[1], e[2]
        if fn[0] == 'un' and fn[1] == '*':
            fn = fn[2]
        if fn[0] != 'id':
            return False
        name = fn[1]
        v = st.vars.get(name)
        if v is not None:
            # a call through a function pointer the meta knows
            if not (v.bt == 'ct' and self.s.is_fnptr(v.ctype)):
                return False
            if name not in run.ins:
                run.ins[name] = ('ct', v)
                run.order.append(name)
            return all(self.scalar_expr(a, st, run, local) for a in args)
        if name in PURE or name.startswith('__builtin_') or self.proven_pure(name):
            return all(self.scalar_expr(a, st, run, local) for a in args)
        if name in self.s.macros and name not in GA.KEEP \
                and self.s.macros[name].params is not None:
            try:
                x = self.s.expand_call(name, args)
            except GA.Resume:
                return False
            return self.scalar_expr(x, st, run, local)
        g = self.s.funcs.get(name)
        if g is not None and g.static and g.ast is not None \
                and self.scalar_type(g.ret) \
                and all(self.scalar_type(t) for t, _ in g.params):
            # a static function on scalars: its run-time copy
            sub = Run()
            mine = frozenset({p for _, p in g.params} | set(local_names(g.ast)))
            if not all(self.scalar_stmt(s, st, sub, 'fn', mine) for s in g.ast):
                return False
            run.raises |= sub.raises
            run.statics |= sub.statics | {name}
            return all(self.scalar_expr(a, st, run, local) for a in args)
        return False

    def proven_pure(self, name):
        """A function of another file of CPython's that the effect rules
        prove pure (_Py_HashDouble): scalar code may call it as EXTERNAL's
        pure functions."""
        if name in self.s.funcs or name in self.s.macros:
            return False
        tr = self.tr
        return tr.effects is not None and name in tr.effects.globals and \
            'CfPure' in tr.effect_flags(tr.effects.globals[name])

    def scalar_stmt(self, s, st, run, ctx, local=frozenset()):
        """Is statement s scalar code? ctx: 'run' (no return), 'tail' (each
        return an error or the result), 'fn' (a called static function's
        body, its own names in local)."""
        k = s[0]
        if k == 'empty':
            return True
        if k == 'block':
            return all(self.scalar_stmt(x, st, run, ctx, local) for x in s[1])
        if k == 'decl':
            for ctype, name, init, dims in s[1]:
                if dims or not self.scalar_type(ctype):
                    return False
                if init is not None and not self.scalar_expr(init, st, run, local):
                    return False
                if ctx != 'fn':
                    run.local[name] = ctype
            return True
        if k == 'expr':
            e = s[1]
            if e[0] == 'assign' and e[1] in ('=', '+=', '-=', '*=', '/=') \
                    and e[2][0] == 'id':
                name = e[2][1]
                if not (name in local or name in run.local
                        or name in policy_table.SCALAR_CODE['variables']
                        or self.scalar_local(name, st)):
                    return False
                if e[1] != '=' and not self.scalar_name(name, st, run, local):
                    return False
                if not self.scalar_expr(e[3], st, run, local):
                    return False
                if name not in local and name not in run.local \
                        and name not in policy_table.SCALAR_CODE['variables']:
                    run.defs.add(name)
                return True
            if e[0] == 'call' and e[1][0] == 'id' and e[1][1] in RAISES:
                if all(a[0] in ('str', 'num') or (a[0] == 'id' and a[1].startswith('PyExc_'))
                       for a in e[2]):
                    run.raises = True
                    return True
                return False
            if e[0] == 'call' and e[1] == ('id', 'assert'):
                return True
            return False
        if k == 'if':
            return self.scalar_expr(s[1], st, run, local) \
                and self.scalar_stmt(s[2], st, run, ctx, local) \
                and (s[3] is None or self.scalar_stmt(s[3], st, run, ctx, local))
        if k == 'return':
            if ctx == 'fn':
                return s[1] is not None and self.scalar_expr(s[1], st, run, local)
            if ctx != 'tail' or s[1] is None:
                return False
            if self.tr.error_value(self.f, s[1]):
                return True
            e = s[1]
            if self.sm.kind == 'obj':
                if not (e[0] == 'call' and e[1][0] == 'id' and len(e[2]) == 1):
                    return False
                for r in dict.fromkeys(self.sreps().values()):
                    if e[1][1] in r.ctors and run.ctor in (None, r):
                        run.ctor = r
                        return self.scalar_expr(e[2][0], st, run, local)
                return False
            if self.sm.kind == 'val' and self.scalar_type(self.f.ret):
                return self.scalar_expr(e, st, run, local)
            return False
        return False

    def scalar_local(self, name, st):
        v = st.vars.get(name)
        return v is not None and not v.isparam and (
            v.ctype in self.scalar_ctypes() or v.ctype.replace('const ', '') in SCALAR_INT)

    # -- runs --

    def scalar_run(self, stmts, i, st, out, frames):
        """Rule copy_through at statement i of a block: the number of
        statements a helper took (0: none)."""
        if not self.sreps():
            return 0
        s = stmts[i]
        if s[0] == 'empty' or (s[0] == 'decl' and all(d[2] is None for d in s[1])):
            return 0
        rest = stmts[i:]
        tail = Run()
        if all(self.scalar_stmt(x, st, tail, 'tail') for x in rest) \
                and any(ends(x) for x in rest) and tail.doubles \
                and not trivial(rest, self, st):
            if self.run_shape(rest, tail, st, True) is not None:
                self.emit_run(rest, tail, st, out, tail=True)
                return len(rest)
        runs, run = [], Run()
        for j in range(i, len(stmts)):
            trial = run.copy()
            if not self.scalar_stmt(stmts[j], st, trial, 'run'):
                break
            run = trial
            runs.append(run)
        # the longest run with one result at most (C has one return value)
        for j in range(len(runs), 0, -1):
            run = runs[j - 1]
            if not run.doubles or trivial(stmts[i:i + j], self, st):
                continue
            if self.run_shape(stmts[i:i + j], run, st, False) is not None:
                self.emit_run(stmts[i:i + j], run, st, out, tail=False)
                return j
        return 0

    def run_shape(self, stmts, run, st, tail):
        """(the result variables, the helper's C type) of a run, or None
        when a helper can't return it."""
        live = set() if tail else self.tr.live.after[id(stmts[-1])]
        outs = sorted(n for n in run.defs if n in live)
        if len(outs) > 1:
            return None
        if tail:
            if self.sm.kind == 'obj':
                if run.ctor is None:
                    return None
                rtype = run.ctor.ctype
            else:
                rtype = self.f.ret
        elif outs:
            rtype = st.vars[outs[0]].ctype
        else:
            rtype = 'int'
        rtype = GA.int_typedef(self.s, rtype.replace('const ', ''))
        return (outs, rtype) if rtype in RET_FLAGS else None

    def helper_args(self, run, st, out):
        """The helper's parameters and the call's format and arguments; the
        fields read are loaded first (released after the call)."""
        params, fmt, args, loads = [], '', [], []
        # variables in the order the function declares them, then fields
        place = {n: k for k, n in enumerate(st.vars)}
        order = sorted(run.order, key=lambda n: (run.ins[n][0] == 'value',
                                                 place.get(n, 0), run.order.index(n)))
        for name in order:
            kind = run.ins[name]
            if kind[0] == 'value':
                val = self.load_value(kind[1], kind[2], st, out)
                loads.append(val)
                params.append(declarator(kind[2].ctype, name))
                fmt += 'd' if kind[2].ctype == 'double' else 'v'
                args.append(val.c)
                continue
            v = kind[1]
            if v.bt == 'rt' and v.owned == 'dead' and not v.isparam:
                raise GA.Refuse('%s read with no value' % name)
            params.append(declarator(v.ctype.replace('const ', ''), name))
            if kind[0] == 'ct':
                fmt += 'l'
                args.append('(long)(%s)' % v.cname)
            else:
                fmt += 'd' if v.ctype in self.scalar_ctypes() else 'v'
                args.append(v.cname)
        return params, fmt, args, loads

    def helper_expr(self, st):
        """C text of an expression in a helper: a field read is the
        parameter it was loaded into."""
        def expr(e):
            if not isinstance(e, tuple):
                return str(e)
            if e[0] in ('arrow', 'call'):
                m = self.value_read(e, st)
                if m is not None:
                    return '%s_%s' % (m[0].name, m[1].field)
            k = e[0]
            if k == 'call':
                fn = expr(e[1]) if e[1][0] != 'un' else '(%s)' % expr(e[1])
                return '%s(%s)' % (fn, ', '.join(expr(a) for a in e[2]))
            if k == 'un':
                return '%s(%s)' % (e[1], expr(e[2]))
            if k == 'cast':
                return '((%s)%s)' % (e[1], expr(e[2]))
            if k == 'bin':
                return '(%s %s %s)' % (expr(e[2]), e[1], expr(e[3]))
            if k == 'cond':
                return '(%s ? %s : %s)' % tuple(expr(x) for x in e[1:])
            if k == 'assign':
                return '%s %s %s' % (expr(e[2]), e[1], expr(e[3]))
            return cparse.c_expr(e)
        return expr

    def add_helper(self, rtype, params, lines, run, key):
        key = ('scalar', self.f.name, Same(key))
        name = next((n for n, (k, _) in self.tr.copy_keys.items() if k == key), None)
        if name is not None:
            self.tr.copy_order.remove(name)
        else:
            name = self.helper_name()
        if run.statics:
            # the static functions called are copied (cimpl_), line by line
            lines = [ln[:len(ln) - len(ln.lstrip())] + self.tr.rename_statics(ln)
                     if ln.strip() else ln for ln in lines]
        text = 'static %s %s(%s) {\n%s\n}\n' % (rtype, name, ', '.join(params) or 'void',
                                                '\n'.join(lines))
        self.tr.copies[name] = '/* %s:%s: scalar code (rule copy_through) */\n%s' % (
            self.s.cfile, self.f.name, text)
        self.tr.copy_order.append(name)
        self.tr.copy_keys[name] = (key, [])
        return name

    def call_helper(self, name, run, rtype, fmt, args, loads, st, out):
        flags = '%s | %s' % ('CfPyErrCheck' if run.raises else 'CfPure', RET_FLAGS[rtype])
        t = self.new_tmp('vinfo_t *')
        out('%s = psyco_generic_call(po, %s, %s, "%s"%s);' % (
            t, name, flags, fmt, ''.join(', ' + a for a in args)))
        # a helper runs no Python: slots and types stay known
        self.ncalls += 1
        st.called = True
        self.any_call = True
        for val in loads:
            out('vinfo_decref(%s, po);' % val.c)
        self.error_exit(st, out, '%s == NULL' % t)
        return t

    def emit_run(self, stmts, run, st, out, tail):
        f = self.f
        outs, rtype = self.run_shape(stmts, run, st, tail)
        params, fmt, args, loads = self.helper_args(run, st, out)
        locs = ['  %s;' % declarator(st.vars[n].ctype.replace('const ', ''), n)
                for n in sorted(run.defs - set(run.ins)) if n in st.vars]
        expr = self.helper_expr(st)

        def fix(s):
            """a return as the helper's: an error is -1, a constructor its
            scalar"""
            h = s[0]
            if h == 'return':
                if self.tr.error_value(f, s[1]):
                    return ('return', ('num', '-1'), s[-1])
                if self.sm.kind == 'obj':
                    return ('return', s[1][2][0], s[-1])
                return s
            if h == 'block':
                return ('block', [fix(x) for x in s[1]], s[2])
            if h == 'if':
                return ('if', s[1], fix(s[2]), fix(s[3]) if s[3] is not None else None, s[4])
            return s
        lines = list(locs)
        for s in stmts:
            lines += c_stmt(fix(s), expr, 1)
        if not tail:
            lines.append('  return %s;' % (outs[0] if outs else '0'))
        name = self.add_helper(rtype, params, lines, run, stmts[0])
        t = self.call_helper(name, run, rtype, fmt, args, loads, st, out)
        if tail:
            if self.sm.kind == 'obj':
                r = self.new_tmp('vinfo_t *')
                out('%s = %s(%s);' % (r, run.ctor.new, t))
                self.ret_value_owned(st, out, GA.Val(
                    'rt', r, 'PyObject *', owned=True,
                    virt={'array': False, 'type': run.ctor.type}))
            else:
                self.ret_value_owned(st, out, GA.Val('rt', t, rtype, owned=True))
            return
        if not outs:
            out('vinfo_decref(%s, po);' % t)
            return
        v = st.vars[outs[0]]
        if v.bt == 'ct' and not v.isparam:
            raise GA.Refuse("%s is compile-time, given a helper's result" % v.name)
        if v.bt is None:
            v.bt = 'rt'
            self.decls[v.cname] = 'vinfo_t *%s;' % v.cname
        if v.owned in ('yes', 'shadow'):
            self.release(v, out)
        out('%s = %s;' % (v.cname, t))
        v.owned = 'yes'
        v.conv = None
        v.rng = None

    def helper_name(self):
        base = 'cimpl_' + self.f.name
        name, k = base, 0
        while name in self.tr.copies:
            k += 1
            name = '%s_s%d' % (base, k)
        return name

    def expr_ctype(self, e):
        """The C type of a scalar expression: int for a test, else the
        representation's."""
        if (e[0] == 'bin' and e[1] in CMP) or (e[0] == 'un' and e[1] == '!'):
            return 'int'
        if e[0] == 'call' and e[1][0] == 'id' and e[1][1] in self.s.decls.protos:
            # a function's result: its prototype's type (_Py_HashDouble's
            # Py_hash_t)
            r = GA.int_typedef(self.s, self.s.decls.protos[e[1][1]].replace('const ', '').strip())
            if r in RET_FLAGS:
                return r
        return sorted(self.scalar_ctypes())[0]

    def emit_expr(self, e, run, st, out, ctype=None):
        """A scalar expression's value, from a helper of its own."""
        ctype = ctype or self.expr_ctype(e)
        params, fmt, args, loads = self.helper_args(run, st, out)
        text = self.helper_expr(st)(e)
        name = self.direct_callee(e, params, text, run)
        if name is None:
            name = self.add_helper(ctype, params, ['  return %s;' % text], run, e)
        t = self.call_helper(name, run, ctype, fmt, args, loads, st, out)
        return GA.Val('rt', t, ctype, owned=True)

    def direct_callee(self, e, params, text, run):
        """A helper that would only pass its parameters, in order, to one
        function of CPython's that no rule renames (a pure function of
        another file, _Py_HashDouble): that function, called directly."""
        if e[0] != 'call' or e[1][0] != 'id' or run.statics or run.raises:
            return None
        fn = e[1][1]
        names = [p.split()[-1].lstrip('*') for p in params]
        if text != '%s(%s)' % (fn, ', '.join(names)) or not self.proven_pure(fn):
            return None
        return fn


def trivial(stmts, walk, st):
    """Copies only: a field read or a variable stored in a variable or
    returned (given to a constructor), which the walk does without a
    helper."""
    def copy(e):
        return e[0] == 'id' or walk.value_read(e, st) is not None
    for s in stmts:
        if s[0] == 'empty':
            continue
        if s[0] == 'expr' and s[1][0] == 'assign' and s[1][1] == '=' and copy(s[1][3]):
            continue
        if s[0] == 'return' and s[1] is not None and (
                copy(s[1]) or (s[1][0] == 'call' and len(s[1][2]) == 1 and copy(s[1][2][0]))):
            continue
        return False
    return True


def scalar_result(tr, f):
    """Does f return a representation's scalar, its meta a vinfo of it
    (Ni's declaration, or a static function)?"""
    if f.ret.replace('const ', '') not in {r.ctype for r in scalar_reps(tr.s).values()}:
        return False
    decl = tr.ni.get(tr.meta_name(f))
    if decl is not None:
        if decl.ret != 'vinfo_t *':
            raise GA.Refuse('Ni declares %s returning %s' % (decl.name, decl.ret))
        return True
    if f.static:
        return True
    raise GA.Refuse('a %s result needs its convention: declare %s' % (f.ret, tr.meta_name(f)))


def error_value(tr, f, e):
    """'return -1.0' from a function returning a scalar: its error (with
    an exception set)."""
    return e in (('un', '-', ('num', '1.0')), ('un', '-', ('num', '1'))) \
        and f.ret.replace('const ', '') in {r.ctype for r in scalar_reps(tr.s).values()}
