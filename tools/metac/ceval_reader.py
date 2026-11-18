"""Read Python/ceval.c's TARGET cases into instruction definitions in the
form CPython's own parser gives for Python/bytecodes.c (DECISIONS D67 (5),
doc/METACOMPILER.md "ceval.c reader"), so that everything after the front
end of gen_ceval.py is shared by every version.

A case is run symbolically over C tokens (ctokens.py). The stack starts
with an unknown number of inputs below the stack pointer; POP, TOP,
SECOND, THIRD, FOURTH and PEEK(n) read them, PUSH, SET_TOP..SET_FOURTH and
SET_VALUE(n, x) write slots, STACKADJ, STACK_GROW and STACK_SHRINK move
the pointer. The deepest slot touched is the base: the inputs are the
slots from the base to the old stack pointer, the outputs the slots from
the base to the final one, each named by the C variable it holds.
Everything else stays in the body, written out in bytecodes.c syntax:

    inst(NAME, (in1, in2 -- out1)) { statements }

and parsed with CPython's parser, so the result is the same InstDef a
bytecodes.c file gives.

Rewrites (each general, none per opcode):
  - 'if (C) goto error;', or a block whose statements are releases of
    inputs and at most one known error-raising statement (ERROR_FORMS)
    before 'goto error', becomes ERROR_IF(C, label). It is accepted only
    if, at that exit, every input read so far is either still in its slot
    or has been released or stored, and every other slot written holds the
    value the condition says is NULL. That is what ERROR_IF means to the
    generator: the inputs are dead after it.
  - 'if (T) A else B', where T only tests types (Py*_Check, Py*_CheckExact,
    !, &&, ||), keeps B: the generic path. A is CPython's fast path for
    exact built-in types; Psyco's counterpart of B dispatches on the type
    itself. This is judgement, stated once here (METACOMPILER).
  - an output that is the same variable as another output gets a new name
    (x2), and the Py_INCREF that made the extra reference becomes
    'x2 = Py_NewRef(x)', which is how bytecodes.c writes DUP_TOP's COPY.
  - 'T x;' then 'x = E;' becomes 'T x = E;'; an output loses its type
    (the DSL declares outputs). DISPATCH, FAST_DISPATCH and PREDICT are
    dropped; the main path must end with one of the first two.

Anything else is refused with a reason and a kind (KINDS), never guessed:
a case that falls into another, jumps to shared code, touches frame or
thread state, has a stack effect that depends on oparg, or has control
flow the rules above don't cover.
"""
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
    'oparg': 'stack effect depends on oparg',
    'preprocessor': 'preprocessor conditional in the case',
    'control flow': 'control flow the reader has no rule for',
    'stack': 'stack use the reader has no rule for',
}

READS = {'POP': None, 'TOP': 1, 'SECOND': 2, 'THIRD': 3, 'FOURTH': 4}
WRITES = {'SET_TOP': 1, 'SET_SECOND': 2, 'SET_THIRD': 3, 'SET_FOURTH': 4}
MOVES = {'STACKADJ': 1, 'STACK_GROW': 1, 'STACK_SHRINK': -1}
STACK_MACROS = set(READS) | set(WRITES) | set(MOVES) | {
    'PEEK', 'PUSH', 'SET_VALUE', 'EXT_POP', 'BASIC_PUSH', 'BASIC_POP',
    'BASIC_STACKADJ'}
DISPATCHES = {'DISPATCH', 'FAST_DISPATCH'}
DROPPED = {'PREDICT'}
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


class Refuse(Exception):
    def __init__(self, kind, why):
        super().__init__(why)
        self.kind = kind


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
        name, nxt = toks[i + 2].text, toks[i + 4]
        if nxt.text == '{':
            out.append((name, toks[i + 5:ctokens.match_close(toks, i + 4)], None))
        elif nxt.text == 'TARGET':
            out.append((name, None, 'falls into TARGET(%s)' % toks[i + 6].text))
        else:   # TARGET(NOP) FAST_DISPATCH();
            end = next(j for j in range(i + 4, len(toks)) if toks[j].text == ';')
            out.append((name, toks[i + 4:end + 1], None))
    return out


# ---- statements -------------------------------------------------------------

def parse(toks):
    """Statement tree: ('stmt', toks) ('block', [..]) ('if', cond, then,
    else) ('goto', label) ('pp', text) ('loop', keyword)."""
    out, i = [], 0
    while i < len(toks):
        node, i = _statement(toks, i)
        out.append(node)
    return out


def _statement(toks, i):
    t = toks[i]
    if t.kind == 'pp':
        return ('pp', t.text), i + 1
    if t.text == '{':
        j = ctokens.match_close(toks, i)
        return ('block', parse(toks[i + 1:j])), j + 1
    if t.text == 'if':
        j = ctokens.match_close(toks, i + 1)
        then, k = _statement(toks, j + 1)
        other = None
        if k < len(toks) and toks[k].text == 'else':
            other, k = _statement(toks, k + 1)
        return ('if', toks[i + 2:j], then, other), k
    if t.text in ('while', 'for', 'switch'):
        j = ctokens.match_close(toks, i + 1)
        _, k = _statement(toks, j + 1)
        return ('loop', t.text), k
    if t.text == 'do':
        _, k = _statement(toks, i + 1)
        k = ctokens.match_close(toks, k + 1)    # while ( ... )
        return ('loop', 'do'), k + 2
    if t.text == 'goto':
        return ('goto', toks[i + 1].text), i + 3
    if t.kind == 'id' and i + 1 < len(toks) and toks[i + 1].text == ':':
        return ('label', t.text), i + 2
    depth, j = 0, i
    while True:
        if toks[j].text in '([{':
            depth += 1
        elif toks[j].text in ')]}':
            depth -= 1
        elif toks[j].text == ';' and depth == 0:
            return ('stmt', toks[i:j]), j + 1
        j += 1


def flat(nodes):
    """Every node, nested ones included."""
    for n in nodes:
        yield n
        if n[0] == 'block':
            yield from flat(n[1])
        elif n[0] == 'if':
            yield from flat([n[2]] + ([n[3]] if n[3] else []))


def _texts(toks):
    return [t.text for t in toks]


def _block(node):
    return node[1] if node[0] == 'block' else [node]


# ---- the symbolic run -------------------------------------------------------

class Case:
    def __init__(self, name):
        self.name = name
        self.sp = 0
        self.low = 0
        self.slots = {}         # position -> variable (absent: unread input)
        self.inputs = {}        # position -> variable read from it
        self.stmts = []         # [tokens texts]
        self.decl = {}          # variable -> type of a bare declaration
        self.released = set()   # variables Py_DECREF'd on the main path
        self.stored = set()     # variables SETLOCAL'd
        self.ended = False

    # stack
    def _pos(self, depth):
        p = self.sp - depth
        self.low = min(self.low, p)
        return p

    def read(self, depth, var):
        p = self._pos(depth)
        if p in self.slots:
            raise Refuse('stack', 'reads back a slot it wrote')
        if p >= 0:
            raise Refuse('stack', 'reads above the stack')
        if p in self.inputs:
            raise Refuse('stack', 'reads input %d twice' % -p)
        self.inputs[p] = var
        self.slots[p] = var
        return p

    def pop(self, var):
        p = self.read(1, var)
        del self.slots[p]
        self.sp -= 1

    def write(self, depth, var):
        p = self._pos(depth)
        if p < 0 and p not in self.inputs:
            raise Refuse('stack', 'overwrites input %d it did not read' % -p)
        self.slots[p] = var

    def move(self, n):
        if n < 0:
            for p in range(self.sp + n, self.sp):
                if p not in self.inputs and p < 0:
                    raise Refuse('stack', 'drops input %d it did not read' % -p)
                v = self.slots.pop(p, None)
                if v is not None and v != self.inputs.get(p):
                    raise Refuse('stack', 'drops %s from the stack' % v)
        self.sp += n
        self.low = min(self.low, self.sp)

    # statements
    def run(self, nodes, top=True):
        for k, node in enumerate(nodes):
            if self.ended:
                raise Refuse('control flow', 'code after DISPATCH')
            kind = node[0]
            if kind == 'block':
                self.run(node[1], top=False)
            elif kind == 'stmt':
                self.statement(node[1], last=top and k == len(nodes) - 1)
            elif kind == 'if':
                self.branch(node)
            elif kind == 'goto':
                raise Refuse('control flow', 'goto %s outside an error test' % node[1])
            else:
                raise Refuse('control flow', '%s' % node[1])

    def branch(self, node):
        _, cond, then, other = node
        body = _block(then)
        if body and body[-1] == ('goto', 'error') and other is None:
            return self.error_exit(cond, body[:-1])
        if other is not None and _is_type_test(cond):
            # the generic path; the exact-type fast path is CPython's
            return self.run(_block(other), top=False)
        raise Refuse('control flow', 'if (%s)' % ' '.join(_texts(cond)))

    def error_exit(self, cond, body):
        label, dead = 'error', set()
        for node in body:
            w = _texts(node[1]) if node[0] == 'stmt' else None
            if w and len(w) == 4 and w[0] == 'Py_DECREF' and w[2] in self.inputs.values():
                dead.add(w[2])
                continue
            form = next((lab for f, lab in ERROR_FORMS if w and w[:len(f)] == f), None)
            if form and label == 'error':
                label = form
                continue
            raise Refuse('control flow', 'error exit does %s'
                         % (' '.join(w) if w else node[0]))
        c = ' '.join(_texts(cond))
        m = re.fullmatch(r'(\w+) == NULL', c)
        null = m.group(1) if m else None
        for p, v in self.inputs.items():
            in_slot = self.slots.get(p) == v and p < self.sp
            gone = v in self.released | self.stored | dead
            if in_slot == gone:
                raise Refuse('stack', 'error exit with input %s %s'
                             % (v, 'released and on the stack' if gone
                                else 'neither on the stack nor released'))
        for p, v in self.slots.items():
            if p < self.sp and self.inputs.get(p) != v and v != null:
                raise Refuse('stack', 'error exit with %s on the stack' % v)
        self.stmts.append(['ERROR_IF', '(', c, ',', label, ')'])

    def statement(self, toks, last):
        w = _texts(toks)
        if not w:
            return
        head = w[0]
        if head in DISPATCHES and w[1:] == ['(', ')']:
            if not last:
                raise Refuse('control flow', '%s before the end' % head)
            self.ended = True
            return
        if head in DROPPED:
            return
        # PyObject *x = STACKREAD
        decl = w[:2] == ['PyObject', '*']
        lhs = w[2:] if decl else w
        if len(lhs) >= 4 and lhs[1] == '=' and lhs[2] in STACK_MACROS:
            x, op, args = lhs[0], lhs[2], _args(lhs[3:])
            if op == 'POP' and args == []:
                return self.pop(x)
            if op in READS and args == []:
                return self.read(READS[op], x)
            if op == 'PEEK' and len(args) == 1:
                return self.read(_const(args[0]), x)
            raise Refuse('stack', ' '.join(w))
        if head in STACK_MACROS and len(w) > 1 and w[1] == '(':
            args = _args(w[1:])
            if head == 'PUSH' and len(args) == 1:
                self.move(1)
                return self.write(1, _var(args[0]))
            if head in WRITES and len(args) == 1:
                return self.write(WRITES[head], _var(args[0]))
            if head == 'SET_VALUE' and len(args) == 2:
                return self.write(_const(args[0]), _var(args[1]))
            if head in MOVES and len(args) == 1:
                return self.move(MOVES[head] * _const(args[0]))
            raise Refuse('stack', ' '.join(w))
        if STACK_MACROS & set(w):
            raise Refuse('stack', 'stack access inside %s' % ' '.join(w))
        # T x;
        if len(w) in (2, 3) and w[-1].isidentifier() and w[0] in ('PyObject', 'int') \
                and (len(w) == 2 or w[1] == '*'):
            self.decl[w[-1]] = w[:-1]
            return
        if len(w) > 2 and w[1] == '=' and w[0] in self.decl:
            w = self.decl.pop(w[0]) + w
        if len(w) == 4 and w[0] == 'Py_DECREF':
            self.released.add(w[2])
        if len(w) == 6 and w[:2] == ['SETLOCAL', '(']:
            self.stored.add(w[4])
        self.stmts.append(w)

    # the definition
    def definition(self):
        if not self.ended:
            raise Refuse('control flow', 'does not end with DISPATCH')
        if any(p not in self.inputs for p in range(self.low, 0)):
            raise Refuse('stack', 'an input slot it does not read')
        ins = [self.inputs[p] for p in range(self.low, 0)]
        outs = []
        for p in range(self.low, self.sp):
            if p not in self.slots:
                raise Refuse('stack', 'pushes a slot it never set')
            outs.append(self.slots[p])
        outs = self.rename(ins, outs)
        stmts = []
        for w in self.stmts:
            if w[:2] == ['PyObject', '*'] and len(w) > 3 and w[3] == '=' and w[2] in outs:
                w = w[2:]
            stmts.append(' '.join(w) + ';')
        return ('inst(%s, (%s -- %s)) {\n%s}\n'
                % (self.name, ', '.join(ins), ', '.join(outs),
                   ''.join('    %s\n' % s for s in stmts)))

    def rename(self, ins, outs):
        """Second and later outputs of one variable get new names, each
        made by one of its Py_INCREFs."""
        seen, new = {}, []
        for v in outs:
            seen[v] = seen.get(v, 0) + 1
            new.append(v if seen[v] == 1 else '%s%d' % (v, seen[v]))
        for v, n in seen.items():
            if n == 1:
                continue
            increfs = [k for k, w in enumerate(self.stmts)
                       if w == ['Py_INCREF', '(', v, ')']]
            owned = 1 if v in ins else 0
            if len(increfs) + owned != n:
                raise Refuse('stack', '%s pushed %d times with %d references'
                             % (v, n, len(increfs) + owned))
            for k, idx in enumerate(increfs[1 - owned:]):
                self.stmts[idx] = ['%s%d' % (v, k + 2), '=', 'Py_NewRef', '(', v, ')']
        return new


def _args(w):
    """['(', a, ',', b, ')'] -> ['a', 'b'] (joined texts)."""
    if not w or w[0] != '(':
        raise Refuse('stack', 'macro without arguments')
    depth, cur, out = 0, [], []
    for k, t in enumerate(w):
        if t in '([':
            depth += 1
            if depth == 1:
                continue
        elif t in ')]':
            depth -= 1
            if depth == 0:
                if cur:
                    out.append(' '.join(cur))
                if k != len(w) - 1:
                    raise Refuse('stack', 'stack access inside an expression')
                return out
        if t == ',' and depth == 1:
            out.append(' '.join(cur))
            cur = []
        else:
            cur.append(t)
    raise Refuse('stack', 'unbalanced')


def _const(text):
    if re.fullmatch(r'-? ?\d+', text):
        return int(text.replace(' ', ''))
    raise Refuse('oparg', 'stack position or adjustment %s' % text)


def _var(text):
    if not text.isidentifier() or text == 'NULL':
        raise Refuse('stack', 'pushes %s, not a variable' % text)
    return text


def _is_type_test(cond):
    for t in cond:
        if not (t.text in ('(', ')', '!', '&&', '||') or t.kind == 'id'):
            return False
    calls = [t.text for k, t in enumerate(cond[:-1])
             if t.kind == 'id' and cond[k + 1].text == '(']
    return bool(calls) and all(TYPE_TEST.fullmatch(c) for c in calls)


# ---- refusals before the run --------------------------------------------------

def precheck(nodes, toks):
    for n in flat(nodes):
        if n[0] == 'pp':
            raise Refuse('preprocessor', n[1])
    for n in flat(nodes):
        if n[0] == 'goto' and n[1] != 'error':
            raise Refuse('shared code', 'goto %s' % n[1])
    w = _texts(toks)
    for k, t in enumerate(w):
        if t == 'why' and w[k + 1:k + 2] == ['=']:
            raise Refuse('shared code', 'why = %s' % w[k + 2])
    for k, t in enumerate(w):
        if t == 'f' and k + 1 < len(w) and w[k + 1] == '->':
            raise Refuse('core', 'frame state (f->%s)' % w[k + 2])
        if t in CORE_NAMES:
            raise Refuse('core', CORE_NAMES[t])
        if t == 'stack_pointer' and (w[k - 1] in ('&', '=') or w[k + 1] == '='):
            # handed to a helper that moves it: the calls
            raise Refuse('core', 'the stack pointer handed to a helper')
    if 'stack_pointer' in w:
        raise Refuse('oparg', 'stack slots addressed from stack_pointer')


# ---- driver -------------------------------------------------------------------

def read(src):
    """(definitions, refused): InstDefs parsed from the DSL text the cases
    give, and [(name, kind, reason)] for the cases refused."""
    texts, refused = [], []
    for name, toks, problem in cases(src):
        if problem:
            refused.append((name, 'falls through', problem))
            continue
        try:
            nodes = parse(toks)
            precheck(nodes, toks)
            c = Case(name)
            c.run(nodes)
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
