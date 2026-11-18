#!/usr/bin/env python3
"""Generate bytecode-compiler cases from instruction definitions in the
form of CPython's Python/bytecodes.c (doc/DECISIONS.md D26 as revised by
D67, doc/METACOMPILER.md "Bytecode stage" and "ceval.c reader").

Input: the CPython source being built for. If it has Python/bytecodes.c,
that file through CPython's own parser (tools/metac/cases_generator,
vendored); otherwise its Python/ceval.c TARGET cases through
ceval_reader.py, which gives the same definitions. Output:
pycompiler_gen.h, cases included in pycompiler.c's opcode switch.

Each instruction becomes:

    case NAME: {
      vinfo_t *in1 = NTOP(n); ...       inputs, left on the stack
      <body>                            C-API calls become Psyco metas
      POP(...); vinfo_decref(...);      only after the last failure point
      PUSH(out); ...
      goto fine;
    }

An error exit is 'break', which leaves the inputs on the stack for the
unwinding code, so DECREF_INPUTS() is done at the end. The DSL's own rule
(no INCREF before the last ERROR_IF) is checked and extended to Psyco's
restart rule: nothing irreversible (incref, SETLOCAL) before the last
statement that can fail or promote.

The vocabulary is small: the statement forms in translate(). A C-API call
F(args) is translated only if PSYCO_HEADERS declare Psyco's counterpart
(PyNumber_Add -> PsycoNumber_Add) with one more parameter (po) and a
matching return kind: PyObject * -> vinfo_t * (NULL: error), int -> bool
(false: error). Anything else is skipped with the reason.

Control flow (D79): 'if (x OP N)' on a run-time int (a counterpart's int
result, NULL for C's negative error values) is a condition code by step
4's rule (gen_abstract.int_condition); arms that only choose Py_True or
Py_False are the object built from the condition (SELECTS, rule
bool_select); a jump on one arm, as the case's last statement, is
pycompiler.c's JUMP_IF_CC, with the policy's ('forget', 'before_branch'),
and an unconditional jump ends with JUMPED(). A C-API function without a
counterpart is a residual call with step 4's flags (Translator.flags).

Every stack effect read is checked against dis.stack_effect() run in the
interpreter being built for, for several opargs: it must be the same. A
mismatch stops generation, and so the build.

Run by generate.py.  gen_ceval.py --try BYTECODES_C CPYTHON_SRC  reports
only, e.g. on 3.12's file.
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'cases_generator'))
import ceval_reader  # noqa: E402
import cparse  # noqa: E402
import ctokens  # noqa: E402
import cslots  # noqa: E402
import parser as dsl  # noqa: E402  (CPython's Tools/cases_generator/parser.py)
import gen_abstract  # noqa: E402  (step 4's rules: conditions, residual flags)
import gen_virtual  # noqa: E402  (the virtual constructors, D80)
import policy_table  # noqa: E402

ROOT = cslots.ROOT
# Psyco's counterparts: the compile-time protocol and the object families
PSYCO_DIR = os.path.join(ROOT, 'ni', 'Objects')
PSYCO_HEADERS = sorted(os.path.join(PSYCO_DIR, h) for h in os.listdir(PSYCO_DIR)
                       if h.startswith('p') and h.endswith('.h'))
BEGIN, END = '// BEGIN BYTECODES //', '// END BYTECODES //'

# ERROR_IF labels: what a failed instruction does in Ni. 'error' is a
# Python exception already set (by the meta), so just stop the opcode.
LABELS = {
    'error': 'break;',
    'unbound_local_error': '{ unbound_local_error(po, co, oparg); break; }',
}
# statements with no effect at compile time
DROPPED = {'assert', 'STAT_INC'}
# C constants that have a compile-time vinfo: (make, release)
CONSTANTS = {'Py_None': 'psyco_vi_None()', 'NULL': 'psyco_vi_Zero()'}
# the objects a run-time branch may choose between, and Psyco's
# constructor of the one the condition picks (PATTERNS P23, rule
# bool_select): {(when true, when false): constructor}
SELECTS = {('Py_True', 'Py_False'): 'PsycoBool_FromCondition'}
# how a jump is written in the cases (pycompiler.c's macros): the jump
# statements, the conditional jump, and the end of a case that jumped
JUMPS = {'JUMPTO', 'JUMPBY'}
# GETITEM(tuple, oparg) of the code object's constants or names: Ni's macro
# for the borrowed object (ceval.c names the tuples; bytecodes.c reaches them
# through the frame)
CODE_TUPLES = {'consts': 'GETCONST(oparg)', 'names': 'GETNAMEV(oparg)',
               'frame -> f_code -> co_consts': 'GETCONST(oparg)',
               'frame -> f_code -> co_names': 'GETNAMEV(oparg)'}


class Skip(Exception):
    def __init__(self, why, kind='vocabulary'):
        super().__init__(why)
        self.kind = kind


# ---- reading definitions -------------------------------------------------

def read_defs(path):
    with open(path) as f:
        src = f.read()
    psr = dsl.Parser(src, filename=os.path.relpath(path, ROOT))
    while (tkn := psr.next(raw=True)) and tkn.text != BEGIN:
        pass
    start = psr.getpos()
    while (tkn := psr.next(raw=True)) and tkn.text != END:
        pass
    del psr.tokens[psr.getpos() - 1:]
    psr.setpos(start)
    out = []
    while (thing := psr.definition()):
        out.append(thing)
    if not psr.eof():
        raise psr.make_syntax_error('unparsed definition')
    return out


def body_tokens(inst):
    """The block's tokens without braces and comments, with the
    #if ENABLE_SPECIALIZATION part removed (its #else part kept: that is
    the interpreter built without specialisation)."""
    toks = [t for t in inst.block.tokens if t.kind != 'COMMENT'][1:-1]
    out, i = [], 0
    while i < len(toks):
        t = toks[i]
        if t.kind == 'MACRO':
            rest = [u.text for u in toks[i + 1:i + 2]]
            if t.text.replace(' ', '') == '#if' and rest == ['ENABLE_SPECIALIZATION']:
                depth, j = 0, i
                while True:
                    m = toks[j].text.replace(' ', '') if toks[j].kind == 'MACRO' else ''
                    if m == '#if':
                        depth += 1
                    elif m == '#endif':
                        depth -= 1
                        if depth == 0:
                            break
                    elif m == '#else' and depth == 1:
                        k = j + 1
                        while toks[k].text.replace(' ', '') != '#endif':
                            k += 1
                        out.extend(toks[j + 1:k])
                        j = k
                        break
                    j += 1
                i = j + 1
                # the token after #endif on its line is the comment, dropped
                continue
            raise Skip('preprocessor line %s' % t.text)
        out.append(t)
        i += 1
    return out


def split_statements(toks):
    """Statements; a 'for (...) {...}' is one, its tokens kept whole (only
    the PATTERNS E fill loop is translated, by translate())."""
    out, cur, depth = [], [], 0
    k = 0
    while k < len(toks):
        t = toks[k]
        if t.text == 'for' and depth == 0 and not cur:
            j = k + 1
            level = 0
            while True:
                level += toks[j].text in ('(', '{')
                level -= toks[j].text in (')', '}')
                if level == 0 and toks[j].text == '}':
                    break
                j += 1
            out.append(toks[k:j + 1])
            k = j + 1
            continue
        if t.text == 'if' and depth == 0 and not cur:
            j = if_end(toks, k)
            out.append(toks[k:j])
            k = j
            continue
        k += 1
        if t.text in ('{', 'if', 'else', 'for', 'while', 'switch') and depth == 0:
            raise Skip('control flow (%s)' % t.text)
        if t.text in '([':
            depth += 1
        elif t.text in ')]':
            depth -= 1
        if t.text == ';' and depth == 0:
            out.append(cur)
            cur = []
        else:
            cur.append(t)
    if cur:
        raise Skip('statement without ;')
    return out


def arm_end(toks, j):
    """toks[j] starts an if's arm: the index after it (a block in braces,
    another if, or one statement up to its ';', as the reader passes
    through a branch that touches nothing it tracks)."""
    if toks[j].text == 'if':
        return if_end(toks, j)
    level = 0
    while True:
        if j >= len(toks):
            raise Skip('an unended statement in an if')
        level += toks[j].text in ('{', '(', '[')
        level -= toks[j].text in ('}', ')', ']')
        if level == 0 and (toks[j].text == ';' or (
                toks[j].text == '}' and toks[j - 1].text != '=')):
            return j + 1
        j += 1


def if_end(toks, k):
    """toks[k] is 'if': the index after its statement."""
    _, j = call_args(toks, k + 1)
    j = arm_end(toks, j)
    if j < len(toks) and toks[j].text == 'else':
        j = arm_end(toks, j + 1)
    return j


def if_parts(stmt):
    """(condition text, then statements, else statements) of an if."""
    args, j = call_args(stmt, 1)
    if len(args) != 1:
        raise Skip('if condition')
    e = if_end(stmt, 0)
    k = arm_end(stmt, j)

    def arm(toks):
        if toks[0].text == 'if':
            return [toks]
        if toks[0].text == '{':
            return split_statements(toks[1:-1])
        return split_statements(toks)
    then = arm(stmt[j:k])
    other = []
    if k < e:
        other = arm(stmt[k + 1:e])
    return args[0], then, other


def words(stmt):
    return ' '.join(t.text for t in stmt)


def call_args(stmt, i):
    """stmt[i] is '(': the argument texts and the index after ')'."""
    depth, args, cur = 0, [], []
    for j in range(i, len(stmt)):
        t = stmt[j].text
        if t in '([':
            depth += 1
            if depth == 1:
                continue
        elif t in ')]':
            depth -= 1
            if depth == 0:
                if cur:
                    args.append(' '.join(cur))
                return args, j + 1
        if t == ',' and depth == 1:
            args.append(' '.join(cur))
            cur = []
        else:
            cur.append(t)
    raise Skip('unbalanced call')


# ---- signatures ------------------------------------------------------------

def c_signatures(src):
    """{name: (return kind, arity)} for PyAPI_FUNC declarations in the source's
    Include/**/*.h (from 3.8 many are in Include/cpython/), Include/*.h
    first."""
    sigs = {}
    inc = os.path.join(src, 'Include')
    paths = [h for h in sorted(os.listdir(inc)) if h.endswith('.h')]
    for d in sorted(os.listdir(inc)):
        if os.path.isdir(os.path.join(inc, d)):
            paths += [os.path.join(d, h) for h in sorted(os.listdir(os.path.join(inc, d)))
                      if h.endswith('.h')]
    for h in paths:
        with open(os.path.join(inc, h), errors='replace') as f:
            toks = ctokens.tokenize(f.read(), h)[0]
        for i, t in enumerate(toks):
            if t.text != 'PyAPI_FUNC' or toks[i + 1].text != '(':
                continue
            close = ctokens.match_close(toks, i + 1)
            ret = ' '.join(u.text for u in toks[i + 2:close])
            name = toks[close + 1].text
            if toks[close + 2].text != '(':
                continue
            pclose = ctokens.match_close(toks, close + 2)
            params = ctokens.split_top(toks[close + 3:pclose], ',')
            arity = 0 if [u.text for u in toks[close + 3:pclose]] == ['void'] else len(params)
            sig = ({'PyObject *': 'object', 'int': 'int'}.get(ret, ret), arity)
            if os.sep in h:
                sigs.setdefault(name, sig)
            else:
                sigs[name] = sig
    return sigs


def psyco_signatures():
    """{name: (return kind, arity without po, parameter types)} for Psyco*
    functions. Kind 'vinfo' (NULL: error) and 'bool' (false: error) take po
    first; 'virtual' doesn't, so it can't raise: it builds a virtual object
    (PATTERNS E)."""
    sigs = {}
    for h in PSYCO_HEADERS:
        with open(h) as f:
            toks = ctokens.tokenize(f.read(), h)[0]
        for i, t in enumerate(toks):
            if (t.kind != 'id' or not t.text.lstrip('_').startswith('Psyco')
                    or i + 1 >= len(toks) or toks[i + 1].text != '('):
                continue
            ret = [u.text for u in toks[max(0, i - 2):i]]
            kind = ('vinfo' if ret[-2:] == ['vinfo_t', '*']
                    else 'bool' if ret[-1:] == ['bool'] else None)
            if kind is None:
                continue
            close = ctokens.match_close(toks, i + 1)
            params = [' '.join(u.text for u in p[:-1]) if len(p) > 1 else p[0].text
                      for p in ctokens.split_top(toks[i + 2:close], ',')]
            if not params:
                continue
            if params[0].split(' ')[:1] == ['PsycoObject']:
                sigs[t.text] = (kind, len(params) - 1, params[1:])
            elif kind == 'vinfo' and toks[close + 1].text == ';':
                sigs[t.text] = ('virtual', len(params), params)
    return sigs


class NoCounterpart(Skip):
    """No Psyco function does what this C-API function does, with its
    arguments: the call is residual."""


def counterpart(fn, csigs, psigs):
    if fn not in csigs:
        raise Skip('%s: no declaration in Include/' % fn)
    pname = gen_abstract.psyco_name(fn)
    if pname not in psigs:
        raise NoCounterpart('%s: no Psyco counterpart %s' % (fn, pname))
    cret, carity = csigs[fn]
    pret, parity, _ = psigs[pname]
    if carity != parity:
        # Ni's function of that name takes other arguments (PsycoList_New
        # takes the items): not a counterpart of this call
        raise NoCounterpart('%s: %d arguments, %s takes %d' % (fn, carity, pname, parity))
    if (cret, pret) not in (('object', 'vinfo'), ('int', 'bool'), ('object', 'virtual'),
                            ('int', 'vinfo')):
        raise Skip('%s returns %s, %s returns %s' % (fn, cret, pname, pret))
    if (cret, pret) == ('int', 'vinfo'):
        return pname, 'intval'      # a run-time int; NULL is C's error value
    if pret == 'virtual':
        # a virtual object only for a constructor policy_table.TYPES names,
        # whose 'new' is this counterpart (D80)
        if gen_virtual.constructors().get(fn) != pname:
            raise Skip('%s: not a virtual constructor in policy_table.TYPES' % fn)
        return pname, 'virtual'
    return pname, cret


def residual_convention(x, stmts):
    """How the C tests x, a call's result, for an error: from the next
    ERROR_IF that reads it (the same conventions as gen_abstract's)."""
    for st in stmts:
        w = [t.text for t in st]
        cond = None
        if w[:2] == ['ERROR_IF', '('] and x in w:
            cond = call_args(st, 1)[0][0]
        elif w[:1] == ['if'] and x in w:
            # 'if (C) { ...; ERROR_IF(true, label); }': the exit on C
            c, then, other = if_parts(st)
            if then and not other and x in c.split(' ') and \
                    words(then[-1]).replace(' ', '').startswith('ERROR_IF(true,'):
                cond = c
        if cond is not None:
            if cond in (x, '%s != 0' % x):
                return 'zero_ok'
            if cond == '%s < 0' % x:
                return 'neg'
            if cond == '%s == NULL' % x:
                return 'null'
            raise Skip('cannot tell how the C checks %s for an error (%s)' % (x, cond))
    raise Skip('the result in %s is never checked' % x)


def constructor(fn, psigs):
    """Psyco's counterpart of PyT_New(n) filled from an array: the 'new' of
    the policy_table.TYPES entry whose virtual constructor is fn (D80),
    taking (int, vinfo_t **) after po if it has one. The items are new
    references to it (the fact about Ni's constructors this relies on), so
    the array C stole is released after."""
    pname = gen_virtual.constructors().get(fn)
    if pname is None:
        raise Skip('%s: not a virtual constructor in policy_table.TYPES' % fn)
    if pname not in psigs:
        raise Skip('%s: no Psyco counterpart %s' % (fn, pname))
    kind, _, params = psigs[pname]
    if [p.replace(' ', '') for p in params] != ['int', 'vinfo_t**']:
        raise Skip('%s: %s does not take (count, items)' % (fn, pname))
    return pname, kind


# ---- translation -----------------------------------------------------------

# C integer types of a case's locals: each is a C int of the compiler
# (compile-time), as oparg is
INT_DECLS = ('int', 'Py_ssize_t')
# opargs at which a loop over an input array is run by the generator, to
# check that it releases every item once
SIM_OPARGS = (0, 1, 2, 3, 4, 7)
# names of types in a case's text (casts and declarations, for cparse)
TYPE_NAMES = {'Py_ssize_t', 'PyObject', 'PyListObject', 'PyTupleObject',
              'PyDictObject', 'PySetObject', 'PyTypeObject', 'size_t'}


def kinds_in(e, out=None):
    """The kinds of the nodes of a cparse expression."""
    if out is None:
        out = set()
    if isinstance(e, tuple) and e and isinstance(e[0], str):
        out.add(e[0])
        for x in e[1:]:
            kinds_in(x, out)
    elif isinstance(e, list):
        for x in e:
            kinds_in(x, out)
    return out


class Lines(list):
    """The case's C lines; append() indents by the blocks open."""
    indent = 0

    def append(self, line):
        super().append('  ' * self.indent + line)


def tidy(text):
    """Token texts joined by spaces, as C is written."""
    s = re.sub(r'([(\[]) ', r'\1', text)
    s = re.sub(r' ([)\],;])', r'\1', s)
    s = re.sub(r'(\+\+|--) (?=\w)', r'\1', s)
    return re.sub(r'(?<=\w) (\+\+|--)', r'\1', s)


class _Unknown(Exception):
    pass


def ct_eval(e, env):
    """The value of a cparse expression of compile-time ints in env (a
    dict, updated by ++, -- and assignments); _Unknown if a name isn't."""
    k = e[0]
    if k == 'num':
        return int(e[1].rstrip('uUlL'), 0)
    if k == 'id':
        if e[1] not in env:
            raise _Unknown(e[1])
        return env[e[1]]
    if k == 'cast':
        return ct_eval(e[2], env)
    if k == 'un':
        v = ct_eval(e[2], env)
        return {'-': -v, '+': v, '!': int(not v), '~': ~v}[e[1]]
    if k == 'bin':
        op = e[1]
        a = ct_eval(e[2], env)
        if op == '&&':
            return int(bool(a) and bool(ct_eval(e[3], env)))
        if op == '||':
            return int(bool(a) or bool(ct_eval(e[3], env)))
        b = ct_eval(e[3], env)
        if op == '/' or op == '%':
            if b == 0:
                raise _Unknown('division by 0')
            q = abs(a) // abs(b) * (1 if (a < 0) == (b < 0) else -1)
            return q if op == '/' else a - q * b
        return {'+': a + b, '-': a - b, '*': a * b, '<<': a << b, '>>': a >> b,
                '&': a & b, '|': a | b, '^': a ^ b, '<': int(a < b), '>': int(a > b),
                '<=': int(a <= b), '>=': int(a >= b), '==': int(a == b),
                '!=': int(a != b)}[op]
    if k == 'cond':
        return ct_eval(e[2] if ct_eval(e[1], env) else e[3], env)
    if k in ('pre', 'post') and e[2][0] == 'id':
        old = ct_eval(e[2], env)
        env[e[2][1]] = old + (1 if e[1] == '++' else -1)
        return env[e[2][1]] if k == 'pre' else old
    if k == 'assign' and e[2][0] == 'id':
        v = ct_eval(e[3], env)
        if e[1] != '=':
            v = ct_eval(('bin', e[1][:-1], e[2], ('num', str(v))), env)
        env[e[2][1]] = v
        return v
    if k == 'comma':
        v = None
        for x in e[1]:
            v = ct_eval(x, env)
        return v
    raise _Unknown(k)


def loop_indices(loops, idx, env):
    """The values idx takes in the innermost of the cparse loops (outermost
    first), the loops run by ct_eval from env."""
    out = []

    def run(k):
        if k == len(loops):
            out.append(ct_eval(idx, env))
            return
        _, init, cond, step = loops[k][:4]
        if init is not None and init[0] == 'decl':
            for d in init[1]:
                env[d[1]] = ct_eval(d[2], env) if d[2] is not None else 0
        elif init is not None:
            ct_eval(init[1], env)
        turns = 0
        while cond is None or ct_eval(cond, env):
            run(k + 1)
            for x in step:
                ct_eval(x, env)
            turns += 1
            if turns > 4096:
                raise _Unknown('a loop that does not end')
    run(0)
    return out


def stack_layout(effects, consts):
    """[(name, kind, size)] for stack effects: kind scalar, cond (size: its
    0/1 atom) or array (size: a Lin in oparg)."""
    env = {'oparg': ceval_reader.Lin.atom('oparg')}
    out = []
    for e in effects:
        if e.type and not e.size:
            raise Skip('stack effect %s has a type' % e.name)
        if e.cond and e.cond.strip() == 'jump':
            out.append((e.name, 'jump', None))
        elif e.cond:
            v = ceval_reader.to_lin(ceval_reader._CTOK.findall(e.cond), env, consts)
            if v is None:
                raise Skip('condition %s' % e.cond)
            out.append((e.name, 'cond', ceval_reader.as_bool(v)))
        elif e.size:
            v = ceval_reader.to_lin(ceval_reader._CTOK.findall(e.size), env, consts)
            if v is None:
                raise Skip('array size %s' % e.size)
            out.append((e.name, 'array', v))
        else:
            out.append((e.name, 'scalar', ceval_reader.Lin(1)))
    return out


def translate(inst, csigs, psigs, consts=None, effects=None, frame=None):
    """C text for one case, and the C-API functions it calls."""
    if inst.kind != 'inst':
        raise Skip('legacy definition without a stack effect')
    inputs = stack_layout([e for e in inst.inputs if isinstance(e, dsl.StackEffect)],
                          consts or {})
    outputs = stack_layout(inst.outputs, consts or {})
    # the bottom slots an instruction leaves as they are (3.12's
    # 'list, unused[oparg-1], v -- list, unused[oparg-1]')
    keep = 0
    while (keep < min(len(inputs), len(outputs))
           and inputs[keep][0] == outputs[keep][0]
           and inputs[keep][1:] == outputs[keep][1:]):
        keep += 1
    for n, kind, _ in outputs[keep:]:
        if (kind != 'scalar' and kind != 'jump') or n == 'unused':
            raise Skip('output %s is an array or conditional' % n)
    for n, kind, _ in inputs[keep:]:
        if n == 'unused':
            raise Skip('unused input popped')
    in_kind = {n: k for n, k, _ in inputs}
    in_names = [n for n, _, _ in inputs if n != 'unused']
    out_names = [n for n, _, _ in outputs if n != 'unused']
    if len(set(in_names)) != len(in_names) or len(set(out_names)) != len(out_names):
        raise Skip('repeated stack name')
    kept = [n for n, _, _ in inputs[:keep]]
    # depth of each input below the top, as C
    depth = {}
    d = ceval_reader.Lin(0)
    for n, kind, size in reversed(inputs):
        d = d + size
        depth[n] = d

    # variable -> origin: 'input', 'local', 'const', 'object' (a meta's
    # result, owned), 'bool' (a meta's int result), 'owned' (incref'd),
    # 'ctint' (a C int of the compiler), 'elem' (an item of an input array,
    # borrowed from the stack)
    var = {n: 'input' for n in in_names}
    released = set()          # inputs Py_DECREF'd
    stolen = set()            # inputs stored (SETLOCAL takes the reference)
    decref_all = False
    body = Lines()            # C lines
    consts = consts or {}
    ctint = {}                # 'ctint' variable -> its value if the generator knows it
    elem_of = {}              # 'elem' variable -> (array, cparse index)
    elem_rel = {}             # array input -> [(cparse index, loops)] Py_DECREF'd
    loops = []                # the cparse loops being emitted, outermost first
    rests = []                # the statements left in each block being read
    pending = set()           # objects declared, not yet assigned
    label = 'error_%s' % inst.name.lower()
    label_used = []           # an error exit inside a loop jumps to the label
    # the generator's values: opcode is the case's own (a case entered by
    # its own TARGET), the constants are the source's
    known_env = dict(consts)
    if inst.name in consts:
        known_env['opcode'] = consts[inst.name]
    decls = []
    calls = []
    last_fail = -1            # index into body of the last failure point
    first_commit = None       # index into body of the first irreversible act
    temps = []                # constants made for a call: released after it
    const_of = {}             # 'const' variable -> Ni's expression for it
    nonneg = set()            # 'int' variables known not negative
    jump = None               # the conditional jump, the case's last statement
    jump_all = None           # an unconditional jump, the case's last statement
    entry = policy_table.ENTRIES.get('Python/ceval.c:TARGET(%s)' % inst.name, {})
    forget = ('forget', 'before_branch') in [tuple(i[:2]) for i in entry.get('policy', [])]
    # the type Ni asserts for the case's output (policy 'restype')
    restype = [i[1] for i in entry.get('policy', []) if i[0] == 'restype']
    if len(restype) > 1 or restype and len(
            [n for n, k, _ in outputs[keep:] if k == 'scalar']) != 1:
        raise Skip('result types %s for the outputs' % ', '.join(restype))

    def commit():
        nonlocal first_commit
        if first_commit is None:
            first_commit = len(body)

    def declare(name, ctype):
        if name in var:
            raise Skip('%s assigned twice' % name)
        d = '%s%s;' % (ctype, name)
        if d not in decls:          # the other arm of a branch declared it
            decls.append(d)
        pending.discard(name)

    def following():
        """The statements after the one being read, outwards."""
        return [x for r in reversed(rests) for x in r]

    def error_code(lab):
        """What an error exit does: leave the opcode with the inputs on
        the stack; inside a loop the C loop is left first."""
        code = LABELS[lab]
        if loops:
            label_used.append(lab)
            code = code.replace('break;', 'goto %s;' % label)
        return code

    def ct_node(text):
        """The cparse expression for a C expression the compiler knows
        (oparg, opcode, the case's C ints, the source's integer
        constants), or None."""
        toks = ctokens.tokenize(text)[0]
        if not toks or any(t.text in ('true', 'false') for t in toks):
            return None
        try:
            e = cparse.parse_expr(toks, TYPE_NAMES, loops=True)
        except cparse.ParseError:
            return None
        if any(k in ('call', 'arrow', 'index', 'str', 'assign') for k in kinds_in(e)):
            return None
        for n in cparse.names_in(e):
            if n not in ('oparg', 'opcode') and var.get(n) != 'ctint' and n not in consts:
                return None
        return e

    def ct_value(e):
        """The generator's value of a compile-time expression, or None."""
        env = dict(known_env)
        env.update((n, v) for n, v in ctint.items() if v is not None)
        try:
            return ct_eval(e, env)
        except _Unknown:
            return None

    def ct_text(text):
        e = ct_node(text)
        if e is None:
            raise Skip('%r is not known at compile time' % text)
        return tidy(text)

    def temp(make):
        t = 'tmp%d' % len(temps)
        temps.append(t)
        decls.append('vinfo_t *%s;' % t)
        body.append('%s = %s;' % (t, make))
        return t

    def operand(text, ptype=None):
        """A call's argument: a vinfo, or (ptype not a vinfo) a C value
        the compiler knows."""
        # a cast between object pointers changes nothing
        text = re.sub(r'^\( (?!PyTypeObject)Py\w*Object \* \) ', '', text)
        if ptype is not None and ptype.replace(' ', '') != 'vinfo_t*':
            return ct_text(text)
        if text == 'NULL':
            return 'NULL'     # Psyco's counterparts take NULL where C does
        if text in var:
            if var[text] == 'const':
                # a borrowed constant passed to a call: a compile-time
                # vinfo for the call's duration
                return temp('vinfo_new(CompileTime_New((long)%s))' % const_of[text])
            if var[text] == 'ctint':
                raise Skip('the C int %s passed as an object' % text)
            return text
        m = re.fullmatch(r'(\w+) \[ (.+) \]', text)
        if m and in_kind.get(m.group(1)) == 'array' and ct_node(m.group(2)) is not None:
            return '%s[%s]' % (m.group(1), tidy(m.group(2)))   # borrowed
        if text in CONSTANTS:
            return temp(CONSTANTS[text])
        raise Skip('operand %r' % text)

    def residual(x, fn, args):
        """A C-API function with no Psyco counterpart: a call at run time
        (P12), with the flags step 4's rules give (gen_abstract.Translator:
        return type, error convention, effects.py's proofs)."""
        nonlocal last_fail
        ctype = {'object': 'PyObject *', 'int': 'int'}.get(csigs[fn][0])
        if ctype is None:
            raise Skip('%s returns %s' % (fn, csigs[fn][0]))
        conv = residual_convention(x, following())
        fmt, ops = '', []
        for a in args:
            if a == 'NULL':
                fmt, o = fmt + 'l', '0L'
            elif ct_node(a) is not None:
                fmt, o = fmt + 'l', '(long)(%s)' % tidy(a)
            else:
                fmt, o = fmt + 'v', operand(a)
            ops.append(o)
        key = effects.globals.get(fn) if effects is not None else None
        flags = gen_abstract.Translator(None, {}, {}, effects).flags(ctype, conv, key)
        call = 'psyco_generic_call(po, %s, %s, "%s"%s)' % (
            fn, flags, fmt, ''.join(', ' + o for o in ops))
        calls.append(fn)
        if var.get(x) == 'ctint':
            # into a C int: 0 or the C's error value, which the meta knows
            if conv != 'zero_ok':
                raise Skip('%s: a run-time int into the C int %s' % (fn, x))
            ctint[x] = None
            body.append('%s = %s != NULL ? 0 : -1;' % (x, call))
        elif conv == 'zero_ok':
            declare(x + '_ok', 'bool ')
            var[x] = 'bool'
            body.append('%s_ok = %s != NULL;' % (x, call))
        else:
            declare(x, 'vinfo_t *')
            var[x] = 'int' if conv == 'neg' else 'object'
            body.append('%s = %s;' % (x, call))
        for t in temps:
            body.append('vinfo_decref(%s, po);' % t)
        temps.clear()
        last_fail = len(body)
        if first_commit is not None:
            raise Skip('call after an irreversible statement')

    def branch(stmt):
        """'if (x OP N) A else B' on a run-time int: its condition code (P05,
        gen_abstract.int_condition), then either the object the arms choose
        (rule bool_select) or a jump on one arm (pycompiler.c:JUMP_IF_CC)."""
        nonlocal last_fail
        cond, then, other = if_parts(stmt)
        m = re.fullmatch(r'(\w+) (==|!=|<|<=|>|>=) (-?\d+)', cond)
        if not m or var.get(m.group(1)) != 'int':
            raise Skip('a branch on %s' % cond)
        x = m.group(1)
        if any(x in [t.text for t in st] for st in following()):
            raise Skip('%s is read after its test' % x)
        call, inv = gen_abstract.int_condition(m.group(2), x, m.group(3), True, x in nonneg)
        cc = 'cc%d' % sum(1 for d in decls if d.startswith('condition_code_t'))
        decls.append('condition_code_t %s;' % cc)
        if first_commit is not None:
            raise Skip('a branch after an irreversible statement (restart rule)')
        body.append('%s = %s;' % (cc, call))
        body.append('vinfo_decref(%s, po);' % x)       # its last use
        var[x] = 'consumed'
        body.append('if (%s == CC_ERROR)' % cc)
        body.append('  ' + error_code('error'))
        last_fail = len(body)

        def invert(flip):
            if inv != flip:
                body.append('%s = INVERT_CC(%s);' % (cc, cc))

        def chosen(arm):
            ws = [words(st) for st in arm]
            inc = [re.fullmatch(r'Py_INCREF \( (\w+) \)', x_) for x_ in ws]
            rest = [x_ for x_, i in zip(ws, inc) if not i]
            a = re.fullmatch(r'(\w+) = (\w+)', rest[0]) if len(rest) == 1 else None
            if a is None or any(i and i.group(1) != a.group(2) for i in inc) \
                    or len(ws) - len(rest) > 1:
                return None
            return a.group(1), a.group(2)
        st, sf = chosen(then), chosen(other)
        if st and sf and st[0] == sf[0]:
            for pair, flip in (((st[1], sf[1]), False), ((sf[1], st[1]), True)):
                if pair in SELECTS:
                    invert(flip)
                    declare(st[0], 'vinfo_t *')
                    var[st[0]] = 'virtual'
                    body.append('%s = %s(po, %s);' % (st[0], SELECTS[pair], cc))
                    return None
            raise Skip('a choice between %s and %s' % (st[1], sf[1]))

        def jumps(arm):
            ws = [words(st) for st in arm]
            j = None
            if ws and re.fullmatch(r'(JUMPTO|JUMPBY) \( .* \)', ws[-1]):
                j = ws.pop().replace(' ', '')
            rel = set()
            for x_ in ws:
                r = re.fullmatch(r'Py_DECREF \( (\w+) \)', x_)
                if not r or in_kind.get(r.group(1)) != 'scalar':
                    raise Skip('a jump arm doing %s' % x_)
                rel.add(r.group(1))
            return j, rel
        (jt, rt), (jf, rf) = jumps(then), jumps(other)
        if (jt is None) == (jf is None):
            raise Skip('a branch with %s' % ('a jump on both arms' if jt else 'no jump'))
        if following() or loops or len(rests) > 1:
            raise Skip('code after a conditional jump')
        invert(jf is not None)
        commit()
        return {'stmt': jt or jf, 'rel': rt if jt else rf, 'fall_rel': rf if jt else rt,
                'cc': cc}

    def release_loop(node):
        """A loop whose body only releases items of input arrays
        (Py_DECREF(values[E])): the releases, recorded."""
        stmts_ = node[4][1] if node[4][0] == 'block' else [node[4]]
        found = []
        for st in stmts_:
            e = st[1] if st[0] == 'expr' else None
            if not (e and e[0] == 'call' and e[1] in (('id', 'Py_DECREF'), ('id', 'Py_XDECREF'))
                    and len(e[2]) == 1 and e[2][0][0] == 'index'
                    and e[2][0][1][0] == 'id' and in_kind.get(e[2][0][1][1]) == 'array'):
                return False
            found.append((e[2][0][1][1], e[2][0][2]))
        for arr, idx in found:
            elem_rel.setdefault(arr, []).append((idx, loops + [node]))
        return bool(found)

    def bound_known(e):
        return all(n in ('oparg', 'opcode') or var.get(n) == 'ctint' or n in consts
                   for n in cparse.names_in(e))

    def for_loop(stmt):
        """A for loop with a bound the compiler knows runs in the compiler
        (rule loops, gen_abstract.loop_rule): the case emits its body once
        per turn."""
        toks = ctokens.tokenize(words(stmt))[0]
        try:
            node = cparse.parse_body(toks, TYPE_NAMES, loops=True)
        except cparse.ParseError as e:
            raise Skip('a loop: %s' % e)
        if len(node) != 1 or node[0][0] != 'for':
            raise Skip('a loop %s' % words(stmt))
        node = node[0]
        if release_loop(node):
            return
        init = node[1]
        if init is not None and init[0] == 'decl':
            for ctype, name, _, dims in init[1]:
                if ctype not in INT_DECLS or dims:
                    raise Skip('a loop declaring %s %s' % (ctype, name))
                declare(name, ctype + ' ')
                var[name] = 'ctint'
        rule = gen_abstract.loop_rule(node, bound_known)
        if rule[0] != 'unroll':
            raise Skip('a loop: %s' % rule[1])
        if var.get(rule[1]) != 'ctint':
            raise Skip('a loop on %s' % rule[1])
        # what the body assigns is not known to the generator after a turn
        for n in gen_abstract.assigned_in(node):
            if n in ctint:
                ctint[n] = None
        _, j = call_args(stmt, 1)
        head = [tidy(x) for x in ' '.join(t.text for t in stmt[2:j - 1]).split(' ; ')]
        if len(head) != 3 or stmt[j].text != '{' or stmt[-1].text != '}':
            raise Skip('a loop %s' % words(stmt))
        if head[0].split(' ')[0] in INT_DECLS:
            head[0] = head[0].split(' ', 1)[1]
        body.append('for (%s; %s; %s) {' % tuple(head))
        body.indent += 1
        loops.append(node)
        ended = run_block(split_statements(stmt[j + 1:-1]))
        loops.pop()
        body.indent -= 1
        body.append('}')
        if ended:
            raise Skip('a loop whose body always fails')

    def ct_if(stmt, node):
        """'if (C) A else B' on a C value the compiler knows: a C if of the
        case (or the arm the generator knows is taken). An arm that ends in
        an error exit leaves the state as it was."""
        cond, then, other = if_parts(stmt)
        v = ct_value(node)
        if v is not None:
            return run_block(then if v else other)
        before = (dict(var), {k: list(x) for k, x in elem_rel.items()})
        outs = []
        for k, arm in enumerate((then, other)):
            if k == 1 and not arm:
                outs.append((False, dict(var)))
                break
            body.append(('if (%s) {' % tidy(cond)) if k == 0 else 'else {')
            body.indent += 1
            ended = run_block(arm)
            body.indent -= 1
            body.append('}')
            outs.append((ended, dict(var)))
            if {k_: list(x) for k_, x in elem_rel.items()} != before[1]:
                raise Skip('items released on one arm of %s' % cond)
            var.clear()
            var.update(before[0])
        live = [o for e, o in outs if not e]
        if not live:
            return True
        if any(o != live[0] for o in live):
            raise Skip('the arms of %s leave different values' % cond)
        var.update(live[0])
        for n in gen_abstract.assigned_in(cparse.parse_body(
                ctokens.tokenize(words(stmt))[0], TYPE_NAMES, loops=True)):
            if n in ctint:
                ctint[n] = None
        return False

    def error_block(stmt, cond):
        """'if (C) { releases; ERROR_IF(true, L); }', C the error test of a
        call's result: that exit, the releases on its path only."""
        _, then, other = if_parts(stmt)
        if other or not then:
            return False
        last = words(then[-1]).replace(' ', '')
        m = re.fullmatch(r'ERROR_IF\(true,(\w+)\)', last)
        if not m or m.group(1) not in LABELS:
            return False
        try:
            c = error_condition(cond, var)
        except Skip:
            return False
        if c is None:
            return False
        if first_commit is not None:
            raise Skip('an error exit after an irreversible statement (restart rule)')
        before = dict(var)
        body.append('if (%s) {' % c[0])
        body.indent += 1
        for x in c[1]:
            var[x] = 'consumed'          # NULL where the test holds
        if not run_block(then):
            raise Skip('an error block that does not end')
        body.indent -= 1
        body.append('}')
        var.clear()
        var.update(before)
        for x in c[1]:
            if var.get(x) == 'int':
                nonneg.add(x)
                if not any(x in [t.text for t in st] for st in following()):
                    # a run-time int read only by its test
                    body.append('vinfo_decref(%s, po);' % x)
                    var[x] = 'consumed'
        return True

    def run_block(stmts):
        """Translate a block's statements; True if it ends in an error exit."""
        rests.append(stmts)
        try:
            return block_stmts(stmts)
        finally:
            rests.pop()

    def block_stmts(stmts):
        nonlocal decref_all, jump, jump_all, last_fail
        while stmts:
            stmt = stmts.pop(0)
            w = [t.text for t in stmt]
            s = words(stmt)
            if w[0] in DROPPED or (len(w) == 4 and w[:3] == ['(', 'void', ')']):
                continue
            if s == 'DECREF_INPUTS ( )':
                decref_all = True
                continue
            if len(w) == 4 and w[0] in ('Py_DECREF', 'Py_XDECREF') and w[2] in in_names \
                    and (w[0] == 'Py_DECREF') == (in_kind[w[2]] == 'scalar'):
                released.add(w[2])
                continue
            if len(w) == 4 and w[0] == 'Py_DECREF' and var.get(w[2]) in ('object', 'owned', 'int'):
                # a value the case owns, released at the C's place
                body.append('vinfo_decref(%s, po);' % w[2])
                var[w[2]] = 'consumed'
                continue
            if len(w) == 4 and w[0] == 'Py_DECREF' and var.get(w[2]) == 'elem':
                # an item of an input array: released with the array (the
                # loops must release each item once)
                arr, idx = elem_of[w[2]]
                elem_rel.setdefault(arr, []).append((idx, list(loops)))
                continue
            if len(w) == 2 and w[0] in INT_DECLS:
                declare(w[1], w[0] + ' ')
                var[w[1]] = 'ctint'
                ctint[w[1]] = None
                continue
            if w[:2] == ['PyObject', '*'] and len(w) == 3:
                pending.add(w[2])
                continue
            if w[0] == 'for':
                for_loop(stmt)
                continue
            if len(w) == 4 and w[0] == 'Py_INCREF':
                x = w[2]
                if var.get(x) == 'local':
                    commit()
                    body.append('vinfo_incref(%s);' % x)
                    var[x] = 'owned'
                elif var.get(x) == 'const':
                    decls.append('vinfo_t *%s;' % x)
                    body.append('%s = vinfo_new(CompileTime_New((long)%s));'
                                % (x, const_of[x]))
                    var[x] = 'owned'
                else:
                    raise Skip('Py_INCREF of %s (%s)' % (x, var.get(x)))
                continue
            # X = Py_NewRef(Y)
            if len(w) == 6 and w[1:4] == ['=', 'Py_NewRef', '('] and w[4] in var:
                x, y = w[0], w[4]
                declare(x, 'vinfo_t *')
                commit()
                body.append('vinfo_incref(%s);' % y)
                body.append('%s = %s;' % (x, y))
                var[x] = 'owned'
                continue
            if w == ['SETLOCAL', '(', 'oparg', ',', 'NULL', ')']:
                # a deleted local is a known NULL (the same rule as GETLOCAL's)
                commit()
                t = 'tmp%d' % len(decls)
                decls.append('vinfo_t *%s;' % t)
                body.append('%s = %s;' % (t, CONSTANTS['NULL']))
                body.append('SETLOCAL(oparg, %s);' % t)
                continue
            if w[0] in JUMPS and len(w) > 1 and w[1] == '(':
                if stmts:
                    raise Skip('code after a jump')
                commit()
                jump_all = s.replace(' ', '')
                continue
            if w[0] == 'if':
                cond = call_args(stmt, 1)[0][0]
                node = ct_node(cond)
                if node is not None:
                    if ct_if(stmt, node):
                        if stmts:
                            raise Skip('code after an error exit')
                        return True
                    continue
                m = re.fullmatch(r'(_?Py[A-Z]\w*) (\( .* \)) (<|==|!=) (.+)', cond)
                if m and m.group(1) in csigs:
                    # the call in an error test is made first, into a
                    # temporary the test then reads
                    t = '_r%d' % len(decls)
                    pre = ctokens.tokenize('int %s = %s %s ;' % (t, m.group(1), m.group(2)))[0]
                    k = call_args(stmt, 1)[1]
                    test = ctokens.tokenize('if ( %s %s %s )' % (t, m.group(3), m.group(4)))[0]
                    stmts[:0] = [pre[:-1], test + stmt[k:]]
                    continue
                if error_block(stmt, cond):
                    continue
                if len(rests) > 1:
                    raise Skip('a branch on %s inside a block' % cond)
                jump = branch(stmt)
                continue
            if w[:4] == ['SETLOCAL', '(', 'oparg', ','] and len(w) == 6:
                x = w[4]
                if var.get(x) not in ('input', 'owned', 'object', 'virtual') or \
                        in_kind.get(x, 'scalar') != 'scalar':
                    raise Skip('SETLOCAL of %s' % x)
                commit()
                body.append('SETLOCAL(oparg, %s);' % x)
                stolen.add(x)
                continue
            if len(w) > 2 and w[:2] == ['ERROR_IF', '(']:
                args, _ = call_args(stmt, 1)
                if len(args) != 2 or args[1] not in LABELS:
                    raise Skip('ERROR_IF %s' % s)
                if first_commit is not None:
                    raise Skip('ERROR_IF after an irreversible statement'
                               ' (restart rule)')
                if args[0] == 'true':
                    # an error exit on this path: the block's last statement
                    if stmts:
                        raise Skip('code after ERROR_IF(true)')
                    live = [t for t, o in var.items()
                            if o in ('object', 'owned', 'virtual', 'int')]
                    if live:
                        raise Skip('an error exit with %s owned' % ', '.join(live))
                    body.append(error_code(args[1]))
                    last_fail = len(body)
                    return True
                cond = error_condition(args[0], var)
                if cond is None:
                    continue        # a virtual object: it can't fail
                for x in cond[1]:
                    if var.get(x) == 'int':
                        nonneg.add(x)   # the C's error values are the negative ones
                live = [t for t, o in var.items()
                        if o in ('object', 'owned', 'virtual', 'int') and t not in cond[1]]
                if live:
                    raise Skip('ERROR_IF with %s owned' % ', '.join(live))
                body.append('if (%s)' % cond[0])
                body.append('  ' + error_code(args[1]))
                last_fail = len(body)
                continue
            # [type] X = RHS
            ctype = ''
            if (w[0] in INT_DECLS or w[0] == 'PyObject') and len(w) > 2:
                ctype = w[0] if w[0] in INT_DECLS else 'PyObject *'
                stmt, w = stmt[1:], w[1:]
                if ctype == 'PyObject *':
                    if w[0] != '*':
                        raise Skip(s)
                    stmt, w = stmt[1:], w[1:]
            if len(w) >= 3 and w[1] == '=':
                x = w[0]
                rhs = ' '.join(w[2:])
                node = ct_node(rhs)
                if node is not None and (ctype in INT_DECLS or var.get(x) == 'ctint'):
                    # a C int of the compiler
                    if var.get(x) != 'ctint':
                        declare(x, ctype + ' ')
                        var[x] = 'ctint'
                    ctint[x] = ct_value(node)
                    body.append('%s = %s;' % (x, tidy(rhs)))
                    continue
                m = re.fullmatch(r'(\w+) \[ (.+) \]', rhs)
                if m and in_kind.get(m.group(1)) == 'array' and ct_node(m.group(2)) is not None:
                    # an item of an input array, borrowed from the stack
                    declare(x, 'vinfo_t *')
                    var[x] = 'elem'
                    elem_of[x] = (m.group(1), cparse.parse_expr(
                        ctokens.tokenize(m.group(2))[0], TYPE_NAMES, loops=True))
                    body.append('%s = %s[%s];' % (x, m.group(1), tidy(m.group(2))))
                    continue
                if len(w) == 3 and var.get(w[2]) in ('object', 'owned') and not ctype:
                    # the value moves to x (C's 'return_value = sum')
                    declare(x, 'vinfo_t *')
                    var[x], var[w[2]] = var[w[2]], 'consumed'
                    body.append('%s = %s;' % (x, w[2]))
                    continue
                if rhs == 'GETLOCAL ( oparg )':
                    declare(x, 'vinfo_t *')
                    body.append('%s = GETLOCAL(oparg);' % x)
                    var[x] = 'local'
                    continue
                m = re.fullmatch(r'(\w+) \[ (.+) \]', rhs)
                if m and m.group(1) in (frame or {}) and ct_node(m.group(2)) is not None:
                    # an item of the frame's locals-plus array (ceval's
                    # freevars): a local of Ni's frame
                    declare(x, 'vinfo_t *')
                    off = frame[m.group(1)]
                    body.append('%s = GETLOCAL(%s%s);' % (
                        x, off + ' + ' if off else '', tidy(m.group(2))))
                    var[x] = 'local'
                    continue
                m = re.fullmatch(r'GETITEM \( (.*) , oparg \)', rhs)
                if m and m.group(1) in CODE_TUPLES:
                    if x in var:
                        raise Skip('%s assigned twice' % x)
                    var[x] = 'const'
                    const_of[x] = CODE_TUPLES[m.group(1)]   # borrowed
                    continue
                m = re.fullmatch(r'(Py[A-Z][a-z]+)_New \( (.+) \)', rhs)
                fill = m and fill_loop(stmts, x, m.group(1), m.group(2), inputs)
                if fill:
                    # PATTERNS E: a new container filled from an input array is
                    # Psyco's constructor taking the items
                    pname, kind = constructor(m.group(1) + '_New', psigs)
                    calls.append(m.group(1) + '_New')
                    declare(x, 'vinfo_t *')
                    var[x] = 'virtual' if kind == 'virtual' else 'object'
                    body.append('%s = %s(%s%s, %s);' % (
                        x, pname, '' if kind == 'virtual' else 'po, ',
                        m.group(2), fill))
                    released.add(fill)
                    last_fail = len(body)
                    if first_commit is not None:
                        raise Skip('call after an irreversible statement')
                    continue
                if len(w) > 3 and re.fullmatch(r'_?Py[A-Z][A-Za-z]*_[A-Za-z]+', w[2]) and w[3] == '(':
                    fn = w[2]
                    args, end = call_args(stmt, 3)
                    if end != len(stmt):
                        raise Skip(s)
                    try:
                        pname, ret = counterpart(fn, csigs, psigs)
                    except NoCounterpart:
                        residual(x, fn, args)
                        continue
                    ops = [operand(a, p) for a, p in zip(args, psigs[pname][2])]
                    calls.append(fn)
                    if var.get(x) == 'ctint':
                        # into a C int: 0 or the C's error value
                        if ret != 'int':
                            raise Skip('%s into the C int %s' % (fn, x))
                        ctint[x] = None
                        body.append('%s = %s(po, %s) ? 0 : -1;' % (x, pname, ', '.join(ops)))
                    elif ret == 'intval':
                        declare(x, 'vinfo_t *')
                        var[x] = 'int'
                        body.append('%s = %s(po, %s);' % (x, pname, ', '.join(ops)))
                    elif ret == 'virtual':
                        declare(x, 'vinfo_t *')
                        var[x] = 'virtual'
                        body.append('%s = %s(%s);' % (x, pname, ', '.join(ops)))
                    elif ret == 'object':
                        declare(x, 'vinfo_t *')
                        var[x] = 'object'
                        body.append('%s = %s(po, %s);' % (x, pname, ', '.join(ops)))
                    else:
                        # C's int (0: done) becomes Psyco's bool (true: done);
                        # the name says which, ERROR_IF tests it as x_ok
                        declare(x + '_ok', 'bool ')
                        var[x] = 'bool'
                        body.append('%s_ok = %s(po, %s);' % (x, pname, ', '.join(ops)))
                    for t in temps:
                        body.append('vinfo_decref(%s, po);' % t)
                    temps.clear()
                    last_fail = len(body)
                    if first_commit is not None:
                        raise Skip('call after an irreversible statement')
                    continue
            raise Skip('statement %r' % s)
        return False

    if run_block(split_statements(body_tokens(inst))):
        raise Skip('every path fails')
    # items of input arrays released in loops: each once, or the array
    # isn't released
    for arr, rels in elem_rel.items():
        size = next(sz for n, k, sz in inputs if n == arr)
        for o in SIM_OPARGS:
            got = []
            for idx, lps in rels:
                env = dict(known_env)
                env['oparg'] = o
                try:
                    got += loop_indices(lps, idx, env)
                except _Unknown as e:
                    raise Skip('the items of %s released: %s' % (arr, e))
            if sorted(got) != list(range(size.value(o, consts))):
                raise Skip('the items of %s are not each released once' % arr)
        released.add(arr)

    # epilogue: inputs not moved to an output are released or stolen; with
    # a conditional jump this is the path that doesn't jump, and outputs
    # 'x if (jump)' are kept only by the one that does
    jout = {n for n, k, _ in outputs if k == 'jump'}
    if jout - set(in_names):
        raise Skip('a jump keeps %s, not an input' % ', '.join(sorted(jout - set(in_names))))
    if jout and jump is None:
        raise Skip('outputs on a jump with no conditional jump')
    out_names = [n for n in out_names if n not in jout]
    if decref_all:
        released |= set(in_names) - set(out_names) - stolen
    jump_line = None
    if jump is not None:
        rel_jump = released | jump['rel']
        popped = [(n, k) for n, k, _ in inputs[keep:] if n not in jout]
        if any(n in rel_jump for n in jout):
            raise Skip('a jump keeps an input it released')
        if out_names or len(popped) > 1 or (popped and (
                popped[0][1] != 'scalar' or popped[0][0] != inputs[-1][0]
                or popped[0][0] not in rel_jump)):
            raise Skip('a jump that pops other than its top input')
        jump_line = 'JUMP_IF_CC(%s, %s, %d, %d);' % (jump['cc'], jump['stmt'],
                                                    len(popped), int(forget))
        released = released | jump['fall_rel']
    for n in in_names:
        if n in kept:
            if n in released or n in stolen:
                raise Skip('input %s is released and also kept' % n)
            continue
        if n in out_names and (n in released or n in stolen):
            raise Skip('input %s is released and also an output' % n)
        if n not in out_names and n not in released | stolen:
            raise Skip('input %s is neither released nor stored' % n)
    for n, origin in var.items():
        if n not in in_names and n not in out_names and n not in jout and \
                origin not in ('bool', 'const', 'local', 'consumed', 'ctint', 'elem'):
            # not a stack value: a result dropped, or interpreter state
            # (3.12's kwnames)
            raise Skip('%s is assigned but is not an output' % n)
    for n in out_names:
        if n in kept:
            continue
        if n not in var or var[n] not in ('input', 'owned', 'object', 'virtual'):
            raise Skip('output %s has no owned value (%s)' % (n, var.get(n)))
    assert first_commit is None or first_commit >= last_fail
    lines = ['case %s: {' % inst.name]
    for n, kind, size in inputs:
        if n == 'unused':
            continue
        at = depth[n].text()
        if kind == 'scalar':
            lines.append('  vinfo_t *%s = NTOP(%s);' % (n, at))
        elif kind == 'cond':
            lines.append('  vinfo_t *%s = %s ? NTOP(%s) : NULL;'
                         % (n, size.text(), at))
        else:
            lines.append('  vinfo_t **%s = STACK_POINTER() - (%s);' % (n, at))
    for n, v in ctint.items():
        # a C int the generator folded wherever it is read: not emitted
        own = re.compile(r'\s*%s = [^;]*;$' % n)
        if v is not None and not any(re.search(r'\b%s\b' % n, b) and not own.match(b)
                                     for b in body):
            body = [b for b in body if not own.match(b)]
            decls = [d for d in decls if not re.fullmatch(r'\w+ %s;' % n, d)]
    lines += ['  ' + d for d in decls]
    lines += ['  ' + b for b in body]
    if jump_line:
        lines.append('  ' + jump_line)
    late = []           # releases, after the pops above them
    for n, kind, size in reversed(inputs[keep:]):
        if kind == 'array':
            lines += ['  ' + x for x in late]
            late = []
            if n not in released:
                raise Skip('array %s popped but not released' % n)
            lines.append('  { int n = %s; while (n--) POP_DECREF(); }' % size.text())
        elif kind == 'cond':
            lines.append('  if %s POP(%s);' % (size.text(), n))
            if n in released:
                late.append('vinfo_xdecref(%s, po);' % n)
        else:
            lines.append('  POP(%s);' % n)
            if n in released:
                late.append('vinfo_decref(%s, po);' % n)
    lines += ['  ' + x for x in late]
    for n, _, _ in outputs[keep:]:
        if n not in jout:
            for t in restype:
                # the policy's type of the output (rule restype)
                lines.append('  Psyco_AssertType(po, %s, &%s);' % (n, t))
            lines.append('  PUSH(%s);' % n)
    if jump_all:
        lines.append('  %s;' % jump_all)
    lines += ['  %s' % ('JUMPED();' if jump_all else 'goto fine;')]
    if label_used:
        lines += [' %s:' % label, '  break;']
    lines += ['}']
    return '\n'.join(lines), calls


def fill_loop(stmts, x, typ, count, inputs):
    """The array name if stmts start (after ERROR_IF(x == NULL, error))
    with the loop that moves every item of an input array of size count
    into x: 'for (int i = N; --i >= 0; ) { PyObject *v = A[i];
    T_SET_ITEM(x, i, v); }'. The loop is taken out of stmts."""
    k = 0
    if k < len(stmts) and words(stmts[k]).replace(' ', '') == \
            'ERROR_IF(%s==NULL,error)' % x:
        k += 1
    if k >= len(stmts):
        return None
    m = re.fullmatch(r'for \( int (\w+) = (.+) ; -- \1 >= 0 ; \) \{ '
                     r'PyObject \* (\w+) = (\w+) \[ \1 \] ; '
                     r'%s_SET_ITEM \( %s , \1 , \3 \) ; \}' % (typ, x),
                     words(stmts[k]))
    if not m or m.group(2) != count:
        return None
    arr = next(((n, sz) for n, kind, sz in inputs if n == m.group(4) and kind == 'array'), None)
    if arr is None or arr[1].text() != count:
        return None
    del stmts[k]
    return arr[0]


def error_condition(cond, var):
    """C condition for ERROR_IF(cond), and the variables it tests."""
    m = re.fullmatch(r'(\w+) == NULL', cond)
    if m and var.get(m.group(1)) == 'object':
        return '%s == NULL' % m.group(1), {m.group(1)}
    if m and var.get(m.group(1)) == 'local':
        # a local is unbound only if its value is known to be NULL; a
        # run-time or virtual value never is (the same rule as DELETE_FAST)
        return 'psyco_knowntobe(%s, 0)' % m.group(1), set()
    m = re.fullmatch(r'(\w+) < 0', cond)
    if m and var.get(m.group(1)) == 'int':
        # a meta's int result: NULL where the C's is negative
        return '%s == NULL' % m.group(1), {m.group(1)}
    m = re.fullmatch(r'(\w+)(?: != 0| < 0)?', cond)
    if m and var.get(m.group(1)) == 'bool':
        return '!%s_ok' % m.group(1), {m.group(1)}
    m = re.fullmatch(r'(\w+) == NULL', cond)
    if m and var.get(m.group(1)) == 'virtual':
        return None
    raise Skip('ERROR_IF condition %r' % cond)


# ---- checks against the target's dis --------------------------------------

# opargs tried: an effect that differs between them depends on oparg
OPARGS = (0, 1, 2, 3, 4, 5, 7, 8, 255, 256, 257, 258, 513)


def stack_effects(names, python):
    """{name: [effect for each of OPARGS] or None} from dis.stack_effect in
    the target interpreter."""
    code = ('import dis, json, opcode, sys\n'
            'out = {}\n'
            'for n in sys.argv[2:]:\n'
            '    op = opcode.opmap.get(n)\n'
            '    if op is None:\n'
            '        out[n] = None  # not an opcode of this version\n'
            '        continue\n'
            '    try:\n'
            '        out[n] = [dis.stack_effect(op, a if op >= opcode.HAVE_ARGUMENT else None)\n'
            '                  for a in json.loads(sys.argv[1])]\n'
            '    except ValueError:\n'
            '        out[n] = None  # unknown to compile.c (NOP)\n'
            'print(json.dumps(out))\n')
    r = subprocess.run([python, '-I', '-c', code, json.dumps(OPARGS)] + names,
                       check=True, stdout=subprocess.PIPE,
                       universal_newlines=True)
    return json.loads(r.stdout)


def check_effects(insts, effects, consts):
    """Problems found comparing each definition's effect, at each of
    OPARGS, with dis."""
    problems = []
    for inst in insts:
        eff = effects.get(inst.name)
        if eff is None:
            if inst.name not in effects:
                problems.append('%s: not an opcode' % inst.name)
            continue
        try:
            declared = [sum(ceval_reader.effect_size(e, o, consts) for e in inst.outputs)
                        - sum(ceval_reader.effect_size(e, o, consts) for e in inst.inputs
                              if isinstance(e, dsl.StackEffect))
                        for o in OPARGS]
        except ceval_reader.Refuse as e:
            problems.append('%s: stack effect %s' % (inst.name, e))
            continue
        if declared != eff:
            problems.append('%s: stack effect %s, dis.stack_effect says %s (opargs %s)'
                            % (inst.name, declared, eff, list(OPARGS)))
    return problems


# ---- driver ----------------------------------------------------------------

def definitions(src):
    """(definitions, refused) from the source: its bytecodes.c if it has
    one, else its ceval.c through the reader. refused: [(name, kind, why)]."""
    bytecodes = os.path.join(src, 'Python', 'bytecodes.c')
    if os.path.exists(bytecodes):
        return [d for d in read_defs(bytecodes) if isinstance(d, dsl.InstDef)], []
    return ceval_reader.read(src)


def frame_arrays(src):
    """{name: offset} for the eval loop's pointers into the frame's
    locals-plus array, from its assignments 'NAME = f->f_localsplus + E;'
    in _PyEval_EvalFrameDefault: NAME[i] is Ni's GETLOCAL(E + i)."""
    with open(os.path.join(src, 'Python', 'ceval.c')) as f:
        toks = ctokens.tokenize(f.read(), 'ceval.c')[0]
    fn = ctokens.functions(toks).get('_PyEval_EvalFrameDefault')
    out = {}
    if fn is None:
        return out
    text = ' '.join(t.text for t in fn[1])
    for m in re.finditer(r'(?:^|(?<=[;{}] ))(\w+) = f -> f_localsplus (?:\+ ([^;]+) )?;', text):
        out.setdefault(m.group(1), tidy(m.group(2)).replace(' -> ', '->') if m.group(2) else '')
    return out


def run(insts, src, effects=None):
    csigs, psigs = c_signatures(src), psyco_signatures()
    consts = ceval_reader.constants(src)
    frame = frame_arrays(src)
    done, skipped = [], []
    for inst in insts:
        try:
            w = [t.text for t in inst.block.tokens]
            copies = {w[k - 1] for k in range(1, len(w) - 1)
                      if w[k] == '=' and w[k + 1] == 'stack_pointer'}
            if any(w[k] == '&' and w[k + 1] in copies for k in range(len(w) - 1)):
                # calls: a copy of the stack pointer handed to the call
                # path, which moves it; that is core
                k = next(k for k in range(2, len(w))
                         if w[k] == '&' and w[k + 1] in copies)
                raise Skip('a copy of the stack pointer handed to %s'
                           % called_at(w, k), 'core')
            code, _ = translate(inst, csigs, psigs, consts, effects, frame)
        except Skip as e:
            skipped.append((inst.name, e.kind, str(e)))
            continue
        done.append((inst.name, code))
    return done, skipped


def called_at(w, k):
    """The function whose argument list w[k] is in."""
    depth = 0
    for j in range(k - 1, 0, -1):
        depth += (w[j] == ')') - (w[j] == '(')
        if depth < 0:
            return w[j - 1]
    return '?'


def header(done, skipped):
    out = ['/* GENERATED by tools/metac/gen_ceval.py from the opcode cases of',
           '   the CPython source built for: do not edit.',
           '   Included in the opcode switch of pycompiler.c, which provides',
           '   NTOP, POP, POP_DECREF, PUSH, STACK_POINTER, GETLOCAL, SETLOCAL,',
           '   GETCONST, GETNAMEV,',
           '   unbound_local_error and the label fine. */', '']
    for _, code in done:
        out += ['      ' + l if l else '' for l in code.split('\n')] + ['']
    out += ['/* not generated (hand-written in pycompiler.c, or unsupported),',
            '   by kind:']
    for kind in ['vocabulary'] + list(ceval_reader.KINDS):
        group = [x for x in skipped if x[1] == kind]
        if group:
            out.append('   %s (%d):' % (kind, len(group)))
            out += ['     %s: %s' % (n, why) for n, _, why in group]
    out += ['*/', '']
    return '\n'.join(out)


def build(target):
    insts, refused = definitions(target.src)
    effects = stack_effects([i.name for i in insts] + [n for n, _, _ in refused],
                            target.python)
    problems = check_effects(insts, effects, ceval_reader.constants(target.src))
    if problems:
        raise cslots.GenError(
            'the definitions read from CPython %s do not match'
            ' dis.stack_effect:\n%s'
            % (target.version, '\n'.join('  ' + p for p in problems)))
    try:
        import effects
        g = effects.analyse(target.src)
    except Exception as e:  # noqa: BLE001 - the build must say why
        raise cslots.GenError('effects.py: %s' % e)
    done, skipped = run(insts, target.src, g)
    skipped += refused
    return header(done, skipped), (
        ['generated  %s' % name for name, _ in done] +
        ['skipped    %-28s %-13s %s' % x for x in skipped])


def main(argv):
    if len(argv) != 3 or argv[0] != '--try':
        print('usage: gen_ceval.py --try BYTECODES_C CPYTHON_SRC')
        return 2
    insts = [d for d in read_defs(argv[1]) if isinstance(d, dsl.InstDef)]
    done, skipped = run(insts, argv[2])
    for name, _ in done:
        print('translated %s' % name)
    for name, _, why in skipped:
        print('skipped    %-28s %s' % (name, why))
    print('%d of %d translated' % (len(done), len(insts)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
