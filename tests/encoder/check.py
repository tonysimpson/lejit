#!/usr/bin/env python3
"""Checks each x86-64 emitter in src/x64/iencoding.h against its annotation,
by assembling both with GNU as and comparing objdump's disassembly:

    /* @asm: mov {r}, qword ptr [{rm}{o}] */

Placeholders are {name} or {name:kind}: registers (r, rm, base, index...),
XMM registers (x, x1, x2),
':d' 32-bit, ':b' 8-bit, offsets ':d8' ':d32', immediates ':i8' ':i16'
':i32' ':i64' ':u8' ':u16' ':sh', cc (condition code), scale.

    python3 tests/encoder/check.py PYTHON [-k MACRO] [-v]

PYTHON is the interpreter whose headers and in-place checked build
(LEJIT_DEV_TRACE=1 PYTHON setup.py build_ext --inplace) are used.
"""
import concurrent.futures, itertools, json, os, re, subprocess, sys, sysconfig, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER = os.path.join(ROOT, 'src', 'x64', 'iencoding.h')
PY = None   # the interpreter, from the command line
# the header that interpreter's build writes: psyco.h includes le_features_gen.h
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from common import build_temp


def gen_dir():
    return os.path.join(build_temp(PY), 'gen')

REG64 = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
         'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15']
REG32 = ['eax', 'ecx', 'edx', 'ebx', 'esp', 'ebp', 'esi', 'edi'] + ['r%dd' % i for i in range(8, 16)]
REG8 = ['al', 'cl', 'dl', 'bl', 'spl', 'bpl', 'sil', 'dil'] + ['r%db' % i for i in range(8, 16)]
CC = ['o', 'no', 'b', 'ae', 'e', 'ne', 'be', 'a', 's', 'ns', 'p', 'np', 'l', 'ge', 'le', 'g']

REG_PARAMS = {'r', 'rm', 'rg', 'rg1', 'rg2', 'r1', 'r2', 'base'}
XMM_PARAMS = {'x', 'x1', 'x2'}
DOMAINS = {
    'reg': list(range(16)),
    'xmm': list(range(16)),
    'index': [i for i in range(16) if i != 4],
    'off': [0, 8, -8, 127, -128, 128, -129, 0x12345, -0x12345],
    'd8': [0, 8, -8, 127, -128],
    'd32': [0, 8, -8, 128, -129, 0x12345],
    'i8': [0, 1, -1, 127, -128],
    'i16': [0, 1, -1, 127, -128, 128, 0x7fff, -0x8000],
    'i32': [0, 1, -1, 128, -129, 0x7fffffff, -0x80000000],
    'i64': [0, -1, 0x7fffffff, 0x80000000, 0x123456789abcdef0, -0x123456789abc],
    'u8': [0, 1, 4, 7],
    'u16': [8, 0x1000],
    'sh': [1, 3, 63],
    'cc': list(range(16)),
    'scale': [1, 2, 4, 8],
}


# an annotation, on one line or wrapped (a continuation line may start with
# ' * '), and the #define on the line after it; spacing doesn't count
ANNOTATION = re.compile(r'/\*\s*@asm:\s*((?:(?!\*/).)*?)\s*\*/[ \t]*\n[ \t]*#[ \t]*define[ \t]+'
                        r'([A-Z0-9_]+)\(([^)]*)\)', re.S)


def parse_annotations(path):
    return parse_annotations_text(open(path).read(), path)


def parse_annotations_text(text, path):
    out = []
    for m in ANNOTATION.finditer(text):
        intent = ' '.join(re.sub(r'\n\s*(\*(?=\s))?', ' ', m.group(1)).split())
        name = m.group(2)
        params = [p.strip() for p in m.group(3).replace('\\\n', ' ').split(',') if p.strip()]
        out.append((name, params, intent))
    # an annotation not directly above its #define would be silently skipped
    orphans = len(re.findall(r'/\*\s*@asm:', text)) - len(out)
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
    if param in XMM_PARAMS:
        return 'xmm'
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
        if name in XMM_PARAMS:
            return 'xmm%d' % v
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


# Each case is a macro expansion of a few kB of C (over 500 MB preprocessed
# in all), and gcc keeps a translation unit's functions until its end even
# at -O0: as one file it needed more than 16 GB. So the cases are split into
# files of PART cases, compiled separately (about 0.5 GB each) and linked.
# -ftrack-macro-expansion=0 drops the location gcc keeps for every token of
# a macro expansion, used only by diagnostics, which halves that.
PART = 15000
CHUNK = 300            # cases per function; PART is a multiple
JOBS = min(4, os.cpu_count() or 1)
CFLAGS = ['-w', '-O0', '-ftrack-macro-expansion=0', '-DALL_CHECKS=0',
          '-DNI_TRACE=0']
HEAD = ['#include <Python.h>', '#include "psyco.h"', '#include "vcompiler.h"',
        '#include <iencoding.h>', '#include <stdio.h>']


def includes():
    inc = subprocess.check_output([PY, '-c', "import sysconfig;print(sysconfig.get_paths()['include'])"]).decode().strip()
    return ['-I' + inc, '-I' + os.path.join(ROOT, 'src'),
            '-I' + os.path.join(ROOT, 'src', 'x64'), '-I' + gen_dir()]


def build_emitted(case_list, workdir):
    sources = []
    for p0 in range(0, len(case_list), PART):
        src = HEAD + ['extern code_t buf[];', 'extern code_t *code;']
        for i in range(p0, min(p0 + PART, len(case_list))):
            name, params, combo, _ = case_list[i]
            if i % CHUNK == 0:
                if i > p0:
                    src.append('}')
                src.append('void f%d(void) {' % (i // CHUNK))
            args = ', '.join(c_literal(v) if p not in REG_PARAMS | XMM_PARAMS | {'index'} else str(v)
                             for p, v in zip(params, combo))
            src.append('  printf("%%d %%ld\\n", %d, (long)(code - buf)); { %s(%s); }' % (i, name, args))
        src.append('}')
        sources.append(src)
    nfun = (len(case_list) + CHUNK - 1) // CHUNK
    # buf is not executable memory: no constant gets a slot
    src = HEAD + ['code_t buf[%d];' % (len(case_list) * 24 + 64), 'code_t *code = buf;',
                  'code_t *psyco_const_slot(code_t *at, long value) { return NULL; }']
    src += ['void f%d(void);' % k for k in range(nfun)]
    src += ['int main(void) {', '  FILE *f;']
    src += ['  f%d();' % k for k in range(nfun)]
    src += ['  printf("end %ld\\n", (long)(code - buf));',
            '  f = fopen(%s, "wb"); fwrite(buf, 1, code - buf, f); fclose(f);' % json.dumps(os.path.join(workdir, 'emitted.bin')),
            '  return 0; }']
    sources.append(src)
    flags = CFLAGS + includes()
    objs = []
    for k, src in enumerate(sources):
        cfile = os.path.join(workdir, 'emit%d.c' % k)
        open(cfile, 'w').write('\n'.join(src) + '\n')
        objs.append(cfile[:-2] + '.o')
    with concurrent.futures.ThreadPoolExecutor(JOBS) as pool:
        list(pool.map(lambda o: subprocess.check_call(
            ['gcc', '-c'] + flags + [o[:-2] + '.c', '-o', o]), objs))
    exe = os.path.join(workdir, 'emit')
    subprocess.check_call(['gcc'] + objs + ['-o', exe])
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
/* every jump here is in reach */
code_t *psyco_code_far_jump(code_t *end, code_t *target) { return target; }
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
    inc = subprocess.check_output([PY, '-c', "import sysconfig;print(sysconfig.get_paths()['include'])"]).decode().strip()
    exe = os.path.join(workdir, 'jumps')
    subprocess.check_call(['gcc', '-w', '-O0', '-DALL_CHECKS=0', '-DNI_TRACE=0',
                           '-I' + inc, '-I' + os.path.join(ROOT, 'src'), '-I' + os.path.join(ROOT, 'src', 'x64'), '-I' + gen_dir(),
                           cfile, '-o', exe])
    p = subprocess.run([exe], stdout=subprocess.PIPE)
    for line in p.stdout.decode().splitlines():
        print('IS_A_SINGLE_JUMP: ' + line)
    return p.returncode != 0


def main():
    global PY
    if len(sys.argv) < 2 or sys.argv[1].startswith('-'):
        sys.exit(__doc__)
    PY, args = sys.argv[1], sys.argv[2:]
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
