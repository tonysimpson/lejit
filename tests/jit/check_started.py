"""lejit.check() finds no difference in a venv whose site starts LeJIT
(a .pth file that calls lejit.start()): the detectors must see the
interpreter, not compiled code. With LEJIT_START_CHECK=warning as well,
the check's own processes must not start LeJIT and check again: the .pth
counts the processes that read it, and starts LeJIT in only the first few,
so that a failure here is bounded.

    python3.X tests/jit/check_started.py
"""
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import common  # noqa: E402

PROGRAM = '''
import warnings, lejit
with warnings.catch_warnings(record=True) as w:
    warnings.simplefilter('always')
    diffs = lejit.check()
print(lejit._started, sorted(diffs), [str(x.message) for x in w])
'''

# one line of a .pth: add this process's id to the file named, and start
# LeJIT unless three processes already have (site may read a .pth twice)
START = ('import os, lejit; f = open(%r, "a+"); '
         'f.write("%%d\\n" %% os.getpid()); f.seek(0); '
         'n = len(set(f.read().split())); f.close(); '
         'n <= 3 and lejit.start()\n')


def main():
    with tempfile.TemporaryDirectory() as venv:
        subprocess.run([sys.executable, '-m', 'venv', '--without-pip', venv],
                       check=True)
        python = os.path.join(venv, 'bin', 'python')
        site = subprocess.run(
            [python, '-c', 'import site; print(site.getsitepackages()[0])'],
            stdout=subprocess.PIPE, universal_newlines=True,
            check=True).stdout.strip()
        runs = os.path.join(venv, 'runs')
        with open(os.path.join(site, 'lejit_started.pth'), 'w') as f:
            f.write(common.ROOT + '\n' + START % runs)
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('LEJIT_')}
        r = subprocess.run([python, '-c', PROGRAM], cwd=venv, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=300)
        os.remove(runs)
        env['LEJIT_START_CHECK'] = 'warning'
        s = subprocess.run([python, '-c', 'pass'], cwd=venv, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=300)
        with open(runs) as f:
            started = len(set(f.read().split()))
    failed = 0
    out = r.stdout.strip()
    if r.returncode or out != 'True [] []':
        print('FAIL lejit.check() with LeJIT started by site: %s' % out[-2000:])
        failed = 1
    else:
        print('lejit.check() with LeJIT started by site: no differences')
    if s.returncode or s.stdout or started != 1:
        print('FAIL LEJIT_START_CHECK=warning with LeJIT started by site: site'
              ' ran in %d processes: %s' % (started, s.stdout[-2000:]))
        failed = 1
    else:
        print('LEJIT_START_CHECK=warning with LeJIT started by site: site ran'
              ' in one process')
    return failed


if __name__ == '__main__':
    sys.exit(main())
