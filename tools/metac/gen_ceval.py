#!/usr/bin/env python3
"""Generate bytecode-compiler cases from instruction definitions in 3.12's
DSL (doc/DECISIONS.md D26, doc/METACOMPILER.md "Bytecode stage").

Input: tools/metac/bytecodes37.c, CPython 3.7's simple opcodes described in
3.12's Python/bytecodes.c syntax, parsed by CPython's own parser
(tools/metac/cases_generator, vendored from 3.12.9). Output:
ni/Python/pycompiler_gen.h, cases included in pycompiler.c's opcode switch.

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
F(args) is translated only if ni/Objects/pabstract.h declares Psyco's
counterpart (PyNumber_Add -> PsycoNumber_Add) with one more parameter (po)
and a matching return kind: PyObject * -> vinfo_t * (NULL: error), int ->
bool (false: error). Anything else is skipped with the reason.

Each 3.7 description is checked against 3.7 itself: ceval.c has the
TARGET, calls every C-API function the description calls, and
dis.stack_effect() (run in python3.7) equals the declared effect.

Usage: gen_ceval.py [--check] [CPYTHON_37_SRC]
       gen_ceval.py --try BYTECODES_C     report only, e.g. on 3.12's file
"""
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'cases_generator'))
import ctokens  # noqa: E402
import cslots  # noqa: E402
import parser as dsl  # noqa: E402  (CPython's Tools/cases_generator/parser.py)

ROOT = cslots.ROOT
DEFS = os.path.join(HERE, 'bytecodes37.c')
OUT = os.path.join(ROOT, 'ni', 'Python', 'pycompiler_gen.h')
PSYCO_HEADERS = [os.path.join(ROOT, 'ni', 'Objects', 'pabstract.h')]
PY37 = os.environ.get('NI_PY37', os.path.expanduser(
    '~/.pyenv/versions/3.7.17/bin/python3.7'))
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
# a const from the code object (3.7: consts; 3.12: frame->f_code->co_consts)
CONSTS_TUPLES = ('consts', 'frame -> f_code -> co_consts')


class Skip(Exception):
    pass


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
    out, cur, depth = [], [], 0
    for t in toks:
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

def c_signatures(src37):
    """{name: (return kind, arity)} for PyAPI_FUNC declarations in 3.7's
    Include/abstract.h and object.h."""
    sigs = {}
    for h in ('abstract.h', 'object.h'):
        with open(os.path.join(src37, 'Include', h)) as f:
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
    """{name: (return kind, arity without po)} for Psyco* functions."""
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
            params = ctokens.split_top(toks[i + 2:close], ',')
            if not params or [u.text for u in params[0]][:1] != ['PsycoObject']:
                continue
            sigs[t.text] = (kind, len(params) - 1)
    return sigs


def counterpart(fn, csigs, psigs):
    if fn not in csigs:
        raise Skip('%s: no declaration in Include/abstract.h' % fn)
    pname = 'Psyco' + fn[2:]
    if pname not in psigs:
        raise Skip('%s: no Psyco counterpart %s' % (fn, pname))
    cret, carity = csigs[fn]
    pret, parity = psigs[pname]
    if carity != parity:
        raise Skip('%s: %d arguments, %s takes %d' % (fn, carity, pname, parity))
    if (cret, pret) not in (('object', 'vinfo'), ('int', 'bool')):
        raise Skip('%s returns %s, %s returns %s' % (fn, cret, pname, pret))
    return pname, cret


# ---- translation -----------------------------------------------------------

def translate(inst, csigs, psigs):
    """C text for one case, and the C-API functions it calls."""
    if inst.kind != 'inst':
        raise Skip('legacy definition without a stack effect')
    inputs = [e for e in inst.inputs if isinstance(e, dsl.StackEffect)]
    outputs = list(inst.outputs)
    for e in inputs + outputs:
        if e.size or e.cond or e.type:
            raise Skip('stack effect %s needs arrays, conditions or types' % e.name)
    in_names = [e.name for e in inputs]
    out_names = [e.name for e in outputs]
    if len(set(in_names)) != len(in_names) or len(set(out_names)) != len(out_names):
        raise Skip('repeated stack name')

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

    def commit():
        nonlocal first_commit
        if first_commit is None:
            first_commit = len(body)

    def declare(name, ctype):
        if name in var:
            raise Skip('%s assigned twice' % name)
        decls.append('%s%s;' % (ctype, name))

    def operand(text):
        if text in var:
            if var[text] == 'const':
                raise Skip('%s: a borrowed constant used before Py_INCREF' % text)
            return text
        if text in CONSTANTS:
            t = 'tmp%d' % len(temps)
            temps.append(t)
            decls.append('vinfo_t *%s;' % t)
            body.append('%s = %s;' % (t, CONSTANTS[text]))
            return t
        raise Skip('operand %r' % text)

    for stmt in split_statements(body_tokens(inst)):
        w = [t.text for t in stmt]
        s = words(stmt)
        if w[0] in DROPPED or (len(w) == 4 and w[:3] == ['(', 'void', ')']):
            continue
        if s == 'DECREF_INPUTS ( )':
            decref_all = True
            continue
        if len(w) == 4 and w[0] == 'Py_DECREF' and w[2] in in_names:
            released.add(w[2])
            continue
        if len(w) == 4 and w[0] == 'Py_INCREF':
            x = w[2]
            if var.get(x) == 'local':
                commit()
                body.append('vinfo_incref(%s);' % x)
                var[x] = 'owned'
            elif var.get(x) == 'const':
                body.append('%s = vinfo_new(CompileTime_New((long)%s));'
                            % (x, const_expr))
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
            if var.get(x) not in ('input', 'owned', 'object'):
                raise Skip('SETLOCAL of %s' % x)
            commit()
            body.append('SETLOCAL(oparg, %s);' % x)
            stolen.add(x)
            continue
        if w[:3] == ['ERROR_IF', '(', w[2]]:
            args, _ = call_args(stmt, 1)
            if len(args) != 2 or args[1] not in LABELS:
                raise Skip('ERROR_IF %s' % s)
            if first_commit is not None:
                raise Skip('ERROR_IF after an irreversible statement'
                           ' (restart rule)')
            cond = error_condition(args[0], var)
            live = [t for t, o in var.items()
                    if o in ('object', 'owned') and t not in cond[1]]
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
            if m and m.group(1) in CONSTS_TUPLES:
                declare(x, 'vinfo_t *')
                var[x] = 'const'
                const_expr = 'GETCONST(oparg)'   # borrowed from the code object
                continue
            if len(w) > 3 and re.fullmatch(r'Py[A-Z][A-Za-z]*_[A-Za-z]+', w[2]) and w[3] == '(':
                fn = w[2]
                args, end = call_args(stmt, 3)
                if end != len(stmt):
                    raise Skip(s)
                pname, ret = counterpart(fn, csigs, psigs)
                ops = [operand(a) for a in args]
                calls.append(fn)
                if ret == 'object':
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
        if n in out_names and (n in released or n in stolen):
            raise Skip('input %s is released and also an output' % n)
        if n not in out_names and n not in released | stolen:
            raise Skip('input %s is neither released nor stored' % n)
    for n, origin in var.items():
        if n not in in_names and n not in out_names and origin != 'bool':
            # not a stack value: a result dropped, or interpreter state
            # (3.12's kwnames)
            raise Skip('%s is assigned but is not an output' % n)
    for n in out_names:
        if n not in var or var[n] not in ('input', 'owned', 'object'):
            raise Skip('output %s has no owned value (%s)' % (n, var.get(n)))
    assert first_commit is None or first_commit >= last_fail
    lines = ['case %s: {' % inst.name]
    for k, n in enumerate(in_names):
        lines.append('  vinfo_t *%s = NTOP(%d);' % (n, len(in_names) - k))
    lines += ['  ' + d for d in decls]
    lines += ['  ' + b for b in body]
    for n in reversed(in_names):
        lines.append('  POP(%s);' % n)
    for n in reversed(in_names):
        if n in released:
            lines.append('  vinfo_decref(%s, po);' % n)
    for n in out_names:
        lines.append('  PUSH(%s);' % n)
    lines += ['  goto fine;', '}']
    return '\n'.join(lines), calls


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
    raise Skip('ERROR_IF condition %r' % cond)


# ---- checks against 3.7 ----------------------------------------------------

def ceval_cases(src37):
    with open(os.path.join(src37, 'Python', 'ceval.c')) as f:
        toks = ctokens.tokenize(f.read(), 'ceval.c')[0]
    cases = {}
    for i, t in enumerate(toks):
        if t.text != 'TARGET' or toks[i + 1].text != '(':
            continue
        if toks[i + 4].text == '{':
            end = ctokens.match_close(toks, i + 4)
        else:   # TARGET(NOP) FAST_DISPATCH();
            end = next(j for j in range(i + 4, len(toks)) if toks[j].text == ';')
        cases[toks[i + 2].text] = {u.text for u in toks[i + 4:end]}
    return cases


def stack_effects(names):
    code = ('import dis, json, opcode, sys\n'
            'out = {}\n'
            'for n in sys.argv[1:]:\n'
            '    op = opcode.opmap.get(n)\n'
            '    if op is None:\n'
            '        out[n] = None  # not a 3.7 opcode: reported via ceval.c\n'
            '        continue\n'
            '    try:\n'
            '        out[n] = dis.stack_effect(op, 0 if op >= opcode.HAVE_ARGUMENT else None)\n'
            '    except ValueError:\n'
            '        out[n] = None  # unknown to compile.c (NOP)\n'
            'print(json.dumps(out))\n')
    r = subprocess.run([PY37, '-I', '-c', code] + names, check=True,
                       stdout=subprocess.PIPE, universal_newlines=True)
    return json.loads(r.stdout)


def check_37(insts, translated, src37):
    """Problems found comparing the descriptions with 3.7."""
    cases = ceval_cases(src37)
    effects = stack_effects([i.name for i in insts])
    problems = []
    for inst in insts:
        if inst.name not in cases:
            problems.append('%s: no TARGET in ceval.c' % inst.name)
            continue
        declared = len(inst.outputs) - len(
            [e for e in inst.inputs if isinstance(e, dsl.StackEffect)])
        if effects[inst.name] is not None and declared != effects[inst.name]:
            problems.append('%s: stack effect %d, dis.stack_effect says %d'
                            % (inst.name, declared, effects[inst.name]))
        for fn in translated.get(inst.name, ()):
            if fn not in cases[inst.name]:
                problems.append('%s: ceval.c does not call %s' % (inst.name, fn))
    return problems


# ---- driver ----------------------------------------------------------------

def run(defs_path, src37):
    insts = [d for d in read_defs(defs_path) if isinstance(d, dsl.InstDef)]
    csigs, psigs = c_signatures(src37), psyco_signatures()
    done, skipped, calls = [], [], {}
    for inst in insts:
        try:
            code, fns = translate(inst, csigs, psigs)
        except Skip as e:
            skipped.append((inst.name, str(e)))
            continue
        done.append((inst.name, code))
        calls[inst.name] = fns
    return insts, done, skipped, calls


def header(done, skipped):
    out = ['/* GENERATED by tools/metac/gen_ceval.py from tools/metac/bytecodes37.c',
           '   (CPython 3.7\'s opcodes in 3.12\'s instruction DSL): do not edit.',
           '   Included in the opcode switch of pycompiler.c, which provides',
           '   NTOP, POP, PUSH, GETLOCAL, SETLOCAL, GETCONST, unbound_local_error',
           '   and the label fine. */', '']
    for _, code in done:
        out += ['      ' + l if l else '' for l in code.split('\n')] + ['']
    out += ['/* not generated (hand-written in pycompiler.c, or unsupported):']
    out += ['   %s: %s' % x for x in skipped]
    out += ['*/', '']
    return '\n'.join(out)


def main(argv):
    if argv[:1] == ['--try']:
        insts, done, skipped, _ = run(argv[1], cslots.DEFAULT_SRC)
        for name, _ in done:
            print('translated %s' % name)
        for name, why in skipped:
            print('skipped    %-28s %s' % (name, why))
        print('%d of %d translated' % (len(done), len(insts)))
        return 0
    check = '--check' in argv
    args = [a for a in argv if a != '--check']
    src37 = args[0] if args else os.environ.get('NI_CPYTHON_SRC', cslots.DEFAULT_SRC)
    insts, done, skipped, calls = run(DEFS, src37)
    problems = check_37(insts, calls, src37)
    if problems:
        print('bytecodes37.c does not match CPython 3.7:')
        print('\n'.join('  ' + p for p in problems))
        return 1
    out = header(done, skipped)
    rel = os.path.relpath(OUT, ROOT)
    if check:
        with open(OUT) as f:
            if f.read() != out:
                print('%s is out of date: run tools/metac/gen_ceval.py' % rel)
                for name, why in skipped:
                    print('  skipped %s: %s' % (name, why))
                return 1
        print('%s up to date (%d opcodes generated)' % (rel, len(done)))
        return 0
    with open(OUT, 'w') as f:
        f.write(out)
    for name, _ in done:
        print('generated  %s' % name)
    for name, why in skipped:
        print('skipped    %-24s %s' % (name, why))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
