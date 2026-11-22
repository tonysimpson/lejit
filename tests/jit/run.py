#!/usr/bin/env python3
"""Runs each case in tests/jit/cases/ (or DIR) plain and with LeJIT, in fresh
processes. Results must match, the function must have been compiled, and it
mustn't leak.

A case file has CASES = [(function_name, calls, options), ...]; calls is a
list of argument tuples, or a Python expression for one. Options:
'xfail': reason, 'interpreted': reason (must not compile), 'leaks': reason
(no leak check), 'warmup': (rounds, reason).

--release tests a release build, which can't say what it compiled: results
and leaks are checked, but not whether each function compiled, and an
'xfail' case that passes isn't an XPASS.

    python3.X tests/jit/run.py [DIR] [-k substring] [-v] [--release]
"""
import glob, os, subprocess, sys, warnings

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CASES = os.path.join(ROOT, 'tests', 'jit', 'cases')
TIMEOUT = 20
LEAK_SLACK = 50   # blocks; noise allowance for the leak check


def run_child(modname, fname, calls, lejit_mode, warmup=9):
    if not isinstance(calls, str):
        calls = repr(calls)
    cmd = [sys.executable, os.path.join(ROOT, 'tests', 'jit', 'child.py'), ROOT, CASES,
           modname, fname, calls, lejit_mode, str(warmup)]
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        out, err = p.communicate(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        p.kill()
        p.communicate()
        return None, 'timeout after %ds' % TIMEOUT
    if p.returncode != 0:
        err = err.decode(errors='replace').strip().splitlines()
        return None, 'exit %d: %s' % (p.returncode, ' | '.join(err[-3:]))
    return out.decode().splitlines(), None


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
    counts = {'pass': 0, 'FAIL': 0, 'xfail': 0, 'XPASS': 0}
    for name, modname, fname, calls, opts in load_cases(filt):
        warmup = opts.get('warmup', (9,))[0]
        expected, err = run_child(modname, fname, calls, '0', warmup)
        if err:
            print('ERROR %s: reference run failed: %s' % (name, err))
            counts['FAIL'] += 1
            continue
        got, err = run_child(modname, fname, calls, 'release' if release else '1', warmup)
        problem = None
        if err:
            problem = err
        else:
            leak_ref = int(expected.pop().split()[1])
            leak_lejit = int(got.pop().split()[1])
            results = got if release else got[:-1]
            if results != expected:
                problem = 'results differ: expected %r, got %r' % (expected, results)
            elif opts.get('interpreted'):
                if not release and got[-1] == 'compiled: yes':
                    problem = 'compiled, but expected to be interpreted'
            elif not release and got[-1] != 'compiled: yes':
                problem = got[-1]
            elif leak_lejit - leak_ref > LEAK_SLACK and not opts.get('leaks'):
                problem = 'leaks: %d more blocks after 100 extra rounds (%d without LeJIT)' % (
                    leak_lejit, leak_ref)
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
