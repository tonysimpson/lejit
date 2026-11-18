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
import ctokens  # noqa: E402
import cslots  # noqa: E402
import parser as dsl  # noqa: E402  (CPython's Tools/cases_generator/parser.py)

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
CONSTANTS = {'Py_None': 'psyco_vi_None()'}
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
    Include/*.h."""
    sigs = {}
    inc = os.path.join(src, 'Include')
    for h in sorted(os.listdir(inc)):
        if not h.endswith('.h'):
            continue
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
            sigs[name] = ({'PyObject *': 'object', 'int': 'int'}.get(ret, ret), arity)
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
            if (t.kind != 'id' or not t.text.startswith('Psyco')
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


def counterpart(fn, csigs, psigs):
    if fn not in csigs:
        raise Skip('%s: no declaration in Include/' % fn)
    pname = 'Psyco' + fn[2:]
    if pname not in psigs:
        raise Skip('%s: no Psyco counterpart %s' % (fn, pname))
    cret, carity = csigs[fn]
    pret, parity, _ = psigs[pname]
    if carity != parity:
        raise Skip('%s: %d arguments, %s takes %d' % (fn, carity, pname, parity))
    if (cret, pret) not in (('object', 'vinfo'), ('int', 'bool'), ('object', 'virtual')):
        raise Skip('%s returns %s, %s returns %s' % (fn, cret, pname, pret))
    return pname, 'virtual' if pret == 'virtual' else cret


def constructor(fn, psigs):
    """Psyco's counterpart of PyT_New(n) filled from an array: PsycoT_New
    taking (int, vinfo_t **) after po if it has one. The items are new
    references to it (the fact about Ni's constructors this relies on), so
    the array C stole is released after."""
    pname = 'Psyco' + fn[2:]
    if pname not in psigs:
        raise Skip('%s: no Psyco counterpart %s' % (fn, pname))
    kind, _, params = psigs[pname]
    if [p.replace(' ', '') for p in params] != ['int', 'vinfo_t**']:
        raise Skip('%s: %s does not take (count, items)' % (fn, pname))
    return pname, kind


# ---- translation -----------------------------------------------------------

def stack_layout(effects, consts):
    """[(name, kind, size)] for stack effects: kind scalar, cond (size: its
    0/1 atom) or array (size: a Lin in oparg)."""
    env = {'oparg': ceval_reader.Lin.atom('oparg')}
    out = []
    for e in effects:
        if e.type and not e.size:
            raise Skip('stack effect %s has a type' % e.name)
        if e.cond:
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


def translate(inst, csigs, psigs, consts=None):
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
        if kind != 'scalar' or n == 'unused':
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
    # result, owned), 'bool' (a meta's int result), 'owned' (incref'd)
    var = {n: 'input' for n in in_names}
    released = set()          # inputs Py_DECREF'd
    stolen = set()            # inputs stored (SETLOCAL takes the reference)
    decref_all = False
    body = []                 # C lines
    decls = []
    calls = []
    last_fail = -1            # index into body of the last failure point
    first_commit = None       # index into body of the first irreversible act
    temps = []                # constants made for a call: released after it
    const_of = {}             # 'const' variable -> Ni's expression for it

    def commit():
        nonlocal first_commit
        if first_commit is None:
            first_commit = len(body)

    def declare(name, ctype):
        if name in var:
            raise Skip('%s assigned twice' % name)
        decls.append('%s%s;' % (ctype, name))

    def temp(make):
        t = 'tmp%d' % len(temps)
        temps.append(t)
        decls.append('vinfo_t *%s;' % t)
        body.append('%s = %s;' % (t, make))
        return t

    def operand(text):
        text = re.sub(r'^\( PyObject \* \) ', '', text)   # a cast changes nothing
        if text == 'NULL':
            return 'NULL'     # Psyco's counterparts take NULL where C does
        if text in var:
            if var[text] == 'const':
                # a borrowed constant passed to a call: a compile-time
                # vinfo for the call's duration
                return temp('vinfo_new(CompileTime_New((long)%s))' % const_of[text])
            return text
        if text in CONSTANTS:
            return temp(CONSTANTS[text])
        raise Skip('operand %r' % text)

    stmts = split_statements(body_tokens(inst))
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
        if w[0] == 'for':
            raise Skip('loop %s' % s)
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
            cond = error_condition(args[0], var)
            if cond is None:
                continue        # a virtual object: it can't fail
            live = [t for t, o in var.items()
                    if o in ('object', 'owned', 'virtual') and t not in cond[1]]
            if live:
                raise Skip('ERROR_IF with %s owned' % ', '.join(live))
            body.append('if (%s)' % cond[0])
            body.append('  ' + LABELS[args[1]])
            last_fail = len(body)
            continue
        # [type] X = RHS
        ctype = ''
        if w[0] in ('int', 'PyObject') and len(w) > 2:
            ctype = 'int' if w[0] == 'int' else 'PyObject *'
            stmt, w = stmt[1:], w[1:]
            if ctype == 'PyObject *':
                if w[0] != '*':
                    raise Skip(s)
                stmt, w = stmt[1:], w[1:]
        if len(w) >= 3 and w[1] == '=':
            x = w[0]
            rhs = ' '.join(w[2:])
            if rhs == 'GETLOCAL ( oparg )':
                declare(x, 'vinfo_t *')
                body.append('%s = GETLOCAL(oparg);' % x)
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
            if len(w) > 3 and re.fullmatch(r'Py[A-Z][A-Za-z]*_[A-Za-z]+', w[2]) and w[3] == '(':
                fn = w[2]
                args, end = call_args(stmt, 3)
                if end != len(stmt):
                    raise Skip(s)
                pname, ret = counterpart(fn, csigs, psigs)
                ops = [operand(a) for a in args]
                calls.append(fn)
                if ret == 'virtual':
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

    # epilogue: inputs not moved to an output are released or stolen
    if decref_all:
        released |= set(in_names) - set(out_names) - stolen
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
        if n not in in_names and n not in out_names and origin not in ('bool', 'const'):
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
    lines += ['  ' + d for d in decls]
    lines += ['  ' + b for b in body]
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
        lines.append('  PUSH(%s);' % n)
    lines += ['  goto fine;', '}']
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


def run(insts, src):
    csigs, psigs = c_signatures(src), psyco_signatures()
    consts = ceval_reader.constants(src)
    done, skipped = [], []
    for inst in insts:
        try:
            w = [t.text for t in inst.block.tokens]
            copies = {w[k - 1] for k in range(1, len(w) - 1)
                      if w[k] == '=' and w[k + 1] == 'stack_pointer'}
            if any(w[k] == '&' and w[k + 1] in copies for k in range(len(w) - 1)):
                # calls: a copy of the stack pointer handed to the call
                # path, which moves it; that is core
                raise Skip('a copy of the stack pointer handed to %s'
                           % next(w[k - 2] for k in range(2, len(w))
                                  if w[k] == '&' and w[k + 1] in copies), 'core')
            code, _ = translate(inst, csigs, psigs, consts)
        except Skip as e:
            skipped.append((inst.name, e.kind, str(e)))
            continue
        done.append((inst.name, code))
    return done, skipped


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
    done, skipped = run(insts, target.src)
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
