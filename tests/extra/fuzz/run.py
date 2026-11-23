"""Differential fuzzer: generated programs (gen.py) run under plain CPython
and with LeJIT, compared exactly.

    python3.X tests/extra/fuzz/run.py [--start S] [-n N] [-j 2] [--timeout 30]
        [--out DIR] [--python PATH]

The programs run under PATH (default: the Python running this), with LeJIT
built in place for it. Each seed's program runs plain, then with LeJIT, in
fresh processes with
PYTHONHASHSEED=0 and a 4 GB address-space limit. Stdout (every call's
result or exception, prints, final globals) and the exit status must match;
hex addresses are masked, and so are lines a NaN's hash makes vary between
plain runs (settle). A program that fails or times out plain is a
generator problem, counted as invalid and kept in DIR/invalid/. A mismatch
keeps its program and both outputs in DIR/mismatch/SEED.*. LeJIT's own
diagnostics on stderr (DIAG) count as a mismatch even when the output agrees.
"""
import argparse, concurrent.futures, difflib, os, re, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
import gen  # noqa: E402

# the programs' interpreter; the tools' --python sets it
PYTHON = sys.executable


def set_python(path):
    global PYTHON
    PYTHON = path


LIMIT = ['prlimit', '--as=4000000000']
MAX_OUT = 8 << 20
ADDR = re.compile(r'0x[0-9a-fA-F]+')
# what LeJIT itself writes to stderr when something is wrong (iencoding.h's
# immediate range checks, psyco_fatal_msg, extra_assert, missing metas)
DIAG = re.compile(r'Bad value for|psyco: |lejit: no |nonnull_refcount|XXX opcode|'
                  r'Fatal Python error|\.[ch]:\d+: ')


def run_one(path, mode, timeout, fail=None, shift=0):
    """(status, stdout, stderr); status is the exit code or 'timeout'.
    Output goes to files and only its first MAX_OUT bytes are read: a
    runaway program can print gigabytes before its timeout. fail and shift:
    launch.py's."""
    env = dict(os.environ, PYTHONHASHSEED='0')
    cmd = LIMIT + [PYTHON, os.path.join(HERE, 'launch.py'), ROOT, mode, path,
                   fail or '-', str(shift)]
    with tempfile.TemporaryFile(dir=os.path.dirname(path)) as out, \
            tempfile.TemporaryFile(dir=os.path.dirname(path)) as err:
        try:
            status = subprocess.run(cmd, stdout=out, stderr=err, timeout=timeout,
                                    env=env).returncode
        except subprocess.TimeoutExpired:
            status = 'timeout'
        out.seek(0)
        err.seek(0)
        return (status, ADDR.sub('0x?', out.read(MAX_OUT).decode(errors='replace')),
                err.read(MAX_OUT).decode(errors='replace'))


def compare(plain, jit):
    """None if the runs agree, else the kind of mismatch:
    'timeout', 'crash STATUS', 'output' or 'diagnostic' (DIAG on LeJIT's stderr)"""
    if jit[0] == 'timeout' and plain[0] != 'timeout':
        return 'timeout'
    if jit[0] != plain[0]:
        return 'crash %s' % jit[0]
    if jit[1] != plain[1]:
        return 'output'
    if DIAG.search(jit[2]) and not DIAG.search(plain[2]):
        return 'diagnostic'
    return None


_nan_hash_varies = {}


def nan_hash_varies():
    """Whether the programs' CPython hashes a NaN by its address (3.10
    on): a set holding NaNs, or such a hash, then prints differently from
    run to run, plainly too."""
    if PYTHON not in _nan_hash_varies:
        p = subprocess.run(LIMIT + [PYTHON, '-c', "a, b = float('nan'), float('nan'); print(hash(a) != hash(b))"],
                           stdout=subprocess.PIPE, universal_newlines=True)
        _nan_hash_varies[PYTHON] = p.stdout.strip() == 'True'
    return _nan_hash_varies[PYTHON]


# the floats launch.py keeps before a plain rerun (its SHIFT): a few
# slots and a few pages over, so that a NaN lands elsewhere
SHIFTS = (1, 3, 17, 1007, 2, 5, 64, 4099)


# a line's items, as nan_order compares them
ITEM = re.compile(r"[^\s,'\"{}()\[\]]+")


def nan_order(a, b):
    """Whether lines a and b hold the same items in another order, a NaN
    among them: the order of a set of NaNs follows their addresses, and a
    NaN made at run time often takes the address of a float just freed
    (CPython's float free list), which SHIFTS hardly moves."""
    items = ITEM.findall(a)
    return 'nan' in items and sorted(items) == sorted(ITEM.findall(b))


def settle(path, kind, plain, jit, timeout):
    """kind, or None for an output mismatch on lines that a NaN's hash
    makes vary between plain runs (nan_hash_varies): those that only
    reorder items with a NaN among them (nan_order), and those that differ
    from the first plain run when it runs again with its floats moved
    (SHIFTS), until together they cover every line LeJIT's run differs on;
    those are not compared. A run that prints another number of lines
    leaves nothing to compare line by line: the mismatch stands."""
    if kind != 'output' or not nan_hash_varies():
        return kind
    first, theirs = plain[1].splitlines(), jit[1].splitlines()
    if len(theirs) != len(first):
        return kind
    varies = [nan_order(a, b) for a, b in zip(first, theirs)]
    if all(v or a == b for v, a, b in zip(varies, first, theirs)):
        return None
    for shift in SHIFTS:
        again = run_one(path, 'plain', timeout, shift=shift)[1].splitlines()
        if len(again) != len(first):
            return kind
        varies = [v or a != b for v, a, b in zip(varies, first, again)]
        if all(v or a == b for v, a, b in zip(varies, first, theirs)):
            return None
    return kind


def first_diff(a, b, context=2):
    d = list(difflib.unified_diff(a.splitlines(), b.splitlines(), 'plain', 'lejit',
                                  n=context, lineterm=''))
    return '\n'.join(d[:40])


def lejit_compiled(err):
    m = re.search(r'lejit-compiled (\d+)/(\d+)', err)
    return (int(m.group(1)), int(m.group(2))) if m else (0, 0)


def fuzz_seed(seed, out, timeout):
    src = gen.generate(seed)
    work = os.path.join(out, 'work', '%d.py' % seed)
    with open(work, 'w') as f:
        f.write(src)
    plain = run_one(work, 'plain', timeout)
    res = {'seed': seed, 'kind': None, 'compiled': (0, 0), 'lines': 0, 'raised': 0}
    if plain[0] != 0:
        res['kind'] = 'invalid'
        os.replace(work, os.path.join(out, 'invalid', '%d.py' % seed))
        with open(os.path.join(out, 'invalid', '%d.txt' % seed), 'w') as f:
            f.write('status %s\n%s' % (plain[0], plain[2][-2000:]))
        return res
    jit = run_one(work, 'lejit', timeout * 3)
    res['compiled'] = lejit_compiled(jit[2])
    lines = plain[1].splitlines()
    res['lines'] = len(lines)
    res['raised'] = sum(' raised ' in l for l in lines)
    kind = settle(work, compare(plain, jit), plain, jit, timeout)
    if kind:
        res['kind'] = kind
        base = os.path.join(out, 'mismatch', '%d' % seed)
        os.replace(work, base + '.py')
        with open(base + '.txt', 'w') as f:
            f.write('seed %d: %s\nplain status %s, LeJIT status %s\n\n%s\n\n-- LeJIT stderr (tail):\n%s\n'
                    % (seed, kind, plain[0], jit[0], first_diff(plain[1], jit[1]),
                       jit[2][-3000:]))
    else:
        os.remove(work)
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--start', type=int, default=1)
    ap.add_argument('-n', type=int, default=100)
    ap.add_argument('-j', type=int, default=2)
    ap.add_argument('--timeout', type=int, default=30)
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'fuzz'))
    ap.add_argument('--python', default=sys.executable, help='the programs\' CPython')
    a = ap.parse_args()
    set_python(a.python)
    for d in ('work', 'invalid', 'mismatch'):
        os.makedirs(os.path.join(a.out, d), exist_ok=True)
    t0 = time.time()
    stats = {'run': 0, 'invalid': 0, 'mismatch': 0, 'fn': 0, 'fn_compiled': 0,
             'lines': 0, 'raised': 0}
    log = open(os.path.join(a.out, 'run-%d-%d.log' % (a.start, a.start + a.n - 1)), 'w')
    with concurrent.futures.ThreadPoolExecutor(a.j) as ex:
        futs = [ex.submit(fuzz_seed, s, a.out, a.timeout)
                for s in range(a.start, a.start + a.n)]
        for fu in concurrent.futures.as_completed(futs):
            r = fu.result()
            stats['run'] += 1
            if r['kind'] == 'invalid':
                stats['invalid'] += 1
            elif r['kind']:
                stats['mismatch'] += 1
                line = 'MISMATCH seed %d: %s' % (r['seed'], r['kind'])
                print(line, flush=True)
                log.write(line + '\n')
                log.flush()
            stats['fn_compiled'] += r['compiled'][0]
            stats['fn'] += r['compiled'][1]
            stats['lines'] += r['lines']
            stats['raised'] += r['raised']
    summary = ('seeds %d..%d: %d run, %d invalid, %d mismatches; %d/%d functions '
               'compiled; %d of %d calls raised; %.0f s' % (
                   a.start, a.start + a.n - 1, stats['run'], stats['invalid'],
                   stats['mismatch'], stats['fn_compiled'], stats['fn'],
                   stats['raised'], stats['lines'], time.time() - t0))
    print(summary)
    log.write(summary + '\n')
    return 1 if stats['mismatch'] else 0


if __name__ == '__main__':
    sys.exit(main())
