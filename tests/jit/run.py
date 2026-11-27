#!/usr/bin/env python3
"""Runs each case in tests/jit/cases/ (or DIR) plain and with LeJIT, in fresh
processes. Results must match, the function must have been compiled, it
mustn't leak, LeJIT's run must write nothing to stderr that the plain
run doesn't, and it must make no more objects than the plain run: no more
allocations in the leak check's 100 rounds (50 allowed, for one made
once), and no more objects of any type the gc tracks left alive after
them. That isn't compared for a case that calls lejit._core's test
functions in its rounds, which the plain run doesn't. A case known to make
more on a Python is listed for it in the cases directory's
more_objects.txt, with the reason: that check is then expected to fail.
One that makes no more doesn't fail, as counts differ between builds of
one Python, but is named at the end, to be taken off the list.

A case file has CASES = [(function_name, calls, options), ...]; calls is a
list of argument tuples, or a Python expression for one. Options:
'xfail': reason, 'interpreted': reason (must not compile), 'leaks': reason
(no leak check), 'warmup': (rounds, reason), 'clear_type_cache': reason (the
leak check empties CPython's type cache around the rounds, and runs the
calls once more before them, as emptying it can make compiled code compile
again),
'checked': reason (needs a checked build),
'expected': [result, ...] (LeJIT's results must be these, one per call, not
plain CPython's: for a language rule some CPython gets wrong),
'compiles_each_call': reason (the function makes and compiles new code on
every call, so the leak check's rounds always compile and aren't repeated),
'objects': reason (no objects check: what the case allocates in its
rounds depends on timing).

--release tests a release build, which can't say what it compiled: results
and leaks are checked, but not whether each function compiled or how many
objects it made, an 'xfail' case that passes isn't an XPASS, and 'checked'
cases are skipped.

The cases run in an order a seed chooses, new each run unless --seed gives
it; the seed also sets each case's hash seed, shared by its two runs.

    python3.X tests/jit/run.py [DIR] [-k substring] [-v] [--release] [--seed N]
"""
import glob, os, signal, subprocess, sys, warnings

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'tests'))
from common import rank, take_seed
CASES = os.path.join(ROOT, 'tests', 'jit', 'cases')
TIMEOUT = 20
LEAK_SLACK = 50   # blocks; noise allowance for the leak check
# allocations in the 100 rounds: one made once, such as a cache filled on
# first use, is not made every round
ALLOCS_SLACK = 50
# cases by whether LeJIT made fewer objects than plain CPython, as many, or
# more, or asked lejit._core in their rounds so weren't compared
OBJECTS = {'fewer': 0, 'equal': 0, 'more': 0, 'asks': 0}
# cases more_objects.txt lists that made no more objects here
NOT_MORE = []
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
              clear_type_cache=False, compiles_each_call=False):
    if not isinstance(calls, str):
        calls = repr(calls)
    cmd = ASLR_OFF + [sys.executable, os.path.join(ROOT, 'tests', 'jit', 'child.py'), ROOT, CASES,
           modname, fname, calls, lejit_mode, str(warmup),
           '1' if clear_type_cache else '0',
           '1' if compiles_each_call else '0',
           # a release build has no counter, and the runs stay as they were
           '0' if '--release' in sys.argv else '1']
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


def objects_compared(ref_line, got_line):
    """'fewer', 'equal' or 'more' objects made with LeJIT than without, and
    what there were more of; 'asks' if the case called lejit._core's test
    functions in its rounds, which a run without LeJIT doesn't; None if
    either run couldn't count."""
    from ast import literal_eval
    ref_allocs, _, ref_alive = ref_line.split(' ', 3)[1:]
    allocs, asked, alive = got_line.split(' ', 3)[1:]
    ref_allocs, allocs = int(ref_allocs), int(allocs)
    if ref_allocs < 0 or allocs < 0:
        return None, None
    if int(asked):
        return 'asks', None
    # a count can fall: a collection stops tracking tuples and dicts that
    # can't be in a cycle
    ref_alive = dict(literal_eval(ref_alive))
    more = ['%s %d (%d)' % (name, n, ref_alive.get(name, 0))
            for name, n in literal_eval(alive) if n > max(ref_alive.get(name, 0), 0)]
    counts = '%d allocations in 100 rounds (%d without LeJIT)' % (allocs, ref_allocs)
    if more:
        counts += '; more left alive, by type (without LeJIT): ' + ', '.join(more)
    if allocs - ref_allocs >= ALLOCS_SLACK or more:
        return 'more', counts
    return ('fewer' if allocs < ref_allocs else 'equal'), counts


def more_objects_known():
    """{case: reason} from more_objects.txt, for this Python."""
    known = {}
    version = '%d.%d' % sys.version_info[:2]
    try:
        lines = open(os.path.join(CASES, 'more_objects.txt')).read().splitlines()
    except FileNotFoundError:
        return known
    for line in lines:
        if line.strip() and not line.startswith('#'):
            name, versions, reason = line.split(None, 2)
            if version in versions.split(','):
                known[name] = reason
    return known


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
    seed = take_seed(args)
    filt = args[args.index('-k') + 1] if '-k' in args else None
    if args and not args[0].startswith('-'):
        CASES = os.path.abspath(args[0])
    verbose = '-v' in args
    release = '--release' in args
    if not aslr_off_works():
        print('address-space randomisation stays on: setarch -R is refused here')
        ASLR_OFF[:] = []
    counts = {'pass': 0, 'FAIL': 0, 'xfail': 0, 'XPASS': 0}
    more_known = more_objects_known()
    if release:
        counts['skip'] = 0
    print('seed %d' % seed)
    sys.stdout.flush()
    cases_dir = [CASES] if CASES != os.path.join(ROOT, 'tests', 'jit', 'cases') else []
    again = '      again: ' + ' '.join([sys.executable, sys.argv[0]] + cases_dir +
                                       ['--release'] * release +
                                       ['--seed', str(seed), '-k', '%s'])
    for name, modname, fname, calls, opts in sorted(load_cases(filt),
                                                    key=lambda c: rank(seed, c[0])):
        if release and opts.get('checked'):
            counts['skip'] += 1
            continue
        warmup = opts.get('warmup', (9,))[0]
        # both runs hash alike: what plain CPython itself keeps after the
        # rounds can depend on the seed (a subinterpreter's, by tens of
        # blocks), more than the leak check allows between two seeds
        clear = 'clear_type_cache' in opts
        fresh = 'compiles_each_call' in opts
        env = dict(os.environ)
        env.setdefault('PYTHONHASHSEED', str(int(rank(seed, name), 16) % (1 << 32)))
        expected, ref_err = run_child(modname, fname, calls, '0', warmup, env, clear,
                                       fresh)
        if expected is None:
            print('ERROR %s: reference run failed: %s' % (name, ref_err))
            print(again % name)
            counts['FAIL'] += 1
            continue
        got, err = run_child(modname, fname, calls, 'release' if release else '1',
                             warmup, env, clear, fresh)
        problem = None
        repeated = ''
        made = object_counts = None
        if got is None:
            problem = err
        else:
            extra = [l for l in err if l not in ref_err]
            expected.pop()
            rounds = got.pop()[len('rounds: '):]
            made, object_counts = objects_compared(expected.pop(), got.pop())
            leak_ref = int(expected.pop().split()[1])
            leak_lejit, repeats = map(int, got.pop().split()[1:])
            if repeats:
                repeated = 'code compiled during the rounds, so they were run %d times' % (repeats + 1)
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
            elif leak_lejit - leak_ref > LEAK_SLACK and not opts.get('leaks'):
                problem = 'leaks: %d more blocks after 100 extra rounds (%d without LeJIT)' % (
                    leak_lejit, leak_ref)
                problem += '; blocks every 10 rounds, then collected: ' + rounds
                if repeated:
                    problem += '; ' + repeated
            elif extra:
                problem = 'stderr the plain run lacks: %s' % ' | '.join(extra[:3])
            if 'objects' in opts:
                made = None
            if made == 'more' and not problem and name not in more_known:
                problem = 'more objects: ' + object_counts
        if made:
            OBJECTS[made] += 1
        xfail = opts.get('xfail')
        if problem and xfail:
            status = 'xfail'
        elif problem:
            status = 'FAIL'
        elif xfail:
            status = 'xfail' if release else 'XPASS'
        elif made == 'more' and name in more_known:
            xfail = more_known[name]
            status = 'xfail'
            problem = 'more objects: ' + object_counts
        else:
            if made in ('fewer', 'equal') and name in more_known:
                NOT_MORE.append(name)
            status = 'pass'
        counts[status] += 1
        if status != 'pass' or verbose:
            line = '%-5s %s' % (status, name)
            if problem:
                line += ': ' + problem
            if xfail:
                line += '  [xfail: %s]' % xfail
            if verbose and made == 'asks':
                line += '  [objects not compared: the case asks lejit._core in its rounds]'
            elif verbose and object_counts and not problem:
                line += '  [%s objects: %s]' % (made, object_counts)
            if repeated and not problem:
                line += '  [%s]' % repeated
            print(line)
            if status in ('FAIL', 'XPASS'):
                print(again % name)
    print(' '.join('%s=%d' % kv for kv in counts.items()) + ' (seed %d)' % seed)
    return 1 if counts['FAIL'] or counts['XPASS'] else 0


if __name__ == '__main__':
    status = main()
    if any(OBJECTS.values()):
        print('objects made with LeJIT against without: fewer=%(fewer)d equal=%(equal)d '
              'more=%(more)d; not compared, as the case asks lejit._core in its '
              'rounds: %(asks)d' % OBJECTS)
    if NOT_MORE:
        print('listed in more_objects.txt but made no more objects here: ' +
              ', '.join(NOT_MORE))
    sys.exit(status)
