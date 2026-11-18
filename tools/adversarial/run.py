"""Ni against plain CPython 3.7 on programs chosen for what Ni does badly
(tools/adversarial/*.py; each one's docstring names its target). Every
benchmark runs in fresh processes for each mode:

  cold   wall time of a process that imports the benchmark, runs main(N)
         once and prints the result: start-up, imports and compiling
         included (best of --cold-runs)
  first  the first main(N) of a process, after the import
  warm   best of --runs calls of main(N), after --warm more to warm up
  rss    peak resident memory of that process; grow is the part reached
         after the warm-up (code and objects that are never freed)

Ratios are Ni / CPython, so above 1 is worse for Ni, and rows are sorted
by their worst ratio. Every result of main(N), in every process, must
equal CPython's first one.

    python3.7 tools/adversarial/run.py                  # table
    python3.7 tools/adversarial/run.py -k gen           # names containing 'gen'
    python3.7 tools/adversarial/run.py --plain --scale 0.01
                                       # CPython only: does each run the same?

Options: --scale F multiplies each N (at least 1), --warm W (3), --runs R
(5), --cold-runs C (3), --timeout S per process (600). Use a release build,
as for tools/bench.py.
"""
import glob
import importlib.util
import os
import resource
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MEMORY_LIMIT = 4000000000

COLD = '''import sys, resource, importlib.util
%s
spec = importlib.util.spec_from_file_location(%r, %r)
mod = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mod
spec.loader.exec_module(mod)
print(repr(mod.main(max(1, int(mod.N * %r)))))
sys.stdout.flush()
sys.stderr.write('RSS %%d\\n' %% resource.getrusage(resource.RUSAGE_SELF).ru_maxrss)
'''


def enable_ni(mode):
    if mode == 'ni':
        sys.path.insert(0, ROOT)
        import ni  # noqa: F401


def module_name(path):
    return 'adversarial_' + os.path.splitext(os.path.basename(path))[0]


def load(path):
    spec = importlib.util.spec_from_file_location(module_name(path), path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def maxrss():
    return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def child(mode, path, scale, warm, runs):
    enable_ni(mode)
    mod = load(path)
    n = max(1, int(mod.N * scale))
    t = time.perf_counter()
    first_result = repr(mod.main(n))
    first = time.perf_counter() - t
    results = {first_result}
    for _ in range(warm):
        results.add(repr(mod.main(n)))
    rss_warm = maxrss()
    times = []
    for _ in range(runs):
        t = time.perf_counter()
        r = mod.main(n)
        times.append(time.perf_counter() - t)
        results.add(repr(r))
    print('RESULT ' + repr({'first': first, 'warm': min(times), 'rss': maxrss(),
                            'grow': maxrss() - rss_warm, 'result': first_result,
                            'stable': len(results) == 1}))


def limit_memory():
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))


def spawn(args, timeout):
    env = dict(os.environ, PYTHONHASHSEED='0')
    t = time.perf_counter()
    try:
        p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=HERE,
                           env=env, timeout=timeout, preexec_fn=limit_memory,
                           universal_newlines=True)
    except subprocess.TimeoutExpired:
        return None, 'timeout after %ds' % timeout, 0.0
    dt = time.perf_counter() - t
    if p.returncode:
        return None, (p.stderr.strip().splitlines() or ['exit %d' % p.returncode])[-1], dt
    return p.stdout, None, dt


def run_cold(mode, path, opts):
    code = COLD % ('sys.path.insert(0, %r); import ni' % ROOT if mode == 'ni' else '',
                   module_name(path), path, opts['scale'])
    best = None
    out = None
    for _ in range(opts['cold_runs']):
        stdout, err, dt = spawn([sys.executable, '-c', code], opts['timeout'])
        if err:
            return None, err
        lines = stdout.strip().splitlines()
        if out is not None and lines[-1:] != out:
            return None, 'cold runs differ'
        out = lines[-1:]
        best = dt if best is None else min(best, dt)
    return {'cold': best, 'cold_result': out[0] if out else ''}, None


def run_warm(mode, path, opts):
    stdout, err, _ = spawn([sys.executable, os.path.abspath(__file__), '--child', mode, path,
                            repr(opts['scale']), str(opts['warm']), str(opts['runs'])],
                           opts['timeout'])
    if err:
        return None, err
    lines = [l for l in stdout.splitlines() if l.startswith('RESULT ')]
    if not lines:
        return None, 'no result'
    return eval(lines[-1][7:]), None


def measure(mode, path, opts):
    cold, err = run_cold(mode, path, opts)
    if err:
        return None, err
    warm, err = run_warm(mode, path, opts)
    if err:
        return None, err
    warm.update(cold)
    return warm, None


def problems(r, reference):
    notes = []
    if not r['stable']:
        notes.append('results vary between calls')
    if r['cold_result'] != r['result']:
        notes.append('cold run differs')
    if reference is not None and r['result'] != reference['result']:
        notes.append('RESULTS DIFFER from CPython')
    return notes


def ratio(a, b):
    return a / b if b > 0 else float('inf')


def ms(t):
    return '%8.1f' % (t * 1e3)


def mb(kb):
    return '%6.1f' % (kb / 1024)


def plain_table(names, results):
    print('%-22s %8s %8s %8s %6s %6s  %s' % ('CPython only', 'cold ms', 'first ms', 'warm ms',
                                             'RSS MB', 'grow', ''))
    for name in names:
        r, err = results[name]
        if r is None:
            print('%-22s failed: %s' % (name, err[:80]))
            continue
        print('%-22s %s %s %s %s %s  %s' % (name, ms(r['cold']), ms(r['first']), ms(r['warm']),
                                            mb(r['rss']), mb(r['grow']),
                                            ', '.join(problems(r, None)) or 'ok'))


def compare_table(names, results):
    rows = []
    for name in names:
        (p, perr), (q, qerr) = results[name]
        if p is None or q is None:
            rows.append((float('inf'), name, None, 'CPython failed: %s' % perr if p is None
                         else 'Ni failed: %s' % qerr))
            continue
        r = {k: ratio(q[k], p[k]) for k in ('cold', 'first', 'warm', 'rss')}
        notes = problems(q, p) + ['CPython: ' + n for n in problems(p, None)]
        worst = max(r.values())
        rows.append((float('inf') if notes else worst, name, (p, q, r, worst), ', '.join(notes)))
    rows.sort(key=lambda row: -row[0])
    print('%-22s %24s %24s %24s %18s %13s %6s' % ('', 'cold (ms)', 'first (ms)', 'warm (ms)',
                                                  'peak RSS (MB)', 'grow (MB)', 'worst'))
    print('%-22s' % '' + ' %8s %8s %6s' % ('CPython', 'Ni', 'Ni/C') * 3
          + ' %6s %6s %5s %6s %6s' % ('CPy', 'Ni', 'Ni/C', 'CPy', 'Ni'))
    logs = {'cold': [], 'warm': []}
    for _, name, data, note in rows:
        if data is None:
            print('%-22s %s' % (name, note[:100]))
            continue
        p, q, r, worst = data
        for k in logs:
            logs[k].append(r[k])
        print('%-22s %s %s %6.2f %s %s %6.2f %s %s %6.2f %s %s %5.2f %s %s %6.2f  %s' % (
            name, ms(p['cold']), ms(q['cold']), r['cold'], ms(p['first']), ms(q['first']),
            r['first'], ms(p['warm']), ms(q['warm']), r['warm'], mb(p['rss']), mb(q['rss']),
            r['rss'], mb(p['grow']), mb(q['grow']), worst, note))
    for k, values in sorted(logs.items()):
        if values:
            g = 1.0
            for v in values:
                g *= v
            print('geometric mean of %s Ni/CPython: %.2f (%d benchmarks)' % (
                k, g ** (1 / len(values)), len(values)))


def option(args, name, default, kind):
    if name in args:
        return kind(args[args.index(name) + 1])
    return default


def main(args):
    opts = {'scale': option(args, '--scale', 1.0, float),
            'warm': option(args, '--warm', 3, int),
            'runs': option(args, '--runs', 5, int),
            'cold_runs': option(args, '--cold-runs', 3, int),
            'timeout': option(args, '--timeout', 600, int)}
    paths = sorted(p for p in glob.glob(os.path.join(HERE, '*.py'))
                   if os.path.basename(p) != 'run.py')
    k = option(args, '-k', None, str)
    if k:
        paths = [p for p in paths if k in os.path.basename(p)]
    names = [os.path.splitext(os.path.basename(p))[0] for p in paths]
    plain = '--plain' in args
    results = {}
    for name, path in zip(names, paths):
        if plain:
            results[name] = measure('plain', path, opts)
        else:
            results[name] = (measure('plain', path, opts), measure('ni', path, opts))
    if not names:
        print('(no benchmark matches)')
    elif plain:
        plain_table(names, results)
    else:
        compare_table(names, results)


if __name__ == '__main__':
    if sys.argv[1:2] == ['--child']:
        child(sys.argv[2], sys.argv[3], float(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]))
    else:
        main(sys.argv[1:])
