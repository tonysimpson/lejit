"""Ni against plain CPython 3.7 on programs chosen for what Ni does badly
(tools/adversarial/*.py; each one's docstring names its target and what
its variant 1 changes).

A run is one fresh process: start the interpreter (and Ni), import the
benchmark, call main(n, 0) --train times to train on variant 0, then time
main(n, 1) --iters times on variant 1, whose data (and often types) differ.
Every call is timed; nothing is best-of. Each benchmark has --processes runs
per mode, interleaved CPython, Ni, CPython, ..., at each run length:

  short   n such that the default 20 calls do about 10 ms of work on CPython
  medium  about 1 s
  long    about 2 minutes (--long; n extrapolated from medium's)

n comes from sizes.txt, written on plain CPython by --calibrate. Per mode the
table gives the median over processes of

  total   wall time of the process: start-up, imports, compiling, all calls
  first   the first call (variant 0, so compiling included)
  median  median of the timed calls (variant 1)
  worst   slowest timed call (the first variant-1 call pays for new types)

and, for Ni, the spread of the per-process medians between processes
(proc%: (max - min) / median) and within a process (iter%: interquartile
range / median, median over processes). shape classifies each process's
timed calls by thirds, after Barrett et al., "Virtual Machine Warmup Blows
Hot and Cold" (OOPSLA 2017), with medians 10% apart counting as different:
S slowdown (a later third slower than an earlier one), N no steady state
(the last third still faster than the middle), W warm-up (the first third
slower, then flat), F flat. One letter per process (up to five); ! marks S
or N, which also counts as unsettled in the score.

Every call's outcome is compared with CPython's, call by call: the repr of
the result or the exception's type, message and traceback; stdout and
stderr written during the call; warnings (category, file, line, message,
all recorded); the benchmark's ORDER list if it has one (finaliser and
callback order, after gc.collect()); and with --trace-midway, the events a
sys.settrace tracer installed from the middle of the timed calls saw. The
process's own stdout and stderr are compared too. Any difference, a crash,
a timeout or a result that varies between processes is a failure.

The score leads with losses: per measure, how many benchmarks Ni is slower
on (and how many by over 10%), the failures and wrong answers, then the
five worst ratios. Geometric means come last, with each failure counted as
a ratio of FAIL_RATIO, not left out. Ratios are Ni / CPython, so above 1
is worse for Ni.

    python3.7 tools/adversarial/run.py                  # short and medium
    python3.7 tools/adversarial/run.py -k gen --long    # names containing 'gen'
    python3.7 tools/adversarial/run.py --plain          # CPython only
    python3.7 tools/adversarial/run.py --calibrate [-k NAME]   # rewrite sizes.txt

Options: --length short|medium|long (repeatable; default short and medium),
--long, --processes P (5), --train T (5), --iters I (15), --scale F
(multiplies each n), --timeout S per process (600), --trace-midway,
--data FILE (also write the results one line per benchmark and measure).
Use a release build, as for tools/bench.py.
"""
import gc
import glob
import hashlib
import importlib.util
import io
import json
import math
import os
import resource
import statistics
import subprocess
import sys
import tempfile
import time
import traceback
import warnings

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
SIZES = os.path.join(HERE, 'sizes.txt')
MEMORY_LIMIT = 4000000000
LENGTHS = {'short': 0.010, 'medium': 1.0, 'long': 120.0}
CALIBRATION_CALLS = 20
SETTLED = 1.10
FAIL_RATIO = 10.0
TEXT_LIMIT = 100000


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


def call(mod, n, variant, tracing):
    """Time one main(n, variant) and return (seconds, text of everything it did)."""
    events = {}

    def tracer(frame, event, arg):
        key = (frame.f_code.co_name, event, frame.f_lineno)
        events[key] = events.get(key, 0) + 1
        return tracer

    out, err = io.StringIO(), io.StringIO()
    saved = sys.stdout, sys.stderr
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        sys.stdout, sys.stderr = out, err
        if tracing:
            sys.settrace(tracer)
        t = time.perf_counter()
        try:
            outcome = 'result ' + repr(mod.main(n, variant))
        except BaseException as e:
            outcome = 'raised ' + ''.join(traceback.format_exception(type(e), e, e.__traceback__))
        dt = time.perf_counter() - t
        sys.settrace(None)
        sys.stdout, sys.stderr = saved
    parts = [outcome]
    if out.getvalue():
        parts.append('stdout\n' + out.getvalue())
    if err.getvalue():
        parts.append('stderr\n' + err.getvalue())
    if caught:
        counts = {}
        for w in caught:
            key = '%s:%d %s: %s' % (os.path.basename(w.filename), w.lineno, w.category.__name__,
                                    w.message)
            counts[key] = counts.get(key, 0) + 1
        parts.append('warnings\n' + '\n'.join('%d x %s' % (c, k) for k, c in sorted(counts.items())))
    order = getattr(mod, 'ORDER', None)
    if order is not None:
        gc.collect()
        parts.append('order\n' + '\n'.join(map(str, order)))
        del order[:]
    if tracing:
        parts.append('trace\n' + '\n'.join('%s %s %d: %d' % (k + (c,))
                                           for k, c in sorted(events.items())))
    return dt, '\n'.join(parts)


def child(mode, path, n, train, iters, trace_from, out_path):
    t = time.perf_counter()
    enable_ni(mode)
    mod = load(path)
    imported = time.perf_counter() - t
    times, digests, texts = [], [], {}
    rss_trained = 0
    for k in range(train + iters):
        tracing = trace_from >= 0 and k >= train + trace_from
        dt, text = call(mod, n, 0 if k < train else 1, tracing)
        digest = hashlib.sha1(text.encode('utf-8', 'backslashreplace')).hexdigest()[:16]
        times.append(dt)
        digests.append(digest)
        texts.setdefault(digest, text[:TEXT_LIMIT])
        if k == train - 1:
            rss_trained = maxrss()
    with open(out_path, 'w') as f:
        json.dump({'import': imported, 'times': times, 'digests': digests, 'texts': texts,
                   'rss': maxrss(), 'grow': maxrss() - rss_trained}, f)


def limit_memory():
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))


def spawn(args, timeout):
    """Run a child; return (stdout, stderr, wall seconds, error or None)."""
    env = dict(os.environ, PYTHONHASHSEED='0')
    t = time.perf_counter()
    try:
        p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=HERE,
                           env=env, timeout=timeout, preexec_fn=limit_memory,
                           universal_newlines=True, errors='backslashreplace')
    except subprocess.TimeoutExpired:
        return '', '', 0.0, 'timeout after %ds' % timeout
    dt = time.perf_counter() - t
    if p.returncode:
        last = (p.stderr.strip().splitlines() or ['exit %d' % p.returncode])[-1]
        return p.stdout, p.stderr, dt, 'crashed (%d): %s' % (p.returncode, last)
    return p.stdout, p.stderr, dt, None


def run_once(mode, path, n, opts, tmp):
    out_path = os.path.join(tmp, 'result.json')
    if os.path.exists(out_path):
        os.unlink(out_path)
    trace_from = opts['iters'] // 2 if opts['trace_midway'] else -1
    stdout, stderr, wall, err = spawn(
        [sys.executable, os.path.abspath(__file__), '--child', mode, path, str(n),
         str(opts['train']), str(opts['iters']), str(trace_from), out_path], opts['timeout'])
    if err:
        return None, err
    try:
        with open(out_path) as f:
            r = json.load(f)
    except (OSError, ValueError):
        return None, 'no result'
    r['total'] = wall
    r['output'] = stdout + '\n--stderr--\n' + stderr
    return r, None


def median(xs):
    return statistics.median(xs) if xs else float('nan')


def shape(xs):
    k = len(xs) // 3
    if k < 1:
        return '?'
    first, mid, last = median(xs[:k]), median(xs[k:len(xs) - k]), median(xs[len(xs) - k:])
    if last > mid * SETTLED or mid > first * SETTLED or last > first * SETTLED:
        return 'S'
    if last * SETTLED < mid:
        return 'N'
    if first > mid * SETTLED:
        return 'W'
    return 'F'


def spread_within(xs):
    if len(xs) < 4:
        return 0.0
    s = sorted(xs)
    return (s[(3 * len(s)) // 4] - s[len(s) // 4]) / median(xs)


def summarise(runs, train):
    """Per-process measures and their medians over processes."""
    per = {'total': [], 'first': [], 'median': [], 'worst': [], 'import': [], 'rss': [],
           'grow': [], 'within': []}
    shapes = ''
    for r in runs:
        warm = r['times'][train:]
        per['total'].append(r['total'])
        per['first'].append(r['times'][0])
        per['median'].append(median(warm))
        per['worst'].append(max(warm))
        per['import'].append(r['import'])
        per['rss'].append(r['rss'] / 1024)
        per['grow'].append(r['grow'] / 1024)
        per['within'].append(spread_within(warm))
        shapes += shape(warm)
    s = {k: median(v) for k, v in per.items()}
    meds = per['median']
    s['between'] = (max(meds) - min(meds)) / median(meds) if meds else 0.0
    s['shape'] = shapes
    s['per'] = per
    s['series'] = [r['times'] for r in runs]
    return s


def first_difference(a, b):
    i = 0
    while i < min(len(a), len(b)) and a[i] == b[i]:
        i += 1
    start = max(0, a.rfind('\n', 0, i) + 1, i - 30)
    return '%r vs %r' % (a[start:i + 30], b[start:i + 30])


def compare(runs, reference, train, first=1):
    """Return a note on how runs (process first, first + 1, ...) differ from the reference, or None."""
    ref = reference['digests']
    for p, r in enumerate(runs, first - 1):
        if r['output'] != reference['output']:
            return 'process output differs (process %d): %s' % (
                p + 1, first_difference(reference['output'], r['output']))
        for k, (a, b) in enumerate(zip(ref, r['digests'])):
            if a != b:
                variant = 0 if k < train else 1
                return 'call %d (variant %d, process %d) differs: %s' % (
                    k + 1, variant, p + 1,
                    first_difference(reference['texts'][a], r['texts'][b]))
    return None


def unstable(r, train, trace_from):
    """Whether calls of one variant (traced or not) give different outcomes in one process."""
    groups = {}
    for k, d in enumerate(r['digests']):
        traced = trace_from >= 0 and k >= train + trace_from
        groups.setdefault((k < train, traced), set()).add(d)
    return any(len(g) > 1 for g in groups.values())


def measure(path, n, modes, opts):
    """Run every process of every mode for one benchmark at one n."""
    runs = {m: [] for m in modes}
    errors = {}
    with tempfile.TemporaryDirectory(prefix='ni-adv-') as tmp:
        for p in range(opts['processes']):
            for m in modes:
                if m in errors:
                    continue
                r, err = run_once(m, path, n, opts, tmp)
                if err:
                    errors[m] = 'process %d: %s' % (p + 1, err)
                else:
                    runs[m].append(r)
    result = {'n': n, 'errors': errors, 'summary': {}, 'notes': []}
    for m in modes:
        if runs[m]:
            result['summary'][m] = summarise(runs[m], opts['train'])
    plain_runs = runs.get('plain')
    if plain_runs:
        note = compare(plain_runs[1:], plain_runs[0], opts['train'], 2)
        if note:
            errors.setdefault('plain', 'varies between processes: ' + note)
        trace_from = opts['iters'] // 2 if opts['trace_midway'] else -1
        if any(unstable(r, opts['train'], trace_from) for r in plain_runs):
            result['notes'].append('CPython varies between calls')
    if 'ni' in modes and runs['ni'] and plain_runs and 'ni' not in errors:
        note = compare(runs['ni'], plain_runs[0], opts['train'])
        if note:
            result['wrong'] = note
    return result


def status(result):
    """('ok' | 'failed' | 'wrong', note); a failure of either mode counts."""
    errs = result['errors']
    if 'plain' in errs:
        return 'failed', 'CPython ' + errs['plain']
    if 'ni' in errs:
        return 'failed', 'Ni ' + errs['ni']
    if result.get('wrong'):
        return 'wrong', result['wrong']
    return 'ok', ''


MEASURES = ('total', 'first', 'median', 'worst')


def ratios(result):
    s = result['summary']
    p, q = s['plain'], s['ni']
    return {k: (q[k] / p[k] if p[k] > 0 else float('inf')) for k in MEASURES + ('rss',)}


def fmt_ms(t):
    if t < 0.0099995:
        return '%8.3f' % (t * 1e3)
    return '%8.1f' % (t * 1e3) if t < 99.99995 else '%7.0fs' % t


def unsettled(shapes, opts):
    return not opts['trace_midway'] and ('S' in shapes or 'N' in shapes)


def fmt_shape(shapes, opts):
    return shapes[:5] + ('!' if unsettled(shapes, opts) else '')


def fmt_ratio(r):
    if r < 99.995:
        return '%5.2f' % r
    return '%5.1f' % r if r < 999.95 else ' >999'


def wrap_note(note, width, indent):
    lines = []
    while note:
        lines.append(' ' * indent + note[:width - indent])
        note = note[width - indent:]
    return lines


def compare_table(names, results, length, opts, out):
    rows = []
    for name in names:
        r = results[name]
        st, note = status(r)
        if st == 'failed':
            rows.append((float('inf'), name, None, note, st))
            continue
        rs = ratios(r)
        worst = max(rs[k] for k in MEASURES)
        rows.append((float('inf') if st == 'wrong' else worst, name, rs, note, st))
    rows.sort(key=lambda row: (-row[0], row[1]))
    ok = [row for row in rows if row[2] is not None and row[4] == 'ok']
    failed = [row for row in rows if row[4] == 'failed']
    wrong = [row for row in rows if row[4] == 'wrong']
    print('== %s: about %g s of CPython work per run; %d processes per mode; train %d calls '
          '(variant 0), time %d (variant 1)' % (
              length, LENGTHS[length], opts['processes'], opts['train'], opts['iters']), file=out)
    if opts['trace_midway']:
        print('sys.settrace on from timed call %d, so later times include the tracer'
              % (opts['iters'] // 2 + 1), file=out)
    losses = []
    for k in MEASURES:
        slower = sum(1 for row in rows if row[4] != 'ok' or row[2][k] > 1.0)
        much = sum(1 for row in rows if row[4] != 'ok' or row[2][k] > 1.1)
        losses.append('%s %d (%d)' % (k, slower, much))
    settle = sum(1 for row in rows if row[2] is not None
                 and unsettled(results[row[1]]['summary']['ni']['shape'], opts))
    print('Losses of %d: failures %d, wrong answers %d, unsettled %d; slower than CPython' % (
        len(rows), len(failed), len(wrong), settle), file=out)
    print('  (by over 10%%) in %s' % ', '.join(losses), file=out)
    worst5 = []
    for row in rows[:5]:
        if row[4] != 'ok':
            worst5.append('%s %s' % (row[1], row[4]))
        else:
            k = max(MEASURES, key=lambda m: row[2][m])
            worst5.append('%s %.2f %s' % (row[1], row[2][k], k))
    for line in wrap_note('Worst five: ' + ', '.join(worst5), 120, 0):
        print(line.strip(), file=out)
    print(file=out)
    print('%-20s %14s %14s %14s %14s %5s %5s %-6s %-6s %5s' % (
        '', 'total (ms)', 'first (ms)', 'median (ms)', 'worst (ms)', 'proc', 'iter',
        'shape', '', 'RSS'), file=out)
    print('%-20s' % '' + ' %8s %5s' % ('CPython', 'Ni/C') * 4
          + ' %5s %5s %-6s %-6s %5s  %s' % ('Ni %', 'Ni %', 'CPy', 'Ni', 'Ni/C', 'notes'),
          file=out)
    for _, name, rs, note, st in rows:
        if rs is None:
            print('%-20s %s' % (name, ('FAILED: ' + note)[:99]), file=out)
            for line in wrap_note(('FAILED: ' + note)[99:], 120, 21)[:2]:
                print(line, file=out)
            continue
        p, q = results[name]['summary']['plain'], results[name]['summary']['ni']
        cells = ''.join(' %s %s' % (fmt_ms(p[k]), fmt_ratio(rs[k])) for k in MEASURES)
        notes = list(results[name]['notes'])
        if st == 'wrong':
            notes.append('WRONG: ' + note)
        line = '%-20s%s %5.0f %5.0f %-6s %-6s %5.2f' % (
            name[:20], cells, q['between'] * 100, q['within'] * 100, fmt_shape(p['shape'], opts),
            fmt_shape(q['shape'], opts), rs['rss'])
        text = '; '.join(notes)
        room = 120 - len(line) - 2
        if len(text) <= room:
            print(line + ('  ' + text if text else ''), file=out)
        else:
            print(line, file=out)
            for l in wrap_note(text, 120, 21)[:3]:
                print(l, file=out)
    gm = []
    for k in MEASURES:
        values = [FAIL_RATIO if row[4] != 'ok' else row[2][k] for row in rows]
        if values:
            g = 1.0
            for v in values:
                g *= v
            gm.append('%s %.2f' % (k, g ** (1 / len(values))))
    print(file=out)
    print('Geometric mean Ni/CPython of all %d (%d failed or wrong, counted as %g): %s' % (
        len(rows), len(failed) + len(wrong), FAIL_RATIO, ', '.join(gm)), file=out)


def plain_table(names, results, length, opts, out):
    print('== %s (CPython only): %d processes, %d training calls, %d timed' % (
        length, opts['processes'], opts['train'], opts['iters']), file=out)
    print('%-20s %6s %8s %8s %8s %8s %5s %5s %-6s %6s  %s' % (
        '', 'n', 'total ms', 'first ms', 'median', 'worst', 'proc%', 'iter%', 'shape',
        'RSS MB', 'notes'), file=out)
    for name in names:
        r = results[name]
        st, note = status(r)
        if st == 'failed':
            print('%-20s %6d FAILED: %s' % (name, r['n'], note[:85]), file=out)
            continue
        p = r['summary']['plain']
        print('%-20s %6d %s %s %s %s %5.0f %5.0f %-6s %6.1f  %s' % (
            name[:20], r['n'], fmt_ms(p['total']), fmt_ms(p['first']), fmt_ms(p['median']),
            fmt_ms(p['worst']), p['between'] * 100, p['within'] * 100, fmt_shape(p['shape'], opts),
            p['rss'],
            '; '.join(r['notes'])), file=out)


def data_lines(names, results, length):
    """One line per benchmark and measure: name length measure key=value..."""
    lines = []
    for name in names:
        r = results[name]
        st, note = status(r)
        lines.append('%s %s status %s%s' % (name, length, st, ' ' + note if note else ''))
        lines.append('%s %s n %d' % (name, length, r['n']))
        s = r['summary']
        modes = [m for m in ('plain', 'ni') if m in s]
        for k, unit, scale in (('total', 'ms', 1e3), ('first', 'ms', 1e3), ('median', 'ms', 1e3),
                               ('worst', 'ms', 1e3), ('import', 'ms', 1e3), ('rss', 'MB', 1),
                               ('grow', 'MB', 1), ('within', 'pct', 100),
                               ('between', 'pct', 100)):
            fields = []
            for m in modes:
                fields.append('%s=%.4g' % ('cpython' if m == 'plain' else m, s[m][k] * scale))
            if len(modes) == 2 and s['plain'][k] > 0:
                fields.append('ratio=%.4f' % (s['ni'][k] / s['plain'][k]))
            for m in modes:
                if k in s[m]['per']:
                    fields.append('%s_runs=%s' % ('cpython' if m == 'plain' else m, ','.join(
                        '%.4g' % (v * scale) for v in s[m]['per'][k])))
            lines.append('%s %s %s_%s %s' % (name, length, k, unit, ' '.join(fields)))
        lines.append('%s %s shape %s' % (name, length, ' '.join(
            '%s=%s' % ('cpython' if m == 'plain' else m, s[m]['shape']) for m in modes)))
        for m in modes:
            for p, series in enumerate(s[m]['series']):
                lines.append('%s %s series_ms.%s.%d %s' % (
                    name, length, 'cpython' if m == 'plain' else m, p + 1,
                    ','.join('%.4g' % (t * 1e3) for t in series)))
    return lines


def read_sizes():
    sizes = {}
    if os.path.exists(SIZES):
        with open(SIZES) as f:
            for line in f:
                if line.strip() and not line.startswith('#'):
                    w = line.split()
                    sizes[w[0]] = {'short': int(w[1]), 'medium': int(w[2]), 'long': int(w[3])}
    return sizes


def calibrate_child(path):
    mod = load(path)

    def per_call(n):
        times = []
        while len(times) < 3 or (sum(times) < 0.3 and len(times) < 51):
            t = time.perf_counter()
            mod.main(n, 1)
            times.append(time.perf_counter() - t)
        return median(times)

    def fit(target, n):
        # proportional steps, kept inside the bracket found so far, since
        # some benchmarks grow faster than linearly in n
        lo, hi = 0, None
        t = per_call(n)
        for _ in range(20):
            if abs(t - target) <= 0.1 * target or (n == 1 and t > target):
                break
            if t < target:
                lo = n
            else:
                hi = n
            new = max(1, round(n * min(max(target / max(t, 1e-7), 1 / 16), 16)))
            if hi is not None:
                if hi - lo <= 1:
                    break
                if not lo < new < hi:
                    new = max(lo + 1, round((max(lo, 1) * hi) ** 0.5))
            n = new
            t = per_call(n)
        return n, t

    mod.main(1, 0)
    short = fit(LENGTHS['short'] / CALIBRATION_CALLS, 1)
    medium = fit(LENGTHS['medium'] / CALIBRATION_CALLS, short[0])
    growth = min(max(math.log2(per_call(2 * medium[0]) / medium[1]), 1.0), 3.0)
    scale = (LENGTHS['long'] / CALIBRATION_CALLS / medium[1]) ** (1 / growth)
    long_n = max(1, round(medium[0] * scale))
    print('SIZES ' + json.dumps({'short': short, 'medium': medium,
                                 'long': [long_n, medium[1] * (long_n / medium[0]) ** growth]}))


def calibrate(names, paths, timeout):
    sizes = {}
    if os.path.exists(SIZES):
        with open(SIZES) as f:
            sizes = {line.split()[0]: line.rstrip('\n') for line in f
                     if line.strip() and not line.startswith('#')}
    for name, path in zip(names, paths):
        stdout, _, _, err = spawn([sys.executable, os.path.abspath(__file__), '--calibrate-child',
                                   path], timeout)
        lines = [l for l in stdout.splitlines() if l.startswith('SIZES ')]
        if err or not lines:
            print('%-22s failed: %s' % (name, err or 'no result'))
            continue
        c = json.loads(lines[-1][6:])
        sizes[name] = '%-22s %9d %9d %9d   %9.3f %9.1f %9.0f' % (
            name, c['short'][0], c['medium'][0], c['long'][0], c['short'][1] * 1e3,
            c['medium'][1] * 1e3, c['long'][1] * 1e3)
        print(sizes[name])
    commit = subprocess.run(['git', 'rev-parse', '--short', 'HEAD'], cwd=HERE,
                            stdout=subprocess.PIPE, universal_newlines=True).stdout.strip()
    with open(SIZES, 'w') as f:
        f.write('# n per call of main(n, variant) for each run length, written by\n'
                '# `run.py --calibrate` on plain %s (commit %s, %s).\n'
                '# n is chosen so that one call of variant 1 takes the run length / %d on\n'
                '# CPython; long is extrapolated from medium and twice medium (time as n to a\n'
                '# power from 1 to 3). ms is that time per call (for long, extrapolated).\n'
                '# %-20s %9s %9s %9s   %9s %9s %9s\n' % (
                    'CPython %d.%d.%d' % sys.version_info[:3], commit,
                    time.strftime('%Y-%m-%d'), CALIBRATION_CALLS, 'benchmark', 'short', 'medium',
                    'long', 'short ms', 'medium ms', 'long ms'))
        for name in sorted(sizes):
            f.write(sizes[name] + '\n')
    print('wrote ' + SIZES)


def option(args, name, default, kind):
    if name in args:
        return kind(args[args.index(name) + 1])
    return default


def main(args):
    opts = {'scale': option(args, '--scale', 1.0, float),
            'processes': option(args, '--processes', 5, int),
            'train': max(1, option(args, '--train', 5, int)),
            'iters': max(1, option(args, '--iters', 15, int)),
            'timeout': option(args, '--timeout', 600, int),
            'trace_midway': '--trace-midway' in args}
    paths = sorted(p for p in glob.glob(os.path.join(HERE, '*.py'))
                   if os.path.basename(p) != 'run.py')
    k = option(args, '-k', None, str)
    if k:
        paths = [p for p in paths if k in os.path.basename(p)]
    names = [os.path.splitext(os.path.basename(p))[0] for p in paths]
    if not names:
        print('(no benchmark matches)')
        return
    if '--calibrate' in args:
        calibrate(names, paths, opts['timeout'])
        return
    lengths = [args[i + 1] for i, a in enumerate(args) if a == '--length']
    if '--long' in args:
        lengths = (lengths or ['short', 'medium']) + ['long']
    lengths = [l for l in LENGTHS if l in (lengths or ['short', 'medium'])]
    sizes = read_sizes()
    missing = [n for n in names if n not in sizes]
    if missing:
        print('not calibrated (run.py --calibrate -k NAME): ' + ' '.join(missing))
        return
    modes = ['plain'] if '--plain' in args else ['plain', 'ni']
    progress = sys.stderr.isatty()
    data = []
    for i, length in enumerate(lengths):
        results = {}
        for name, path in zip(names, paths):
            if progress:
                sys.stderr.write('\r%-8s %-24s' % (length, name))
                sys.stderr.flush()
            n = max(1, int(sizes[name][length] * opts['scale']))
            results[name] = measure(path, n, modes, opts)
        if progress:
            sys.stderr.write('\r' + ' ' * 40 + '\r')
        if i:
            print()
        if modes == ['plain']:
            plain_table(names, results, length, opts, sys.stdout)
        else:
            compare_table(names, results, length, opts, sys.stdout)
        sys.stdout.flush()
        data.extend(data_lines(names, results, length))
    path = option(args, '--data', None, str)
    if path:
        with open(path, 'w') as f:
            f.write('# tools/adversarial/run.py %s\n' % ' '.join(args))
            f.write('# benchmark length measure values (cpython, ni, ratio = ni/cpython, '
                    '*_runs = one per process; series_ms = every call of one process)\n')
            for line in sorted(data):
                f.write(line + '\n')


if __name__ == '__main__':
    if sys.argv[1:2] == ['--child']:
        child(sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5]), int(sys.argv[6]),
              int(sys.argv[7]), sys.argv[8])
    elif sys.argv[1:2] == ['--calibrate-child']:
        calibrate_child(sys.argv[2])
    else:
        main(sys.argv[1:])
