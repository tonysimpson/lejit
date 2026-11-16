#!/usr/bin/env python3
"""Generate the compile-time int slots from CPython's Objects/longobject.c.

First meta-compiler slice (doc/QUESTIONS.md 26, doc/METACOMPILER.md). Every
slot in long_as_number is tried. A slot is translated if its C body has the
shape of 3.7's one-digit fast path:

    [decls]  [CHECK_BINOP(a, b);]
    if (Py_ABS(Py_SIZE(a)) <= 1 && ...) { [T v = EXPR;]* return CTOR(EXPR); }
    rest

EXPR is + - * unary - and casts over MEDIUM_VALUE(param) and locals; a
range analysis proves the C doesn't overflow and the result fits a word.
The fast path becomes word arithmetic on virtual ints; 'rest' becomes a
residual call of the slot itself (the code before the guard has no side
effects, so rerunning it is the same). Anything else is skipped, with the
reason printed: that list is the failures log.

Usage: gen_long.py [--check] [CPYTHON_SRC]   (writes ni/Objects/plongobject_gen.h)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ctokens  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
OUT = os.path.join(ROOT, 'ni', 'Objects', 'plongobject_gen.h')
DEFAULT_SRC = os.path.expanduser('~/.pyenv/sources/3.7.17/Python-3.7.17')

SHIFT = 30                      # PyLong_SHIFT; asserted in the output
MEDIUM = (-(2 ** SHIFT - 1), 2 ** SHIFT - 1)
WORD = 64                       # sizeof(long) * 8; asserted in the output
# signed integer types a fast path may use, and their width in bits
INT_TYPES = {('int',): 32, ('sdigit',): 32, ('long',): 64,
             ('long', 'long'): 64, ('stwodigits',): 64, ('Py_ssize_t',): 64}
# value constructors whose argument becomes the virtual int's word
CTORS = {'PyLong_FromLong': 64, 'PyLong_FromLongLong': 64}
OPS = {'+': 'integer_add', '-': 'integer_sub', '*': 'integer_mul'}


class Skip(Exception):
    """This slot doesn't have a shape the generator knows."""


def text(toks):
    """Tokens back to C, with the source's own spacing."""
    return ''.join((' ' if t.sp and k else '') + t.text
                   for k, t in enumerate(toks))


def is_seq(toks, words):
    return [t.text for t in toks] == words.split()


def statements(toks):
    """Split a body into statements: 'x;', 'if (...) S', '{...}'."""
    out, i = [], 0
    while i < len(toks):
        t = toks[i]
        if t.text == '{':
            j = ctokens.match_close(toks, i)
            out.append(toks[i:j + 1])
            i = j + 1
        else:
            k = _stmt_end(toks, i)
            out.append(toks[i:k + 1])
            i = k + 1
    return out


def _stmt_end(toks, i):
    if toks[i].text == '{':
        return ctokens.match_close(toks, i)
    if toks[i].text == 'if':
        k = _stmt_end(toks, ctokens.match_close(toks, i + 1) + 1)
        if k + 1 < len(toks) and toks[k + 1].text == 'else':
            k = _stmt_end(toks, k + 2)
        return k
    depth = 0
    for j in range(i, len(toks)):
        if toks[j].text in '([{':
            depth += 1
        elif toks[j].text in ')]}':
            depth -= 1
        elif toks[j].text == ';' and depth == 0:
            return j
    raise Skip('statement without ;')


def expand(stmt, macros):
    """Expand a statement that is one call of a function-like macro, and
    unwrap do { ... } while (0)."""
    if (len(stmt) >= 4 and stmt[0].text in macros and stmt[1].text == '('
            and macros[stmt[0].text].params is not None):
        m = macros[stmt[0].text]
        close = ctokens.match_close(stmt, 1)
        args = ctokens.split_top(stmt[2:close], ',')
        if len(args) != len(m.params) or stmt[close + 1:] != stmt[-1:]:
            return [stmt]
        env = dict(zip(m.params, args))
        body = []
        for t in m.body:
            body.extend(env.get(t.text, [t]) if t.kind == 'id' else [t])
        if (body and body[0].text == 'do'
                and is_seq(body[-4:], 'while ( 0 )')):
            body = body[2:-5]
        return statements(body)
    return [stmt]


# ---- expressions: a tiny parser, then range and width checking ----

class Node:
    def __init__(self, kind, args=(), name=None, lo=None, hi=None, bits=None,
                 src=''):
        self.kind, self.args, self.name = kind, list(args), name
        self.lo, self.hi, self.bits, self.src = lo, hi, bits, src


def parse_expr(toks, params, local_vars):
    p = _Parser(toks, params, local_vars)
    n = p.additive()
    if p.i != len(toks):
        raise Skip('unexpected %r in expression' % toks[p.i].text)
    return n


class _Parser:
    def __init__(self, toks, params, local_vars):
        self.t, self.i, self.params, self.locals = toks, 0, params, local_vars

    def peek(self, k=0):
        return self.t[self.i + k].text if self.i + k < len(self.t) else None

    def additive(self):
        n = self.mult()
        while self.peek() in ('+', '-'):
            op = self.peek()
            self.i += 1
            n = _binop(op, n, self.mult())
        return n

    def mult(self):
        n = self.unary()
        while self.peek() == '*':
            self.i += 1
            n = _binop('*', n, self.unary())
        return n

    def unary(self):
        if self.peek() == '-':
            self.i += 1
            a = self.unary()
            return _check(Node('neg', [a], lo=-a.hi, hi=-a.lo,
                               bits=max(32, a.bits),
                               src='-' + ('(%s)' % a.src if a.kind in OPS
                                          else a.src)))
        if self.peek() == '(':
            close = ctokens.match_close(self.t, self.i)
            inner = tuple(t.text for t in self.t[self.i + 1:close])
            if inner in INT_TYPES:
                self.i = close + 1
                a = self.unary()
                bits = INT_TYPES[inner]
                if not _fits(a.lo, a.hi, bits):
                    raise Skip('cast to %s may truncate' % ' '.join(inner))
                return Node('cast', [a], lo=a.lo, hi=a.hi, bits=bits,
                            src='(%s)%s' % (' '.join(inner), a.src))
            self.i += 1
            n = self.additive()
            if self.peek() != ')':
                raise Skip('unbalanced expression')
            self.i += 1
            return n
        t = self.t[self.i]
        if t.kind == 'num' and t.text.isdigit():
            self.i += 1
            v = int(t.text)
            return Node('const', name=v, lo=v, hi=v, bits=32, src=t.text)
        if (t.text == 'MEDIUM_VALUE' and self.peek(1) == '('
                and self.peek(3) == ')' and self.peek(2) in self.params):
            self.i += 4
            return Node('medium', name=self.t[self.i - 2].text, lo=MEDIUM[0],
                        hi=MEDIUM[1], bits=32,
                        src='MEDIUM_VALUE(%s)' % self.t[self.i - 2].text)
        if t.kind == 'id' and t.text in self.locals:
            self.i += 1
            v = self.locals[t.text]
            return Node('local', name=t.text, lo=v.lo, hi=v.hi, bits=v.bits,
                        src=t.text)
        raise Skip('unknown operand %r' % t.text)


def _fits(lo, hi, bits):
    return -2 ** (bits - 1) <= lo and hi < 2 ** (bits - 1)


def _binop(op, a, b):
    if a.kind == 'const' and b.kind == 'const':
        raise Skip('constant expression %s %s %s' % (a.src, op, b.src))
    if 'const' in (a.kind, b.kind) and op == '*':
        raise Skip('multiplication by a constant')
    if op == '*':
        c = [a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi]
        lo, hi = min(c), max(c)
    elif op == '+':
        lo, hi = a.lo + b.lo, a.hi + b.hi
    else:
        lo, hi = a.lo - b.hi, a.hi - b.lo
    return _check(Node(op, [a, b], lo=lo, hi=hi, bits=max(32, a.bits, b.bits),
                       src='%s %s %s' % (a.src, op, b.src)))


def _check(n):
    # signed overflow in C is undefined: the C itself must not overflow
    if not _fits(n.lo, n.hi, n.bits):
        raise Skip('%s may overflow its %d-bit C type' % (n.src, n.bits))
    return n


# ---- recognising a slot ----

class Slot:
    def __init__(self, cname, slot, params):
        self.cname, self.slot, self.params = cname, slot, params
        self.type_check = None        # params that CHECK_BINOP-style checks
        self.guarded = []             # params with a one-digit guard
        self.guard_src = ''
        self.lets = []                # (name, Node) local declarations
        self.result = None            # Node of the returned value
        self.result_src = ''


def recognise(cname, slot, params_toks, body, macros):
    params = [[t.text for t in p if t.kind == 'id'][-1]
              for p in ctokens.split_top(params_toks, ',')]
    s = Slot(cname, slot, params)
    stmts = []
    for st in statements(body):
        stmts.extend(expand(st, macros))
    i = 0
    while i < len(stmts) and _is_decl(stmts[i]):
        i += 1
    if i < len(stmts) and _type_check(stmts[i], params) is not None:
        s.type_check = _type_check(stmts[i], params)
        i += 1
    if i >= len(stmts) or stmts[i][0].text != 'if':
        raise Skip('no fast-path if after the type check')
    st = stmts[i]
    close = ctokens.match_close(st, 1)
    s.guarded = _guard(st[2:close], params)
    s.guard_src = 'if (%s)' % text(st[2:close])
    then = st[close + 1:]
    if then and then[0].text == '{':
        then = then[1:ctokens.match_close(then, 0)]
    elif any(t.text == 'else' for t in then):
        raise Skip('fast path has an else')
    if set(s.guarded) != set(params):
        raise Skip('not every operand is guarded')
    local_vars = {}
    fast = statements(then)
    for k, ft in enumerate(fast):
        last = k == len(fast) - 1
        if last and ft[0].text == 'return':
            if (len(ft) < 5 or ft[1].text not in CTORS or ft[2].text != '('
                    or ctokens.match_close(ft, 2) != len(ft) - 2):
                raise Skip('fast path returns %r, not a known constructor'
                           % text(ft[1:-1]))
            n = parse_expr(ft[3:-2], params, local_vars)
            if not _fits(n.lo, n.hi, CTORS[ft[1].text]):
                raise Skip('%s argument may not fit' % ft[1].text)
            if not _fits(n.lo, n.hi, WORD):
                raise Skip('result may not fit a word')
            s.result, s.result_src = n, text(ft[:-1]) + ';'
        elif not last and '=' in [t.text for t in ft]:
            eq = [t.text for t in ft].index('=')
            ctype = tuple(t.text for t in ft[:eq - 1])
            if ctype not in INT_TYPES or ft[eq - 1].kind != 'id':
                raise Skip('fast-path declaration %r' % text(ft))
            n = parse_expr(ft[eq + 1:-1], params, local_vars)
            if not _fits(n.lo, n.hi, INT_TYPES[ctype]):
                raise Skip('%s may not fit %s' % (n.src, ' '.join(ctype)))
            local_vars[ft[eq - 1].text] = n
            s.lets.append((ft[eq - 1].text, n, text(ft)))
        else:
            raise Skip('fast-path statement %r' % text(ft))
    if s.result is None:
        raise Skip('fast path does not return')
    return s


def _is_decl(st):
    return (len(st) >= 3 and st[-1].text == ';'
            and all(t.kind == 'id' or t.text in ('*', ',') for t in st[:-1])
            and st[0].text not in ('return', 'if'))


def _type_check(st, params):
    """'if (!PyLong_Check(a) || !PyLong_Check(b)) Py_RETURN_NOTIMPLEMENTED;'
    (CHECK_BINOP expanded): the checked params, or None."""
    if st[0].text != 'if' or not is_seq(st[-2:], 'Py_RETURN_NOTIMPLEMENTED ;'):
        return None
    close = ctokens.match_close(st, 1)
    if close != len(st) - 3:
        return None
    out = []
    for term in ctokens.split_top(st[2:close], '||'):
        w = [t.text for t in term]
        if len(w) != 5 or w[:3] != ['!', 'PyLong_Check', '('] or w[4] != ')':
            return None
        out.append(w[3])
    return out


def _guard(cond, params):
    """'Py_ABS(Py_SIZE(a)) <= 1 && ...': the guarded params."""
    out = []
    for term in ctokens.split_top(cond, '&&'):
        w = [t.text for t in term]
        if (len(w) != 9 or w[:4] != ['Py_ABS', '(', 'Py_SIZE', '(']
                or w[5:] != [')', ')', '<=', '1'] or w[4] not in params):
            raise Skip('guard %r is not a one-digit test' % text(term))
        out.append(w[4])
    return out


# ---- emitting ----

def emit(s):
    p = s.params
    py = ['p' + x for x in p]                  # vinfo_t * for each operand
    med = {x: 'm' + x for x in p}              # its MEDIUM_VALUE
    fallback = ('plong_fallback%d(po, PyLong_Type.tp_as_number->%s, %s)'
                % (len(p), s.slot, ', '.join(py)))
    # straight-line code: (dest, fn, args, comment)
    code, n_tmp = [], [0]

    def gen(n):
        if n.kind == 'medium':
            return med[n.name]
        if n.kind == 'local':
            return env[n.name]
        if n.kind == 'cast':                   # range-checked: a no-op
            return gen(n.args[0])
        if n.kind == 'const':
            raise Skip('a constant on its own')
        consts = [a for a in n.args if a.kind == 'const']
        if consts:                             # x + c, c + x, x - c
            x = [a for a in n.args if a.kind != 'const'][0]
            c = consts[0].name
            if n.kind == '-' and n.args[0].kind == 'const':
                raise Skip('constant minus a value')
            args = [gen(x)]
            n_tmp[0] += 1
            d = 't%d' % n_tmp[0]
            code.append((d, 'integer_add_i', args + [str(-c if n.kind == '-'
                                                         else c)], n))
            return d
        args = [gen(a) for a in n.args]
        n_tmp[0] += 1
        d = 't%d' % n_tmp[0]
        if n.kind == 'neg':
            code.append((d, 'integer_neg', args, n))
        else:
            code.append((d, OPS[n.kind], args, n))
        return d

    env = {}
    for name, n, _ in s.lets:
        env[name] = gen(n)
    result = gen(s.result)
    if result in med.values():
        raise Skip('fast path returns an operand unchanged')
    last_use = {}
    for k, (d, fn, args, n) in enumerate(code):
        for a in args:
            last_use[a] = k

    out = ['/* %s: %s */' % ('Objects/longobject.c:' + s.cname, s.slot)]
    out.append('static vinfo_t *p%s(PsycoObject *po, %s) {'
               % (s.cname, ', '.join('vinfo_t *' + x for x in py)))
    out.append('  vinfo_t %s;' % ', '.join(
        '*' + v for v in [med[x] for x in p] + [d for d, _, _, _ in code]))
    out.append('  int r;')
    if s.type_check:
        out.append('  /* %s */' % ('if (' + ' || '.join(
            '!PyLong_Check(%s)' % x for x in s.type_check)
            + ') Py_RETURN_NOTIMPLEMENTED;'))
        for k, x in enumerate(s.type_check):
            pre = '' if k == 0 else 'if (r == 1)\n    '
            out.append('  %sr = Psyco_VerifyType(po, p%s, &PyLong_Type);'
                       % (pre, x))
        out.append('  if (r <= 0)')
        out.append('    return r < 0 ? NULL : psyco_vi_NotImplemented();')
    out.append('  /* %s; else the rest of %s, as a residual call */'
               % (s.guard_src, s.cname))
    held = []
    for x in s.guarded:
        out.append('  r = plong_medium_value(po, p%s, &%s);' % (x, med[x]))
        if held:
            out.append('  if (r <= 0) {')
            for h in reversed(held):
                out.append('    vinfo_decref(%s, po);' % h)
            out.append('    return r < 0 ? NULL : %s;' % fallback)
            out.append('  }')
        else:
            out.append('  if (r <= 0)')
            out.append('    return r < 0 ? NULL : %s;' % fallback)
        held.append(med[x])
    for _, _, src in s.lets:
        out.append('  /* %s */' % src)
    out.append('  /* %s */' % s.result_src)
    live = list(held)
    for k, (d, fn, args, n) in enumerate(code):
        out.append('  /* %s: in [%d, %d] */' % (n.src, n.lo, n.hi))
        out.append('  %s = %s(po, %s, false);' % (d, fn, ', '.join(args)))
        for a in args:
            if last_use.get(a) == k and a in live:
                out.append('  vinfo_decref(%s, po);' % a)
                live.remove(a)
        out.append('  if (%s == NULL) {' % d if live else '  if (%s == NULL)' % d)
        for h in reversed(live):
            out.append('    vinfo_decref(%s, po);' % h)
        out.append('    return NULL;')
        if live:
            out.append('  }')
        live.append(d)
    live.remove(result)
    for h in reversed(live):
        out.append('  vinfo_decref(%s, po);' % h)
    out.append('  return PsycoLong_FROM_WORD(%s);' % result)
    out.append('}')
    return '\n'.join(out)


def generate(src):
    with open(os.path.join(src, 'Objects', 'longobject.c')) as f:
        toks, macros = ctokens.tokenize(f.read(), 'longobject.c')
    with open(os.path.join(src, 'Include', 'object.h')) as f:
        fields = ctokens.struct_fields(ctokens.tokenize(f.read())[0],
                                       'PyNumberMethods')
    funcs = ctokens.functions(toks)
    table = ctokens.initializer(toks, 'long_as_number')
    done, skipped = [], []
    for slot, cname in zip(fields, table):
        if cname == '0':
            continue
        if cname not in funcs:
            skipped.append((slot, cname, 'no definition found'))
            continue
        try:
            s = recognise(cname, slot, funcs[cname][0], funcs[cname][1], macros)
            done.append((slot, cname, emit(s)))
        except Skip as e:
            skipped.append((slot, cname, str(e)))
    head = ['/* GENERATED by tools/metac/gen_long.py from CPython 3.7\'s',
            '   Objects/longobject.c: do not edit. Included by plongobject.c,',
            '   which provides plong_medium_value and plong_fallback1/2. */',
            '',
            '_Static_assert(PyLong_SHIFT == %d, "one-digit ranges assume'
            ' PyLong_SHIFT %d");' % (SHIFT, SHIFT),
            '_Static_assert(sizeof(long) * 8 == %d, "results are machine'
            ' words");' % WORD,
            '_Static_assert(sizeof(long long) == sizeof(long),'
            ' "PyLong_FromLongLong is a word");', '']
    body = [code + '\n' for _, _, code in done]
    reg = ['static void plong_generated_init(PyNumberMethods *m) {']
    reg += ['  Psyco_DefineMeta(m->%s, p%s);' % (slot, c) for slot, c, _ in done]
    reg += ['}', '']
    report = ['/* not generated:'] + ['   %s (%s): %s' % x for x in skipped] + \
        ['*/', '']
    return '\n'.join(head + body + reg + report), done, skipped


def main(argv):
    check = '--check' in argv
    args = [a for a in argv if a != '--check']
    src = args[0] if args else os.environ.get('NI_CPYTHON_SRC', DEFAULT_SRC)
    out, done, skipped = generate(src)
    if check:
        with open(OUT) as f:
            if f.read() != out:
                print('%s is out of date: run tools/metac/gen_long.py'
                      % os.path.relpath(OUT, ROOT))
                return 1
        print('%s up to date (%d slots generated)'
              % (os.path.relpath(OUT, ROOT), len(done)))
        return 0
    with open(OUT, 'w') as f:
        f.write(out)
    for slot, cname, _ in done:
        print('generated  %-13s %s' % (slot, cname))
    for slot, cname, why in skipped:
        print('skipped    %-13s %-17s %s' % (slot, cname, why))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
