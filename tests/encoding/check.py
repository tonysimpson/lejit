#!/usr/bin/env python3
"""Check x86-64 emitters against their intended instruction (doc/BUILD.md, Tests).

Each emitter macro in ni/x64/iencoding.h may be preceded by an annotation

    /* @asm: mov {r}, qword ptr [{rm}{o}] */

giving, in Intel syntax, the instruction(s) it is meant to emit. The checker:
  1. instantiates every annotated macro over the operand domains below,
     writing the bytes it emits (a generated C program);
  2. assembles the annotated text for the same operands with GNU as;
  3. disassembles both with objdump and compares the text.
Comparing disassembly rather than bytes accepts equivalent encodings
(e.g. disp8 vs disp32, 8B vs 89 forms of mov) but catches wrong registers,
wrong operand sizes, missing prefixes and mis-decoding.

Placeholders: {name} or {name:kind}. The parameter name picks the domain:
  registers  r rm rg rg1 rg2 r1 r2 base (all 16)   index (no rsp)
  kinds      d (64-bit reg, default) d->32-bit name with ':d', 8-bit ':b'
             offsets: o/offset default, ':d8', ':d32'
             immediates: ':i8' ':i32' ':i64' ':u16' ':sh' (shift count)
             cc (condition code 0..15), scale (1, 2, 4, 8)

Usage: check.py [-k MACRO] [-v]      exits 1 on any mismatch
"""
import itertools, json, os, re, subprocess, sys, sysconfig, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER = os.path.join(ROOT, 'ni', 'x64', 'iencoding.h')
PY37 = os.environ.get('NI_PY37', os.path.expanduser('~/.pyenv/versions/3.7.17/bin/python3.7'))
# the headers the build generates: psyco.h includes layouts_gen.h
_gen = sorted(__import__('glob').glob(os.path.join(ROOT, 'build', 'temp.*', 'gen')))
GEN = _gen[0] if _gen else os.path.join(ROOT, 'build', 'gen-missing')

REG64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
         'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']
REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'] + ['r%dd' % i for i in range(8, 16)]
REG8 = ['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil'] + ['r%db' % i for i in range(8, 16)]
CC = ['o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g']

REG_PARAMS = {'r', 'rm', 'rg', 'rg1', 'rg2', 'r1', 'r2', 'base'}
DOMAINS = {
    'reg': list(range(16)),
    'index': [i for i in range(16) if i != 4],
    'off': [0, 8, -8, 127, -128, 128, -129, 0x12345, -0x12345],
    'd8': [0, 8, -8, 127, -128],
    'd32': [0, 8, -8, 128, -129, 0x12345],
    'i8': [0, 1, -1, 127, -128],
    'i32': [0, 1, -1, 128, -129, 0x7fffffff, -0x80000000],
    'i64': [0, -1, 0x7fffffff, 0x80000000, 0x123456789abcdef0, -0x123456789abc],
    'u16': [8, 0x1000],
    'sh': [1, 3, 63],
    'cc': list(range(16)),
    'scale': [1, 2, 4, 8],
}


def parse_annotations(path):
    text = open(path).read()
    out = []
    for m in re.finditer(r'/\* @asm: (.*?) \*/\n#define ([A-Z0-9_]+)\(([^)]*)\)', text):
        intent, name, params = m.group(1), m.group(2), [p.strip() for p in m.group(3).split(',') if p.strip()]
        out.append((name, params, intent))
    # an annotation not directly above its #define would be silently skipped
    orphans = len(re.findall(r'/\* @asm:', text)) - len(out)
    if orphans:
        sys.exit('%s: %d @asm annotation(s) not directly followed by a '
                 '#define; fix them, or they go untested' % (path, orphans))
    return out


def placeholder_kinds(intent):
    return {m.group(1): (m.group(2) or '') for m in re.finditer(r'\{(\w+)(?::(\w+))?\}', intent)}


def domain_for(param, kind):
    if param == 'index':
        return 'index'
    if param in REG_PARAMS:
        return 'reg'
    if param == 'cc':
        return 'cc'
    if param == 'scale':
        return 'scale'
    if param in ('o', 'offset'):
        return kind if kind in ('d8', 'd32') else 'off'
    if kind in DOMAINS:
        return kind
    raise ValueError('no domain for parameter %r kind %r' % (param, kind))


def render(intent, values):
    def sub(m):
        name, kind = m.group(1), m.group(2) or ''
        v = values[name]
        if name in REG_PARAMS or name == 'index':
            return {'b': REG8, 'd': REG32}.get(kind, REG64)[v]
        if name == 'cc':
            return CC[v]
        if name in ('o', 'offset'):
            return '%+d' % v if v else ''
        return str(v)
    return re.sub(r'\{(\w+)(?::(\w+))?\}', sub, intent)


def cases(annotations, filt):
    for name, params, intent in annotations:
        if filt and filt != name:
            continue
        kinds = placeholder_kinds(intent)
        missing = [p for p in params if p not in kinds]
        if missing:
            raise ValueError('%s: parameters %s not in annotation' % (name, missing))
        doms = [DOMAINS[domain_for(p, kinds[p])] for p in params]
        for combo in itertools.product(*doms):
            values = dict(zip(params, combo))
            # offset 0 means "no displacement" in some encoders; keep it, the
            # disassembly comparison accepts either form
            yield name, params, combo, render(intent, values)


def c_literal(v):
    if v < 0:
        return '(-%dL)' % -v
    return '%dUL' % v


def build_emitted(case_list, workdir):
    src = ['#include <Python.h>', '#include "psyco.h"', '#include "vcompiler.h"',
           '#include <iencoding.h>', '#include <stdio.h>',
           'static code_t buf[%d];' % (len(case_list) * 24 + 64),
           'static code_t *code = buf;']
    chunk = 300
    nfun = 0
    for i, (name, params, combo, _) in enumerate(case_list):
        if i % chunk == 0:
            if i:
                src.append('}')
            src.append('static void f%d(void) {' % nfun)
            nfun += 1
        args = ', '.join(c_literal(v) if p not in REG_PARAMS | {'index'} else str(v)
                         for p, v in zip(params, combo))
        src.append('  printf("%%d %%ld\\n", %d, (long)(code - buf)); { %s(%s); }' % (i, name, args))
    if case_list:
        src.append('}')
    src += ['int main(void) {', '  FILE *f;']
    src += ['  f%d();' % k for k in range(nfun)]
    src += ['  printf("end %ld\\n", (long)(code - buf));',
            '  f = fopen(%s, "wb"); fwrite(buf, 1, code - buf, f); fclose(f);' % json.dumps(os.path.join(workdir, 'emitted.bin')),
            '  return 0; }']
    cfile = os.path.join(workdir, 'emit.c')
    open(cfile, 'w').write('\n'.join(src) + '\n')
    inc = subprocess.check_output([PY37, '-c', "import sysconfig;print(sysconfig.get_paths()['include'])"]).decode().strip()
    exe = os.path.join(workdir, 'emit')
    subprocess.check_call(['gcc', '-w', '-O0', '-DALL_CHECKS=0', '-DNI_TRACE=0', '-DALL_STATIC=0',
                           '-I' + inc, '-I' + os.path.join(ROOT, 'ni'), '-I' + os.path.join(ROOT, 'ni', 'x64'), '-I' + GEN,
                           cfile, '-o', exe])
    out = subprocess.check_output([exe]).decode().split('\n')
    starts = {}
    end = None
    for line in out:
        if line.startswith('end '):
            end = int(line.split()[1])
        elif line:
            i, off = line.split()
            starts[int(i)] = int(off)
    data = open(os.path.join(workdir, 'emitted.bin'), 'rb').read()
    bounds = [starts[i] for i in range(len(case_list))] + [end]
    return data, bounds


def build_intended(case_list, workdir):
    lines = ['.intel_syntax noprefix', '.text']
    for i, (_, _, _, text) in enumerate(case_list):
        lines.append('c%d:' % i)
        lines += [t.strip() for t in text.split(';')]
    lines.append('cend:')
    s = os.path.join(workdir, 'intent.s')
    o = os.path.join(workdir, 'intent.o')
    open(s, 'w').write('\n'.join(lines) + '\n')
    subprocess.check_call(['as', '--64', s, '-o', o])
    b = os.path.join(workdir, 'intent.bin')
    subprocess.check_call(['objcopy', '-O', 'binary', '-j', '.text', o, b])
    syms = {}
    for line in subprocess.check_output(['nm', o]).decode().split('\n'):
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
    bounds = [syms['c%d' % i] for i in range(len(case_list))] + [syms['cend']]
    return open(b, 'rb').read(), bounds


def disassemble(data, bounds, workdir, tag):
    """Disassemble each case's bytes. objdump runs over the whole blob; if an
    instruction crosses a case boundary (bad bytes), that case is marked and
    disassembly restarts at the next boundary, so one bad case can't
    desynchronise the rest."""
    path = os.path.join(workdir, tag + '.raw')
    open(path, 'wb').write(data)
    per_case = [[] for _ in range(len(bounds) - 1)]
    k = 0
    while k < len(per_case):
        out = subprocess.check_output(['objdump', '-D', '-b', 'binary', '-m', 'i386:x86-64',
                                       '-M', 'intel', '--start-address=%d' % bounds[k],
                                       path]).decode()
        restart = None
        for line in out.split('\n'):
            m = re.match(r'\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(.*)$', line)
            if not m:
                m2 = re.match(r'\s+([0-9a-f]+):\t([0-9a-f ]+?)\s*$', line)
                if m2:      # continuation line of raw bytes
                    nbytes = len(m2.group(2).split())
                    if addr + size + nbytes > bounds[k + 1]:
                        pass
                    size += nbytes
                    if addr + size > bounds[k + 1]:
                        per_case[k].append('<crosses case boundary>')
                        restart = k + 1
                        break
                continue
            addr = int(m.group(1), 16)
            size = len(m.group(2).split())
            while k < len(per_case) and addr >= bounds[k + 1]:
                k += 1
            if k >= len(per_case):
                break
            per_case[k].append(' '.join(m.group(3).split()))
            if addr + size > bounds[k + 1]:
                per_case[k].append('<crosses case boundary>')
                restart = k + 1
                break
        if restart is None:
            break
        k = restart
    return ['; '.join(c) for c in per_case]


def normalise(t):
    """Make equivalent encodings compare equal."""
    parts = []
    for ins in t.split('; '):
        ins = re.sub(r'#.*', '', ins)
        ins = ins.replace('movabs', 'mov').replace('rex.W nop', 'nop')
        ins = ins.replace('+0x0]', ']')                 # disp8 0 == no displacement
        ins = re.sub(r'\s+', ' ', ins).strip()
        m = re.match(r'xchg (\S+?),(\S+)$', ins)       # xchg is symmetric
        if m:
            ins = 'xchg ' + ','.join(sorted(m.groups()))
        m = re.match(r'mov (e[a-z]{2}|r\d+d),(0x[0-9a-f]+)$', ins)  # 32-bit MOV
        if m:                                          # zero-extends: same as 64
            r = m.group(1)
            ins = 'mov %s,%s' % ('r' + r[1:] if r[0] == 'e' else r[:-1], m.group(2))
        if ins == 'xchg rax,rax':
            ins = 'nop'
        parts.append(ins)
    return '; '.join(parts)


JUMP_DECODER = r'''
#include <Python.h>
#include "psyco.h"
#include "vcompiler.h"
#include <iencoding.h>
#include <stdio.h>
static code_t buf[1 << 18];
int main(void) {
  static const long disps[] = {0, 1, -1, 0xE9, -0x17, 0xE9E9, 0x12E9,
                               -0x1000, 0x10000, -0x1FF17};
  int bad = 0;
  unsigned i;
  for (i = 0; i < sizeof(disps) / sizeof(disps[0]); i++) {
    code_t *start = buf + (1 << 17), *code = start, *target;
    code_t *want = start + SIZE_OF_FAR_JUMP + disps[i];
    JUMP_TO(want);
    if (!IS_A_SINGLE_JUMP(start, code, target) || target != want) {
      printf("JMP with displacement %ld not decoded\n", disps[i]);
      bad++;
    }
  }
  {
    code_t *start = buf, *target; /* mov eax, 0xe9e9e9e9 */
    start[0] = 0xB8;
    start[1] = start[2] = start[3] = start[4] = 0xE9;
    if (IS_A_SINGLE_JUMP(start, start + 5, target)) {
      printf("mov eax, imm32 taken for a JMP\n");
      bad++;
    }
  }
  return bad != 0;
}
'''


def check_jump_decoder(workdir):
    """IS_A_SINGLE_JUMP (x64/idispatcher.h:make_code_conditional) decodes
    rather than emits, so it has no @asm annotation: check that it recognises
    what JUMP_TO writes, with its target, and nothing else of that length,
    reading the JMP from its first byte."""
    cfile = os.path.join(workdir, 'jumps.c')
    open(cfile, 'w').write(JUMP_DECODER)
    inc = subprocess.check_output([PY37, '-c', "import sysconfig;print(sysconfig.get_paths()['include'])"]).decode().strip()
    exe = os.path.join(workdir, 'jumps')
    subprocess.check_call(['gcc', '-w', '-O0', '-DALL_CHECKS=0', '-DNI_TRACE=0', '-DALL_STATIC=0',
                           '-I' + inc, '-I' + os.path.join(ROOT, 'ni'), '-I' + os.path.join(ROOT, 'ni', 'x64'), '-I' + GEN,
                           cfile, '-o', exe])
    p = subprocess.run([exe], stdout=subprocess.PIPE)
    for line in p.stdout.decode().splitlines():
        print('IS_A_SINGLE_JUMP: ' + line)
    return p.returncode != 0


def main():
    args = sys.argv[1:]
    filt = args[args.index('-k') + 1] if '-k' in args else None
    verbose = '-v' in args
    annotations = parse_annotations(HEADER)
    case_list = list(cases(annotations, filt))
    with tempfile.TemporaryDirectory() as wd:
        edata, ebounds = build_emitted(case_list, wd)
        idata, ibounds = build_intended(case_list, wd)
        etext = disassemble(edata, ebounds, wd, 'e')
        itext = disassemble(idata, ibounds, wd, 'i')
        jumps_bad = check_jump_decoder(wd)
    bad = {}
    for i, (name, params, combo, intent) in enumerate(case_list):
        e, it = normalise(etext[i]), normalise(itext[i])
        elen = ebounds[i + 1] - ebounds[i]
        if e != it:
            raw = edata[ebounds[i]:ebounds[i + 1]].hex(' ')
            bad.setdefault(name, []).append('%s(%s): want "%s"  got "%s"  [%s]' % (
                name, ', '.join(map(str, combo)), it, e, raw))
        elif verbose:
            print('ok   %s(%s): %s (%d bytes)' % (name, ', '.join(map(str, combo)), e, elen))
    total = len(case_list)
    nbad = sum(len(v) for v in bad.values())
    for name in sorted(bad):
        print('%s: %d mismatches, e.g.' % (name, len(bad[name])))
        for line in bad[name][:4]:
            print('    ' + line)
    print('%d macros, %d cases, %d mismatches in %d macros' % (
        len({c[0] for c in case_list}), total, nbad, len(bad)))
    return 1 if nbad or jumps_bad else 0


if __name__ == '__main__':
    sys.exit(main())
