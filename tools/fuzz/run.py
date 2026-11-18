"""Differential fuzzer: generated programs (gen.py) run under plain CPython
3.7 and with Ni, compared exactly (doc/BUILD.md, Fuzzing).

    python3 tools/fuzz/run.py [--start S] [-n N] [-j 2] [--timeout 30] [--out DIR]

Each seed's program runs plain, then with Ni, in fresh processes with
PYTHONHASHSEED=0 and a 4 GB address-space limit. Stdout (every call's
result or exception, prints, final globals) and the exit status must match;
hex addresses are masked. A program that fails or times out plain is a
generator problem, counted as invalid and kept in DIR/invalid/. A mismatch
keeps its program and both outputs in DIR/mismatch/SEED.*.
"""
import argparse, concurrent.futures, difflib, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get('NI_ROOT', os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, HERE)
import gen  # noqa: E402

PY37 = os.environ.get('NI_PY37', os.path.expanduser('~/.pyenv/versions/3.7.17/bin/python3.7'))
LIMIT = ['prlimit', '--as=4000000000']
ADDR = re.compile(r'0x[0-9a-fA-F]+')


def run_one(path, mode, timeout):
    """(status, stdout, stderr); status is the exit code or 'timeout'"""
    env = dict(os.environ, PYTHONHASHSEED='0')
    cmd = LIMIT + [PY37, os.path.join(HERE, 'launch.py'), ROOT, mode, path]
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=timeout, env=env)
    except subprocess.TimeoutExpired as e:
        return 'timeout', ADDR.sub('0x?', (e.stdout or b'').decode(errors='replace')), \
            (e.stderr or b'').decode(errors='replace')
    return p.returncode, ADDR.sub('0x?', p.stdout.decode(errors='replace')), \
        p.stderr.decode(errors='replace')


def compare(plain, ni):
    """None if the runs agree, else the kind of mismatch:
    'timeout', 'crash STATUS' or 'output'"""
    if ni[0] == 'timeout' and plain[0] != 'timeout':
        return 'timeout'
    if ni[0] != plain[0]:
        return 'crash %s' % ni[0]
    if ni[1] != plain[1]:
        return 'output'
    return None


def first_diff(a, b, context=2):
    d = list(difflib.unified_diff(a.splitlines(), b.splitlines(), 'plain', 'ni',
                                  n=context, lineterm=''))
    return '\n'.join(d[:40])


def ni_compiled(err):
    m = re.search(r'ni-compiled (\d+)/(\d+)', err)
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
    ni = run_one(work, 'ni', timeout * 3)
    res['compiled'] = ni_compiled(ni[2])
    lines = plain[1].splitlines()
    res['lines'] = len(lines)
    res['raised'] = sum(' raised ' in l for l in lines)
    kind = compare(plain, ni)
    if kind:
        res['kind'] = kind
        base = os.path.join(out, 'mismatch', '%d' % seed)
        os.replace(work, base + '.py')
        with open(base + '.txt', 'w') as f:
            f.write('seed %d: %s\nplain status %s, ni status %s\n\n%s\n\n-- ni stderr (tail):\n%s\n'
                    % (seed, kind, plain[0], ni[0], first_diff(plain[1], ni[1]),
                       ni[2][-3000:]))
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
    a = ap.parse_args()
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
