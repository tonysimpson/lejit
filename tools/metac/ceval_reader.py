"""Read Python/ceval.c's TARGET cases into instruction definitions in the
form CPython's own parser gives for Python/bytecodes.c (DECISIONS D67 (5),
D73, doc/METACOMPILER.md "ceval.c reader"), so that everything after the
front end of gen_ceval.py is shared by every version.

A case is run symbolically over C tokens (ctokens.py). Stack positions are
linear forms in oparg (Lin), counted from the stack pointer on entry, so
PEEK(oparg), STACKADJ(-oparg) and a loop that pops oparg items are as
readable as POP(). POP, TOP, SECOND, THIRD, FOURTH and PEEK(n) read
input slots, PUSH, SET_TOP..SET_FOURTH and SET_VALUE(n, x) write slots,
STACKADJ, STACK_GROW, STACK_SHRINK and 'stack_pointer += n' move the
pointer. At the end the positions touched cut the stack into intervals:
the inputs are those below the entry pointer, the outputs those below the
final one, each named by the C variable it holds (an array, values[n],
when a loop or a pointer reaches it; unused[n] when nothing does). The
body is written out in bytecodes.c syntax:

    inst(NAME, (in1, values[oparg], in3 if (oparg & 1) -- out1)) { ... }

and parsed with CPython's parser, so the result is the same InstDef a
bytecodes.c file gives.

Rules (each general, none per opcode):
  - Error exits. 'if (C) goto error;', or a block ending in 'goto error'
    whose statements are releases of inputs and at most one known
    error-raising statement (ERROR_FORMS), becomes ERROR_IF(C, label); any
    other block becomes 'if (C) { statements; ERROR_IF(true, error); }'.
    'if (C) A else { ...; goto error; }' is the exit on !(C), then A.
    Every exit is checked once the layout is known, on every path to it:
    each input is either still in its slot or released or stored, never
    both, and any other slot written holds the value C says is NULL. That
    is what ERROR_IF means to the generator: the inputs are dead after it.
  - Generic path. 'if (T) A else B', where T only tests types (Py*_Check,
    Py*_CheckExact) and sizes of an exact type against oparg, keeps B: A
    is CPython's fast path for exact built-in types; Psyco's counterpart
    of B dispatches on the type itself. This is judgement, stated once
    here (METACOMPILER).
  - Branches. 'if (C)' on a compile-time condition (oparg, its int
    aliases and #define'd constants) whose then-arm starts with 'x =
    POP()' and whose else-arm is empty or 'x = NULL' (also 'x = C ? POP()
    : NULL') is a conditional input, 'x if (C)'. A branch on a run-time
    value is run on both arms, as two paths; the paths must agree on the
    stack where they meet. A branch on 'opcode' is folded: in a case
    reached by its own TARGET, opcode is that case.
  - Arms that end the case (D79). 'if (C) { A; DISPATCH(); } REST' is
    'if (C) A else { REST }'. An if-chain on one int compared with
    constants whose last arm is an error exit tests that exit first, on
    the values no arm takes. 'if (x == K) { A; DISPATCH(); }', K one of
    SINGLETONS, is dropped when the callee the rest applies to x answers
    K first in its own body and the rest, folded with that answer, does A
    (judgement checked against the C: singleton_test_generic_path).
  - Jumps. JUMPTO/JUMPBY mark their path; slots only jumping paths keep
    are outputs 'x if (jump)'. A #if whose arms are each one dispatch is
    a dispatch.
  - Loops. 'while (--oparg >= 0)' or 'while (oparg--)' whose body pops k
    values reads an array of k*oparg inputs; 'for (i = N; i > 0; i--)'
    whose body reads PEEK(a*i + b) reads an array of a*N. Either is
    written out over values[...].
  - Pointers. 'stack_pointer - n' handed to a call reads the n slots below
    the pointer (an array); 'stack_pointer + n' handed to a call and then
    'STACKADJ(n)' are n outputs the call wrote (unused[n]). A pointer to
    a local copy of the stack pointer ('&sp') handed to a function of
    ceval.c moves it by what that function's body pops (HELPER).
  - Duplicates. An output that is the same variable as another output
    gets a new name (x2), and the Py_INCREF that made the extra reference
    becomes 'x2 = Py_NewRef(x)', which is how bytecodes.c writes
    DUP_TOP's COPY.
  - Tidying. 'T x;' then 'x = E;' becomes 'T x = E;'; an output or input
    loses its declaration (the DSL declares them). DISPATCH, FAST_DISPATCH
    and PREDICT are dropped; the main path must end with one of the first
    two.

Anything else is refused with a reason and a kind (KINDS), never guessed:
a case that falls into another, jumps to shared code, touches frame or
thread state, or has stack use or control flow the rules above don't
cover.
"""
import functools
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'cases_generator'))
import ctokens  # noqa: E402
import parser as dsl  # noqa: E402  (CPython's Tools/cases_generator/parser.py)

KINDS = {
    'falls through': 'shares its body with another case',
    'shared code': 'jumps to code shared between cases',
    'core': 'frame, block stack, thread state or the stack pointer itself',
    'oparg': 'stack position the reader cannot express in oparg',
    'preprocessor': 'preprocessor conditional in the case',
    'control flow': 'control flow the reader has no rule for',
    'stack': 'stack use the reader has no rule for',
}

READS = {'TOP': 1, 'SECOND': 2, 'THIRD': 3, 'FOURTH': 4}
WRITES = {'SET_TOP': 1, 'SET_SECOND': 2, 'SET_THIRD': 3, 'SET_FOURTH': 4}
MOVES = {'STACKADJ': 1, 'STACK_GROW': 1, 'STACK_SHRINK': -1}
STACK_MACROS = set(READS) | set(WRITES) | set(MOVES) | {
    'POP', 'PEEK', 'PUSH', 'SET_VALUE', 'EXT_POP', 'BASIC_PUSH', 'BASIC_POP',
    'BASIC_STACKADJ'}
DISPATCHES = {'DISPATCH', 'FAST_DISPATCH'}
DROPPED = {'PREDICT'}


def _pop_count(w):
    """n for a statement that only pops n slots, 'STACKADJ(-n)' (3.7) or
    'STACK_SHRINK(n)' (3.8), else 0."""
    if w and w[0] in MOVES and w[-1] == ')' and w[1] == '(':
        arg = w[2:-1]
        if MOVES[w[0]] == 1 and len(arg) == 2 and arg[0] == '-' and arg[1].isdigit():
            return int(arg[1])
        if MOVES[w[0]] == -1 and len(arg) == 1 and arg[0].isdigit():
            return int(arg[0])
    return 0


# identifiers that mean the case works on the interpreter itself
CORE_NAMES = {
    'tstate': 'thread state',
    'PyFrame_BlockSetup': 'the block stack',
    'PyFrame_BlockPop': 'the block stack',
}
# error-raising statements that bytecodes.c names as an error label: the
# statement's first tokens, and the label. 3.12's ceval.c label
# unbound_local_error is this statement followed by 'goto error'.
ERROR_FORMS = [
    (['format_exc_check_arg', '(', 'PyExc_UnboundLocalError', ',',
      'UNBOUNDLOCAL_ERROR_MSG', ','], 'unbound_local_error'),
]
TYPE_TEST = re.compile(r'Py[A-Za-z]+_Check(Exact)?')
SIZE_TEST = re.compile(r'Py[A-Za-z]+_GET_SIZE')
DECL_TYPES = ('PyObject', 'int', 'Py_ssize_t')
CASTS = ('Py_ssize_t', 'int', 'long', 'size_t')
# opargs at which two positions are compared, and effects checked: small
# ones, and both sides of the byte boundaries EXTENDED_ARG and UNPACK_EX's
# 'oparg >> 8' make
SAMPLES = (1, 2, 3, 4, 5, 6, 7, 8, 9, 255, 256, 257, 258, 511, 513, 65535)
JUMPS = {'JUMPTO', 'JUMPBY'}
# the objects a fast path tests for by identity (rule singleton_test_generic_path)
SINGLETONS = ('Py_True', 'Py_False', 'Py_None')
NEGATE = {'==': '!=', '!=': '==', '<': '>=', '>=': '<', '>': '<=', '<=': '>'}


class Refuse(Exception):
    def __init__(self, kind, why):
        super().__init__(why)
        self.kind = kind


# ---- compile-time integers ----------------------------------------------------

def constants(src):
    """{NAME: int} for object-like macros whose body is one integer literal,
    in Include/*.h and Python/ceval.c. A name defined to two values is left
    out, so using it is refused."""
    out, bad = {}, set()
    paths = [os.path.join(src, 'Include', h)
             for h in sorted(os.listdir(os.path.join(src, 'Include')))
             if h.endswith('.h')] + [os.path.join(src, 'Python', 'ceval.c')]
    enums = []
    for p in paths:
        with open(p, errors='replace') as f:
            toks, macros = ctokens.tokenize(f.read(), p)
        for m in macros.values():
            body = [t.text for t in m.body]
            while len(body) == 3 and body[0] == '(' and body[2] == ')':
                body = body[1:2]
            if m.params is None and len(body) == 1 and re.fullmatch(
                    r'0[xX][0-9a-fA-F]+|[1-9]\d*|0', body[0]):
                v = int(body[0], 0)
                if out.get(m.name, v) != v:
                    bad.add(m.name)
                out[m.name] = v
        enums += _enums(toks)
    # enumerators, after the macros their values name (opcode.h's
    # 'enum cmp_op {PyCmp_LT=Py_LT, ..., PyCmp_IS, ...}'); one whose value
    # can't be read ends its enum
    for items in enums:
        v = -1
        for name, value in items:
            if value is None:
                v += 1
            else:
                try:
                    v = cvalue(value, 0, out)
                except Refuse:
                    break
            if out.get(name, v) != v:
                bad.add(name)
            out[name] = v
    for n in bad:
        del out[n]
    return out


def _enums(toks):
    """[[(name, value text or None)]] for each 'enum [tag] { ... }' body."""
    out = []
    for i, t in enumerate(toks):
        if t.text != 'enum':
            continue
        j = i + 1
        if j < len(toks) and toks[j].kind == 'id':
            j += 1
        if j >= len(toks) or toks[j].text != '{':
            continue
        end = ctokens.match_close(toks, j)
        items = []
        for part in ctokens.split_top(toks[j + 1:end], ','):
            w = [u.text for u in part]
            if not w:
                continue
            if len(w) == 1 and part[0].kind == 'id':
                items.append((w[0], None))
            elif len(w) >= 3 and w[1] == '=' and part[0].kind == 'id':
                items.append((w[0], ' '.join(w[2:])))
            else:
                break
        out.append(items)
    return out


_CTOK = re.compile(r'0[xX][0-9a-fA-F]+|\d+|[A-Za-z_]\w*|&&|\|\||==|!=|<=|>=|'
                   r'<<|>>|[-+*%&|^~!<>()]')
_PY = {'&&': 'and', '||': 'or', '!': 'not'}
_CMP = {'==', '!=', '<', '>', '<=', '>='}
_BITS = {'&', '|', '^', '<<', '>>'}


def cvalue(text, oparg, consts):
    """The value of a C integer expression in oparg and constants. C and
    Python differ in the precedence of bitwise operators against
    comparisons, and of '!': mixing them unparenthesised is refused."""
    text = re.sub(r'\(\s*(%s)\s*\)' % '|'.join(CASTS), ' ', text)
    toks = _CTOK.findall(text)
    if ''.join(toks) != re.sub(r'\s+', '', text):
        raise Refuse('oparg', 'expression %s' % text)
    out, ops, depth = [], [set()], 0
    for t in toks:
        if t == '(':
            ops.append(set())
        elif t == ')':
            ops.pop()
        elif t in _CMP or t in _BITS or t == '!':
            ops[-1].add('cmp' if t in _CMP else '!' if t == '!' else 'bits')
            if len(ops[-1]) > 1:
                raise Refuse('oparg', 'precedence in %s' % text)
        if t == 'oparg':
            out.append(str(oparg))
        elif t in consts:
            out.append(str(consts[t]))
        elif re.fullmatch(r'0[xX][0-9a-fA-F]+|\d+', t):
            out.append(str(int(t, 0)))
        elif t in _PY:
            out.append(' %s ' % _PY[t])
        elif re.fullmatch(r'[A-Za-z_]\w*', t):
            raise Refuse('oparg', 'unknown name %s in %s' % (t, text))
        elif t == '%':
            raise Refuse('oparg', 'expression %s' % text)
        else:
            out.append(t)
    return int(eval(' '.join(out), {'__builtins__': {}}))


def is_bool(text):
    """Is this expression (an atom's text) 0 or 1?"""
    w = _CTOK.findall(text)
    while w and w[0] == '(' and _close(w, 0) == len(w) - 1:
        w = w[1:-1]
    if w and w[0] == '!':
        return True
    depth = 0
    for t in w:
        depth += t == '('
        depth -= t == ')'
        if depth == 0 and (t in _CMP or t in ('&&', '||')):
            return True
    return False


def _close(w, i):
    depth = 0
    for j in range(i, len(w)):
        depth += w[j] in ('(', '[')
        depth -= w[j] in (')', ']')
        if depth == 0:
            return j
    return -1


class Lin:
    """c + sum(k * atom): an atom is 'oparg', a loop variable, or the text
    of an expression in oparg that isn't linear, '(oparg & 0xFF)'."""
    __slots__ = ('c', 't')

    def __init__(self, c=0, t=None):
        self.c = c
        self.t = {a: k for a, k in (t or {}).items() if k}

    @staticmethod
    def atom(a):
        return Lin(0, {a: 1})

    def key(self):
        return (self.c, tuple(sorted(self.t.items())))

    def __eq__(self, o):
        return isinstance(o, Lin) and self.key() == o.key()

    def __hash__(self):
        return hash(self.key())

    def __add__(self, o):
        o = o if isinstance(o, Lin) else Lin(o)
        t = dict(self.t)
        for a, k in o.t.items():
            t[a] = t.get(a, 0) + k
        return Lin(self.c + o.c, t)

    def __neg__(self):
        return Lin(-self.c, {a: -k for a, k in self.t.items()})

    def __sub__(self, o):
        return self + -(o if isinstance(o, Lin) else Lin(o))

    def scale(self, n):
        return Lin(self.c * n, {a: k * n for a, k in self.t.items()})

    def const(self):
        return None if self.t else self.c

    def coeff(self, a):
        return self.t.get(a, 0)

    def subst(self, m):
        out = Lin(self.c)
        for a, k in self.t.items():
            out = out + (m[a] if a in m else Lin.atom(a)).scale(k)
        return out

    def value(self, oparg, consts):
        return self.c + sum(k * (oparg if a == 'oparg' else
                                 cvalue(a, oparg, consts))
                            for a, k in self.t.items())

    def text(self):
        parts = []
        for a, k in sorted(self.t.items(), key=lambda x: (x[0] != 'oparg', x[0])):
            parts.append((k < 0, a if abs(k) == 1 else '%d * %s' % (abs(k), a)))
        if self.c or not parts:
            parts.append((self.c < 0, str(abs(self.c))))
        s = ('-' if parts[0][0] else '') + parts[0][1]
        return s + ''.join(' %s %s' % ('-' if n else '+', p) for n, p in parts[1:])

    def __repr__(self):
        return 'Lin(%s)' % self.text()


class _NotLin(Exception):
    pass


def to_lin(w, env, consts):
    """Token texts -> Lin, or None if they aren't a compile-time integer
    (names: env's, which map to Lins, and constants)."""
    w = [t for t in w]
    try:
        v, i = _sum(w, 0, env, consts)
        if i != len(w):
            raise _NotLin
        return v
    except _NotLin:
        return _atom(w, env, consts)


def _sum(w, i, env, consts):
    v, i = _term(w, i, env, consts)
    while i < len(w) and w[i] in ('+', '-'):
        op = w[i]
        u, i = _term(w, i + 1, env, consts)
        v = v + u if op == '+' else v - u
    return v, i


def _term(w, i, env, consts):
    v, i = _factor(w, i, env, consts)
    while i < len(w) and w[i] == '*':
        u, i = _factor(w, i + 1, env, consts)
        if v.const() is not None:
            v = u.scale(v.c)
        elif u.const() is not None:
            v = v.scale(u.c)
        else:
            raise _NotLin
    return v, i


def _factor(w, i, env, consts):
    if i >= len(w):
        raise _NotLin
    t = w[i]
    if t == '-':
        v, i = _factor(w, i + 1, env, consts)
        return -v, i
    if t == '(':
        j = _close(w, i)
        if j < 0:
            raise _NotLin
        if j == i + 2 and w[i + 1] in CASTS:
            return _factor(w, j + 1, env, consts)
        inner = to_lin(w[i + 1:j], env, consts)
        if inner is None:
            raise _NotLin
        return inner, j + 1
    if re.fullmatch(r'0[xX][0-9a-fA-F]+|\d+', t):
        return Lin(int(t, 0)), i + 1
    if t in env:
        return env[t], i + 1
    if t in consts:
        return Lin(consts[t]), i + 1
    raise _NotLin


def _atom(w, env, consts):
    """A non-linear compile-time expression as one atom, its names
    replaced by what env says they are."""
    out = []
    for t in w:
        if re.fullmatch(r'[A-Za-z_]\w*', t) and t not in consts and t not in CASTS:
            v = env.get(t)
            if v is None or any(a != 'oparg' and not a.startswith('(')
                                for a in v.t):
                return None     # a run-time name, or a loop variable
            out.append(t if t == 'oparg' else '(%s)' % v.text())
        elif t in ('?', ':', '=', '[', ']', '.', '->', ',') or t.startswith('"'):
            return None
        else:
            out.append(t)
    if not out:
        return None
    text = '(%s)' % ' '.join(out)
    text = re.sub(r'\( (%s) \) ' % '|'.join(CASTS), '', text)
    return Lin.atom(text)


def as_bool(v):
    """The 0/1 atom for a condition."""
    if not v.c and len(v.t) == 1:
        (a, k), = v.t.items()
        if k == 1 and is_bool(a):
            return v
    return Lin.atom('(%s != 0)' % v.text())


# ---- cases ----------------------------------------------------------------

def cases(src):
    """[(name, body tokens or None, problem)] for each TARGET in ceval.c,
    in source order."""
    with open(os.path.join(src, 'Python', 'ceval.c')) as f:
        toks = ctokens.tokenize(f.read(), 'ceval.c', directives=True)[0]
    out = []
    for i, t in enumerate(toks):
        if t.text != 'TARGET' or toks[i + 1].text != '(':
            continue
        # the label is TARGET(op) or, where TARGET gives only the name,
        # case TARGET(op): (3.8); the case is what follows it
        k = i + 5 if toks[i + 4].text == ':' else i + 4
        if toks[k].text == 'case' and toks[k + 1].text == 'TARGET':
            k += 1
        name, nxt = toks[i + 2].text, toks[k]
        if nxt.text == '{':
            body = toks[k + 1:ctokens.match_close(toks, k)]
            # PREDICTED(op) as the first statement (3.8; 3.7 puts it before
            # the TARGET) is the label PREDICT jumps to: the case's start
            if [x.text for x in body[:5]] == ['PREDICTED', '(', name, ')', ';']:
                body = body[5:]
            out.append((name, body, None))
        elif nxt.text == 'TARGET':
            out.append((name, None, 'falls into TARGET(%s)' % toks[k + 2].text))
        else:   # TARGET(NOP) FAST_DISPATCH();
            end = next(j for j in range(k, len(toks)) if toks[j].text == ';')
            out.append((name, toks[k:end + 1], None))
    return out


class Context:
    """What the reader knows about the source beyond one case."""

    def __init__(self, src):
        self.consts = constants(src)
        with open(os.path.join(src, 'Python', 'ceval.c')) as f:
            toks = ctokens.tokenize(f.read(), 'ceval.c')[0]
        self.functions = ctokens.functions(toks)
        self.helpers = {}
        self.src = src
        self.answers = {}

    def helper(self, name):
        if name not in self.helpers:
            try:
                self.helpers[name] = helper_effect(name, self.functions, self.consts)
            except Refuse as e:
                self.helpers[name] = e
        h = self.helpers[name]
        if isinstance(h, Refuse):
            raise h
        return h


    def answer(self, fn, k):
        """The value fn returns for the object k, if its body starts by
        testing its first parameter for identity with k ('if (v == Py_True)
        return 1;', as PyObject_IsTrue does), else None."""
        if (fn, k) not in self.answers:
            self.answers[fn, k] = _answer(self.src, fn, k)
        return self.answers[fn, k]


def _definition(src, fn):
    """(params, body) tokens of the function fn, defined in Objects/ or
    Python/ (CPython puts a definition's name at the start of a line)."""
    import csource
    pat = re.compile(r'^%s\(' % re.escape(fn), re.M)
    tree = csource.Tree(src)
    for d in ('Objects', 'Python'):
        for name in sorted(os.listdir(os.path.join(src, d))):
            if not name.endswith('.c'):
                continue
            with open(os.path.join(src, d, name), encoding='latin-1') as f:
                if not pat.search(f.read()):
                    continue
            df = tree.find('%s/%s:%s' % (d, name, fn))
            if df is not None and df.kind == 'function':
                return df.params, df.body
    return None


def _answer(src, fn, k):
    d = _definition(src, fn)
    if d is None:
        return None
    params, body = d
    first = ctokens.split_top(params, ',')
    if not first or not first[0]:
        return None
    p = first[0][-1].text
    nodes = parse(body)
    while nodes:
        n = nodes.pop(0)
        if n.kind == 'stmt' and len(_texts(n.w)) >= 2 and '=' not in _texts(n.w) \
                and all(t.isidentifier() or t == '*' for t in _texts(n.w)):
            continue        # a declaration
        if n.kind != 'if':
            return None
        w = _texts(n.cond)
        ret = _texts(n.then.w) if n.then.kind == 'stmt' else []
        if len(w) != 3 or w[:2] != [p, '=='] or w[2] not in SINGLETONS or \
                len(ret) != 2 or ret[0] != 'return' or not re.fullmatch(r'-?\d+', ret[1]):
            return None
        if w[2] == k:
            return int(ret[1])
        if n.other is not None:
            nodes.insert(0, n.other)
    return None


def helper_effect(name, functions, consts):
    """(parameter names, n): a ceval.c function given a pointer to the
    stack pointer pops n values (a Lin in its parameters). The shape read:
    'PyObject **B = (*P) - E;' and 'while ((*P) > B) { w = EXT_POP(*P);
    Py_DECREF(w); }' as the only change to *P."""
    if name not in functions:
        raise Refuse('core', 'the stack pointer handed to %s, not a function'
                     ' of ceval.c' % name)
    params, body = functions[name]
    names, pp = [], None
    for k, p in enumerate(ctokens.split_top(params, ',')):
        w = [t.text for t in p]
        names.append(w[-1])
        if w[-4:-1] == ['*', '*', '*']:
            pp = w[-1]
    w = [t.text for t in body]
    star = ['(', '*', pp, ')']
    bases = {}
    for k in range(len(w)):
        if w[k:k + 3] == ['PyObject', '*', '*'] and w[k + 4:k + 9] == ['='] + star \
                and w[k + 9] == '-':
            end = w.index(';', k)
            env = {p: Lin.atom(p) for p in names}
            off = to_lin(['0'] + w[k + 9:end], env, consts)
            bases[w[k + 3]] = None if off is None else -off
    at = base = None
    for k in range(len(w)):
        if w[k:k + 6] == ['while', '('] + star and w[k + 6] == '>' \
                and w[k + 7] in bases and w[k + 8:k + 10] == [')', '{']:
            at, base = k, w[k + 7]
    if at is None:
        raise Refuse('core', '%s: does not pop to a base of the stack' % name)
    n = bases[base]
    if n is None or any(a not in names for a in n.t):
        raise Refuse('core', '%s: pops to %s, not linear in its parameters'
                     % (name, base))
    loop = ['while', '('] + star + ['>', base, ')', '{']
    end = ctokens.match_close([_T(t) for t in w], at + len(loop) - 1)
    inner = w[at + len(loop):end]
    m = re.fullmatch(r'(\w+) = EXT_POP \( \* %s \) ; Py_DECREF \( \1 \) ;' % pp,
                     ' '.join(inner))
    if not m:
        raise Refuse('core', '%s: pops other than EXT_POP and release' % name)
    # *P changes nowhere else
    for k in range(len(w)):
        if at <= k < end:
            continue
        if w[k] == pp and w[k - 1] == '*':
            before, after = w[k - 2] if k > 1 else '', w[k + 1:k + 3]
            if before in ('++', '--', 'EXT_POP') or (
                    after[:1] == [')'] and after[1:2] and after[1] in
                    ('=', '+=', '-=', '++', '--')) or after[:1] in (
                    ['='], ['+='], ['-='], ['++'], ['--']):
                raise Refuse('core', '%s: moves the stack pointer elsewhere' % name)
    return names, n


# ---- statements -------------------------------------------------------------

class Node:
    """kind: stmt (toks), block (nodes), if (cond, then, other), while
    (cond, body), for (header, body), switch (cond, body), do (body,
    cond), goto (label), label (name), pp (text). toks: all its tokens."""

    def __init__(self, kind, toks, **kw):
        self.kind, self.toks = kind, toks
        self.__dict__.update(kw)


def parse(toks):
    out, i = [], 0
    while i < len(toks):
        node, i = _statement(toks, i)
        out.append(node)
    return out


def _statement(toks, i):
    t = toks[i]
    if t.kind == 'pp':
        return Node('pp', toks[i:i + 1], text=t.text), i + 1
    if t.text == '{':
        j = ctokens.match_close(toks, i)
        return Node('block', toks[i:j + 1], nodes=parse(toks[i + 1:j])), j + 1
    if t.text == 'if':
        j = ctokens.match_close(toks, i + 1)
        then, k = _statement(toks, j + 1)
        other = None
        if k < len(toks) and toks[k].text == 'else':
            other, k = _statement(toks, k + 1)
        return Node('if', toks[i:k], cond=toks[i + 2:j], then=then,
                    other=other), k
    if t.text in ('while', 'for', 'switch'):
        j = ctokens.match_close(toks, i + 1)
        body, k = _statement(toks, j + 1)
        return Node(t.text, toks[i:k], cond=toks[i + 2:j], body=body), k
    if t.text == 'do':
        body, k = _statement(toks, i + 1)
        j = ctokens.match_close(toks, k + 1)    # while ( ... )
        return Node('do', toks[i:j + 2], body=body, cond=toks[k + 2:j]), j + 2
    if t.text == 'goto':
        return Node('goto', toks[i:i + 3], label=toks[i + 1].text), i + 3
    if t.kind == 'id' and i + 1 < len(toks) and toks[i + 1].text == ':':
        return Node('label', toks[i:i + 2], name=t.text), i + 2
    depth, j = 0, i
    while True:
        if toks[j].text in '([{':
            depth += 1
        elif toks[j].text in ')]}':
            depth -= 1
        elif toks[j].text == ';' and depth == 0:
            return Node('stmt', toks[i:j + 1], w=toks[i:j]), j + 1
        j += 1


def children(n):
    if n.kind == 'block':
        return n.nodes
    if n.kind == 'if':
        return [n.then] + ([n.other] if n.other else [])
    if n.kind in ('while', 'for', 'switch', 'do'):
        return [n.body]
    return []


def flat(nodes):
    """Every node, nested ones included."""
    for n in nodes:
        yield n
        yield from flat(children(n))


def _texts(toks):
    return [t.text for t in toks]


def _block(node):
    return node.nodes if node.kind == 'block' else [node]


def _args(w):
    """['(', a, ',', b, ')'] -> [['a'], ['b']] (token texts each)."""
    if not w or w[0] != '(' or _close(w, 0) != len(w) - 1:
        raise Refuse('stack', 'macro without arguments')
    out, cur, depth = [], [], 0
    for t in w[1:-1]:
        depth += t in ('(', '[')
        depth -= t in (')', ']')
        if t == ',' and depth == 0:
            out.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        out.append(cur)
    return out


def _var(w):
    if len(w) != 1 or not w[0].isidentifier() or w[0] == 'NULL':
        raise Refuse('stack', 'pushes %s, not a variable' % ' '.join(w))
    return w[0]


def _is_type_test(cond):
    """Only tests of exact type, and of an exact type's size against
    oparg: CPython's fast paths."""
    w = _texts(cond)
    k = 0
    calls = []
    while k < len(w):
        t = w[k]
        if t in ('(', ')', '!', '&&', '||'):
            k += 1
        elif cond[k].kind == 'id' and w[k + 1:k + 2] == ['(']:
            j = _close(w, k + 1)
            if not re.fullmatch(r'\( \w+ \)', ' '.join(w[k + 1:j + 1])):
                return False
            calls.append(t)
            if SIZE_TEST.fullmatch(t):
                if w[j + 1:j + 3] not in (['==', 'oparg'], ['!=', 'oparg']):
                    return False
                j += 2
            elif not TYPE_TEST.fullmatch(t):
                return False
            k = j + 1
        else:
            return False
    return bool(calls) and any(TYPE_TEST.fullmatch(c) for c in calls)


# ---- restructuring: arms that end the case, if-chains, fast paths ------------

class _Tok:
    """A token made for a node the reader builds."""
    kind = 'op'

    def __init__(self, text):
        self.text = text


def _is_dispatch(n):
    w = _texts(n.w) if n.kind == 'stmt' else []
    return len(w) == 3 and w[0] in DISPATCHES and w[1:] == ['(', ')']


def _is_error(n):
    return n.kind == 'goto' and n.label == 'error'


def _ends(node, test):
    b = _block(node) if node is not None else []
    return bool(b) and test(b[-1])


def _mk_block(nodes):
    toks = [_Tok('{')] + [t for n in nodes for t in n.toks] + [_Tok('}')]
    return Node('block', toks, nodes=list(nodes))


def _mk_if(cond, then, other):
    toks = [_Tok('if'), _Tok('(')] + list(cond) + [_Tok(')')] + list(then.toks)
    if other is not None:
        toks += [_Tok('else')] + list(other.toks)
    return Node('if', toks, cond=list(cond), then=then, other=other)


def _cond_toks(text):
    return [_Tok(t) for t in text.split(' ')]


def _negate(c):
    """'! ( c )', or c with its operator turned round if it is one compare."""
    w = c.split(' ')
    if len(w) == 3 and w[1] in NEGATE:
        return '%s %s %s' % (w[0], NEGATE[w[1]], w[2])
    return '! ( %s )' % c


def _int_compare(cond):
    """(variable, op, value) for 'x OP N', N an integer literal."""
    w = _texts(cond)
    if len(w) == 4 and w[2] == '-':
        w = [w[0], w[1], '-' + w[3]]
    if len(w) == 3 and w[0].isidentifier() and w[1] in NEGATE and \
            re.fullmatch(r'-?\d+', w[2]):
        return w[0], w[1], int(w[2])
    return None


# sets of integers: sorted disjoint [lo, hi] (None: unbounded)
def _iset(op, n):
    return {'==': [(n, n)], '!=': [(None, n - 1), (n + 1, None)],
            '<': [(None, n - 1)], '<=': [(None, n)], '>': [(n + 1, None)],
            '>=': [(n, None)]}[op]


def _ikey(v, side):
    return (float('-inf') if side == 0 else float('inf')) if v is None else v


def _iunion(a, b):
    out = []
    for lo, hi in sorted(a + b, key=lambda x: _ikey(x[0], 0)):
        if out and (out[-1][1] is None or (lo is not None and lo <= out[-1][1] + 1)):
            if out[-1][1] is not None and (hi is None or hi > out[-1][1]):
                out[-1] = (out[-1][0], hi)
        else:
            out.append((lo, hi))
    return out


def _icomplement(a):
    out, at = [], None      # at: first value not yet covered (None: -inf)
    for lo, hi in a:
        if lo is not None and (at is None or lo > at):
            out.append((at, lo - 1))
        if hi is None:
            return out
        at = hi + 1
    out.append((at, None))
    return out


def _icond(x, a):
    """C text testing x in a, for a ray; None otherwise."""
    if len(a) == 1:
        lo, hi = a[0]
        if lo is None and hi is not None:
            return '%s < %d' % (x, hi + 1)
        if hi is None and lo is not None:
            return '%s > %d' % (x, lo - 1)
    return None


def _ihas(a, v):
    return any(_ikey(lo, 0) <= v <= _ikey(hi, 1) for lo, hi in a)


def _flat_stmts(nodes):
    """Statement texts of nodes, blocks flattened, empty statements
    dropped; None if there is anything else."""
    out = []
    for n in nodes:
        if n.kind == 'block':
            sub = _flat_stmts(n.nodes)
            if sub is None:
                return None
            out += sub
        elif n.kind == 'stmt':
            if _texts(n.w) and not _is_dispatch(n):
                out.append(_texts(n.w))
        else:
            return None
    return out


# ---- references to the stack, resolved once the layout is known --------------

class Ref:
    """A slot (index None), an element of an array (index: a Lin), or the
    address of an array (base) in the body's text."""

    def __init__(self, lo, index=None, base=False):
        self.lo, self.index, self.base = lo, index, base


class WriteMark:
    """A write of var into the slot at pos, in the body's text."""

    def __init__(self, pos, var):
        self.pos, self.var = pos, var


class Interval:
    """[lo, lo + size) below or above the entry stack pointer. A loop's
    is filled in when the loop ends."""

    def __init__(self, lo=None, size=None):
        self.lo, self.size = lo, size

    @property
    def hi(self):
        return self.lo + self.size


class State:
    """One path through the case."""

    def __init__(self):
        self.sp = Lin()
        self.writes = []    # [(Interval, var or None: written by a call)]
        self.gone = []      # input Intervals no longer in their slots
        self.released = []  # input Intervals released or stored
        self.bound = {}     # variable -> the input Interval it names
        self.pending = None  # Interval a call was given to fill
        self.alias = {}     # local copies of the stack pointer -> position
        self.env = {'oparg': Lin.atom('oparg')}
        self.jumped = False  # JUMPTO/JUMPBY on this path

    def copy(self):
        s = State.__new__(State)
        s.sp, s.pending, s.jumped = self.sp, self.pending, self.jumped
        s.writes, s.gone, s.released = list(self.writes), list(self.gone), list(self.released)
        s.bound, s.alias, s.env = dict(self.bound), dict(self.alias), dict(self.env)
        return s


# ---- the symbolic run -------------------------------------------------------

class Case:
    def __init__(self, name, ctx):
        self.name, self.ctx = name, ctx
        self.reads = []     # [(Interval, var or None, kind)] kind: var array args
        self.lines = []     # [[str | Ref]]
        self.decl = {}      # variable -> type of a bare declaration (depth 0)
        self.snaps = []     # [(State, null variable)] at each error exit
        self.final = []     # States that reached DISPATCH
        self.depth = 0
        self.ended = False
        self.loop = None    # the for-loop being read: (var, N, accesses)

    def cmp(self, a, b):
        d = a - b
        if d.const() is not None:
            return (d.c > 0) - (d.c < 0)
        vals = [d.value(o, self.ctx.consts) for o in SAMPLES]
        if all(v == 0 for v in vals):
            return 0
        if all(v >= 0 for v in vals):
            return 1
        if all(v <= 0 for v in vals):
            return -1
        raise Refuse('oparg', 'cannot order stack positions %s and %s'
                     % (a.text(), b.text()))

    def covers(self, j, i):
        return self.cmp(j.lo, i.lo) <= 0 and self.cmp(i.hi, j.hi) <= 0

    def lin(self, w, state):
        v = to_lin(w, state.env, self.ctx.consts)
        if v is None:
            if 'oparg' in w and 'oparg' not in state.env:
                raise Refuse('control flow', 'oparg used after a loop counts it down')
            raise Refuse('oparg', 'stack position or adjustment %s' % ' '.join(w))
        return v

    def emit(self, items):
        self.lines.append(['    ' * self.depth] + items if self.depth else items)

    def record(self, iv, var, kind):
        for j, v, k in self.reads:
            if j.lo == iv.lo and j.size == iv.size and v == var and k == kind:
                return
            if var and v and v != var and j.lo == iv.lo and kind == k == 'var':
                raise Refuse('stack', 'reads input %s twice' % iv.lo.text())
        self.reads.append((iv, var, kind))

    def write_at(self, state, pos):
        return next((x for x in state.writes if x[0].lo == pos), None)

    # stack effects on one state
    def read(self, state, depth, var):
        pos = state.sp - depth
        if self.write_at(state, pos) or self.cmp(pos, Lin(-1)) > 0:
            raise Refuse('stack', 'reads back a slot it wrote')
        iv = Interval(pos, Lin(1))
        if any(self.covers(g, iv) for g in state.gone):
            raise Refuse('stack', 'reads input %s after it went' % pos.text())
        self.record(iv, var, 'var')
        if var:
            state.bound[var] = iv
        return iv

    def pop(self, state, var):
        iv = self.read(state, Lin(1), var)
        state.gone.append(iv)
        state.sp = state.sp - 1
        return iv

    def write(self, state, depth, var):
        pos = state.sp - depth
        old = self.write_at(state, pos)
        iv = Interval(pos, Lin(1))
        if old:
            state.writes.remove(old)
        elif self.cmp(pos, Lin(0)) < 0 and not any(self.covers(g, iv)
                                                   for g in state.gone):
            names = [v for j, v, k in self.reads if k == 'var' and j.lo == pos]
            if not names and not any(self.covers(j, iv) for j, _, _ in self.reads):
                raise Refuse('stack', 'overwrites input %s it did not read' % pos.text())
            if names != [var]:
                state.gone.append(iv)
        state.writes.append((Interval(pos, Lin(1)), var))

    def move(self, state, n):
        if self.cmp(n, Lin(0)) < 0:
            drop = Interval(state.sp + n, -n)
            for x in list(state.writes):
                if self.covers(drop, x[0]):
                    raise Refuse('stack', 'drops %s from the stack' % x[1])
            if self.cmp(drop.lo, Lin(0)) < 0:
                below = Interval(drop.lo, Lin(0) - drop.lo) \
                    if self.cmp(drop.hi, Lin(0)) > 0 else drop
                if not self.readable(below):
                    raise Refuse('stack', 'drops input %s it did not read'
                                 % below.lo.text())
                state.gone.append(below)
        elif state.pending is not None and state.pending.lo == state.sp \
                and state.pending.size == n:
            state.writes.append((state.pending, None))
            state.pending = None
        elif n.const() is None:
            raise Refuse('stack', 'pushes %s slots it never set' % n.text())
        state.sp = state.sp + n

    def readable(self, iv):
        """Is every slot of iv read by something?"""
        inside = [p for j, _, _ in self.reads for p in (j.lo, j.hi)
                  if self.cmp(iv.lo, p) <= 0 and self.cmp(p, iv.hi) <= 0]
        pts = sorted({iv.lo, iv.hi} | set(inside), key=functools.cmp_to_key(self.cmp))
        for a, b in zip(pts, pts[1:]):
            part = Interval(a, b - a)
            if self.cmp(a, b) == 0:
                continue
            if not any(self.covers(j, part) for j, _, _ in self.reads):
                return False
        return True

    def release(self, state, var):
        if var in state.bound:
            state.released.append(state.bound[var])

    # the walk: text is emitted once, effects are applied to every path
    def run(self, nodes, states, top=True):
        for k, node in enumerate(nodes):
            if self.ended and top:
                raise Refuse('control flow', 'code after DISPATCH')
            if not states:
                if node.kind in ('label', 'pp'):
                    raise Refuse('control flow', node.kind)
                continue
            states = self.node(node, states, top and k == len(nodes) - 1)
        return states

    def node(self, node, states, last):
        kind = node.kind
        if kind == 'block':
            return self.run(node.nodes, states, top=False)
        if kind == 'stmt':
            return self.statement(node.w, states, last)
        if kind == 'if':
            return self.branch(node, states)
        if kind == 'while':
            return self.count_down(node, states)
        if kind == 'for':
            return self.for_loop(node, states)
        if kind in ('switch', 'do') and not self.touches(node.toks, states):
            self.emit([' '.join(_texts(node.toks))])
            return states
        if kind == 'goto':
            raise Refuse('control flow', 'goto %s outside an error test' % node.label)
        raise Refuse('control flow', getattr(node, 'text', kind))

    def touches(self, toks, states):
        """Does this code do anything to the stack, jump, or release or
        rebind a variable that names an input?"""
        w = _texts(toks)
        bound = set().union(*(s.bound for s in states)) | set().union(
            *(s.alias for s in states))
        for k, t in enumerate(w):
            if t in STACK_MACROS or t in DISPATCHES or t in JUMPS or t in (
                    'stack_pointer', 'goto', 'SETLOCAL', 'return'):
                return True
            if t in bound and (w[k + 1:k + 2] == ['='] or w[k - 2:k] in (
                    ['Py_DECREF', '('], ['Py_XDECREF', '('], ['Py_CLEAR', '('])):
                return True
        return False

    # -- restructuring, before the run (rules in the module's docstring)
    def restructure(self, nodes):
        nodes = list(nodes)
        k = 0
        while k < len(nodes):
            n = nodes[k]
            if n.kind == 'if' and _ends(n.then, _is_dispatch):
                rest = nodes[k + 1:]
                if self.singleton_arm(n, rest):
                    del nodes[k]
                    continue
                end = next(x for x in reversed(_block(n.then)) if _is_dispatch(x))
                return nodes[:k] + self.chain(self.absorb(n, rest)) + [end]
            if n.kind == 'if':
                c = self.chain(n)
                nodes[k:k + 1] = c
                k += len(c)
                continue
            k += 1
        return nodes

    def absorb(self, n, rest):
        """'if (C) { A; DISPATCH(); } else B; REST' is 'if (C) A else { B;
        REST }': the rest of the case is the other arm."""
        then = _mk_block(_block(n.then)[:-1])
        o = n.other
        if o is None:
            other = _mk_block(self.tail(rest))
        elif o.kind == 'if' and _ends(o.then, _is_dispatch):
            other = self.absorb(o, rest)
        elif _ends(o, _is_dispatch) or _ends(o, _is_error):
            if rest:
                raise Refuse('control flow', 'code after DISPATCH')
            other = _mk_block(_block(o)[:-1]) if _ends(o, _is_dispatch) else o
        else:
            other = _mk_block(_block(o) + self.tail(rest))
        return _mk_if(n.cond, then, other)

    def tail(self, rest):
        r = self.restructure(rest)
        if r and _is_dispatch(r[-1]):
            return r[:-1]
        if r and _is_error(r[-1]):
            return r
        raise Refuse('control flow', 'does not end with DISPATCH')

    def chain(self, n):
        """'if (x == 0) A else if (x > 0) B else { ...; goto error; }', x an
        int compared with constants: the error arm is taken for the values
        no other arm takes, so it is tested first, as its own exit; an arm
        left with only the values it tests becomes the else."""
        arms, x = [], n
        while True:
            c = _int_compare(x.cond) if x.kind == 'if' else None
            if c is None or x.other is None or (arms and c[0] != arms[0][0][0]):
                return [n]
            if x is n and x.other.kind != 'if':
                return [n]      # one test and an error arm: the rule below
            arms.append((c, x.then))
            if x.other.kind != 'if':
                final = x.other
                break
            x = x.other
        if not _ends(final, _is_error):
            return [n]
        var = arms[0][0][0]
        covered = []
        for (_, op, v), _ in arms:
            covered = _iunion(covered, _iset(op, v))
        err = _icond(var, _icomplement(covered))
        if err is None:
            raise Refuse('control flow', 'the error arm of a chain on %s is no ray' % var)
        exit_ = _mk_if(_cond_toks(err), final, None)
        # the chain without its error arm; the last arm's test is implied
        # if every value reaching it passes it
        before = []
        for (_, op, v), _ in arms[:-1]:
            before = _iunion(before, _iset(op, v))
        (_, op, v), body = arms[-1]
        reach = _icomplement(_iunion(_icomplement(covered), before))
        if _iunion(_iset(op, v), reach) == _iunion(_iset(op, v), []):
            other = body
        else:
            other = _mk_if(_cond_toks('%s %s %d' % (var, op, v)), body, None)
        for (_, op, v), body in reversed(arms[:-1]):
            other = _mk_if(_cond_toks('%s %s %d' % (var, op, v)), body, other)
        return [exit_, other]

    def singleton_arm(self, n, rest):
        """Rule singleton_test_generic_path: 'if (x == K) { A; DISPATCH(); }'
        for K one of SINGLETONS is CPython's fast path, dropped, when the rest
        of the case calls a function on x that answers K itself first (its
        body starts 'if (v == K) return r;') and, with that r, does A."""
        w = _texts(n.cond)
        if n.other is not None or len(w) != 3 or w[1] != '==' or w[2] not in SINGLETONS:
            return False
        x, k = w[0], w[2]
        arm = _flat_stmts(_block(n.then))
        gen, var, r = [], None, None
        for m in rest:
            if m.kind == 'if' and var is None:
                c = _texts(m.cond)
                if len(c) == 3 and c[:2] == [x, '=='] and c[2] in SINGLETONS and c[2] != k:
                    continue        # not taken when x is k
            if m.kind == 'stmt':
                if _is_dispatch(m):
                    break
                d = _texts(m.w)
                while d and d[0] in DECL_TYPES:
                    d = d[1:]
                if len(d) == 6 and d[1] == '=' and d[3] == '(' and d[4:] == [x, ')'] \
                        and var is None:
                    var, r = d[0], self.ctx.answer(d[2], k)
                    if r is None:
                        return False
                    continue
                if len(d) == 1:
                    continue        # a declaration
                if d:
                    gen.append(_texts(m.w))
                continue
            if m.kind == 'if' and var is not None:
                picked = self.fold_chain(m, var, r)
                if picked is None:
                    return False
                sub = _flat_stmts(picked)
                if sub is None:
                    return False
                gen += sub
                continue
            return False
        return var is not None and arm is not None and arm == gen

    def fold_chain(self, m, var, r):
        """The nodes the if-chain m runs when var is r; None for an error."""
        while m is not None and m.kind == 'if':
            c = _int_compare(m.cond)
            if c is None or c[0] != var:
                return None
            if _ihas(_iset(c[1], c[2]), r):
                m = m.then
                break
            m = m.other
        if m is None:
            return []
        b = _block(m)
        if b and _is_error(b[-1]):
            return None
        return b

    # -- branches
    def branch(self, node, states):
        cond, then, other = node.cond, node.then, node.other
        c = ' '.join(_texts(cond))
        dead = self.opcode_test(cond)
        if dead is not None:
            arm = then if dead else other
            return self.node(arm, states, False) if arm else states
        body = _block(then)
        ci = (self.expression(_texts(cond), states) if self.touches(cond, states)
              else [c])
        if body and body[-1].kind == 'goto' and body[-1].label == 'error' and other is None:
            return self.error_exit(c, body[:-1], states, ci)
        if other is not None and _is_type_test(cond):
            # the generic path; the exact-type fast path is CPython's
            return self.run(_block(other), states, top=False)
        if other is not None:
            obody = _block(other)
            if obody and obody[-1].kind == 'goto' and obody[-1].label == 'error' \
                    and not self.ends_in_error(then):
                neg = _negate(c)
                states = self.error_exit(neg, obody[:-1], states,
                                         neg.split(' ') if ci == [c] else
                                         ['!', '('] + ci + [')'])
                return self.run(body, states, top=False)
        ct = to_lin(_texts(cond), states[0].env, self.ctx.consts)
        if ct is not None and all(to_lin(_texts(cond), s.env, self.ctx.consts) == ct
                                  for s in states):
            popped = self.conditional_pop(body, other)
            if popped:
                return self.cond_input(as_bool(ct), c, popped, body[1:], states)
        if not self.touches(node.toks, states):
            self.emit([' '.join(_texts(node.toks))])
            return states
        # a branch on a run-time value: both arms, as separate paths
        self.emit(['if', '('] + ci + [')', '{'])
        self.depth += 1
        a = self.run(body, [s.copy() for s in states], top=False)
        self.depth -= 1
        if other is not None:
            self.emit(['}', 'else', '{'])
            self.depth += 1
            null = re.fullmatch(r'(\w+) != NULL', c)
            ss = [s.copy() for s in states]
            if null:        # x is NULL on this arm: nothing to release
                for s in ss:
                    self.release(s, null.group(1))
            b = self.run(_block(other), ss, top=False)
            self.depth -= 1
        else:
            b = states
        null = re.fullmatch(r'(\w+) == NULL', c)
        if null:
            for s in a:
                self.release(s, null.group(1))
        self.emit(['}'])
        return a + b

    def ends_in_error(self, node):
        b = _block(node)
        return bool(b) and b[-1].kind == 'goto' and b[-1].label == 'error'

    def opcode_test(self, cond):
        """True or False if cond starts with a test of opcode that decides
        it, else None."""
        w = _texts(cond)
        if len(w) >= 3 and w[0] == 'opcode' and w[1] in ('==', '!='):
            same = w[2] == self.name
            rest = w[3:]
            if rest and rest[0] not in ('&&', '||'):
                return None
            val = same if w[1] == '==' else not same
            if not rest or (rest[0] == '&&' and not val) or (rest[0] == '||' and val):
                return val
        return None

    def conditional_pop(self, body, other):
        """x if the arm starts 'x = POP()' and the other arm is empty or
        'x = NULL'."""
        if not body or body[0].kind != 'stmt':
            return None
        w = _texts(body[0].w)
        if w[:2] == ['PyObject', '*']:
            w = w[2:]
        if len(w) != 5 or w[1:] != ['=', 'POP', '(', ')']:
            return None
        x = w[0]
        if other is not None:
            o = _block(other)
            if len(o) != 1 or o[0].kind != 'stmt' or _texts(o[0].w) != [x, '=', 'NULL']:
                return None
        return x

    def cond_input(self, atom, c, x, rest, states):
        for s in states:
            iv = Interval(s.sp - atom, atom)
            if self.write_at(s, iv.lo):
                raise Refuse('stack', 'conditional pop of a slot it wrote')
            self.record(iv, x, 'var')
            s.bound[x] = iv
            s.gone.append(iv)
            s.sp = s.sp - atom
        self.decl.pop(x, None)
        if not rest:
            return states
        self.emit(['if', '(', c, ')', '{'])
        self.depth += 1
        a = self.run(rest, [s.copy() for s in states], top=False)
        self.depth -= 1
        self.emit(['}'])
        return a + states

    def error_exit(self, c, body, states, ci):
        label, simple, dead, moves = 'error', True, [], []
        for node in body:
            w = _texts(node.w) if node.kind == 'stmt' else None
            if w and len(w) == 4 and w[0] in ('Py_DECREF', 'Py_XDECREF') \
                    and all(w[2] in s.bound for s in states):
                dead.append(w[2])
                continue
            n = _pop_count(w)
            if n:
                moves.append(Lin(-n))   # pops: the exit pops anyway
                continue
            u = w[:2] + w[4:] if w and w[2:4] == ['tstate', ','] else w
            form = next((lab for f, lab in ERROR_FORMS if u and u[:len(f)] == f), None)
            if form and label == 'error':
                label = form
                continue
            simple = False
        m = re.fullmatch(r'(\w+) == NULL', c)
        null = m.group(1) if m else None
        if simple:
            for s in states:
                s2 = s.copy()
                for v in dead:
                    self.release(s2, v)
                for n in moves:
                    self.move(s2, n)
                self.snaps.append((s2, null))
            self.emit(['ERROR_IF', '('] + ci + [',', label, ')', ';'])
            return states
        self.emit(['if', '('] + ci + [')', '{'])
        self.depth += 1
        copies = [s.copy() for s in states]
        for node in body:
            w = _texts(node.w) if node.kind == 'stmt' else None
            if w and len(w) == 4 and w[0] in ('Py_DECREF', 'Py_XDECREF') \
                    and all(w[2] in s.bound for s in copies):
                for s in copies:
                    self.release(s, w[2])
                continue
            copies = self.node(node, copies, False)
        for s in copies:
            for n in moves:
                self.move(s, n)
            self.snaps.append((s, null))
        self.emit(['ERROR_IF', '(', 'true', ',', 'error', ')', ';'])
        self.depth -= 1
        self.emit(['}'])
        return states

    # -- loops
    def count_down(self, node, states):
        """while (--oparg >= 0) or while (oparg--), popping k per turn."""
        w = _texts(node.cond)
        if w not in (['--', 'oparg', '>=', '0'], ['oparg', '--']):
            raise Refuse('control flow', 'while (%s)' % ' '.join(w))
        if any('oparg' not in s.env for s in states):
            raise Refuse('control flow', 'oparg used after a loop counts it down')
        body = _block(node.body)
        i = self.fresh()
        pops, released, lines, elem = 0, 0, [], {}
        for n in body:
            if n.kind != 'stmt':
                raise Refuse('control flow', 'loop over oparg with %s' % n.kind)
            w = _texts(n.w)
            d = w[2:] if w[:2] == ['PyObject', '*'] else w
            if len(d) == 5 and d[1:] == ['=', 'POP', '(', ')']:
                pops += 1
                elem[d[0]] = pops
                lines.append(w[:-3] + [('elem', pops)])
            elif w == ['Py_DECREF', '(', 'POP', '(', ')', ')']:
                pops += 1
                released += 1
                lines.append(['Py_DECREF', '(', ('elem', pops), ')'])
            elif len(w) == 4 and w[0] == 'Py_DECREF' and w[2] in elem:
                released += 1
                lines.append(w)
            elif STACK_MACROS & set(w) or 'stack_pointer' in w:
                raise Refuse('stack', 'loop over oparg does %s' % ' '.join(w))
            else:
                lines.append([i if t == 'oparg' else t for t in w])
        if not pops:
            raise Refuse('control flow', 'loop over oparg without pops')
        if released not in (0, pops):
            raise Refuse('stack', 'loop over oparg releases some of what it pops')
        n_ = states[0].env['oparg'].scale(pops)
        lo = None
        for s in states:
            iv = Interval(s.sp - n_, n_)
            if lo is not None and iv.lo != lo:
                raise Refuse('stack', 'paths disagree on the stack')
            lo = iv.lo
            if any(self.cmp(x[0].lo, iv.lo) >= 0 for x in s.writes):
                raise Refuse('stack', 'loop pops a slot it wrote')
            if self.cmp(iv.hi, Lin(0)) > 0:
                raise Refuse('stack', 'loop pops above the stack')
            self.record(iv, None, 'array')
            s.gone.append(iv)
            if released:
                s.released.append(iv)
            s.sp = iv.lo
            del s.env['oparg']
        k = Lin.atom(i)
        self.emit(['for', '(', 'int', i, '=', 'oparg', ';', '--', i, '>=', '0', ';', ')', '{'])
        for ln in lines:
            items = []
            for t in ln:
                if isinstance(t, tuple):
                    items.append(Ref(lo, k.scale(pops) + (pops - t[1])))
                else:
                    items.append(t)
            self.emit(['    '] + items + [';'])
        self.emit(['}'])
        return states

    def fresh(self):
        used = {t for ln in self.lines for t in ln if isinstance(t, str)}
        return next(v for v in ('i', 'j', 'k', 'n') if v not in used
                    and v not in self.decl)

    def for_loop(self, node, states):
        """for (i = N; i > 0; i--), reading PEEK(a*i + b)."""
        w = _texts(node.cond)
        m = re.fullmatch(r'(\w+) = (.+) ; \1 > 0 ; \1 --', ' '.join(w))
        if not m or self.loop:
            raise Refuse('control flow', 'for (%s)' % ' '.join(w))
        i = m.group(1)
        n_ = self.lin(m.group(2).split(' '), states[0])
        if i in self.decl:
            self.emit(self.decl.pop(i) + [i, ';'])
        self.loop = (i, n_, [])
        for s in states:
            s.env[i] = Lin.atom(i)
        sp0 = [s.sp for s in states]
        self.emit(['for', '('] + w + [')', '{'])
        self.depth += 1
        out = self.run(_block(node.body), states, top=False)
        self.depth -= 1
        self.emit(['}'])
        _, _, acc = self.loop
        self.loop = None
        for s in out:
            s.env.pop(i, None)
        if any(s.sp != sp0[0] for s in out) or any(p != sp0[0] for p in sp0):
            raise Refuse('stack', 'loop moves the stack pointer')
        if not acc:
            return out
        a = {e.coeff(i) for e, _ in acc}
        if len(a) != 1 or a.pop() <= 0:
            raise Refuse('stack', 'loop reads the stack unevenly')
        a = acc[0][0].coeff(i)
        bs = [e - Lin.atom(i).scale(a) for e, _ in acc]
        bmax = max(bs, key=functools.cmp_to_key(self.cmp))
        offs = {(bmax - b).const() for b in bs}
        if None in offs or offs != set(range(a)):
            raise Refuse('stack', 'loop reads the stack unevenly')
        lo = sp0[0] - n_.scale(a) - bmax
        for (e, ref), b in zip(acc, bs):
            ref.lo = lo
            ref.index = n_.scale(a) - Lin.atom(i).scale(a) + (bmax - b)
            ref.iv.lo, ref.iv.size = lo, n_.scale(a)
        self.record(acc[0][1].iv, None, 'array')
        return out

    # -- statements
    def statement(self, toks, states, last):
        w = _texts(toks)
        if not w:
            return states
        head = w[0]
        if head in DISPATCHES and w[1:] == ['(', ')']:
            if not last:
                raise Refuse('control flow', '%s before the end' % head)
            self.ended = True
            self.final += states
            return []
        if head in DROPPED:
            return states
        if head in DECL_TYPES and self.split_decl(w):
            for part in self.split_decl(w):
                states = self.statement_w(part, states)
            return states
        return self.statement_w(w, states)

    def split_decl(self, w):
        parts = ctokens.split_top([_T(t) for t in w], ',')
        if len(parts) < 2:
            return None
        typ = [w[0]]
        return [[t.text for t in p] if k == 0 else typ + [t.text for t in p]
                for k, p in enumerate(parts)]

    def statement_w(self, w, states):
        head = w[0]
        # T x;  T *x;  T **x;
        if (len(w) >= 2 and w[0] in DECL_TYPES and w[-1].isidentifier()
                and all(t == '*' for t in w[1:-1])):
            if self.depth == 0 and w[0] in ('PyObject', 'int'):
                self.decl[w[-1]] = w[:-1]
            else:
                self.emit(w + [';'])
            return states
        if len(w) > 2 and w[1] == '=' and w[0] in self.decl and self.depth == 0:
            w = self.decl.pop(w[0]) + w
        decl = []
        k = 0
        while k < len(w) and (w[k] in DECL_TYPES or w[k] == '*'):
            k += 1
        decl, lhs = w[:k], w[k:]
        # x = STACKREAD
        if len(lhs) >= 4 and lhs[1] == '=' and lhs[2] in STACK_MACROS:
            x, op, rest = lhs[0], lhs[2], lhs[3:]
            if self.loop and op == 'PEEK' and any(
                    self.lin(_args(rest)[0], s).coeff(self.loop[0]) for s in states):
                self.emit(decl + [x, '=', self.peek_in_loop(rest, states, x), ';'])
                return states
            if op == 'POP' and rest == ['(', ')']:
                for s in states:
                    self.pop(s, x)
                self.decl.pop(x, None)
                return states
            if op in READS and rest == ['(', ')']:
                for s in states:
                    self.read(s, Lin(READS[op]), x)
                self.decl.pop(x, None)
                return states
            if op == 'PEEK':
                for s in states:
                    self.read(s, self.lin(_args(rest)[0], s), x)
                self.decl.pop(x, None)
                return states
            raise Refuse('stack', ' '.join(w))
        # x = C ? POP() : NULL
        m = re.fullmatch(r'(\w+) = (.+) \? POP \( \) : NULL', ' '.join(lhs))
        if m:
            ct = to_lin(m.group(2).split(' '), states[0].env, self.ctx.consts)
            if ct is None:
                raise Refuse('control flow', ' '.join(w))
            return self.cond_input(as_bool(ct), m.group(2), m.group(1), [], states)
        if head in STACK_MACROS and len(w) > 1 and w[1] == '(':
            args = _args(w[1:])
            if head == 'PUSH' and len(args) == 1:
                for s in states:
                    self.move(s, Lin(1))
                    self.write(s, Lin(1), _var(args[0]))
                self.mark(states, Lin(1), args[0])
                return states
            if head in WRITES and len(args) == 1:
                for s in states:
                    self.write(s, Lin(WRITES[head]), _var(args[0]))
                self.mark(states, Lin(WRITES[head]), args[0])
                return states
            if head == 'SET_VALUE' and len(args) == 2:
                for s in states:
                    self.write(s, self.lin(args[0], s), _var(args[1]))
                self.mark(states, self.lin(args[0], states[0]), args[1])
                return states
            if head in MOVES and len(args) == 1:
                for s in states:
                    self.move(s, self.lin(args[0], s).scale(MOVES[head]))
                return states
            raise Refuse('stack', ' '.join(w))
        if w[:2] == ['stack_pointer', '+='] or w[:2] == ['stack_pointer', '-=']:
            for s in states:
                n = self.lin(w[2:], s)
                self.move(s, n if w[1] == '+=' else -n)
            return states
        if len(w) == 3 and w[:2] == ['stack_pointer', '='] and w[2] in states[0].alias:
            for s in states:
                s.sp = s.alias[w[2]]
            return states
        if w == ['(', 'void', ')', 'POP', '(', ')']:
            for s in states:
                self.pop(s, None)
            return states
        if w == ['Py_DECREF', '(', 'POP', '(', ')', ')']:
            refs = []
            for s in states:
                iv = self.pop(s, None)
                s.released.append(iv)
                refs.append(iv.lo)
            self.same(refs)
            self.emit(['Py_DECREF', '(', Ref(refs[0]), ')', ';'])
            return states
        if head in JUMPS:
            for s in states:
                s.jumped = True
        if 'POP' in w:
            raise Refuse('stack', 'pop inside %s' % ' '.join(w))
        if any(t in WRITES or t in MOVES or t in ('PUSH', 'SET_VALUE') for t in w):
            raise Refuse('stack', 'stack access inside %s' % ' '.join(w))
        items = self.expression(w, states)
        # what the statement does to the variables that name inputs
        if len(w) == 4 and w[0] in ('Py_DECREF', 'Py_XDECREF'):
            for s in states:
                self.release(s, w[2])
        if len(w) == 6 and w[:2] == ['SETLOCAL', '(']:
            for s in states:
                self.release(s, w[4])
        if len(lhs) > 2 and lhs[1] == '=' and lhs[0].isidentifier():
            for s in states:
                s.bound.pop(lhs[0], None)
            if decl[:1] in (['int'], ['Py_ssize_t']) and len(decl) == 1:
                for s in states:
                    v = to_lin(lhs[2:], s.env, self.ctx.consts)
                    if v is not None:
                        s.env[lhs[0]] = v
        self.emit(items + [';'])
        return states

    def mark(self, states, depth, var):
        """Where the write goes, for definition(): if paths write different
        variables into one output, it is named and each write assigns it."""
        self.emit([WriteMark(self.same([s.sp - depth for s in states]), _var(var))])

    def same(self, values):
        if any(v != values[0] for v in values):
            raise Refuse('stack', 'paths disagree on the stack')
        return values[0]

    def peek_in_loop(self, rest, states, var):
        i, n_, acc = self.loop
        args = _args(rest)
        if len(args) != 1:
            raise Refuse('stack', 'PEEK without one argument')
        es = [self.lin(args[0], s) for s in states]
        e = self.same(es)
        if not e.coeff(i):
            ivs = [self.read(s, e, var) for s in states]
            return Ref(self.same([iv.lo for iv in ivs]))
        ref = Ref(None)
        ref.iv = acc[0][1].iv if acc else Interval()
        acc.append((e, ref))
        if var:
            for s in states:
                s.bound[var] = ref.iv
        return ref

    def expression(self, w, states):
        """Token texts with stack reads and stack pointers in an
        expression replaced by references."""
        out, k = [], 0
        while k < len(w):
            t = w[k]
            if t in READS or t == 'PEEK':
                j = _close(w, k + 1)
                if j < 0:
                    raise Refuse('stack', 'macro without arguments')
                args = _args(w[k + 1:j + 1])
                if t == 'PEEK' and self.loop:
                    out.append(self.peek_in_loop(w[k + 1:j + 1], states, None))
                else:
                    ivs = [self.read(s, Lin(READS[t]) if t in READS
                                     else self.lin(args[0], s), None) for s in states]
                    out.append(Ref(self.same([iv.lo for iv in ivs])))
                k = j + 1
                continue
            if t == 'stack_pointer':
                j = k + 1
                depth = 0
                while j < len(w) and not (depth == 0 and w[j] in (',', ')', ';')):
                    depth += w[j] in ('(', '[')
                    depth -= w[j] in (')', ']')
                    j += 1
                offs = [self.lin(['0'] + w[k + 1:j], s) for s in states]
                off = self.same(offs)
                pos = self.same([s.sp + off for s in states])
                sign = self.cmp(off, Lin(0))
                if sign < 0 and self.cmp(states[0].sp, Lin(0)) <= 0:
                    iv = Interval(pos, -off)
                    for s in states:
                        if any(self.covers(g, iv) for g in s.gone) or s.writes and any(
                                self.cmp(x[0].lo, pos) >= 0 for x in s.writes):
                            raise Refuse('stack', 'pointer to slots that went')
                    self.record(iv, None, 'array')
                    out.append(Ref(pos, base=True))
                else:
                    if sign > 0:
                        for s in states:
                            s.pending = Interval(s.sp, off)
                    out += ['stack_pointer'] + ([] if pos.const() == 0 else
                                                ['+', pos.text()] if not pos.text().startswith('-')
                                                else ['-', (-pos).text()])
                k = j
                continue
            if t == '&' and k + 1 < len(w) and w[k + 1] in states[0].alias:
                self.helper_call(w, k, states)
            out.append(t)
            k += 1
        # PyObject **sp = stack_pointer...: a copy of the stack pointer
        d = [x for x in w]
        m = 0
        while m < len(d) and (d[m] in DECL_TYPES or d[m] == '*'):
            m += 1
        if len(d) > m + 2 and d[m + 1] == '=' and d[m + 2] == 'stack_pointer':
            name = d[m]
            for s in states:
                rest = d[m + 3:]
                s.alias[name] = s.sp + self.lin(['0'] + rest, s)
        return out

    def helper_call(self, w, k, states):
        """f(..., &sp, ...): f pops what its body says."""
        j = k
        depth = 0
        while j > 0:
            j -= 1
            if w[j] == ')':
                depth += 1
            elif w[j] == '(':
                if depth == 0:
                    break
                depth -= 1
        fn = w[j - 1]
        args = _args(w[j:_close(w, j) + 1])
        names, n = self.ctx.helper(fn)
        if len(args) != len(names):
            raise Refuse('core', '%s called with %d arguments' % (fn, len(args)))
        alias = w[k + 1]
        for s in states:
            m = {}
            for p, a in zip(names, args):
                v = to_lin(a, s.env, self.ctx.consts)
                if v is not None:
                    m[p] = v
            if any(a not in m for a in n.t):
                raise Refuse('oparg', '%s pops %s' % (fn, n.text()))
            pops = n.subst(m)
            iv = Interval(s.alias[alias] - pops, pops)
            if self.cmp(iv.hi, Lin(0)) > 0 or any(
                    self.covers(g, iv) or self.covers(iv, g) for g in s.gone):
                raise Refuse('stack', '%s pops slots that are not inputs' % fn)
            self.record(iv, None, 'args')
            s.gone.append(iv)
            s.released.append(iv)
            s.alias[alias] = iv.lo

    # -- the definition
    def definition(self):
        if not self.ended or not self.final:
            raise Refuse('control flow', 'does not end with DISPATCH')
        # paths that jump may keep more of the stack than those that don't
        # (JUMP_IF_FALSE_OR_POP): those slots are outputs 'x if (jump)'
        fall = [s for s in self.final if not s.jumped]
        jumps = [s for s in self.final if s.jumped]
        ends = fall or jumps
        sp = ends[0].sp
        for group in (ends, jumps if fall else []):
            for s in group[1:]:
                if s.sp != group[0].sp or sorted(x[0].lo.key() for x in s.writes) \
                        != sorted(x[0].lo.key() for x in group[0].writes):
                    raise Refuse('stack', 'paths end with different stacks')
        if fall and jumps and (
                sorted(x[0].lo.key() for x in jumps[0].writes)
                != sorted(x[0].lo.key() for x in fall[0].writes)
                or self.cmp(jumps[0].sp, sp) < 0):
            raise Refuse('stack', 'paths end with different stacks')
        # a slot paths write different variables into is named (res)
        names = {}
        for s in self.final:
            for iv, v in s.writes:
                names.setdefault(iv.lo.key(), set()).add(v)
        taken = {v for j, v, k in self.reads if v} | {v for n in names.values()
                                                      for v in n if v}
        w0 = []
        for iv, v in ends[0].writes:
            if len(names[iv.lo.key()]) > 1:
                if None in names[iv.lo.key()]:
                    raise Refuse('stack', 'paths end with different stacks')
                v = next(n for n in ('res', 'res2', 'res3') if n not in taken)
                taken.add(v)
            w0.append((iv, v))
        ipts = {Lin(0)}
        for j, _, _ in self.reads:
            ipts |= {j.lo, j.hi}
        for s in ends + [x[0] for x in self.snaps]:
            for j in s.gone + s.released:
                ipts |= {j.lo, j.hi}
        ins = [c for c in self.cuts(ipts) if self.cmp(c.hi, Lin(0)) <= 0]
        named = self.name_inputs(ins)
        # outputs: what each slot below the final pointer holds
        opts = {sp}
        for x in w0:
            opts |= {x[0].lo, x[0].hi}
        opts |= {p for p in ipts if self.cmp(p, sp) <= 0 and not any(
            self.cmp(x[0].lo, p) < 0 < self.cmp(x[0].hi, p) for x in w0)}
        outs = []
        for c in self.cuts(opts):
            if self.cmp(c.hi, sp) > 0:
                break
            wr = next((x for x in w0 if x[0].lo == c.lo), None)
            if wr and wr[0].size == c.size:
                outs.append((wr[1] or 'unused', c))
                continue
            same = next((i for i in ins if i.lo == c.lo and i.size == c.size), None)
            if same and not any(self.covers(g, c) for s in ends for g in s.gone):
                outs.append((named[c.lo][0], c))
                continue
            raise Refuse('stack', 'pushes a slot it never set')
        conds = [None] * len(outs)
        if fall and jumps and self.cmp(jumps[0].sp, sp) > 0:
            at = sp
            for c in ins:
                if self.cmp(c.lo, at) != 0 or self.cmp(c.hi, jumps[0].sp) > 0:
                    continue
                if any(self.covers(g, c) for s in jumps for g in s.gone):
                    raise Refuse('stack', 'a jump keeps a slot it popped')
                outs.append((named[c.lo][0], c))
                conds.append('jump')
                at = c.hi
            if self.cmp(at, jumps[0].sp) != 0:
                raise Refuse('stack', 'a jump keeps slots that are not inputs')
        self.check_exits(ins, named)
        in_names = [named[c.lo][0] for c in ins]
        out_names = self.rename(in_names, [n for n, _ in outs])
        lines = []
        for ln in self.lines:
            items = []
            mark = next((t for t in ln if isinstance(t, WriteMark)), None)
            if mark is not None:
                name = next((n for n, c in outs if self.cmp(c.lo, mark.pos) == 0), None)
                if name is not None and name != mark.var:
                    lines.append([t for t in ln if isinstance(t, str)] +
                                 [name, '=', mark.var, ';'])
                continue
            for t in ln:
                if isinstance(t, Ref):
                    name, kind = named.get(t.lo, (None, None))
                    if name is None:
                        raise Refuse('stack', 'reference to a slot that is not an input')
                    if t.index is not None:
                        if kind != 'array':
                            if t.index.const() != 0:
                                raise Refuse('stack', 'element of a slot')
                            items.append(name)
                        else:
                            items += [name, '[', t.index.text(), ']']
                    else:
                        items.append(name)
                else:
                    items.append(t)
            lines.append(items)
        stmts = []
        dsl_vars = set(in_names) | set(out_names)
        for v, typ in self.decl.items():
            if v not in dsl_vars and any(v in ln for ln in lines):
                stmts.append(' '.join(typ + [v]) + ';')
        for w in lines:
            ind = w[0] if w and isinstance(w[0], str) and w[0].strip() == '' else ''
            body = w[1:] if ind else w
            k = 0
            while k < len(body) and (body[k] in DECL_TYPES or body[k] == '*'):
                k += 1
            if k and len(body) > k + 1 and body[k] in dsl_vars and (
                    body[k + 1] == '=' and not ind or body[k + 1] == ';'):
                if body[k + 1] == ';' or body[k + 2:] == ['NULL', ';'] \
                        and body[k] in in_names:
                    continue
                body = body[k:]
            s = ' '.join(body)
            if s.endswith(' ;'):
                s = s[:-2] + ';'
            stmts.append(ind + (s if s.endswith(('{', '}', ';')) else s + ';'))
        return ('inst(%s, (%s -- %s)) {\n%s}\n'
                % (self.name, ', '.join(self.effect(n, c, named) for n, c in
                                         zip(in_names, ins)),
                   ', '.join(self.effect(n, c, named, j) for n, (_, c), j in
                             zip(out_names, outs, conds)),
                   ''.join('    %s\n' % s for s in stmts)))

    def cuts(self, pts):
        """The intervals between the points, in order."""
        pts = sorted(pts, key=functools.cmp_to_key(self.cmp))
        uniq = []
        for p in pts:
            if not uniq or self.cmp(uniq[-1], p) != 0:
                uniq.append(p)
        return [Interval(a, b - a) for a, b in zip(uniq, uniq[1:])]

    def effect(self, name, c, named, cond=None):
        if cond:
            return '%s if (%s)' % (name, cond)
        if c.size.const() == 1:
            return name
        if not c.size.c and len(c.size.t) == 1:
            (a, k), = c.size.t.items()
            if k == 1 and is_bool(a):
                return '%s if %s' % (name, a)
        return '%s[%s]' % (name, c.size.text())

    def name_inputs(self, ins):
        """{lo: (name, kind)} for the input intervals."""
        named, used = {}, {}

        def fresh(base):
            used[base] = used.get(base, 0) + 1
            return base if used[base] == 1 else '%s%d' % (base, used[base])
        for c in ins:
            exact = [(v, k) for j, v, k in self.reads
                     if j.lo == c.lo and self.cmp(j.hi, c.hi) == 0]
            vs = {v for v, k in exact if v}
            if len(vs) > 1:
                raise Refuse('stack', 'input %s has two names' % c.lo.text())
            if vs:
                named[c.lo] = (vs.pop(), 'var')
                continue
            for j, v, k in self.reads:
                if k == 'array' and self.covers(j, c) and not (
                        j.lo == c.lo and self.cmp(j.hi, c.hi) == 0):
                    raise Refuse('stack', 'an array the reader cannot keep whole')
            kinds = {k for _, k in exact}
            if 'array' in kinds:
                named[c.lo] = (fresh('values'), 'array')
            elif 'var' in kinds:
                named[c.lo] = (fresh('in'), 'var')
            elif any(k == 'args' and self.covers(j, c) for j, _, k in self.reads):
                named[c.lo] = (fresh('args'), 'array')
            else:
                named[c.lo] = ('unused', 'unused')
            if c.size.const() == 1 and named[c.lo][1] == 'array':
                named[c.lo] = (named[c.lo][0], 'var')
        return named

    def check_exits(self, ins, named):
        for s, null in self.snaps:
            for c in ins:
                if named[c.lo][1] == 'unused' and not any(
                        self.covers(g, c) for g in s.gone + s.released):
                    continue
                in_slot = not any(self.covers(g, c) for g in s.gone)
                gone = any(self.covers(r, c) for r in s.released)
                if in_slot == gone:
                    raise Refuse('stack', 'error exit with input %s %s'
                                 % (named[c.lo][0], 'released and on the stack'
                                    if gone else 'neither on the stack nor released'))
            for iv, v in s.writes:
                if self.cmp(iv.lo, s.sp) < 0 and v != null and not (
                        v and named.get(iv.lo, (None,))[0] == v):
                    raise Refuse('stack', 'error exit with %s on the stack' % v)

    def rename(self, ins, outs):
        """Second and later outputs of one variable get new names, each
        made by one of its Py_INCREFs."""
        seen, new = {}, []
        for v in outs:
            if v == 'unused':
                new.append(v)
                continue
            seen[v] = seen.get(v, 0) + 1
            new.append(v if seen[v] == 1 else '%s%d' % (v, seen[v]))
        for v, n in seen.items():
            if n == 1:
                continue
            increfs = [k for k, w in enumerate(self.lines)
                       if w == ['Py_INCREF', '(', v, ')', ';']]
            owned = 1 if v in ins else 0
            if len(increfs) + owned != n:
                raise Refuse('stack', '%s pushed %d times with %d references'
                             % (v, n, len(increfs) + owned))
            for k, idx in enumerate(increfs[1 - owned:]):
                self.lines[idx] = ['%s%d' % (v, k + 2), '=', 'Py_NewRef', '(', v, ')', ';']
        return new


class _T:
    """A token text as ctokens.split_top wants it."""
    def __init__(self, text):
        self.text = text


# ---- refusals before the run --------------------------------------------------

def fold_pp(nodes):
    """'#if X' A '#else' B '#endif' where A and B are each one dispatch:
    both arms do the same to the generator (JUMP_ABSOLUTE's FAST_LOOPS
    choice between FAST_DISPATCH and DISPATCH), so it is a dispatch."""
    out, k = [], 0
    while k < len(nodes):
        n = nodes[k]
        if n.kind == 'pp' and n.text.replace(' ', '').startswith('#if'):
            arms, cur, j = [], [], k + 1
            while j < len(nodes):
                m = nodes[j]
                t = m.text.replace(' ', '') if m.kind == 'pp' else ''
                if t.startswith('#if'):
                    break
                if t.startswith(('#else', '#elif', '#endif')):
                    arms.append(cur)
                    cur = []
                    if t.startswith('#endif'):
                        break
                else:
                    cur.append(m)
                j += 1
            if j < len(nodes) and nodes[j].kind == 'pp' and \
                    nodes[j].text.replace(' ', '').startswith('#endif') and \
                    len(arms) >= 2 and all(len(a) == 1 and _is_dispatch(a[0]) for a in arms):
                out.append(arms[0][0])
                k = j + 1
                continue
        out.append(n)
        k += 1
    return out


def precheck(nodes, toks):
    for n in flat(nodes):
        if n.kind == 'pp':
            raise Refuse('preprocessor', n.text)
    for n in flat(nodes):
        if n.kind == 'goto' and n.label != 'error':
            raise Refuse('shared code', 'goto %s' % n.label)
    w = _texts(toks)
    for k, t in enumerate(w):
        if t == 'why' and w[k + 1:k + 2] == ['=']:
            raise Refuse('shared code', 'why = %s' % w[k + 2])
    for k, t in enumerate(w):
        if t == 'f' and k + 1 < len(w) and w[k + 1] == '->':
            raise Refuse('core', 'frame state (f->%s)' % w[k + 2])
        if t in CORE_NAMES and not (t == 'tstate' and _is_argument(w, k)):
            raise Refuse('core', CORE_NAMES[t])


def _is_argument(w, k):
    """w[k] is a whole argument of a call: 3.8 hands the running thread's
    state to the error functions and ceval.c's helpers ('_PyErr_Format(tstate,
    ...)', 'format_exc_check_arg(tstate, ...)'), as 3.7 reads it inside them.
    Its fields (tstate->c_tracefunc) are the interpreter's, so still core."""
    if not (1 < k < len(w) - 1 and w[k + 1] in (',', ')')):
        return False
    if w[k - 1] == '(':      # the first argument: '(' after a function's name
        return re.fullmatch(r'[A-Za-z_]\w*', w[k - 2]) is not None and \
            w[k - 2] not in ('if', 'while', 'for', 'switch', 'return', 'sizeof')
    return w[k - 1] == ','


# ---- driver -------------------------------------------------------------------

def read(src):
    """(definitions, refused): InstDefs parsed from the DSL text the cases
    give, and [(name, kind, reason)] for the cases refused."""
    ctx = Context(src)
    texts, refused = [], []
    for name, toks, problem in cases(src):
        if problem:
            refused.append((name, 'falls through', problem))
            continue
        try:
            nodes = fold_pp(parse(toks))
            precheck(nodes, toks)
            c = Case(name, ctx)
            c.run(c.restructure(nodes), [State()])
            texts.append(c.definition())
        except Refuse as e:
            refused.append((name, e.kind, str(e)))
    return parse_defs(''.join(texts)), refused


def parse_defs(text):
    psr = dsl.Parser(text, filename='ceval.c (read)')
    out = []
    while (thing := psr.definition()):
        out.append(thing)
    if not psr.eof():
        raise psr.make_syntax_error('unparsed definition')
    return out


def effect_size(e, oparg, consts):
    """How many slots a parsed StackEffect takes at this oparg. 'jump' is
    whether the instruction jumped: dis.stack_effect gives the larger of
    the two effects, which is the jump's for the slots a jump keeps."""
    if e.cond and e.cond.strip() == 'jump':
        return 1
    if e.cond:
        return 1 if cvalue(e.cond, oparg, consts) else 0
    if e.size:
        return cvalue(e.size, oparg, consts)
    return 1


def dsl_text(src):
    """The definitions as bytecodes.c text, for reading."""
    return '\n'.join(d.text for d in read(src)[0])


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: ceval_reader.py CPYTHON_SRC  (prints what it reads)')
    src = sys.argv[1]
    defs, refused = read(src)
    for d in defs:
        print(d.text, '\n')
    for r in refused:
        print('refused %-28s %-14s %s' % r)
    print('%d read, %d refused' % (len(defs), len(refused)))
