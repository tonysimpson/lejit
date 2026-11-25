#!/usr/bin/env python3
"""Runs each case in tests/jit/cases/ (or DIR) plain and with LeJIT, in fresh
processes. Results must match, the function must have been compiled, it
mustn't leak, and LeJIT's run must write nothing to stderr that the plain
run doesn't.

A case file has CASES = [(function_name, calls, options), ...]; calls is a
list of argument tuples, or a Python expression for one. Options:
'xfail': reason, 'interpreted': reason (must not compile), 'leaks': reason
(no leak check), 'leaks_with_aslr': reason (no leak check when address-space
randomisation can't be turned off: what it counts depends on addresses),
'warmup': (rounds, reason), 'clear_type_cache': reason (the
leak check empties CPython's type cache first; only where that keeps the
types' version tags, as from 3.11), 'checked': reason (needs a checked build),
'expected': [result, ...] (LeJIT's results must be these, one per call, not
plain CPython's: for a language rule some CPython gets wrong).

--release tests a release build, which can't say what it compiled: results
and leaks are checked, but not whether each function compiled, an 'xfail'
case that passes isn't an XPASS, and 'checked' cases are skipped.

    python3.X tests/jit/run.py [DIR] [-k substring] [-v] [--release]
"""
import glob, os, random, signal, subprocess, sys, warnings

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CASES = os.path.join(ROOT, 'tests', 'jit', 'cases')
TIMEOUT = 20
LEAK_SLACK = 50   # blocks; noise allowance for the leak check
# each child runs with address-space randomisation off: what the leak check
# counts can depend on where objects land (a table keyed by id() is sized
# by them), so a plain run and a LeJIT run of the same case could differ
ASLR_OFF = ['setarch', os.uname().machine, '-R']


def aslr_off_works():
    # a container's seccomp profile can refuse it
    try:
        return subprocess.run(ASLR_OFF + [sys.executable, '-c', ''],
                              stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL).returncode == 0
    except OSError:
        return False


def kill_group(p):
    try:
        os.killpg(p.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass


def run_child(modname, fname, calls, lejit_mode, warmup=9, env=None,
              clear_type_cache=False):
    if not isinstance(calls, str):
        calls = repr(calls)
    cmd = ASLR_OFF + [sys.executable, os.path.join(ROOT, 'tests', 'jit', 'child.py'), ROOT, CASES,
           modname, fname, calls, lejit_mode, str(warmup),
           '1' if clear_type_cache else '0']
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env,
                         start_new_session=True)
    try:
        out, err = p.communicate(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        kill_group(p)
        p.communicate()
        return None, 'timeout after %ds' % TIMEOUT
    finally:
        # processes the case started and left running
        kill_group(p)
    if p.returncode != 0:
        err = err.decode(errors='replace').strip().splitlines()
        return None, 'exit %d: %s' % (p.returncode, ' | '.join(err[-3:]))
    return out.decode().splitlines(), err.decode(errors='replace').splitlines()


def load_cases(filt):
    sys.path.insert(0, CASES)
    for path in sorted(glob.glob(os.path.join(CASES, '*.py'))):
        modname = os.path.basename(path)[:-3]
        src = {}
        with warnings.catch_warnings():
            # some cases test identity with 'is' on purpose
            warnings.simplefilter('ignore', SyntaxWarning)
            exec(compile(open(path).read(), path, 'exec'), src)
        for fname, calls, opts in src.get('CASES', []):
            name = '%s.%s' % (modname, fname)
            if filt and filt not in name:
                continue
            yield name, modname, fname, calls, opts


def main():
    global CASES
    args = sys.argv[1:]
    filt = args[args.index('-k') + 1] if '-k' in args else None
    if args and not args[0].startswith('-'):
        CASES = os.path.abspath(args[0])
    verbose = '-v' in args
    release = '--release' in args
    aslr_off = aslr_off_works()
    if not aslr_off:
        print('address-space randomisation stays on: setarch -R is refused here')
        ASLR_OFF[:] = []
    counts = {'pass': 0, 'FAIL': 0, 'xfail': 0, 'XPASS': 0}
    if release:
        counts['skip'] = 0
    for name, modname, fname, calls, opts in load_cases(filt):
        if release and opts.get('checked'):
            counts['skip'] += 1
            continue
        warmup = opts.get('warmup', (9,))[0]
        # both runs hash alike: what plain CPython itself keeps after the
        # rounds can depend on the seed (a subinterpreter's, by tens of
        # blocks), more than the leak check allows between two seeds
        clear = 'clear_type_cache' in opts
        env = dict(os.environ)
        env.setdefault('PYTHONHASHSEED', str(random.randrange(4294967296)))
        expected, ref_err = run_child(modname, fname, calls, '0', warmup, env, clear)
        if expected is None:
            print('ERROR %s: reference run failed: %s' % (name, ref_err))
            counts['FAIL'] += 1
            continue
        got, err = run_child(modname, fname, calls, 'release' if release else '1',
                             warmup, env, clear)
        problem = None
        if got is None:
            problem = err
        else:
            extra = [l for l in err if l not in ref_err]
            leak_ref = int(expected.pop().split()[1])
            leak_lejit = int(got.pop().split()[1])
            results = got if release else got[:-1]
            if 'expected' in opts:
                expected = ['ok ' + repr(r) for r in opts['expected']]
            if results != expected:
                problem = 'results differ: expected %r, got %r' % (expected, results)
            elif opts.get('interpreted'):
                if not release and got[-1] == 'compiled: yes':
                    problem = 'compiled, but expected to be interpreted'
            elif not release and got[-1] != 'compiled: yes':
                problem = got[-1]
            elif (leak_lejit - leak_ref > LEAK_SLACK and not opts.get('leaks') and
                  (aslr_off or not opts.get('leaks_with_aslr'))):
                problem = 'leaks: %d more blocks after 100 extra rounds (%d without LeJIT)' % (
                    leak_lejit, leak_ref)
            elif extra:
                problem = 'stderr the plain run lacks: %s' % ' | '.join(extra[:3])
        xfail = opts.get('xfail')
        if problem and xfail:
            status = 'xfail'
        elif problem:
            status = 'FAIL'
        elif xfail:
            status = 'xfail' if release else 'XPASS'
        else:
            status = 'pass'
        counts[status] += 1
        if status != 'pass' or verbose:
            line = '%-5s %s' % (status, name)
            if problem:
                line += ': ' + problem
            if xfail:
                line += '  [xfail: %s]' % xfail
            print(line)
    print(' '.join('%s=%d' % kv for kv in counts.items()))
    return 1 if counts['FAIL'] or counts['XPASS'] else 0


if __name__ == '__main__':
    sys.exit(main())
