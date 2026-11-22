"""Targeted fuzzer: programs combined from the building blocks (blocks/) by a
covering array, run plain and with LeJIT. Each block makes LeJIT commit to
one fact (a promoted type, a guard, a virtual object, a merge point's cap)
and later makes it wrong (a __class__ change, a rebound global, a size past
a limit).

    python3.X tests/extra/fuzz/combine.py [--strength 2|3] [--limit N] [--seed S]
        [-j 2] [--timeout 30] [--out DIR] [--rows A:B] [--fail SITE:N]
        [--minimise] [--blocks a,b,...] [--show ROW] [--list] [--python PATH]

A program holds three blocks, outer to inner: each block's P_run calls the
next one's from its hook, the innermost calls ident. The factors are the
block at each position and the round each block's invalidation runs in
(BREAKS: 1, 3, 6 or never); the covering array of the given strength
covers every pair (or triple) of factor values at least once, greedily
(AETG), so the rows are deterministic for a seed. The slots' values come
from a generator seeded by (seed, row). --show prints one row's program,
--list the blocks. Rows of triples reach about what pairs do: strength 3
is for interactions, not coverage.

Oracles, each a mismatch kind (run.py:compare, plus):
  leak        after the rounds, 109 warm calls then 100 more through the same
              function: LeJIT's growth in sys.getallocatedblocks() past the
              plain run's by LEAK_SLACK
              (not for programs holding a block with `leaks`)
  fail-*      --fail SITE:N: a third run with lejit._core._fail_after(SITE, N)
              (checked builds) must print what the first LeJIT run printed
Triage: a mismatch is reduced to the fewest of its blocks that still show
the same kind and detail (or takes an earlier reduction's blocks, when
they are all in it), and its signature is kind|blocks|detail (detail: the
diagnostic or crash line, or on the first differing line the exception
names, else the first differing words with digits masked). Signatures matching a pattern in known.txt are marked with its
note. DIR/mismatch/ROW.py and .txt keep each program and its diff;
DIR/buckets.txt lists the signatures, new ones first; --minimise runs
minimise.py on the first program of each new bucket after the run.
"""
import argparse, collections, concurrent.futures, difflib, fnmatch, itertools, json, os, random, re, \
    subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import blocks as blockmod  # noqa: E402
import run  # noqa: E402

ROUNDS = 10
BREAK_ROUNDS = [1, 3, 6, None]
POSITIONS = 3
LEAK_SLACK = 100
CANDIDATES = 8

DRIVER = '''
ROUNDS = %(rounds)d
BREAKS = %(breaks)s
for _r in range(ROUNDS):
    for _k, _b in BREAKS:
        if _k == _r:
            try:
                _b()
            except BaseException as _e:
                print(_r, 'break raised', type(_e).__name__)
    try:
        _txt = repr(%(outer)s(_r))
    except BaseException as _e:
        _txt = 'raised %%s: %%s' %% (type(_e).__name__, _e)
    print(_r, _txt[:3000])
'''
# The leak oracle's functions come before the rounds and the warm calls run
# through the same function as the measured ones, so the measurement adds no
# name to the program's globals and compiles no code of its own: a new global
# can resize the keys and fail every cached global's guard of the module, and
# what that recompiles is not a leak.
LEAK_DEFS = '''
def _leak_call():
    try:
        %(outer)s(ROUNDS)
    except BaseException:
        pass
def _leak_calls(n):
    import gc
    gc.collect()
    before = sys.getallocatedblocks()
    for i in range(n):
        _leak_call()
    gc.collect()
    return sys.getallocatedblocks() - before
'''
LEAK_RUN = '''
_leak_calls(109)
sys.stderr.write('fuzz-leak %d\\n' % _leak_calls(100))
'''


def covering_array(levels, strength, rng, limit=None):
    """rows (tuples of level indices) covering every strength-tuple of
    factor values; AETG: each row starts from an uncovered tuple and fills
    the other factors one at a time, each with the level that covers the
    most new tuples, the best of CANDIDATES tries kept"""
    k = len(levels)
    combos = list(itertools.combinations(range(k), strength))
    uncovered = set()
    for c in combos:
        for vals in itertools.product(*(range(levels[i]) for i in c)):
            uncovered.add((c, vals))
    pool = sorted(uncovered)
    rows = []
    by_factor = {f: [c for c in combos if f in c] for f in range(k)}

    def gain(row, f, v):
        n = 0
        row[f] = v
        for c in by_factor[f]:
            if all(row[i] is not None for i in c) and (c, tuple(row[i] for i in c)) in uncovered:
                n += 1
        row[f] = None
        return n

    while uncovered and (limit is None or len(rows) < limit):
        best, best_n = None, -1
        for _ in range(CANDIDATES):
            while pool[-1] not in uncovered:
                pool.pop()
            c, vals = pool[rng.randrange(len(pool))] if rng.random() < 0.5 else pool[-1]
            if (c, vals) not in uncovered:
                c, vals = pool[-1]
            row = [None] * k
            for i, v in zip(c, vals):
                row[i] = v
            rest = [f for f in range(k) if row[f] is None]
            rng.shuffle(rest)
            for f in rest:
                scores = [(gain(row, f, v), rng.random(), v) for v in range(levels[f])]
                row[f] = max(scores)[2]
            n = sum(1 for cc in combos if (cc, tuple(row[i] for i in cc)) in uncovered)
            if n > best_n:
                best, best_n = tuple(row), n
        rows.append(best)
        for cc in combos:
            uncovered.discard((cc, tuple(best[i] for i in cc)))
    return rows


class Program:
    def __init__(self, idx, blocks, breaks, seed):
        self.idx, self.blocks, self.breaks, self.seed = idx, blocks, breaks, seed

    def source(self, keep=None):
        """the program, with only the positions in keep (default all)"""
        keep = range(len(self.blocks)) if keep is None else keep
        rng = random.Random('%d:%d' % (self.seed, self.idx))
        parts, chosen = [], []
        # every position draws its slots, kept or not, so a reduced program
        # has the same values as the full one
        drawn = {}
        for pos in reversed(range(len(self.blocks))):
            drawn[pos] = rng.getstate()
            for k, v in sorted(self.blocks[pos].slots.items()):
                rng.choice(v)
        kept = [p for p in range(len(self.blocks)) if p in keep]
        nxt = 'ident'
        for pos in reversed(kept):
            b = self.blocks[pos]
            r2 = random.Random()
            r2.setstate(drawn[pos])
            src, ch = b.instantiate('b%d' % pos, nxt, r2)
            parts.append('# block %d: %s %s\n%s\n' % (pos, b.name, ch, src))
            nxt = 'b%d_run' % pos
        breaks = [(self.breaks[p], 'b%d_break' % p) for p in kept if self.breaks[p] is not None]
        head = ('# targeted fuzz program: seed %d row %d: %s\nimport sys\n\ndef ident(r):\n'
                '    return r\n\n' % (self.seed, self.idx,
                                      ' > '.join(self.blocks[p].name for p in kept)))
        brk = '[%s]' % ', '.join('(%d, %s)' % b for b in breaks)
        leak = not any(self.blocks[p].leaks for p in kept)
        text = head + '\n'.join(parts)
        if leak:
            text += LEAK_DEFS % {'outer': nxt}
        text += DRIVER % {'rounds': ROUNDS, 'breaks': brk, 'outer': nxt}
        if leak:
            text += LEAK_RUN
        return text

    def names(self, keep=None):
        keep = range(len(self.blocks)) if keep is None else keep
        return [self.blocks[p].name for p in keep]


def leak_of(err):
    m = re.search(r'fuzz-leak (-?\d+)', err)
    return int(m.group(1)) if m else None


def judge(path, timeout, fail=None):
    """(kind or None, plain, LeJIT run)"""
    plain = run.run_one(path, 'plain', timeout)
    if plain[0] != 0:
        return 'invalid', plain, None
    jit = run.run_one(path, 'lejit', timeout * 3)
    kind = run.settle(path, run.compare(plain, jit), plain, jit, timeout)
    if kind is None:
        lp, ln = leak_of(plain[2]), leak_of(jit[2])
        if lp is not None and ln is not None and ln - lp > LEAK_SLACK:
            kind = 'leak'
    if kind is None and fail:
        nf = run.run_one(path, 'lejit', timeout * 3, fail=fail)
        k2 = run.compare(jit, nf)
        if k2:
            kind, jit = 'fail-' + k2, nf
    return kind, plain, jit


MASK = [(re.compile(r'0x[0-9a-fA-F]+|0x\?'), '0x?'), (re.compile(r'\b\d+\b'), 'N')]


def detail(kind, plain, jit):
    if kind == 'diagnostic' or kind.startswith('crash') or kind.endswith('crash'):
        lines = [l for l in jit[2].splitlines() if run.DIAG.search(l)]
        if not lines:
            lines = [l for l in jit[2].splitlines() if l.strip() and 'lejit-compiled' not in l][-1:]
        d = lines[0] if lines else ''
        d = re.sub(r'0x[0-9a-fA-F]+', '0x?', d)
        return d.strip()[:120]
    if kind in ('output', 'fail-output'):
        for a, b in zip(plain[1].splitlines(), jit[1].splitlines()):
            if a != b:
                ea = sorted(set(re.findall(r'\b(\w+(?:Error|Exception|Iteration))\b', a)))
                eb = sorted(set(re.findall(r'\b(\w+(?:Error|Exception|Iteration))\b', b)))
                if ea != eb:
                    return 'exceptions %s vs %s' % (','.join(ea) or '-', ','.join(eb) or '-')
                wa, wb = a.split(), b.split()
                for op, i1, i2, j1, j2 in difflib.SequenceMatcher(None, wa, wb).get_opcodes():
                    if op != 'equal':
                        da, db = (re.sub(r'\d+', 'N', ' '.join(x))[:40] for x in (wa[i1:i2], wb[j1:j2]))
                        return 'values %r vs %r' % (da, db)
                return 'values'
        return 'length'
    return ''


def load_known():
    known = []
    path = os.path.join(HERE, 'known.txt')
    for line in open(path):
        line = line.rstrip('\n')
        if line.strip() and not line.startswith('#'):
            pat, bug = line.split('\t')[:2]
            known.append((pat, bug))
    return known


def known_bug(sig, known):
    for pat, bug in known:
        if fnmatch.fnmatchcase(sig, pat):
            return bug
    return None


class Runner:
    def __init__(self, a, programs):
        self.a = a
        self.programs = programs
        self.reduced = {}      # (kind, blocks, detail) -> positions kept
        self.found = {}        # (kind, detail) -> [blocks kept], from reductions run

    def write(self, prog, keep, tag):
        path = os.path.join(self.a.out, 'work', '%s.py' % tag)
        with open(path, 'w') as f:
            f.write(prog.source(keep))
        return path

    def one(self, prog):
        path = self.write(prog, None, '%d' % prog.idx)
        kind, plain, jit = judge(path, self.a.timeout, self.a.fail)
        res = {'row': prog.idx, 'blocks': prog.names(), 'kind': kind,
               'compiled': run.lejit_compiled(jit[2]) if jit else (0, 0)}
        if kind is None or kind == 'invalid':
            if kind == 'invalid':
                os.replace(path, os.path.join(self.a.out, 'invalid', '%d.py' % prog.idx))
                with open(os.path.join(self.a.out, 'invalid', '%d.txt' % prog.idx), 'w') as f:
                    f.write('status %s\n%s' % (plain[0], plain[2][-3000:]))
            else:
                os.remove(path)
            return res
        det = detail(kind, plain, jit)
        crude = (kind, tuple(sorted(prog.names())), det)
        keep = list(range(len(prog.blocks)))
        # a reduction already found for this kind and detail whose blocks
        # are all here is taken as this one's
        names = prog.names()
        known_sets = self.found.get((kind, det), [])
        reuse = next((ks for ks in known_sets if all(names.count(n) >= ks.count(n) for n in ks)), None)
        if crude in self.reduced:
            keep = self.reduced[crude]
        elif reuse is not None:
            keep, used = [], list(reuse)
            for i, n in enumerate(names):
                if n in used:
                    used.remove(n)
                    keep.append(i)
            self.reduced[crude] = keep
        else:
            # the fewest blocks that still show the same kind and detail
            for size in range(1, len(prog.blocks)):
                found = None
                for sub in itertools.combinations(range(len(prog.blocks)), size):
                    p2 = self.write(prog, sub, '%d-%s' % (prog.idx, ''.join(map(str, sub))))
                    k2, pl2, jit2 = judge(p2, self.a.timeout, self.a.fail)
                    os.remove(p2)
                    if k2 == kind and detail(k2, pl2, jit2) == det:
                        found = list(sub)
                        break
                if found:
                    keep = found
                    break
            self.reduced[crude] = keep
            self.found.setdefault((kind, det), []).append(prog.names(keep))
        sig = '%s|%s|%s' % (kind, '+'.join(sorted(prog.names(keep))), det)
        res['sig'] = sig
        base = os.path.join(self.a.out, 'mismatch', '%d' % prog.idx)
        os.replace(path, base + '.py')
        with open(base + '.reduced.py', 'w') as f:
            f.write(prog.source(keep))
        with open(base + '.txt', 'w') as f:
            f.write('row %d: %s\nsignature %s\nplain status %s, LeJIT status %s\n\n%s\n\n'
                    '-- LeJIT stderr (tail):\n%s\n' % (
                        prog.idx, kind, sig, plain[0], jit[0],
                        run.first_diff(plain[1], jit[1]), jit[2][-3000:]))
        return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--strength', type=int, default=2, choices=[2, 3])
    ap.add_argument('--limit', type=int)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('-j', type=int, default=2)
    ap.add_argument('--timeout', type=int, default=30)
    ap.add_argument('--out', default=os.path.join(run.ROOT, 'build', 'fuzz-targeted'))
    ap.add_argument('--rows', help='A:B, only rows A to B-1')
    ap.add_argument('--fail', help='SITE:N, a third run with lejit._core._fail_after(SITE, N)')
    ap.add_argument('--minimise', action='store_true')
    ap.add_argument('--blocks', help='only these blocks, comma-separated')
    ap.add_argument('--show', type=int, help='print one row\'s program')
    ap.add_argument('--list', action='store_true', help='list the blocks')
    ap.add_argument('--python', default=sys.executable, help='the programs\' CPython')
    a = ap.parse_args()
    run.set_python(a.python)
    blocks = blockmod.load()
    if a.list:
        for b in blocks:
            print('%-24s %-11s %s' % (b.name, b.area, b.invalidation))
        return 0
    if a.blocks:
        want = a.blocks.split(',')
        blocks = [b for b in blocks if b.name in want]
    levels = [len(blocks)] * POSITIONS + [len(BREAK_ROUNDS)] * POSITIONS
    t0 = time.time()
    rows = covering_array(levels, a.strength, random.Random(a.seed), a.limit)
    programs = [Program(i, [blocks[r[p]] for p in range(POSITIONS)],
                        [BREAK_ROUNDS[r[POSITIONS + p]] for p in range(POSITIONS)], a.seed)
                for i, r in enumerate(rows)]
    if a.show is not None:
        sys.stdout.write(programs[a.show].source())
        return 0
    if a.rows:
        lo, hi = map(int, a.rows.split(':'))
        programs = programs[lo:hi]
    for d in ('work', 'invalid', 'mismatch'):
        os.makedirs(os.path.join(a.out, d), exist_ok=True)
    print('%d blocks, strength %d: %d rows (%.0f s to build)' % (
        len(blocks), a.strength, len(rows), time.time() - t0), flush=True)
    known = load_known()
    runner = Runner(a, programs)
    buckets = collections.OrderedDict()
    stats = collections.Counter()
    log = open(os.path.join(a.out, 'run.log'), 'w')
    with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
        for r in ex.map(runner.one, programs):
            stats['run'] += 1
            stats['fn'] += r['compiled'][1]
            stats['fn_compiled'] += r['compiled'][0]
            if r['kind'] == 'invalid':
                stats['invalid'] += 1
                line = 'INVALID row %d: %s' % (r['row'], ' > '.join(r['blocks']))
            elif r['kind']:
                stats['mismatch'] += 1
                buckets.setdefault(r['sig'], []).append(r['row'])
                bug = known_bug(r['sig'], known)
                line = 'MISMATCH row %d: %s [%s]' % (r['row'], r['sig'], bug or 'new')
            else:
                continue
            print(line, flush=True)
            log.write(line + '\n')
            log.flush()
    elapsed = time.time() - t0
    with open(os.path.join(a.out, 'buckets.txt'), 'w') as f:
        order = sorted(buckets.items(), key=lambda kv: (known_bug(kv[0], known) is not None, kv[0]))
        for sig, rws in order:
            f.write('%s\t%s\t%d\t%s\n' % (known_bug(sig, known) or 'NEW', sig, len(rws),
                                          ' '.join(map(str, rws[:20]))))
    new = [s for s in buckets if not known_bug(s, known)]
    summary = ('%d programs: %d invalid, %d mismatches in %d buckets (%d new); %d/%d functions '
               'compiled; %.1f min' % (stats['run'], stats['invalid'], stats['mismatch'],
                                       len(buckets), len(new), stats['fn_compiled'], stats['fn'],
                                       elapsed / 60))
    print(summary)
    log.write(summary + '\n')
    log.close()
    if a.minimise:
        for sig in new:
            row = buckets[sig][0]
            src = os.path.join(a.out, 'mismatch', '%d.reduced.py' % row)
            if sig.startswith('leak') or sig.startswith('fail-'):
                continue
            print('minimising row %d (%s)' % (row, sig), flush=True)
            subprocess.run([sys.executable, os.path.join(HERE, 'minimise.py'), src,
                            '-o', src[:-len('.reduced.py')] + '.min.py',
                            '--timeout', str(a.timeout), '--python', a.python])
    return 1 if new else 0


if __name__ == '__main__':
    sys.exit(main())
