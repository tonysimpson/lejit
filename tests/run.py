#!/usr/bin/env python3
"""Differential test runner for Ni (doc/BUILD.md, Tests).

Each case runs in two fresh python3.7 processes, one plain and one with Ni.
The results (values or exceptions) must match, and the Ni run must report
that the function was really compiled. Crashes and hangs fail only that case.

Cases live in tests/cases/*.py, each with a CASES list of
    (function_name, calls, options_dict)
calls is a list of argument tuples, or a string holding a Python expression
for one, evaluated in the case module (for arguments without a usable repr).
options:
  'xfail': 'KNOWN_BUGS id + reason'   expected to fail; never silent
  'interpreted': 'reason'             must NOT be compiled (e.g. an opcode
                                      outside the whitelist); results must
                                      still match

Usage: run.py [-k substring] [-v]
"""
import glob, os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, 'tests', 'cases')
PY37 = os.environ.get('NI_PY37', os.path.expanduser('~/.pyenv/versions/3.7.17/bin/python3.7'))
TIMEOUT = 20


def run_child(modname, fname, calls, use_ni):
    if not isinstance(calls, str):
        calls = repr(calls)
    cmd = [PY37, os.path.join(ROOT, 'tests', 'child.py'), ROOT, CASES,
           modname, fname, calls, '1' if use_ni else '0']
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        return None, 'timeout after %ds' % TIMEOUT
    if p.returncode != 0:
        err = p.stderr.decode(errors='replace').strip().splitlines()
        return None, 'exit %d: %s' % (p.returncode, ' | '.join(err[-3:]))
    return p.stdout.decode().splitlines(), None


def load_cases(filt):
    sys.path.insert(0, CASES)
    for path in sorted(glob.glob(os.path.join(CASES, '*.py'))):
        modname = os.path.basename(path)[:-3]
        src = {}
        exec(compile(open(path).read(), path, 'exec'), src)
        for fname, calls, opts in src.get('CASES', []):
            name = '%s.%s' % (modname, fname)
            if filt and filt not in name:
                continue
            yield name, modname, fname, calls, opts


def main():
    args = sys.argv[1:]
    filt = args[args.index('-k') + 1] if '-k' in args else None
    verbose = '-v' in args
    counts = {'pass': 0, 'FAIL': 0, 'xfail': 0, 'XPASS': 0}
    for name, modname, fname, calls, opts in load_cases(filt):
        expected, err = run_child(modname, fname, calls, False)
        if err:
            print('ERROR %s: reference run failed: %s' % (name, err))
            counts['FAIL'] += 1
            continue
        got, err = run_child(modname, fname, calls, True)
        problem = None
        if err:
            problem = err
        else:
            if got[:-1] != expected:
                problem = 'results differ: expected %r, got %r' % (expected, got[:-1])
            elif opts.get('interpreted'):
                if got[-1] == 'compiled: yes':
                    problem = 'compiled, but expected to be interpreted'
            elif got[-1] != 'compiled: yes':
                problem = got[-1]
        xfail = opts.get('xfail')
        if problem and xfail:
            status = 'xfail'
        elif problem:
            status = 'FAIL'
        elif xfail:
            status = 'XPASS'
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
