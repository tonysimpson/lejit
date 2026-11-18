"""Does Ni emit the same machine code with two builds? (doc/FINDINGS.md,
"Same code"; doc/GENERATION.md step 0)

    python3.7 tools/samecode.py A B               # tests and the seven
    python3.7 tools/samecode.py A B -k richards   # units whose name has it
    python3.7 tools/samecode.py A B --pyperf      # all of pyperformance too
    python3.7 tools/samecode.py A B --no-tests    # benchmarks only
    python3.7 tools/samecode.py A B -v            # every differing copy

A and B are each a git revision (checked out with `git archive` and built
with NI_CODESTATS=1 under build/samecode/<sha>, kept for next time), a
directory holding a ni*.so, or the .so itself. A .so must be an
NI_CODESTATS build. The workload is this checkout's: each tests/cases file
in one process (its cases called ten times, as tests/child.py warms them),
and each benchmark in one process, run until it compiles nothing new
(at most 20 iterations), with PYTHONHASHSEED=0, address randomisation off
(setarch -R) and a 4 GB address-space limit. Two processes at a time.

Each process takes ni._codestats(): every copy of every bytecode
instruction compiled (the bytes from the start of one instruction to the
start of the next, as emitted; respawn replays aren't counted), and
disassembles it with capstone, writing each address as what it points to:
an instruction copy (`file:name:line@offset`, `+` if inside it, `.+n`
within the copy itself), a live Python object that starts there (type,
function, code, module, str, int, a function's globals, else `obj:` and
its type name) or a field of a type, a C symbol (`nm` of the mapped file;
gcc's `.isra.N`-style suffixes dropped), other emitted code (`code`), or
other memory (`P`). Values below 0x10000 and outside any mapping stay. Two
builds emit the same code for an instruction when the multisets of
(how the compiler was entered, normalised text) of its copies are equal;
copy order and block addresses don't count. Stubs, function headers and
glue aren't per-instruction copies; only their bytes per buffer kind are
compared.
"""
import bisect
import collections
import concurrent.futures
import difflib
import gc
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import types

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, 'tests', 'cases')
BMDIR = os.path.join(ROOT, 'tools', 'pyperformance')
PY37 = os.environ.get('NI_PY37',
                      os.path.expanduser('~/.pyenv/versions/3.7.17/bin/python3.7'))
SEVEN = ('richards', 'deltablue', 'raytrace', 'go', 'hexiom', 'chaos',
         'spectral_norm')
KINDS = ('analyse', 'call', 'pause', 'respawn', 'promotion')
TIMEOUT = {'test': 120, 'bench': 600}
# units whose compiled code depends on timing; differences are shown only
TIMING = {'test/eval_breaker': 'signals and threads arrive by the clock'}


# ---------------------------------------------------------------- child

def load_ni(so):
    import importlib.machinery
    import importlib.util
    loader = importlib.machinery.ExtensionFileLoader('ni', so)
    spec = importlib.util.spec_from_file_location('ni', so, loader=loader)
    ni = importlib.util.module_from_spec(spec)
    sys.modules['ni'] = ni
    loader.exec_module(ni)
    return ni


def run_tests(ni, name):
    import importlib
    sys.path.insert(0, CASES)
    mod = importlib.import_module(name)
    env = dict(vars(mod), nan=float('nan'), inf=float('inf'))
    for fname, calls, opts in getattr(mod, 'CASES', []):
        fn = getattr(mod, fname)
        if isinstance(calls, str):
            calls = eval(calls, env)
        for _ in range(10):
            for args in calls:
                try:
                    fn(*args)
                except Exception:
                    pass
    return 10


def run_bench(ni, name):
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import pyperf37
    iterations = 0
    for _, timed in pyperf37.load(os.path.join(BMDIR, 'bm_%s.py' % name)):
        last = -1
        for _ in range(20):
            timed(1)
            iterations += 1
            now = ni._stats()['code_bytes']
            if now == last:
                break
            last = now
    return iterations


def co_label(co):
    f = co.co_filename
    if f.startswith(ROOT + os.sep):
        f = f[len(ROOT) + 1:]
    return '%s:%s:%d' % (f, co.co_name, co.co_firstlineno)


class Symbols:
    """addresses in mapped files, as symbol+offset from `nm`"""

    def __init__(self):
        self.maps = []          # (start, end, path or '', executable)
        bases = {}
        with open('/proc/self/maps') as f:
            for line in f:
                p = line.split()
                a, b = (int(x, 16) for x in p[0].split('-'))
                path = p[5] if len(p) > 5 and p[5].startswith('/') else ''
                self.maps.append((a, b, path, 'x' in p[1]))
                if path:
                    base = a - int(p[2], 16)
                    bases[path] = min(bases.get(path, base), base)
        self.maps.sort()
        self.starts = [m[0] for m in self.maps]
        self.bases = bases
        self.tables = {}

    def mapping(self, v):
        i = bisect.bisect_right(self.starts, v) - 1
        if i >= 0 and v < self.maps[i][1]:
            return self.maps[i]
        return None

    def table(self, path):
        t = self.tables.get(path)
        if t is None:
            syms = []
            for opt in (['-S', '--defined-only'], ['-D', '-S', '--defined-only']):
                out = subprocess.run(['nm'] + opt + [path],
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.DEVNULL,
                                     universal_newlines=True).stdout
                for line in out.splitlines():
                    p = line.split()
                    if len(p) == 4:
                        # gcc's clone suffixes depend on the build
                        name = re.sub(r'\.(isra|constprop|part|cold)\.\d+',
                                      '', p[3])
                        syms.append((int(p[0], 16), int(p[1], 16), name))
                if syms:
                    break
            syms.sort()
            t = self.tables[path] = ([s[0] for s in syms], syms)
        return t

    def symbol(self, v, path):
        starts, syms = self.table(path)
        off = v - self.bases[path]
        i = bisect.bisect_right(starts, off) - 1   # aliases: the largest
        if i >= 0 and off < syms[i][0] + max(syms[i][1], 1):
            d = off - syms[i][0]
            return syms[i][2] + ('+%d' % d if d else '')
        return os.path.basename(path)


def describe(o, globals_of):
    t = type(o)
    try:
        if isinstance(o, type):
            return 'type:%s.%s' % (o.__module__, o.__qualname__)
        if t in (int, bool, float, complex, type(None), bytes) or \
                o is Ellipsis or o is NotImplemented:
            if t is int and o.bit_length() > 128:
                return 'int'
            r = repr(o)
            return r if len(r) <= 60 else t.__name__
        if t is str:
            return 'str:' + repr(o)[:60]
        if t is types.FunctionType:
            return 'fn:%s.%s' % (o.__module__, o.__qualname__)
        if t is types.CodeType:
            return 'code:' + co_label(o)
        if t is types.ModuleType:
            return 'mod:' + o.__name__
        if t is types.BuiltinFunctionType:
            s = getattr(o, '__self__', None)
            return 'cfn:%s.%s' % ('' if s is None or isinstance(s, types.ModuleType)
                                  else type(s).__name__, o.__name__)
        if t is dict and id(o) in globals_of:
            return 'globals:' + globals_of[id(o)]
    except Exception:
        pass
    return 'obj:' + t.__qualname__


class Namer:
    """what an address in the emitted code points to (see the docstring)"""

    def __init__(self, copies):
        self.sym = Symbols()
        objs = {}
        for o in gc.get_objects():
            objs[id(o)] = o
            for r in gc.get_referents(o):
                objs.setdefault(id(r), r)
        for m in list(sys.modules.values()):
            objs.setdefault(id(m), m)
        globals_of = {}
        for o in list(objs.values()):
            if type(o) is types.FunctionType:
                globals_of.setdefault(id(o.__globals__), o.__module__)
        self.objs = []
        for a, o in objs.items():
            try:
                n = sys.getsizeof(o)
            except Exception:
                n = 16
            self.objs.append((a, a + max(n, 16), o))
        self.objs.sort(key=lambda x: x[0])
        self.ostarts = [x[0] for x in self.objs]
        self.globals_of = globals_of
        self.descs = {}
        self.copies = sorted(copies)           # (start, end, label)
        self.cstarts = [c[0] for c in self.copies]
        self.unknown = 0

    def name(self, v, here, here_end):
        if here <= v < here_end:
            return '.+%d' % (v - here)
        m = self.sym.mapping(v)
        if m is None:
            return None
        i = bisect.bisect_right(self.cstarts, v) - 1
        labels = set()
        for j in range(i, max(i - 8, -1), -1):
            a, b, lab = self.copies[j]
            if a <= v < b:
                labels.add(lab if a == v else lab + '+')
        if labels:
            return '|'.join(sorted(labels))
        i = bisect.bisect_right(self.ostarts, v) - 1
        # inside an object only for types (their fields); elsewhere it may
        # be a C record whose memory a live object happens to reuse
        if i >= 0 and v < self.objs[i][1] and (
                v == self.objs[i][0] or isinstance(self.objs[i][2], type)):
            a, _, o = self.objs[i]
            d = self.descs.get(a)
            if d is None:
                d = self.descs[a] = describe(o, self.globals_of)
            return d + ('+%d' % (v - a) if v != a else '')
        start, end, path, x = m
        if path:
            return self.sym.symbol(v, path)
        self.unknown += 1
        return 'code' if x else 'P'


HEX = re.compile(r'0x[0-9a-f]+')
RIP = re.compile(r'rip ([+-]) (0x[0-9a-f]+|\d+)')


def normalise(md, namer, addr, code):
    out = []
    end = addr + len(code)
    for insn in md.disasm(code, addr):
        ops = insn.op_str
        m = RIP.search(ops)
        if m:
            d = int(m.group(2), 0)
            target = insn.address + insn.size + (d if m.group(1) == '+' else -d)
            ops = ops[:m.start()] + (namer.name(target, addr, end) or
                                     hex(target)) + ops[m.end():]

        def sub(m):
            v = int(m.group(0), 16)
            if v < 0x10000:
                return m.group(0)
            return namer.name(v, addr, end) or m.group(0)
        ops = HEX.sub(sub, ops)
        out.append(insn.mnemonic + ' ' + ops if ops else insn.mnemonic)
    # bytes capstone couldn't decode
    done = sum(i.size for i in md.disasm(code, addr))
    if done < len(code):
        out.append('.bytes %d' % (len(code) - done))
    return out


def child(so, unit, outfile):
    ni = load_ni(so)
    if not hasattr(ni, '_codestats'):
        sys.exit('%s: not an NI_CODESTATS=1 build' % so)
    kind, name = unit.split('/', 1)
    import random
    import tempfile
    random.seed(0)
    tempfile._RandomNameSequence.rng = property(
        lambda self: self.__dict__.setdefault('_seeded', random.Random(0)))
    ni._codestats()
    iterations = (run_tests if kind == 'test' else run_bench)(ni, name)
    stats, lost, bufs, _ = ni._codestats()
    import capstone
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    labels = {}
    seen = collections.Counter()
    for co, _ in stats:
        if co not in labels:
            lab = co_label(co)
            seen[lab] += 1
            labels[co] = lab if seen[lab] == 1 else '%s#%d' % (lab, seen[lab])
    copies = []
    for (co, i), (op, n, b, cs) in stats.items():
        for c in cs:
            copies.append((c[0], c[0] + len(c[1]), '%s@%d' % (labels[co], i)))
    namer = Namer(copies)
    code = {}
    for (co, i), (op, n, b, cs) in stats.items():
        line = co.co_firstlineno
        for off, ln in __import__('dis').findlinestarts(co):
            if off > i:
                break
            line = ln
        code.setdefault(labels[co], {})[str(i)] = [
            op, line, [[KINDS[c[2]], normalise(md, namer, c[0], c[1])]
                       for c in cs]]
    with open(outfile, 'w') as f:
        json.dump({'unit': unit, 'iterations': iterations, 'lost': lost,
                   'buffers': bufs, 'unknown': namer.unknown, 'code': code}, f)


# ---------------------------------------------------------------- parent

def resolve(spec, cache):
    """the ni*.so for a revision, a directory or a file"""
    if os.path.isfile(spec):
        return os.path.abspath(spec), spec
    if os.path.isdir(spec):
        found = glob.glob(os.path.join(spec, 'ni*.so'))
        if len(found) != 1:
            sys.exit('%s: expected one ni*.so, found %d' % (spec, len(found)))
        return os.path.abspath(found[0]), spec
    sha = subprocess.run(['git', '-C', ROOT, 'rev-parse', '--verify',
                          spec + '^{commit}'], stdout=subprocess.PIPE,
                         universal_newlines=True)
    if sha.returncode:
        sys.exit('%s: not a file, directory or revision' % spec)
    sha = sha.stdout.strip()
    d = os.path.join(cache, sha[:12])
    found = glob.glob(os.path.join(d, 'ni*.so'))
    if found:
        return found[0], '%s (%s)' % (spec, sha[:12])
    os.makedirs(d, exist_ok=True)
    archive = subprocess.Popen(['git', '-C', ROOT, 'archive', sha],
                               stdout=subprocess.PIPE)
    subprocess.run(['tar', '-x', '-C', d], stdin=archive.stdout, check=True)
    archive.wait()
    for old in glob.glob(os.path.join(d, 'ni*.so')):
        os.remove(old)
    log = os.path.join(d, 'samecode-build.log')
    print('building %s in %s' % (spec, d), file=sys.stderr)
    with open(log, 'w') as f:
        r = subprocess.run([PY37, 'setup.py', 'build_ext', '--inplace'],
                           cwd=d, stdout=f, stderr=subprocess.STDOUT,
                           env=dict(os.environ, NI_CODESTATS='1'))
    found = glob.glob(os.path.join(d, 'ni*.so'))
    if r.returncode or not found:
        sys.exit('%s: build failed, see %s' % (spec, log))
    return found[0], '%s (%s)' % (spec, sha[:12])


def units(args):
    out = []
    if '--no-tests' not in args:
        for path in sorted(glob.glob(os.path.join(CASES, '*.py'))):
            if 'CASES' in open(path).read():
                out.append('test/' + os.path.basename(path)[:-3])
    if '--no-bench' not in args:
        names = SEVEN
        if '--pyperf' in args:
            names = sorted(os.path.basename(p)[3:-3] for p in
                           glob.glob(os.path.join(BMDIR, 'bm_*.py')))
        out += ['bench/' + n for n in names]
    return out


def run_child(so, unit, outfile):
    cmd = [PY37, os.path.abspath(__file__), '--child', so, unit, outfile]
    if os.path.exists('/usr/bin/setarch'):
        cmd = ['setarch', 'x86_64', '-R'] + cmd
    cmd = ['prlimit', '--as=4000000000'] + cmd
    # a fresh directory of the same length for A and B, as the working
    # directory and TMPDIR (tempfile's names are made deterministic, below)
    tmp = outfile[:-len('.json')] + '.tmp'
    os.makedirs(tmp)
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           universal_newlines=True, cwd=tmp,
                           timeout=TIMEOUT[unit.split('/')[0]],
                           env=dict(os.environ, PYTHONHASHSEED='0', TMPDIR=tmp,
                                    PYTHONDONTWRITEBYTECODE='1'))
    except subprocess.TimeoutExpired:
        return None, 'timeout'
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if p.returncode or not os.path.exists(outfile):
        err = (p.stderr.strip().splitlines() or ['?'])[-1]
        return None, 'exit %d: %s' % (p.returncode, err[:100])
    with open(outfile) as f:
        return json.load(f), None


def copies_key(cs):
    return collections.Counter((k, '\n'.join(t)) for k, t in cs)


def first_difference(ca, cb):
    """the unmatched copies on each side, and where the closest pair
    first differs: (a lines, b lines, index)"""
    ka, kb = copies_key(ca), copies_key(cb)
    ua, ub = list((ka - kb).elements()), list((kb - ka).elements())
    if not ua or not ub:
        return ua, ub, None
    best = None
    for a in ua[:6]:
        for b in ub[:6]:
            r = difflib.SequenceMatcher(None, a[1], b[1]).quick_ratio()
            if best is None or r > best[0]:
                best = (r, a, b)
    _, a, b = best
    la, lb = a[1].split('\n'), b[1].split('\n')
    i = 0
    while i < min(len(la), len(lb)) and la[i] == lb[i]:
        i += 1
    return ua, ub, (a[0], la, b[0], lb, i)


def compare(da, db, verbose, byop):
    """lines describing the differences of one unit, [] if the same"""
    import dis
    out = []
    ca, cb = da['code'], db['code']
    nsame = 0
    for lab in sorted(set(ca) | set(cb)):
        if lab not in cb or lab not in ca:
            side = 'A' if lab in ca else 'B'
            out.append('  %s: compiled only in %s' % (lab, side))
            continue
        ia, ib = ca[lab], cb[lab]
        diffs = []
        for i in sorted(set(ia) | set(ib), key=int):
            ea, eb = ia.get(i), ib.get(i)
            if ea and eb and copies_key(ea[2]) == copies_key(eb[2]):
                continue
            e = ea or eb
            diffs.append((int(i), e[0], e[1], ea, eb))
        if diffs:
            byop[dis.opname[diffs[0][1]]] += 1
        else:
            nsame += 1
            continue
        out.append('  %s: %d of %d instructions differ' % (
            lab, len(diffs), len(set(ia) | set(ib))))
        for n, (i, op, line, ea, eb) in enumerate(diffs):
            if n and not verbose:
                break
            head = '    @%d %s (line %d): ' % (i, dis.opname[op], line)
            if not ea or not eb:
                out.append(head + 'compiled only in %s' % ('A' if ea else 'B'))
                continue
            ua, ub, fd = first_difference(ea[2], eb[2])
            out.append(head + '%d copies in A, %d in B; %d and %d unmatched' % (
                len(ea[2]), len(eb[2]), len(ua), len(ub)))
            if fd:
                ka, la, kb, lb, j = fd
                for k in range(max(j - 2, 0), j):
                    out.append('        ' + la[k])
                out.append('      A ' + (la[j] if j < len(la) else '<end>') +
                           '   [%s, line %d of %d]' % (ka, j + 1, len(la)))
                out.append('      B ' + (lb[j] if j < len(lb) else '<end>') +
                           '   [%s, line %d of %d]' % (kb, j + 1, len(lb)))
    if da['buffers'] != db['buffers']:
        out.append('  buffers differ: ' + ', '.join(
            '%s %s -> %s' % (k, da['buffers'].get(k), db['buffers'].get(k))
            for k in sorted(set(da['buffers']) | set(db['buffers']))
            if da['buffers'].get(k) != db['buffers'].get(k)))
    if da['iterations'] != db['iterations']:
        out.append('  iterations differ: %d, %d' % (da['iterations'],
                                                     db['iterations']))
    return out, nsame


def main():
    args = sys.argv[1:]
    if args and args[0] == '--child':
        return child(*args[1:4])
    filt = None
    if '-k' in args:
        i = args.index('-k')
        filt = args[i + 1]
        del args[i:i + 2]
    cache = os.path.join(ROOT, 'build', 'samecode')
    if '--cache' in args:
        i = args.index('--cache')
        cache = os.path.abspath(args[i + 1])
        del args[i:i + 2]
    verbose = '-v' in args
    pos = [a for a in args if not a.startswith('-')]
    if len(pos) != 2:
        sys.exit(__doc__)
    (soa, namea), (sob, nameb) = (resolve(p, cache) for p in pos)
    todo = [u for u in units(args) if not filt or filt in u]
    out = os.path.join(cache, 'dumps-%d' % os.getpid())
    os.makedirs(out, exist_ok=True)
    print('A = %s\nB = %s\n%d units' % (namea, nameb, len(todo)))
    jobs = {}
    with concurrent.futures.ThreadPoolExecutor(2) as pool:
        for u in todo:
            for side, so in (('A', soa), ('B', sob)):
                f = os.path.join(out, '%s-%s.json' % (side, u.replace('/', '-')))
                jobs[u, side] = pool.submit(run_child, so, u, f)
        byop = collections.Counter()
        totals = collections.Counter()
        for u in todo:
            (da, ea), (db, eb) = jobs[u, 'A'].result(), jobs[u, 'B'].result()
            if da is None or db is None:
                same_failure = ea == eb
                print('%-28s %s' % (u, 'failed in both: %s' % ea if same_failure
                                    else 'A: %s; B: %s' % (ea or 'ran',
                                                           eb or 'ran')))
                totals['failed in both' if same_failure else 'units differ'] += 1
                continue
            lines, nsame = compare(da, db, verbose, byop)
            ncode = len(set(da['code']) | set(db['code']))
            totals['code objects'] += ncode
            totals['code objects same'] += nsame
            if lines and u in TIMING:
                totals['units same'] += 1
                print('%-28s %d of %d code objects differ, not counted: %s' % (
                    u, ncode - nsame, ncode, TIMING[u]))
                print('\n'.join(lines))
            elif lines:
                totals['units differ'] += 1
                print('%-28s %d of %d code objects differ' % (
                    u, ncode - nsame, ncode))
                print('\n'.join(lines))
            else:
                totals['units same'] += 1
                print('%-28s same (%d code objects, %d iterations)' % (
                    u, ncode, da['iterations']))
    for f in glob.glob(os.path.join(out, '*.json')):
        os.remove(f)
    os.rmdir(out)
    print('\n%d units same, %d differ, %d failed in both; '
          '%d of %d code objects same' % (
              totals['units same'], totals['units differ'],
              totals['failed in both'], totals['code objects same'],
              totals['code objects']))
    if byop:
        print('first differing instruction of each code object, by opcode: '
              + ', '.join(
            '%s %d' % kv for kv in byop.most_common()))
    return 1 if totals['units differ'] else 0


if __name__ == '__main__':
    sys.exit(main())
