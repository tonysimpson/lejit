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

D88 (policy_table RULES): a cell's field written (field_store) and its old
value released with psyco_xdecref_v; a ceval.c helper the Inliner refuses
called as a run-time copy (helper_copy); an error block's handler moved
into a wrapper of the call it follows (error_handler); an input array and
its count, and a temporary of constants, passed through a wrapper
(residual_wrapper); a switch on a compile-time value as an if-chain with
function pointers known per arm (switch_arms); a type test that is only a
shortcut false for a type Ni doesn't know (type_test_generic).

Every stack effect read is checked against dis.stack_effect() run in the
interpreter being built for, for several opargs: it must be the same. A
mismatch stops generation, and so the build.

Run by generate.py.  gen_ceval.py --try BYTECODES_C CPYTHON_SRC  reports
only, e.g. on 3.12's file.
"""
import glob
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'cases_generator'))
import ceval_inline  # noqa: E402  (ceval.c's static helpers, inlined)
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
# what setting and testing an error are where the compiler runs a ceval.c
# helper (rule compile_time_copy): Ni's compile-time exception
CT_ERRORS = {'PyErr_Format': 'PycException_SetFormat',
             'PyErr_SetString': 'PycException_SetString',
             'PyErr_Occurred': 'PycException_Occurred'}
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
        if t.text == 'switch' and depth == 0 and not cur:
            # 'switch (...) { ... }' is one statement (translate() makes it
            # an if-chain)
            j = ctokens.match_close(toks, ctokens.match_close(toks, k + 1) + 1)
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
# ceval's names for what Ni's compiler has under the same name: the code
# object being compiled
CODE_NAMES = ('co',)
# the variable kinds that hold an object (a vinfo of one)
OBJECT_KINDS = ('input', 'local', 'object', 'owned', 'virtual', 'elem')
# names of types in a case's text (casts and declarations, for cparse)
TYPE_NAMES = {'Py_ssize_t', 'PyObject', 'PyListObject', 'PyTupleObject',
              'PyDictObject', 'PySetObject', 'PyTypeObject', 'size_t'}


def _has(e, pred):
    """Does a node of the cparse expression e satisfy pred?"""
    if isinstance(e, tuple) and e and isinstance(e[0], str):
        return pred(e) or any(_has(x, pred) for x in e[1:])
    if isinstance(e, list):
        return any(_has(x, pred) for x in e)
    return False


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


def translate(inst, csigs, psigs, consts=None, effects=None, frame=None, ctx=None):
    """C text for one case, the C-API functions it calls, and the run-time
    copies of inlined helpers' arms it calls (D82). ctx: the source's
    Context, for inlining and type tests."""
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
    # borrowed from the stack), 'field' (a field read: Ni's vinfo, the C's
    # borrowed reference, maybe NULL at run time)
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
    nonnull = set()           # 'field' variables known not NULL on this path
    fnval = {}                # 'fnptr' variable -> the C function it points to here
    jump = None               # the conditional jump, the case's last statement
    jump_all = None           # an unconditional jump, the case's last statement
    cond_of = {}              # 'cond' variable -> (its condition code, len(calls) then)
    # what the policy entry says a Py_INCREF of a field value is
    # (('ref', 'need_reference', ...): Ni takes a reference only if it has none)
    entry_policy = [tuple(i[:2]) for i in policy_table.ENTRIES.get(
        'Python/ceval.c:TARGET(%s)' % inst.name, {}).get('policy', [])]
    sel_of = {}               # 'selected' variable -> (constructor, condition code)
    copies = []               # run-time copies of inlined arms: (name, C text)
    wrappers = {}             # run-time wrappers of calls with error handlers -> return type
    regions = []              # resumable arms being read (innermost last)
    facts = []                # type tests true on this path: (macro, variable)
    resume_labels = []        # the labels of resumable arms' copies
    loaded = {}               # objects whose length and items Ni has loaded -> known_length
    ctx = ctx or Context()
    entry =policy_table.ENTRIES.get('Python/ceval.c:TARGET(%s)' % inst.name, {})
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
        if name in var and (var[name] != 'consumed' or name in in_names):
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
        if 'vinfo_t *%s;' % t not in decls:
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

    def residual(x, fn, args, ret=None, key=None):
        """A C-API function with no Psyco counterpart: a call at run time
        (P12), with the flags step 4's rules give (gen_abstract.Translator:
        return type, error convention, effects.py's proofs). ret, key: the
        return type and effects key of a function not in csigs (a ceval.c
        helper's run-time copy)."""
        nonlocal last_fail
        kind = csigs[fn][0] if ret is None else \
            {'PyObject *': 'object', 'int': 'int'}.get(ret, ret)
        ctype = {'object': 'PyObject *', 'int': 'int'}.get(kind)
        if ctype is None:
            raise Skip('%s returns %s' % (fn, kind))
        try:
            conv = residual_convention(x, following())
        except Skip as e:
            # an int the C reads but never tests for an error: not an
            # error indicator (PyErr_GivenExceptionMatches)
            if ctype != 'int' or 'never checked' not in str(e) or var.get(x) == 'ctint':
                raise
            conv = None
        if any(in_kind.get(a) == 'array' for a in args):
            return wrapped(x, fn, args, ctype, conv, key)
        if temp_arg(x, fn, args):
            return
        fmt, ops = '', []
        for a in args:
            if a == 'NULL':
                fmt, o = fmt + 'l', '0L'
            elif in_kind.get(a) == 'cond':
                # NULL where the input is absent, as the C's
                fmt, o = fmt + 'v', '%s ? %s : %s' % (a, a, temp(CONSTANTS['NULL']))
            elif ct_node(a) is not None:
                fmt, o = fmt + 'l', '(long)(%s)' % tidy(a)
            else:
                fmt, o = fmt + 'v', operand(a)
            ops.append(o)
        if key is None and ret is None:
            key = effects.globals.get(fn) if effects is not None else None
        tr = gen_abstract.Translator(None, {}, {}, effects)
        if conv is None:
            flags = ' | '.join(['CfReturnTypeInt'] + tr.effect_flags(key))
        else:
            flags = tr.flags(ctype, conv, key)
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
            var[x] = 'int' if conv in ('neg', None) else 'object'
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
        alias = {}        # 'PyObject *item = values[E];' then Py_DECREF(item)
        for st in stmts_:
            if st[0] == 'decl' and len(st[1]) == 1 and st[1][0][0] == 'PyObject *' \
                    and st[1][0][2] is not None and st[1][0][2][0] == 'index':
                alias[st[1][0][1]] = st[1][0][2]
                continue
            e = st[1] if st[0] == 'expr' else None
            if e and e[0] == 'call' and len(e[2]) == 1 and e[2][0][0] == 'id' \
                    and e[2][0][1] in alias:
                e = (e[0], e[1], [alias.pop(e[2][0][1])])
            if not (e and e[0] == 'call' and e[1] in (('id', 'Py_DECREF'), ('id', 'Py_XDECREF'))
                    and len(e[2]) == 1 and e[2][0][0] == 'index'
                    and e[2][0][1][0] == 'id' and in_kind.get(e[2][0][1][1]) == 'array'):
                return False
            found.append((e[2][0][1][1], e[2][0][2]))
        if alias:
            return False
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

    def ct_if(stmt, node, text=None, fact=None):
        """'if (C) A else B' on a C value the compiler knows: a C if of the
        case (or the arm the generator knows is taken). An arm that ends in
        an error exit leaves the state as it was. text: the compiler's C
        for C when it isn't C's own (a type test); fact: the type test
        that holds on the then arm."""
        cond, then, other = if_parts(stmt)
        v = ct_value(node) if node is not None else None
        if v is not None:
            return run_block(then if v else other)
        nonlocal first_commit
        before = (dict(var), {k: list(x) for k, x in elem_rel.items()})
        commit0, released0 = first_commit, set(released)
        commits, rels = [], []
        outs = []
        for k, arm in enumerate((then, other)):
            if k == 1 and not arm:
                outs.append((False, dict(var)))
                rels.append(set(released))
                break
            first_commit = commit0
            released.clear()
            released.update(released0)
            body.append(('if (%s) {' % (text or tidy(cond))) if k == 0 else 'else {')
            body.indent += 1
            if fact and fact[k]:
                facts.append(fact[k])
            ended = run_block(arm)
            if fact and fact[k]:
                facts.pop()
            body.indent -= 1
            body.append('}')
            outs.append((ended, dict(var)))
            if not ended:
                commits.append(first_commit)
                rels.append(set(released))
            if {k_: list(x) for k_, x in elem_rel.items()} != before[1]:
                raise Skip('items released on one arm of %s' % cond)
            var.clear()
            var.update(before[0])
        # a path's irreversible statements are its own; after the join
        # everything is irreversible if one arm's was
        first_commit = commit0 if all(c == commit0 for c in commits) else len(body)
        released.clear()
        released.update(released0)
        for r in rels:
            # an input released on some arms only: only a conditional one,
            # NULL on the others (its release is vinfo_xdecref)
            for n_ in r ^ rels[0]:
                if in_kind.get(n_) != 'cond':
                    raise Skip('the arms of %s release %s differently' % (cond, n_))
            released.update(r)
        live = [o for e, o in outs if not e]
        if not live:
            return True

        def held(o):
            # what each arm leaves that the rest of the case can use: owned
            # values (a virtual object is an owned object there) and inputs
            return {n: 'object' if k_ in ('virtual', 'owned') else k_ for n, k_ in o.items()
                    if k_ in ('input', 'object', 'owned', 'virtual', 'int', 'cond',
                              'selected', 'bool')}
        if any(held(o) != held(live[0]) for o in live):
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

    # ---- conditions held as condition codes, and type tests (D82) ----

    def new_cc():
        cc = 'cc%d' % sum(1 for d in decls if d.startswith('condition_code_t'))
        decls.append('condition_code_t %s;' % cc)
        return cc

    def make_cond(x, call):
        """x = the condition code of call (P05), tested for CC_ERROR."""
        nonlocal last_fail
        if first_commit is not None:
            raise Skip('a condition after an irreversible statement (restart rule)')
        cc = new_cc()
        body.append('%s = %s;' % (cc, call))
        body.append('if (%s == CC_ERROR)' % cc)
        body.append('  ' + error_code('error'))
        last_fail = len(body)
        var[x] = 'cond'
        cond_of[x] = (cc, len(calls))
        return cc

    def as_cond(x):
        """The condition code of x, a C truth value: a 'cond', or a
        run-time int tested against 0 here (gen_abstract.int_condition)."""
        nonlocal last_fail
        if var.get(x) == 'int':
            call, inv = gen_abstract.int_condition('!=', x, '0', True, x in nonneg)
            cc = new_cc()
            if first_commit is not None:
                raise Skip('a condition after an irreversible statement (restart rule)')
            body.append('%s = %s;' % (cc, call))
            body.append('vinfo_decref(%s, po);' % x)       # its last use
            body.append('if (%s == CC_ERROR)' % cc)
            body.append('  ' + error_code('error'))
            last_fail = len(body)
            if inv:
                body.append('%s = INVERT_CC(%s);' % (cc, cc))
            var[x] = 'cond'
            cond_of[x] = (cc, len(calls))
        if var.get(x) != 'cond':
            raise Skip('%s is not a condition (%s)' % (x, var.get(x)))
        cc, at = cond_of[x]
        if at != len(calls):
            # a condition code is used before any other code is emitted
            raise Skip('the condition %s held across a call' % x)
        var[x] = 'consumed'
        return cc

    def fallback(why):
        """Where a type test needs what Ni doesn't know: the innermost
        resumable arm runs at run time instead, if nothing was emitted in
        it yet (its run-time copy, ceval_inline.Inliner.resumable)."""
        if not regions:
            raise Skip('%s, outside a resumable arm (policy promote known_only)' % why)
        r = regions[-1]
        if len(calls) != r['calls']:
            raise Skip('%s after a call in a resumable arm' % why)
        r['used'] = True
        return 'goto %s;' % r['label']

    def object_var(e):
        while e[0] == 'cast':
            e = e[2]
        if e[0] == 'id' and var.get(e[1]) in OBJECT_KINDS:
            return e[1]
        return None

    def type_test(cond):
        """The compiler's C for a type test of run-time values (Py*_Check
        macros, expanded as the source compiles them), with the lines
        that find what it needs: Psyco_KnownType of an object whose type
        it reads, the constant of one it reads as a type object. Never
        promotes (policy promote known_only); unknown, the arm resumes at
        run time (fallback). None if cond isn't a type test."""
        toks = ctokens.tokenize(cond)[0]
        if ctx.macros is None or not any(
                t.kind == 'id' and ceval_inline.TYPE_TEST.match(t.text) for t in toks):
            return None
        try:
            e = cparse.parse_expr(ctx.expand(toks), ctx.types)
        except cparse.ParseError as err:
            raise Skip('a type test %s: %s' % (cond, err))
        pre, types_ = [], {}
        # ('shortcut', 'specialise_only') on the case: the test's arm is a
        # shortcut of the other, so a type Ni doesn't know makes the test
        # false and the generic arm runs (rule type_test_generic)
        generic = ('shortcut', 'specialise_only') in entry_policy and not regions
        if generic and (_has(e, lambda n: n[0] == 'un' and n[1] == '!') or
                     _has(e, lambda n: n[0] == 'bin' and n[1] == '||')):
            raise Skip('a promoted type test %s that is not positive' % cond)

        def type_of(x):
            if x not in types_:
                t = 'tp%d' % sum(1 for d in decls if d.startswith('PyTypeObject *'))
                decls.append('PyTypeObject *%s;' % t)
                if generic:
                    pre.append('%s = Psyco_KnownType(%s);' % (t, x))
                else:
                    pre.append('%s = Psyco_KnownType(%s);' % (t, x))
                    pre.append('if (%s == NULL) %s' % (t, fallback('the type of %s' % x)))
                types_[x] = t
            return types_[x]

        def ev(e):
            k = e[0]
            if k == 'arrow':
                x = object_var(e[1])
                if e[2] == 'ob_type' and x:
                    return type_of(x)
                inner = e[1]
                if inner[0] == 'cast' and inner[1].replace(' ', '') == 'PyTypeObject*' \
                        and object_var(inner[2]):
                    # the object read as a type: a constant's own fields
                    x = object_var(inner[2])
                    pre.append('if (!is_compiletime(%s->source)) %s' % (
                        x, fallback('%s as a type' % x)))
                    return '((PyTypeObject *)CompileTime_Get(%s->source)->value)->%s' % (
                        x, e[2])
                return '%s->%s' % (ev(inner), e[2])
            if k == 'cast':
                return '((%s)%s)' % (e[1], ev(e[2]))
            if k == 'num':
                return e[1]
            if k == 'bin':
                return '(%s %s %s)' % (ev(e[2]), e[1], ev(e[3]))
            if k == 'un':
                return '%s(%s)' % (e[1], ev(e[2]))
            if k == 'id' and in_kind.get(e[1]) == 'cond':
                # a conditional input is NULL exactly when it is absent:
                # its vinfo pointer (a stack value is never NULL)
                return e[1]
            if k == 'id' and e[1] not in var and e[1] not in in_kind and \
                    re.fullmatch(r'_?Py\w+_Type', e[1]):
                return e[1]      # a type object of the source
            raise Skip('a type test reading %s' % cparse.c_expr(e))
        text = ev(e)
        if generic:
            text = '(%s)' % ' && '.join(['%s != NULL' % t for t in types_.values()] + [text])
        for line in pre:
            body.append(line)
        # the test that holds on each arm, for the accessors (known length)
        m = re.fullmatch(r'(!? ?)(\w+) \( (\w+) \)', cond)
        fact = (None, None)
        if m and ceval_inline.TYPE_TEST.match(m.group(2)):
            fact = ((m.group(2), m.group(3)), None) if not m.group(1) else \
                (None, (m.group(2), m.group(3)))
        return text, fact

    def length_of(fn, x):
        """'n = F(x)' where F is a checked accessor of the size of a type
        Ni knows the length of (policy TYPES known_length), and this path
        tested x's type: Ni's length, or the fallback."""
        acc = ctx.accessor(fn)
        if acc is None:
            return None
        test, field = acc
        if (test, x) not in facts:
            return None
        kl = ctx.known_length(test)
        if kl is None or kl['size'] != field:
            return None
        return kl

    def field_null(stmt, x):
        """'if (x == NULL) { ...; ERROR_IF(true, L); }', x a field value:
        a run-time test, the error arm the unlikely one."""
        nonlocal last_fail
        _, then, other = if_parts(stmt)
        if other or not then or not re.fullmatch(
                r'ERROR_IF\(true,\w+\)', words(then[-1]).replace(' ', '')):
            raise Skip('a test of the field value %s that is not an error exit' % x)
        if first_commit is not None:
            raise Skip('a run-time test after an irreversible statement (restart rule)')
        cc = new_cc()
        call, inv = gen_abstract.int_condition('!=', x, '0', True)
        body.append('%s = %s;' % (cc, call))
        body.append('if (%s == CC_ERROR) {' % cc)
        body.append('  vinfo_decref(%s, po);' % x)
        body.append('  ' + error_code('error'))
        body.append('}')
        if inv:
            body.append('%s = INVERT_CC(%s);' % (cc, cc))
        last_fail = len(body)
        body.append('if (!runtime_condition_t(po, %s)) {' % cc)
        body.indent += 1
        body.append('vinfo_decref(%s, po);' % x)
        before = dict(var)
        var[x] = 'consumed'
        if not run_block(then):
            raise Skip('an error block that does not end')
        body.indent -= 1
        body.append('}')
        var.clear()
        var.update(before)
        nonnull.add(x)
        return False

    pre_calls = []    # [(variable, fn, args)]: temporaries made inside a wrapper

    def switch_chain(stmt, rest):
        """'switch (V) { case L: A; break; ... default: D; break; }' and the
        statements after it: an if-chain on V, each arm followed by those
        statements (rule switch_arms, as ceval_inline does in a helper),
        since each arm may set what they read (a function pointer)."""
        close = ctokens.match_close(stmt, 1)
        value = words(stmt[2:close])
        if ct_node(value) is None:
            raise Skip('a switch on %s' % value)
        arms, labels, cur, k = [], [], None, close + 2
        toks = stmt[:-1]
        depth = 0
        while k < len(toks):
            t = toks[k]
            depth += t.text in '([{'
            depth -= t.text in ')]}'
            if depth == 0 and t.text in ('case', 'default'):
                j = k + 1
                while toks[j].text != ':':
                    j += 1
                if cur:
                    arms.append((labels, cur))
                    labels = []
                labels = labels + ['default' if t.text == 'default' else words(toks[k + 1:j])]
                cur = []
                k = j + 1
                continue
            cur.append(t)
            k += 1
        if cur:
            arms.append((labels, cur))
        tail = ' '.join(words(st_) + ('' if words(st_).endswith('}') else ' ;')
                        for st_ in rest)
        chain, default = [], None
        for labs, toks_ in arms:
            sts = split_statements(toks_)
            if not sts or words(sts[-1]) != 'break':
                raise Skip('a switch arm that does not end in break')
            text = '{ %s %s }' % (' '.join(words(st_) + ('' if words(st_).endswith('}')
                                                         else ' ;') for st_ in sts[:-1]), tail)
            if 'default' in labs:
                default = text
            else:
                chain.append('if ( %s ) %s' % (' || '.join('%s == %s' % (value, l_)
                                                         for l_ in labs), text))
        out = ' else '.join(chain)
        out += ' else %s' % (default if default is not None else '{ %s }' % tail)
        return ctokens.tokenize(out)[0]

    def temp_arg(x, fn, args):
        """'x = F(constants); ERROR_IF(x == NULL, L); y = G(.. x ..);
        Py_DECREF(x);' with G residual too and x read nowhere else: x is a
        temporary of G's call, made inside G's wrapper (rule
        residual_wrapper). The three statements are taken from the block
        and G's call made in place of F's."""
        if pre_calls or any(ct_node(a) is None or ct_value(ct_node(a)) is None for a in args):
            return False
        st = rests[-1]
        if len(st) < 3 or words(st[0]).replace(' ', '') != 'ERROR_IF(%s==NULL,error)' % x \
                or words(st[2]) != 'Py_DECREF ( %s )' % x:
            return False
        m = re.fullmatch(r'(\w+) = (_?Py[A-Z]\w*) \( (.*) \)', words(st[1]))
        if not m or m.group(2) not in csigs or x not in m.group(3).split(' '):
            return False
        try:
            counterpart(m.group(2), csigs, psigs)
            return False
        except NoCounterpart:
            pass
        if any(x in [t.text for t in s_] for s_ in st[3:]) or \
                any(x in [t.text for t in s_] for r in rests[:-1] for s_ in r):
            return False
        y, g = m.group(1), m.group(2)
        gargs = call_args(st[1], 3)[0]
        del st[:3]
        pre_calls.append((x, fn, [tidy(a) for a in args]))
        if y not in var and y not in in_names:
            pending.discard(y)
        residual(y, g, gargs)
        return True

    def wrapped(x, fn, args, ctype, conv, key):
        """A residual call through a wrapper written into the copies (rule
        residual_wrapper): the temporaries pre_calls holds are made in it,
        and an input array with its count (C's 'values, oparg') is passed
        as up to ARRAY_ARGS_MAX words, the rest NULL, or past that a tuple
        of the items (the TARGET's ('limit', 'max_args'))."""
        nonlocal last_fail
        pre = list(pre_calls)
        pre_calls.clear()
        tmps = {t for t, _, _ in pre}
        params, cargs, ops, fmt = [], [], [], ''
        arr = None
        k = 0
        while k < len(args):
            a = args[k]
            if a in tmps:
                cargs.append(a)
            elif in_kind.get(a) == 'array':
                size = next(sz for n_, kd, sz in inputs if n_ == a)
                if arr is not None or k + 1 >= len(args) or \
                        tidy(args[k + 1]).replace(' ', '') != size.text().replace(' ', ''):
                    raise Skip('the array %s passed to %s without its count' % (a, fn))
                if ('limit', 'max_args') not in entry_policy:
                    raise Skip("the array %s passed to %s, with no ('limit', 'max_args')"
                               % (a, fn))
                arr = (a, size.text())
                cargs += ['_items', '_n']
                k += 2
                continue
            elif a == 'NULL':
                cargs.append('NULL')
            elif ct_node(a) is not None:
                v = ct_value(ct_node(a))
                if v is not None:
                    cargs.append(str(v))
                else:
                    params.append(('long', 'p%d' % len(params)))
                    cargs.append('(Py_ssize_t)' + params[-1][1])
                    fmt, o = fmt + 'l', '(long)(%s)' % tidy(a)
                    ops.append(o)
            else:
                params.append(('PyObject *', 'p%d' % len(params)))
                cargs.append(params[-1][1])
                fmt += 'v'
                ops.append(operand(a))
            k += 1
        nmax = policy_table.LIMITS['ARRAY_ARGS_MAX'][1]
        if arr is not None:
            params += [('long', '_count')] + [('PyObject *', '_a%d' % i) for i in range(nmax)]
        name = 'ceval_%s_%d' % (inst.name, len(wrappers))
        bad = 'NULL' if ctype == 'PyObject *' else '-1'
        lines = []
        for t, f, fa in pre:
            lines.append('PyObject *%s = %s(%s);' % (t, f, ', '.join(fa)))
            lines.append('if (%s == NULL) return %s;' % (t, bad))
        if arr is not None:
            lines.append('PyObject *_words[%d] = {%s};' % (
                nmax, ', '.join('_a%d' % i for i in range(nmax))))
            lines.append('PyObject **_items = _words;')
            lines.append('Py_ssize_t _n = _count;')
            lines.append('if (_count < 0) {  /* a tuple of the items */')
            lines.append('  _items = &PyTuple_GET_ITEM(_a0, 0);')
            lines.append('  _n = PyTuple_GET_SIZE(_a0);')
            lines.append('}')
        lines.append('%s_r = %s(%s);' % (ctype if ctype.endswith('*') else ctype + ' ',
                                         fn, ', '.join(cargs)))
        for t, _, _ in pre:
            lines.append('Py_DECREF(%s);' % t)
        lines.append('return _r;')
        head = 'static %s%s%s(%s)' % (ctype, '' if ctype.endswith('*') else ' ', name,
                                      ', '.join('%s%s%s' % (t, '' if t.endswith('*') else ' ', n)
                                                for t, n in params))
        wrappers[name] = ctype
        copies.append((name, '%s {\n  %s\n}' % (head, '\n  '.join(lines))))
        calls.append(name)
        tr = gen_abstract.Translator(None, {}, {}, effects)
        keys = [key] + [effects.globals.get(f) if effects is not None else None
                        for _, f, _ in pre]
        flags = tr.flags(ctype, conv, None)
        common = None
        for kk in keys:
            ef = set(tr.effect_flags(kk))
            common = ef if common is None else common & ef
        flags += ''.join(' | ' + f for f in ['CfPure', 'CfNoPython', 'CfNoBlock']
                         if f in (common or ()))
        declare(x, 'vinfo_t *')
        var[x] = 'int' if conv in ('neg', None) else 'object'
        if arr is None:
            body.append('%s = psyco_generic_call(po, %s, %s, "%s"%s);' % (
                x, name, flags, fmt, ''.join(', ' + o for o in ops)))
        else:
            a, size = arr
            z = temp(CONSTANTS['NULL'])
            words_ = ', '.join('%s > %d ? %s[%d] : %s' % (size, i, a, i, z)
                               for i in range(nmax))
            body.append('if (%s <= ARRAY_ARGS_MAX)' % size)
            body.append('  %s = psyco_generic_call(po, %s, %s, "%sl%s"%s, (long)(%s), %s);' % (
                x, name, flags, fmt, 'v' * nmax, ''.join(', ' + o for o in ops), size, words_))
            body.append('else {')
            body.append('  vinfo_t *_t = PsycoTuple_New(%s, %s);' % (size, a))
            body.append('  %s = psyco_generic_call(po, %s, %s, "%slv%s"%s, -1L, _t%s);' % (
                x, name, flags, fmt, 'l' * (nmax - 1), ''.join(', ' + o for o in ops),
                ', 0L' * (nmax - 1)))
            body.append('  vinfo_decref(_t, po);')
            body.append('}')
        for t in temps:
            body.append('vinfo_decref(%s, po);' % t)
        temps.clear()
        last_fail = len(body)
        if first_commit is not None:
            raise Skip('call after an irreversible statement')

    def error_handler(fn, args_text, arms, test):
        """'if (F(args) OP V) { H; releases; ERROR_IF(true, L); }' where H
        handles the exception F raised (tests it, replaces its message):
        H runs at run time, in a wrapper of the call written into the
        copies (rule error_handler), which returns F's result; the case
        calls the wrapper and keeps the releases and the exit. (call text,
        the arms' tokens left)."""
        _, then, other = if_parts(ctokens.tokenize('if ( x )')[0] + arms)
        k = len(then)
        while k and (words(then[k - 1]).replace(' ', '').startswith('ERROR_IF(true,')
                     or re.fullmatch(r'Py_X?DECREF \( \w+ \)', words(then[k - 1]))):
            k -= 1
        handler = then[:k]
        if other or not handler or k == len(then):
            return '%s %s' % (fn, args_text), arms
        params, body = [], []
        local = set()
        cps = []
        # an item of an input array the call takes (values[oparg - i] in a
        # loop) is a parameter of the wrapper, in the handler too
        cargs = call_args(ctokens.tokenize(args_text)[0], 0)[0]
        items = {}
        for a in cargs:
            m_ = re.fullmatch(r'(\w+) \[ .+ \]', a)
            if m_ and in_kind.get(m_.group(1)) == 'array' and a not in items:
                items[a] = 'item%d' % len(items)
        if items:
            texts_ = []
            for st in handler:
                t_ = words(st)
                for a, p_ in items.items():
                    t_ = t_.replace(a, p_)
                texts_.append(ctokens.tokenize(t_)[0])
            handler = texts_
        for st in handler:
            # the locals the handler declares (PyObject *exc, *val, *tb)
            ws = [t_.text for t_ in ctokens.tokenize(words(st))[0]]
            for j in range(len(ws) - 1):
                if ws[j] in ('*', ',') and re.fullmatch(r'\w+', ws[j + 1]) and \
                        (ws[0] == 'PyObject' or (ws[:1] == ['{'] and 'PyObject' in ws)):
                    local.add(ws[j + 1])
        text = []
        for st in handler:
            # 3.8's _PyErr_X(tstate, ...) is PyErr_X(...) at run time; any
            # other tstate is the interpreter's, which the copy doesn't have
            # (a helper's is helper_copy's to refuse)
            ws = [t_.text for t_ in ceval_inline.untstate(
                ctokens.tokenize(words(st))[0], csigs)]
            if any(x_ == 'tstate' and not (j > 1 and ws[j - 1] == '('
                                           and ws[j - 2] in ctx.helpers)
                   for j, x_ in enumerate(ws)):
                raise Skip('an error handler reading tstate')
            for j, x_ in enumerate(ws):
                prev = ws[j - 1] if j else ''
                if not re.fullmatch(r'[A-Za-z_]\w*', x_) or prev in ('.', '->'):
                    continue
                if x_ in var and x_ not in local:
                    if var[x_] not in OBJECT_KINDS:
                        raise Skip('an error handler reading %s (%s)' % (x_, var[x_]))
                    if x_ not in params:
                        params.append(x_)
                elif x_ in ctx.helpers and j + 1 < len(ws) and ws[j + 1] == '(':
                    cps += ctx.helper_copy(x_, 'an error handler calling %s' % x_)
                    ws[j] = 'ceval_' + x_
                elif x_ in ctx.strings:
                    ws[j] = ctx.strings[x_]
                elif x_ in ctx.ceval_macros or x_ in in_names:
                    raise Skip('an error handler reading %s' % x_)
            text.append(' '.join(ws) + ('' if ws[-1] == '}' else ' ;'))
        # a pointer cast of a variable ('(PyListObject *) sum') stays in the
        # wrapper; the variable is its parameter
        bare = [re.sub(r'^\( \w+ \* \) (\w+)$', r'\1', a) for a in cargs]
        for a in bare:
            if a in var:
                if var[a] not in OBJECT_KINDS:
                    raise Skip('an error handler around %s of %s' % (fn, a))
            elif a not in items and not re.fullmatch(r'-?\d+|NULL', a):
                raise Skip('an error handler around %s ( %s )' % (fn, a))
        ps = [a for a in bare if a in var or a in items]
        ps += [p_ for p_ in params if p_ not in ps]
        ret = {'object': 'PyObject *', 'int': 'int'}.get(csigs[fn][0])
        if ret is None:
            raise Skip('an error handler around %s returning %s' % (fn, csigs[fn][0]))
        name = 'ceval_%s_%d' % (inst.name, len(wrappers))
        head = 'static %s%s%s(%s)' % (ret, '' if ret.endswith('*') else ' ', name,
                                      ', '.join('PyObject *%s' % items.get(p_, p_)
                                                for p_ in ps))
        code = '%s {\n  %s r = %s ( %s );\n  if (r %s) {\n    %s\n  }\n  return r;\n}' % (
            head, ret, fn, ', '.join(items.get(a, a) for a in cargs), test,
            tidy(' '.join(text)))
        wrappers[name] = ret
        copies.extend(cps + [(name, code)])
        calls.extend(n for n, _ in cps)
        rest = ' '.join(words(st) + ('' if words(st).endswith('}') else ' ;')
                        for st in then[k:])
        return '%s ( %s )' % (name, ', '.join(ps)), ctokens.tokenize('{ %s }' % rest)[0]

    def field_exit():
        """An error exit where the case holds field values: their
        vinfos released first (Ni's own references, not the C's)."""
        held = [n for n, k in var.items() if k == 'field']
        code = error_code('error')
        if held:
            code = '{ %s%s }' % (''.join('vinfo_decref(%s, po); ' % n for n in held), code)
        return code

    def field_store(fs):
        """'M(y, v)' storing v into a mutable field of y (PyCell_SET):
        psyco_put_field with Ni's descriptor; the field takes the
        reference v held (the C doesn't incref it), so Ni moves its own
        (consume_reference) and v is released at the vinfo level."""
        nonlocal last_fail
        y, desc, v = fs
        if first_commit is not None:
            raise Skip('a field store after an irreversible statement (restart rule)')
        if v == 'NULL':
            val = temp(CONSTANTS['NULL'])
        elif var.get(v) in ('input', 'object', 'owned', 'virtual') and \
                in_kind.get(v, 'scalar') == 'scalar':
            val = v
        else:
            raise Skip('a field store of %s (%s)' % (v, var.get(v)))
        ok = 'st%d' % sum(1 for d in decls if d.startswith('bool st'))
        decls.append('bool %s;' % ok)
        body.append('%s = psyco_put_field(po, %s, %s, %s);' % (ok, y, desc, val))
        for t in temps:
            body.append('vinfo_decref(%s, po);' % t)
        temps.clear()
        body.append('if (!%s)' % ok)
        body.append('  ' + field_exit())
        last_fail = len(body)
        if v != 'NULL':
            commit()
            body.append('consume_reference(po, %s);' % v)
            if var[v] == 'input':
                released.add(v)
            else:
                body.append('vinfo_decref(%s, po);' % v)
                var[v] = 'consumed'

    def resumable(stmt, cond):
        """'if (NI_RESUME(copy, x, args...)) { arm }' (ceval_inline): the
        arm translated; where its type tests need what Ni doesn't know, a
        jump to a residual call of its run-time copy, which gives x (a C
        truth value, or -1 for an error)."""
        nonlocal last_fail
        args = call_args(ctokens.tokenize(cond)[0], 1)[0]
        name, x, params = args[0], args[1], args[2:]
        _, then, other = if_parts(stmt)
        if other:
            raise Skip('a resumable arm with an else')
        r = {'label': 'resume_%d' % len(resume_labels), 'calls': len(calls), 'used': False}
        resume_labels.append(r['label'])
        regions.append(r)
        ended = run_block(then)
        regions.pop()
        if ended:
            raise Skip('a resumable arm that always fails')
        if var.get(x) != 'int':
            raise Skip('a resumable arm leaving %s as %s' % (x, var.get(x)))
        if r['used']:
            if first_commit is not None:
                raise Skip('a run-time copy after an irreversible statement')
            ops = [operand(p) for p in params]
            body.append('if (0) {')
            body.append(' %s:' % r['label'])
            body.append('  %s = psyco_generic_call(po, %s, CfCommonIntBoolOrError, "%s"%s);'
                        % (x, name, 'v' * len(ops), ''.join(', ' + o for o in ops)))
            body.append('  if (%s == NULL)' % x)
            body.append('    ' + error_code('error'))
            body.append('}')
            calls.append(name)
            last_fail = len(body)

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
            if len(w) == 4 and w[0] in ('Py_DECREF', 'Py_XDECREF') and \
                    var.get(w[2]) == 'field':
                # a value read at run time, which Ni holds no reference
                # to, released by the C (the old content of a field it has
                # overwritten): an emitted decref of the word
                x = w[2]
                xd = w[0] == 'Py_XDECREF' and x not in nonnull
                commit()
                body.append('psyco_%s_v(po, %s);' % ('xdecref' if xd else 'decref', x))
                body.append('vinfo_decref(%s, po);' % x)
                var[x] = 'consumed'
                continue
            fs = w[0] != 'ERROR_IF' and ctx.field_store(s, var)
            if fs:
                field_store(fs)
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
            m = re.fullmatch(r'PyObject \* \( \* (\w+) \) \( [^()]* \)', s)
            if m:
                # a pointer to a C function: the generator knows which one
                # on each path, from its assignment (rule switch_arms)
                var[m.group(1)] = 'fnptr'
                fnval[m.group(1)] = None
                continue
            if w[0] == 'switch':
                stmts[:] = [switch_chain(stmt, stmts)]
                continue
            m = re.fullmatch(r'(\w+) = (\w+)', s)
            if m and var.get(m.group(1)) == 'fnptr':
                if m.group(2) != 'NULL' and m.group(2) not in csigs:
                    raise Skip('the function pointer %s set to %s' % m.groups())
                fnval[m.group(1)] = m.group(2)
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
                elif var.get(x) == 'field':
                    if ('ref', 'need_reference') not in entry_policy:
                        raise Skip("Py_INCREF of the field value %s, with no policy"
                                   " ('ref', 'need_reference')" % x)
                    body.append('need_reference(po, %s);' % x)
                    var[x] = 'owned'
                elif var.get(x) == 'selected':
                    # the object a condition chose, owned: Psyco's
                    # constructor of it (rule bool_select, P23)
                    make, cc = sel_of.pop(x)
                    del var[x]
                    declare(x, 'vinfo_t *')
                    body.append('%s = %s(po, %s);' % (x, make, cc))
                    var[x] = 'virtual'
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
                m = re.fullmatch(r'(\w+) (==|!=) NULL', cond)
                if m and var.get(m.group(1)) == 'fnptr':
                    f = fnval[m.group(1)]
                    if f is None:
                        raise Skip('the function pointer %s unknown here' % m.group(1))
                    _, then, other = if_parts(stmt)
                    if run_block(then if (f != 'NULL') == (m.group(2) == '!=') else other):
                        if stmts:
                            raise Skip('code after an error exit')
                        return True
                    continue
                if cond.startswith(ceval_inline.RESUME + ' ('):
                    resumable(stmt, cond)
                    continue
                node = ct_node(cond)
                if node is not None:
                    if ct_if(stmt, node):
                        if stmts:
                            raise Skip('code after an error exit')
                        return True
                    continue
                m = re.fullmatch(r'(\w+) == NULL', cond)
                if m and var.get(m.group(1)) == 'field':
                    if field_null(stmt, m.group(1)):
                        if stmts:
                            raise Skip('code after an error exit')
                        return True
                    continue
                tt = type_test(cond)
                if tt is not None:
                    if ct_if(stmt, None, tt[0], tt[1]):
                        if stmts:
                            raise Skip('code after an error exit')
                        return True
                    continue
                m = re.fullmatch(r'(_?Py[A-Z]\w*) (\( .* \)) (<|==|!=) (.+)', cond)
                if m and m.group(1) in csigs:
                    # the call in an error test is made first, into a
                    # temporary the test then reads
                    t = '_r%d' % len(decls)
                    k = call_args(stmt, 1)[1]
                    call = error_handler(m.group(1), m.group(2), stmt[k:],
                                         '%s %s' % (m.group(3), m.group(4)))
                    pre = ctokens.tokenize('int %s = %s ;' % (t, call[0]))[0]
                    test = ctokens.tokenize('if ( %s %s %s )' % (t, m.group(3), m.group(4)))[0]
                    stmts[:0] = [pre[:-1], test + call[1]]
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
                            if o in ('object', 'owned', 'virtual', 'int', 'field')]
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
                        if o in ('object', 'owned', 'virtual', 'int', 'field')
                        and t not in cond[1]]
                if live:
                    raise Skip('ERROR_IF with %s owned' % ', '.join(live))
                body.append('if (%s)' % cond[0])
                body.append('  ' + error_code(args[1]))
                last_fail = len(body)
                continue
            if w[0] in ctx.helpers and w[1] == '(' and stmts and \
                    words(stmts[0]).replace(' ', '').startswith('ERROR_IF(true,'):
                # a helper of ceval.c that sets the error: its compile-time
                # copy (rule compile_time_copy)
                args, _ = call_args(stmt, 1)
                cps = ctx.ct_copy(w[0])
                fn, ps = cps[0][0], ctx.ct_params(w[0])
                copies.extend(cps)
                calls.extend(n for n, _ in cps)
                args = [a for a, (t, _) in zip(args, ctx.helpers[w[0]].params)
                        if t.replace(' ', '') != 'PyThreadState*']
                if len(args) != len(ps) or any(
                        a not in CODE_NAMES and ct_node(a) is None for a in args):
                    raise Skip('%s ( %s )' % (w[0], ', '.join(args)))
                body.append('%s(po%s);' % (fn, ''.join(', ' + tidy(a) for a in args)))
                continue
            if w[:2] == ['PyErr_SetString', '('] and stmts and \
                    words(stmts[0]).replace(' ', '').startswith('ERROR_IF(true,'):
                # an error the compiler finds: raised at compile time (P24)
                args, _ = call_args(stmt, 1)
                if len(args) != 2 or not re.fullmatch(r'PyExc_\w+', args[0]) or \
                        not args[1].startswith('"'):
                    raise Skip(s)
                body.append('PycException_SetString(po, %s, %s);' % tuple(args))
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
                if len(w) > 3 and var.get(w[2]) == 'fnptr' and w[3] == '(':
                    # a call through a pointer the generator knows: that
                    # function
                    if not fnval[w[2]] or fnval[w[2]] == 'NULL':
                        raise Skip('a call through %s' % w[2])
                    stmts.insert(0, ctokens.tokenize(' '.join(
                        ([ctype] if ctype else []) + [x, '='] + [fnval[w[2]]] + w[3:]))[0])
                    continue
                if len(w) == 3 and x in in_names and in_kind[x] == 'scalar' and \
                        x in released and not ctype and \
                        var.get(w[2]) in ('object', 'owned', 'virtual'):
                    # an input assigned again after its release: a new
                    # local from here (the C reuses the name)
                    new_ = '%s_%d' % (x, sum(1 for n_ in var if n_.startswith(x + '_')) + 1)
                    for r in rests:
                        r[:] = [[t._replace(text=new_) if t.kind == 'id' and t.text == x
                                 else t for t in st_] for st_ in r]
                    stmts.insert(0, ctokens.tokenize('%s = %s' % (new_, w[2]))[0])
                    continue
                if len(w) == 3 and var.get(w[2]) == 'input' and in_kind[w[2]] == 'scalar' \
                        and not ctype and x not in in_names and w[2] not in released:
                    # an input moves to x (C's 'result = value'): Ni's own
                    # reference goes with it, and the input slot is released
                    declare(x, 'vinfo_t *')
                    commit()
                    body.append('vinfo_incref(%s);' % w[2])
                    body.append('%s = %s;' % (x, w[2]))
                    var[x] = 'owned'
                    released.add(w[2])
                    continue
                m = re.fullmatch(r'(_?Py[A-Z]\w*) (\( .* \))', ' '.join(w[2:]))
                if m and m.group(1) in csigs and stmts and \
                        words(stmts[0][:6]) == 'if ( %s == NULL )' % x:
                    # the call's result tested next, with a handler of its
                    # error: a wrapper as for a call in the test (rule
                    # error_handler)
                    call = error_handler(m.group(1), m.group(2), stmts[0][6:], '== NULL')
                    if call[0] != '%s %s' % (m.group(1), m.group(2)):
                        stmts[0] = stmts[0][:6] + call[1]
                        w = w[:2] + [t_.text for t_ in ctokens.tokenize(call[0])[0]]
                        stmt = stmt[:2] + ctokens.tokenize(call[0])[0]
                if len(w) > 3 and w[2] in wrappers and w[3] == '(':
                    # a call with its error handler, at run time (rule
                    # error_handler)
                    args, end = call_args(stmt, 3)
                    residual(x, w[2], args, ret=wrappers[w[2]], key=None)
                    continue
                if len(w) > 3 and w[2] in ctx.helpers and w[3] == '(' and not ctype:
                    # a static helper of ceval.c: its body in place (D82)
                    args, end = call_args(stmt, 3)
                    if end != len(stmt):
                        raise Skip(s)
                    try:
                        inl = ceval_inline.Inliner(
                            ctx.helpers[w[2]], args, x, stmts, set(in_names), ctx.strings,
                            ctx.known_only(w[2]), csigs)
                        stmts[:] = inl.run()
                    except ceval_inline.Refused as e:
                        # no compile-time form: a residual call of the
                        # helper's run-time copy (rule helper_copy)
                        cp = ctx.helper_copy(w[2], '%s: %s' % (w[2], e))
                        residual(x, cp[0][0], args, ret=ctx.helpers[w[2]].ret,
                                 key=ctx.helper_key(w[2], effects))
                        copies.extend(cp)
                        calls.extend(n for n, _ in cp[1:])
                        continue
                    copies.extend(inl.copies)
                    continue
                fr = not ctype or ctype == 'PyObject *'
                fr = fr and x not in var and ctx.field_read(rhs, var)
                if fr:
                    # a field of an object: Ni's descriptor of it (D76)
                    y, desc = fr[:2]
                    declare(x, 'vinfo_t *')
                    body.append('%s = psyco_get_field(po, %s, %s);' % (x, y, desc))
                    body.append('if (%s == NULL)' % x)
                    body.append('  ' + error_code('error'))
                    last_fail = len(body)
                    var[x] = 'field'
                    continue
                m = re.fullmatch(r'(\( )?(\w+) (==|!=) (\w+)(?(1) \))', rhs)
                if m and ctype in INT_DECLS and x not in var and \
                        var.get(m.group(2)) in OBJECT_KINDS and var.get(m.group(4)) in OBJECT_KINDS:
                    # the identity of two objects: a compare of their words
                    # (rule identity_compare), held as a condition code
                    make_cond(x, 'integer_cmp(po, %s, %s, %s)' % (
                        m.group(2), m.group(4), gen_abstract.PY_OPS[m.group(3)]))
                    continue
                m = re.fullmatch(r'! (\w+)', rhs)
                if m and ctype in INT_DECLS and x not in var and \
                        var.get(m.group(1)) in ('cond', 'int'):
                    cc = as_cond(m.group(1))
                    body.append('%s = INVERT_CC(%s);' % (cc, cc))
                    var[x] = 'cond'
                    cond_of[x] = (cc, len(calls))
                    continue
                m = re.fullmatch(r'(\w+) \? (\w+) : (\w+)', rhs)
                if m and ctype == 'PyObject *' and x not in var and \
                        var.get(m.group(1)) in ('cond', 'int'):
                    # an object chosen by a condition (rule bool_select)
                    pair = (m.group(2), m.group(3))
                    if pair not in SELECTS and pair[::-1] not in SELECTS:
                        raise Skip('a choice between %s and %s' % pair)
                    cc = as_cond(m.group(1))
                    if pair not in SELECTS:
                        body.append('%s = INVERT_CC(%s);' % (cc, cc))
                        pair = pair[::-1]
                    var[x] = 'selected'
                    sel_of[x] = (SELECTS[pair], cc)
                    continue
                m = re.fullmatch(r'(\w+) \( (\w+) \)', rhs)
                kl = m and (ctype in INT_DECLS or var.get(x) == 'ctint') and \
                    var.get(m.group(2)) in OBJECT_KINDS and length_of(m.group(1), m.group(2))
                if kl:
                    # the length of an object whose type this path tested:
                    # Ni's, if it knows it (rule loops' known_length)
                    if var.get(x) != 'ctint':
                        declare(x, ctype + ' ')
                        var[x] = 'ctint'
                    ctint[x] = None
                    y = m.group(2)
                    body.append('%s = %s(%s);' % (x, gen_abstract.ni_short(kl['load']), y))
                    body.append('if (%s < 0) %s' % (x, fallback('the length of %s' % y)))
                    loaded[y] = kl
                    continue
                item = ctype == 'PyObject *' and x not in var and ctx.item(rhs, var, loaded)
                if item:
                    # an item of an object of known length, borrowed
                    y, kl, idx = item
                    if ct_node(idx) is None:
                        raise Skip('item %s of %s' % (idx, y))
                    declare(x, 'vinfo_t *')
                    body.append('%s = %s(%s, %s);' % (x, gen_abstract.ni_short(kl['item']),
                                                      y, tidy(idx)))
                    var[x] = 'local'
                    continue
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
                if len(w) == 3 and var.get(w[2]) in ('object', 'owned', 'virtual') and not ctype:
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
                        body.append('%s = %s(po%s) ? 0 : -1;' % (x, pname, ''.join(', ' + o for o in ops)))
                    elif ret == 'intval':
                        declare(x, 'vinfo_t *')
                        var[x] = 'int'
                        body.append('%s = %s(po%s);' % (x, pname, ''.join(', ' + o for o in ops)))
                    elif ret == 'virtual':
                        declare(x, 'vinfo_t *')
                        var[x] = 'virtual'
                        body.append('%s = %s(%s);' % (x, pname, ', '.join(ops)))
                    elif ret == 'object':
                        declare(x, 'vinfo_t *')
                        var[x] = 'object'
                        body.append('%s = %s(po%s);' % (x, pname, ''.join(', ' + o for o in ops)))
                    else:
                        # C's int (0: done) becomes Psyco's bool (true: done);
                        # the name says which, ERROR_IF tests it as x_ok
                        declare(x + '_ok', 'bool ')
                        var[x] = 'bool'
                        body.append('%s_ok = %s(po%s);' % (x, pname, ''.join(', ' + o for o in ops)))
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
                origin not in ('bool', 'const', 'local', 'consumed', 'ctint', 'elem', 'fnptr'):
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
    lines += concat_shortcut(inst, entry, inputs[keep:], outputs[keep:])
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
    used = [(n, t) for n, t in copies if n in calls]
    return '\n'.join(lines), calls, used


def concat_shortcut(inst, entry, popped, pushed):
    """Policy ('virtual', 'concat', why): where Ni knows the length of
    every item of the case's one popped array as an object of the case's
    'restype' (its policy_table.TYPES entry's 'known_length'), the one
    output is a virtual of that type holding their items in order, made
    by Ni's 'new', and the C does not run (P22: f(*a, *b) and (*a, *b) of
    known tuples)."""
    items = [i for i in entry.get('policy', []) if tuple(i[:2]) == ('virtual', 'concat')]
    if not items:
        return []
    restype = [i[1] for i in entry.get('policy', []) if i[0] == 'restype']
    t = next((t for t in policy_table.TYPES.values()
              if restype and t.get('type') == restype[0]), {})
    kl, virt = t.get('known_length'), t.get('virtual', {})
    if len(items) > 1 or kl is None or 'new' not in virt:
        raise Skip("policy ('virtual', 'concat') with no restype of a known length"
                   " and a virtual constructor")
    if len(popped) != 1 or popped[0][1] != 'array' or len(pushed) != 1 \
            or pushed[0][1] != 'scalar':
        raise Skip("policy ('virtual', 'concat') on a case that does not pop one"
                   " array and push one value")
    load, item, new = (x.split(':')[1] for x in (kl['load'], kl['item'], virt['new']))
    a, size = popped[0][0], popped[0][2].text()
    return ['  { /* %s (policy virtual concat, P22) */' % items[0][2]] + ['    ' + x for x in [
        'int i_, j_, k_, m_ = 0, n_ = 0;',
        'for (i_ = 0; i_ < %s; i_++) {' % size,
        '  k_ = %s(%s[i_]);' % (load, a),
        '  if (k_ < 0)',
        '    break;',
        '  n_ += k_;',
        '}',
        'if (i_ == %s) {' % size,
        '  vinfo_t *v_, **items_ = (vinfo_t **)PyMem_Malloc((n_ + 1) * sizeof(*items_));',
        '  if (items_ == NULL) {',
        '    PycException_SetString(po, PyExc_MemoryError, "");',
        '    break;',
        '  }',
        '  for (i_ = 0; i_ < %s; i_++)' % size,
        '    for (j_ = 0, k_ = %s(%s[i_]); j_ < k_; j_++)' % (load, a),
        '      items_[m_++] = %s(%s[i_], j_);' % (item, a),
        '  v_ = %s(n_, items_);' % new,
        '  PyMem_Free(items_);',
        '  { int n = %s; while (n--) POP_DECREF(); }' % size,
        '  PUSH(v_);',
        '  goto fine;',
        '}']] + ['  }']


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


def run(insts, src, effects=None, ctx=None):
    csigs, psigs = c_signatures(src), psyco_signatures()
    consts = ceval_reader.constants(src)
    frame = frame_arrays(src)
    done, skipped, copies = [], [], []
    for inst in insts:
        try:
            w = [t.text for t in inst.block.tokens]
            sps = {w[k - 1] for k in range(1, len(w) - 1)
                   if w[k] == '=' and w[k + 1] == 'stack_pointer'}
            if any(w[k] == '&' and w[k + 1] in sps for k in range(len(w) - 1)):
                # calls: a copy of the stack pointer handed to the call
                # path, which moves it; that is core
                k = next(k for k in range(2, len(w))
                         if w[k] == '&' and w[k + 1] in sps)
                raise Skip('a copy of the stack pointer handed to %s'
                           % called_at(w, k), 'core')
            code, _, used = translate(inst, csigs, psigs, consts, effects, frame, ctx)
            copies += used
        except Skip as e:
            skipped.append((inst.name, e.kind, str(e)))
            continue
        done.append((inst.name, code))
    if ctx is not None and ctx.src is not None and any(
            name in code for _, code in done for name in
            (lab for _, lab in ceval_reader.ERROR_FORMS)):
        copies = ctx.label_copies() + copies
    return done, skipped, copies


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


class Context:
    """What the back half reads from the source besides the cases (D82):
    ceval.c's static helpers and string macros (ceval_inline), the macros
    as ceval.c is compiled (for type tests), and the checked accessors of
    Objects/ (PyTuple_Size). Empty without a target (gen_ceval.py --try)."""

    def __init__(self, src=None, pyconfig=None, defines=None):
        self.src = src
        self.helpers, self.strings, self.ceval_macros = {}, {}, set()
        self.macros, self.types = None, set()
        self.accessors = {}
        if src is None:
            return
        self.helpers, self.strings = ceval_inline.helpers(src)
        self.ceval_macros = ceval_inline.macro_names(src)
        if pyconfig is not None:
            macros, pptoks = cparse.cpp(src, pyconfig, 'Python/ceval.c', defines)
            self.macros = macros
            self.types = cparse.Decls(pptoks).types | set(TYPE_NAMES)

    def expand(self, toks):
        """toks with macros expanded as ceval.c is compiled, less the
        '((void)(0)),' an assert leaves under NDEBUG (3.8's _PyTuple_CAST):
        it does nothing, and the parser reads no comma operator."""
        out = cparse.expand(toks, self.macros, set())
        noop = '( ( void ) ( 0 ) ) ,'.split()
        k = 0
        while k < len(out):
            if [t.text for t in out[k:k + len(noop)]] == noop:
                del out[k:k + len(noop)]
            else:
                k += 1
        return out

    def field_read(self, rhs, var):
        """(object, Ni's descriptor) when rhs reads one field of an object
        (a macro, expanded) that policy_table.TYPES lists and LAYOUTS lays
        out; the descriptor is Ni's #define of CPY_FIELD(struct, field, ..),
        FMUT where TYPES says the field is mutable."""
        if self.macros is None or not re.fullmatch(r'\w+ \( \w+ \)', rhs):
            return None
        try:
            e = cparse.parse_expr(self.expand(ctokens.tokenize(rhs)[0]), self.types)
        except cparse.ParseError:
            return None
        return self._field_of(e, var)

    def field_store(self, stmt, var):
        """(object, Ni's descriptor, value text) when the statement stores
        into one field of an object (a macro, expanded: PyCell_SET) that
        policy_table.TYPES lists as mutable; the value is a variable or
        NULL."""
        if self.macros is None or not re.fullmatch(r'\w+ \( \w+ , \w+ \)', stmt):
            return None
        try:
            e = cparse.parse_expr(self.expand(ctokens.tokenize(stmt)[0]), self.types)
        except cparse.ParseError:
            return None
        if e[0] != 'assign' or e[1] != '=':
            return None
        fr = self._field_of(e[2], var)
        if fr is None:
            return None
        if not fr[2]:
            raise Skip('a store into the immutable field of %s' % fr[0])
        v = e[3]
        if v == ('id', 'NULL') or (v[0] == 'cast' and v[1].replace(' ', '') == 'void*'
                                   and v[2] == ('num', '0')):
            return fr[0], fr[1], 'NULL'
        if v[0] != 'id':
            return None
        return fr[0], fr[1], v[1]

    def _field_of(self, e, var):
        """(object, descriptor, mutable) for 'arrow' node e reading a field
        of a variable holding an object, as field_read."""
        if e[0] != 'arrow' or e[1][0] != 'cast' or e[1][2][0] != 'id':
            return None
        st, y, fld = e[1][1].replace(' ', '').rstrip('*'), e[1][2][1], e[2]
        if var.get(y) not in OBJECT_KINDS:
            return None
        t = next((t for k, t in policy_table.TYPES.items() if k.endswith(':' + st)), None)
        lay = policy_table.LAYOUTS.get(st, {}).get('fields', [])
        if t is None or fld not in [f for f, _, _ in lay]:
            return None
        mutable = fld in t.get('mutable', ())
        if not mutable and fld not in t.get('immutable', ()):
            return None
        desc = self.descriptors().get((st, fld))
        if desc is None or desc[1] != mutable:
            raise Skip('no descriptor of %s.%s in Ni%s' % (
                st, fld, ' (mutable)' if mutable else ''))
        return y, desc[0], mutable

    def descriptors(self):
        """{(struct, field): (Ni's name, FMUT)} from Ni's sources."""
        if not hasattr(self, '_descs'):
            self._descs = {}
            for path in sorted(glob.glob(os.path.join(ROOT, 'ni', '**', '*.[ch]'),
                                         recursive=True)):
                for m in re.finditer(r'^#define (\w+) (FMUT\()?CPY_FIELD\((\w+), (\w+),',
                                     open(path).read(), re.M):
                    self._descs.setdefault((m.group(3), m.group(4)),
                                           (m.group(1), bool(m.group(2))))
        return self._descs

    def helper_copy(self, name, why):
        """[(copy name, C text)] for the run-time copy of a ceval.c static
        helper that can't be inlined (rule helper_copy): its body as the
        source has it, the helpers it calls copied too, each renamed
        ceval_<name>. Refused (Skip, with why) when a parameter isn't an
        object, or the body reads a macro of ceval.c other than a string,
        or has a preprocessor line."""
        out, todo, seen = [], [name], set()
        lines = self._ceval_lines()
        while todo:
            n = todo.pop(0)
            if n in seen:
                continue
            seen.add(n)
            h = self.helpers[n]
            for ctype, p in h.params:
                if ctype.replace(' ', '') != 'PyObject*':
                    raise Skip('%s (and no run-time copy: it takes %s %s)' % (why, ctype, p))
            if any(lines[k].lstrip().startswith('#') for k in range(
                    h.body[0].line - 1, h.body[-1].line)):
                raise Skip('%s (and no run-time copy: a preprocessor line)' % why)
            w = []
            for k, t in enumerate(h.body):
                if t.kind == 'id' and t.text in self.helpers and k + 1 < len(h.body) \
                        and h.body[k + 1].text == '(':
                    todo.append(t.text)
                    w.append('ceval_' + t.text)
                elif t.kind == 'id' and t.text in self.strings:
                    w.append(self.strings[t.text])
                elif t.kind == 'id' and t.text in self.ceval_macros:
                    raise Skip('%s (and no run-time copy: it reads %s)' % (why, t.text))
                else:
                    w.append(t.text)
            head = 'static %s%sceval_%s(%s)' % (
                h.ret, '' if h.ret.endswith('*') else ' ', n,
                ', '.join('PyObject *%s' % p for _, p in h.params))
            out.append(('ceval_' + n, '%s;\n%s {\n  %s\n}' % (
                head, head, tidy(' '.join(w)))))
        return out

    def _ceval_lines(self):
        if not hasattr(self, '_lines'):
            with open(os.path.join(self.src, 'Python', 'ceval.c'), encoding='latin-1') as f:
                self._lines = f.read().split('\n')
        return self._lines

    def helper_key(self, name, effects):
        """The effects key of a ceval.c static helper."""
        if effects is None:
            return None
        return effects.by_file.get('Python/ceval.c', {}).get(name)

    def ct_params(self, helper):
        """The parameters of a helper's compile-time copy (after po): its
        own, less the thread state."""
        return [(t, p) for t, p in self.helpers[helper].params
                if t.replace(' ', '') != 'PyThreadState*']

    def ct_copy(self, name):
        """[(copy name, C text)] for the compile-time copy of a ceval.c
        static helper that sets an error, run by the compiler with
        arguments it knows (rule compile_time_copy): ct_<name>, taking po
        first, its body as the source has it with CT_ERRORS' calls as
        Ni's compile-time exception (a 3.8 _PyErr_X(tstate, ...) as
        PyErr_X), the helpers it calls as their compile-time copies and
        string macros as their strings. Refused (Skip) for a helper that
        returns a value, or whose body reads another ceval.c macro or has
        a preprocessor line."""
        out, todo, seen = [], [name], set()
        lines = self._ceval_lines()
        while todo:
            n = todo.pop(0)
            if n in seen:
                continue
            seen.add(n)
            h = self.helpers[n]
            if h.ret.strip() != 'void':
                raise Skip('%s: no compile-time copy (it returns %s)' % (n, h.ret))
            if any(lines[k].lstrip().startswith('#') for k in range(
                    h.body[0].line - 1, h.body[-1].line)):
                raise Skip('%s: no compile-time copy (a preprocessor line)' % n)
            b, w, k = h.body, [], 0
            while k < len(b):
                t = b[k]
                call = t.kind == 'id' and k + 1 < len(b) and b[k + 1].text == '('
                if call and t.text.startswith('_PyErr_') and k + 3 < len(b) \
                        and b[k + 2].text == 'tstate' and b[k + 3].text in (',', ')'):
                    # 3.8: _PyErr_X(tstate, ...) is PyErr_X(...)
                    b = b[:k] + [t._replace(text=t.text[1:])] + \
                        b[k + 1:k + 2] + b[k + 3 + (b[k + 3].text == ','):]
                    continue
                if call and (t.text in CT_ERRORS or t.text in self.helpers):
                    if t.text in self.helpers:
                        todo.append(t.text)
                    w += [CT_ERRORS.get(t.text, 'ct_' + t.text), '(', 'po']
                    nxt = b[k + 2]
                    if nxt.text == 'tstate' and t.text in self.helpers:
                        k += 1 + (b[k + 3].text == ',')
                        nxt = b[k + 2]
                    if nxt.text != ')':
                        w.append(',')
                    k += 2
                    continue
                if t.kind == 'id' and t.text in self.strings:
                    w.append(self.strings[t.text])
                elif t.kind == 'id' and t.text in self.ceval_macros:
                    raise Skip('%s: no compile-time copy (it reads %s)' % (n, t.text))
                else:
                    w.append(t.text)
                k += 1
            head = 'static void ct_%s(PsycoObject *po%s)' % (
                n, ''.join(', %s%s%s' % (c, '' if c.endswith('*') else ' ', p)
                           for c, p in self.ct_params(n)))
            out.append(('ct_' + n, '%s;\n%s {\n  %s\n}' % (head, head, tidy(' '.join(w)))))
        return out

    def label_copies(self):
        """[(name, C text)] for the error labels the reader makes of a
        statement (ceval_reader.ERROR_FORMS): the function LABELS calls,
        taking what the case has at compile time (po, co, oparg), whose
        body is that statement of the eval loop, every occurrence the same
        once a thread-state argument is left out (ceval_reader.error_form),
        with its helper's compile-time copy (rule compile_time_copy)."""
        out = []
        with open(os.path.join(self.src, 'Python', 'ceval.c'), encoding='latin-1') as f:
            toks = ctokens.tokenize(f.read(), 'ceval.c')[0]
        # the statements of the cases (the eval loop's), not of the helpers
        fn = ctokens.functions(toks).get('_PyEval_EvalFrameDefault')
        texts = [t.text for t in fn[1]] if fn else []
        for form, label in ceval_reader.ERROR_FORMS:
            found = set()
            for k in range(len(texts) - len(form)):
                if texts[k] == form[0] and texts[k - 1] in (';', '{', '}'):
                    lab, stmt = ceval_reader.error_form(texts[k:texts.index(';', k) + 1])
                    if lab == label:
                        found.add(tuple(stmt))
            if len(found) != 1:
                raise Skip('label %s: %d forms of its statement' % (label, len(found)))
            stmt = list(found.pop())
            helper = stmt[0]
            cps = self.ct_copy(helper)
            names = set(stmt) - set(form)
            for x in names:
                if re.fullmatch(r'[A-Za-z_]\w*', x) and x not in ('co', 'oparg') \
                        and x not in self.strings and not x.startswith('Py') \
                        and not x.startswith('co_'):
                    raise Skip('label %s: its statement reads %s' % (label, x))
            body = ['ct_' + helper, '(', 'po', ','] + [
                self.strings.get(x, x) for x in stmt[2:]]
            head = 'static void %s(PsycoObject *po, PyCodeObject *co, int oparg)' % label
            out += cps + [(label, '%s {\n  %s\n}' % (head, tidy(' '.join(body))))]
        return out

    def known_only(self, helper):
        """The helper's type tests use the types Ni knows, never promote
        (its policy entry's ('promote', 'known_only'))."""
        e = policy_table.ENTRIES.get('Python/ceval.c:%s' % helper, {})
        return ('promote', 'known_only') in [tuple(i[:2]) for i in e.get('policy', [])]

    def known_length(self, test):
        """TYPES' known_length of the type a Py<T>_Check tests (Py<T>_Type)."""
        m = re.fullmatch(r'(Py\w+)_Check', test)
        if not m:
            return None
        for key, t in policy_table.TYPES.items():
            if t.get('type') == m.group(1) + '_Type':
                return gen_abstract.known_length_of(key.split(':')[1] + ' *')
        return None

    def accessor(self, fn):
        """(test, field) when fn's body is 'if (!T(p)) { ...; return -1; }
        [else] return S(p);', S(p) reading field of p: a checked accessor,
        which is that field where T(p) holds."""
        if fn in self.accessors:
            return self.accessors[fn]
        self.accessors[fn] = None
        if self.macros is None or not re.fullmatch(r'Py\w+', fn):
            return None
        d = ceval_reader._definition(self.src, fn)
        if d is None:
            return None
        params, body = d
        ps = ctokens.split_top(params, ',')
        if len(ps) != 1:
            return None
        p = ps[0][-1].text
        w = ' '.join(t.text for t in body)
        m = re.fullmatch(r'if \( ! (\w+) \( %s \) \) \{ [^{}]*return - 1 ; \} '
                         r'(?:else )?return (\w+) \( %s \) ;' % (p, p), w)
        if not m or not ceval_inline.TYPE_TEST.match(m.group(1)):
            return None
        try:
            e = cparse.parse_expr(self.expand(ctokens.tokenize('%s ( x )' % m.group(2))[0]),
                                  self.types)
        except cparse.ParseError:
            return None
        if e[0] != 'arrow':
            return None
        self.accessors[fn] = (m.group(1), e[2])
        return self.accessors[fn]

    def item(self, rhs, var, loaded):
        """(object, known_length, index text) when rhs reads an item of a
        loaded object of known length (a macro reading its items field)."""
        if self.macros is None or not re.fullmatch(r'\w+ \( .* \)', rhs):
            return None
        try:
            e = cparse.parse_expr(self.expand(ctokens.tokenize(rhs)[0]), self.types)
        except cparse.ParseError:
            return None
        if e[0] != 'index' or e[1][0] != 'arrow':
            return None
        o = e[1][1]
        while o[0] == 'cast':
            o = o[2]
        if o[0] != 'id' or o[1] not in loaded or loaded[o[1]]['items'] != e[1][2]:
            return None
        return o[1], loaded[o[1]], cparse.c_expr(e[2])


_RUNS = {}


def generated(target):
    """(done, skipped, copies) for the target, computed once for both
    headers."""
    if target.src in _RUNS:
        return _RUNS[target.src]
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
        g = effects.analyse(target.python, target.src)
    except Exception as e:  # noqa: BLE001 - the build must say why
        raise cslots.GenError('effects.py: %s' % e)
    ctx = Context(target.src, target.pyconfig, gen_abstract.target_defines(target.python))
    done, skipped, copies = run(insts, target.src, g, ctx)
    skipped += refused
    _RUNS[target.src] = done, skipped, copies
    return _RUNS[target.src]


def build(target):
    done, skipped, _ = generated(target)
    return header(done, skipped), (
        ['generated  %s' % name for name, _ in done] +
        ['skipped    %-28s %-13s %s' % x for x in skipped])


def build_copies(target):
    """pycompiler_copies_gen.h: the run-time copies of inlined helpers'
    arms the cases call (ceval_inline.Inliner.resumable), included by
    pycompiler.c before its opcode switch."""
    done, _, copies = generated(target)
    out = ['/* GENERATED by tools/metac/gen_ceval.py from Python/ceval.c of the',
           '   CPython source built for: do not edit. The run-time copies of',
           '   the arms of ceval.c\'s helpers that the generated cases call',
           '   where Ni knows too little at compile time (D82). */', '']
    seen = set()
    for name, text in copies:
        if name not in seen:
            seen.add(name)
            out += [text, '']
    return '\n'.join(out), ['generated  %s' % n for n in sorted(seen)]


def main(argv):
    if len(argv) != 3 or argv[0] != '--try':
        print('usage: gen_ceval.py --try BYTECODES_C CPYTHON_SRC')
        return 2
    insts = [d for d in read_defs(argv[1]) if isinstance(d, dsl.InstDef)]
    done, skipped, _ = run(insts, argv[2])
    for name, _ in done:
        print('translated %s' % name)
    for name, _, why in skipped:
        print('skipped    %-28s %s' % (name, why))
    print('%d of %d translated' % (len(done), len(insts)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
