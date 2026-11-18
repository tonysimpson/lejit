"""Run other projects' own test suites with and without Ni and compare.

    thirdparty.py lock                  resolve projects.py into lock.txt
    thirdparty.py run [-j N] [--mode plain|ni] [project ...]
    thirdparty.py report [project ...]  compare the last runs again

Each project gets a venv under build/thirdparty/<project>/ made from
CPython 3.7.17 ($NI_PY37), with exactly the files lock.txt names (pip
--require-hashes, no dependency resolution) and Ni importable: a .pth file
imports it at start-up when NI_THIRDPARTY=1, so every process of the Ni
run, subprocesses included, runs with Ni. The tests come from the project's
sdist, checked against its sha256 and unpacked afresh for each run. Both
runs record one JSON line per test phase (ni_tp_pytest.py, ni_tp_django.py);
a run that crashes or times out is restarted without the tests already run
and the one it was in, which is recorded as a crash or a hang. The report
lists tests that pass plainly and fail, error, crash or hang with Ni, and
the reverse, with each run's time.
"""
import glob, hashlib, json, os, resource, shutil, signal, subprocess, sys
import tarfile, time, urllib.request, zipfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TP = os.path.join(ROOT, 'build', 'thirdparty')
LOCK = os.path.join(HERE, 'lock.txt')
PY37 = os.environ.get('NI_PY37', os.path.expanduser(
    '~/.pyenv/versions/3.7.17/bin/python3.7'))
MEMORY_LIMIT = 4000000000       # bytes of address space per process
TEST_TIMEOUT = 300              # seconds per test (pytest-timeout)
ATTEMPTS = 8                    # runs per mode, restarting after crashes
MODES = ('plain', 'ni')
GOOD = ('passed', 'skipped', 'xfail', 'xpass')
RANK = ['passed', 'xpass', 'skipped', 'xfail', 'failed', 'error',
        'timeout', 'hang', 'crash']

sys.path.insert(0, HERE)
from projects import PROJECTS   # noqa: E402


def sha256(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            h.update(block)
    return h.hexdigest()


def limit_memory():
    resource.setrlimit(resource.RLIMIT_AS, (MEMORY_LIMIT, MEMORY_LIMIT))


def call(cmd, **kw):
    print('  $ ' + ' '.join(cmd))
    sys.stdout.flush()
    subprocess.run(cmd, check=True, preexec_fn=limit_memory, timeout=1800,
                   **kw)


# -- lock ------------------------------------------------------------------

def lock():
    resolver = os.path.join(TP, '_resolve')
    if not os.path.exists(os.path.join(resolver, 'bin', 'python')):
        call([PY37, '-m', 'venv', resolver])
    lines = ['# project kind requirement sha256 file-or-url',
             '# written by tools/thirdparty/thirdparty.py lock']
    for proj, cfg in PROJECTS.items():
        dest = os.path.join(TP, '_resolve', 'dl', proj)
        shutil.rmtree(dest, ignore_errors=True)
        call([os.path.join(resolver, 'bin', 'pip'), 'download', '-q', '--disable-pip-version-check',
              '--only-binary=:all:', '-d', dest] + cfg['install'])
        for fn in sorted(os.listdir(dest)):
            if fn.startswith('source-'):
                continue
            name, version = fn.split('-')[:2]
            lines.append('%s wheel %s==%s %s %s' % (
                proj, name, version, sha256(os.path.join(dest, fn)), fn))
        if 'source_url' in cfg:
            url = cfg['source_url']
            path = os.path.join(dest, 'source-' + url.rsplit('/', 1)[1])
            with urllib.request.urlopen(url, timeout=300) as r, \
                    open(path, 'wb') as f:
                shutil.copyfileobj(r, f)
            digest = sha256(path)
        else:
            name, version = cfg['source'].split('==')
            info = 'https://pypi.org/pypi/%s/%s/json' % (name, version)
            with urllib.request.urlopen(info, timeout=60) as r:
                sd = [u for u in json.load(r)['urls']
                      if u['packagetype'] == 'sdist'][0]
            url, digest = sd['url'], sd['digests']['sha256']
        lines.append('%s sdist %s %s %s' % (proj, cfg['source'], digest, url))
    with open(LOCK, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote', LOCK)


def read_lock(proj):
    wheels, sdist = [], None
    with open(LOCK) as f:
        for line in f:
            if line.startswith('#') or not line.strip():
                continue
            p, kind, req, digest, where = line.split()
            if p != proj:
                continue
            if kind == 'wheel':
                wheels.append((req, digest))
            else:
                sdist = (req, digest, where)
    if sdist is None:
        sys.exit('%s is not in %s: run lock' % (proj, LOCK))
    return wheels, sdist


# -- set-up ----------------------------------------------------------------

PTH = ('import os, sys; os.environ.get("NI_THIRDPARTY") == "1" and '
       '(sys.path.insert(0, %r), __import__("ni"), sys.path.remove(%r))\n')

DJANGO_SETTINGS = ('from test_sqlite import *  # noqa\n'
                   'TEST_RUNNER = "ni_tp_django.Runner"\n')


def site_packages(venv):
    return os.path.join(venv, 'lib', 'python3.7', 'site-packages')


def setup(proj):
    cfg = PROJECTS[proj]
    wheels, (req, digest, url) = read_lock(proj)
    base = os.path.join(TP, proj)
    venv = os.path.join(base, 'venv')
    stamp = os.path.join(venv, 'ni_tp_stamp')
    want = json.dumps([wheels, cfg.get('pure', [])])
    if not (os.path.exists(stamp) and open(stamp).read() == want):
        print('%s: making %s' % (proj, venv))
        shutil.rmtree(venv, ignore_errors=True)
        call([PY37, '-m', 'venv', venv])
        reqs = os.path.join(base, 'requirements.txt')
        with open(reqs, 'w') as f:
            for r, d in wheels:
                f.write('%s --hash=sha256:%s\n' % (r, d))
        call([os.path.join(venv, 'bin', 'pip'), 'install', '-q',
              '--disable-pip-version-check', '--no-deps', '--only-binary=:all:',
              '--require-hashes', '-r', reqs])
        for pattern in cfg.get('pure', []):
            for path in glob.glob(os.path.join(site_packages(venv), pattern)):
                os.remove(path)
        with open(stamp, 'w') as f:
            f.write(want)
    sp = site_packages(venv)
    for fn in ('ni_tp_pytest.py', 'ni_tp_django.py'):
        shutil.copy(os.path.join(HERE, fn), sp)
    with open(os.path.join(sp, 'ni_tp_django_settings.py'), 'w') as f:
        f.write(DJANGO_SETTINGS)
    with open(os.path.join(sp, 'ni_thirdparty.pth'), 'w') as f:
        f.write(PTH % (ROOT, ROOT))
    dl = os.path.join(TP, '_sdist')
    os.makedirs(dl, exist_ok=True)
    archive = os.path.join(dl, proj + '-' + url.rsplit('/', 1)[1])
    if not os.path.exists(archive) or sha256(archive) != digest:
        print('%s: fetching %s' % (proj, url))
        with urllib.request.urlopen(url, timeout=300) as r, \
                open(archive + '.part', 'wb') as f:
            shutil.copyfileobj(r, f)
        if sha256(archive + '.part') != digest:
            sys.exit('%s: sha256 mismatch for %s' % (proj, url))
        os.rename(archive + '.part', archive)
    return venv, archive


def unpack(archive, dest):
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    if archive.endswith('.zip'):
        zipfile.ZipFile(archive).extractall(dest)
    else:
        tarfile.open(archive).extractall(dest)
    (top,) = os.listdir(dest)
    return os.path.join(dest, top)


# -- running ---------------------------------------------------------------

def command(cfg, venv, src, attempt_dir):
    py = os.path.join(venv, 'bin', 'python')
    if cfg['runner'] == 'pytest':
        return src, [py, '-m', 'pytest', '-p', 'ni_tp_pytest',
                     '-p', 'no:cacheprovider', '-q', '--tb=short',
                     '--continue-on-collection-errors', '--rootdir=' + src,
                     '--timeout=%d' % TEST_TIMEOUT, '--timeout-method=signal',
                     '--junitxml=' + os.path.join(attempt_dir, 'junit.xml')
                     ] + cfg['args']
    return os.path.join(src, 'tests'), [
        py, 'runtests.py', '--settings=ni_tp_django_settings',
        '--parallel=1', '--noinput', '-v', '1'] + cfg['args']


def parse(path):
    """id -> (status, detail) from a results file."""
    recs = {}
    order = []
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue        # a line cut short by a crash
                if r['when'] == 'session':
                    continue
                if r['id'] not in recs:
                    order.append(r['id'])
                recs.setdefault(r['id'], []).append(r)
    out = {}
    for tid in order:
        status, detail = None, ''
        for r in recs[tid]:
            o, when = r.get('outcome'), r['when']
            if when == 'start':
                continue
            if o in ('crash', 'hang'):
                s = o
            elif o == 'passed':
                s = 'xpass' if r['xfail'] and when == 'call' else 'passed'
            elif o == 'skipped':
                s = 'xfail' if r['xfail'] else 'skipped'
            elif o == 'error' or (o == 'failed' and when != 'call'):
                s = 'error'
            else:
                s = 'failed'
            if s == 'failed' and 'Timeout >' in r.get('detail', ''):
                s = 'timeout'
            if status is None or RANK.index(s) > RANK.index(status):
                status, detail = s, r.get('detail', '')
        out[tid] = (status or 'started', detail)
    return out


def finished(path, runner):
    """Ids with a final record, and the id last started without one."""
    done, last = set(), None
    final = 'teardown' if runner == 'pytest' else 'call'
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                try:
                    r = json.loads(line)
                except ValueError:
                    continue
                if r['when'] == 'start':
                    last = r['id']
                elif r['when'] in (final, 'crash'):
                    done.add(r['id'])
    if last in done:
        last = None
    return done, last


def run_mode(proj, mode):
    cfg = PROJECTS[proj]
    base = os.path.join(TP, proj)
    venv = os.path.join(base, 'venv')
    archive = os.path.join(base, 'archive')
    with open(archive) as f:
        archive = f.read()
    results = os.path.join(base, mode + '.jsonl')
    deselect = os.path.join(base, mode + '.deselect')
    for p in (results, deselect):
        if os.path.exists(p):
            os.remove(p)
    for old in glob.glob(os.path.join(base, mode + '-attempt*')):
        shutil.rmtree(old)
    open(deselect, 'w').close()
    src = unpack(archive, os.path.join(base, 'src-' + mode))
    tmp = os.path.join(base, 'tmp-' + mode)
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(('PYTHON', 'NI_', 'VIRTUAL_ENV'))}
    env.update(PYTHONHASHSEED='0', TMPDIR=tmp, NI_TP_RESULTS=results,
               NI_TP_DESELECT=deselect, PATH=os.path.join(venv, 'bin') +
               os.pathsep + env.get('PATH', ''))
    if mode == 'ni':
        env['NI_THIRDPARTY'] = '1'
    t0 = time.time()
    notes = []
    for attempt in range(ATTEMPTS):
        left = cfg['timeout'] - (time.time() - t0)
        if left < 30:
            notes.append('out of time before attempt %d' % attempt)
            break
        adir = os.path.join(base, '%s-attempt%d' % (mode, attempt))
        os.makedirs(adir, exist_ok=True)
        cwd, cmd = command(cfg, venv, src, adir)
        with open(os.path.join(adir, 'output.log'), 'wb') as log:
            p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=log,
                                 stderr=subprocess.STDOUT,
                                 stdin=subprocess.DEVNULL,
                                 preexec_fn=limit_memory,
                                 start_new_session=True)
            try:
                rc = p.wait(timeout=left)
                hung = False
            except subprocess.TimeoutExpired:
                os.killpg(p.pid, signal.SIGKILL)
                rc = p.wait()
                hung = True
        if not hung and rc in (0, 1):
            break
        done, last = finished(results, cfg['runner'])
        what = 'timed out' if hung else 'exit %d' % rc
        if last is None:
            notes.append('attempt %d %s outside any test' % (attempt, what))
            break
        notes.append('attempt %d %s in %s' % (attempt, what, last))
        with open(results, 'a') as f:
            f.write(json.dumps({'id': last, 'when': 'crash',
                                'outcome': 'hang' if hung else 'crash',
                                'xfail': False,
                                'detail': '%s (%s/output.log)' % (what, adir)}
                               ) + '\n')
        with open(deselect, 'w') as f:
            f.write('\n'.join(sorted(done | {last})) + '\n')
    else:
        notes.append('gave up after %d attempts' % ATTEMPTS)
    meta = {'seconds': round(time.time() - t0, 1), 'notes': notes}
    with open(os.path.join(base, mode + '.meta.json'), 'w') as f:
        json.dump(meta, f)
    print('%s %s: %.0f s %s' % (proj, mode, meta['seconds'], '; '.join(notes)))
    sys.stdout.flush()


def run(projs, jobs, modes):
    if not any(fn.startswith('ni.') and fn.endswith('.so')
               for fn in os.listdir(ROOT)):
        sys.exit('no ni.so in %s: build first (doc/BUILD.md)' % ROOT)
    for proj in projs:
        venv, archive = setup(proj)
        with open(os.path.join(TP, proj, 'archive'), 'w') as f:
            f.write(archive)
        with ThreadPoolExecutor(jobs) as ex:
            list(ex.map(lambda m: run_mode(proj, m), modes))
    report(projs)


# -- report ----------------------------------------------------------------

def counts(res):
    c = {}
    for s, _ in res.values():
        c[s] = c.get(s, 0) + 1
    return ' '.join('%s=%d' % (k, c[k]) for k in RANK + ['started'] if k in c)


def ni_buffers(path):
    """Ni's code buffers at the end of each attempt, '-' if Ni wasn't on."""
    seen = []
    if os.path.exists(path):
        with open(path) as f:
            for line in f:
                if '"<session>"' in line:
                    seen.append(json.loads(line)['ni_buffers'])
    return ','.join(str(b) if b is not None else '-' for b in seen) or '?'


def report(projs):
    lines, details = [], []
    for proj in projs:
        base = os.path.join(TP, proj)
        res, meta = {}, {}
        for m in MODES:
            res[m] = parse(os.path.join(base, m + '.jsonl'))
            mp = os.path.join(base, m + '.meta.json')
            meta[m] = json.load(open(mp)) if os.path.exists(mp) else {}
        lines.append('== %s' % proj)
        for m in MODES:
            lines.append('  %-5s %6s s  %s  (Ni buffers %s)%s' % (
                m, meta[m].get('seconds', '-'), counts(res[m]),
                ni_buffers(os.path.join(base, m + '.jsonl')),
                ''.join('\n        ' + n for n in meta[m].get('notes', []))))
        plain, ni = res['plain'], res['ni']
        for title, a, b, bname in (('Ni-only', plain, ni, 'ni'),
                                   ('plain-only', ni, plain, 'plain')):
            if not a or not b:
                continue
            bad = [(t, b.get(t, ('missing', ''))) for t in a
                   if a[t][0] in GOOD
                   and b.get(t, ('missing',))[0] not in GOOD]
            bad += [(t, b[t]) for t in b if t not in a and b[t][0] not in GOOD]
            lines.append('  %s failures: %d' % (title, len(bad)))
            for t, (s, d) in bad:
                lines.append('    %-8s %s' % (s, t))
                details.append('---- %s %s %s (%s)\n%s\n' % (
                    proj, title, t, s, d))
    text = '\n'.join(lines) + '\n'
    print(text)
    with open(os.path.join(TP, 'report.txt'), 'w') as f:
        f.write(text)
    with open(os.path.join(TP, 'report-details.txt'), 'w') as f:
        f.write('\n'.join(details))
    print('details of each failure:', os.path.join(TP, 'report-details.txt'))


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ('lock', 'run', 'report'):
        sys.exit(__doc__)
    cmd, args = args[0], args[1:]
    jobs = 2
    modes = MODES
    if '-j' in args:
        k = args.index('-j')
        jobs = int(args[k + 1])
        del args[k:k + 2]
    if '--mode' in args:
        k = args.index('--mode')
        modes = (args[k + 1],)
        del args[k:k + 2]
    projs = args or list(PROJECTS)
    for p in projs:
        if p not in PROJECTS:
            sys.exit('unknown project %s' % p)
    os.makedirs(TP, exist_ok=True)
    if cmd == 'lock':
        lock()
    elif cmd == 'run':
        run(projs, jobs, modes)
    else:
        report(projs)


if __name__ == '__main__':
    main()
