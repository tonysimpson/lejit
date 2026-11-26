"""lejit.check() finds no difference in a venv whose site starts LeJIT
(a .pth file that calls lejit.start()): the detectors must see the
interpreter, not compiled code.

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


def main():
    with tempfile.TemporaryDirectory() as venv:
        subprocess.run([sys.executable, '-m', 'venv', '--without-pip', venv],
                       check=True)
        python = os.path.join(venv, 'bin', 'python')
        site = subprocess.run(
            [python, '-c', 'import site; print(site.getsitepackages()[0])'],
            stdout=subprocess.PIPE, universal_newlines=True,
            check=True).stdout.strip()
        with open(os.path.join(site, 'lejit_started.pth'), 'w') as f:
            f.write(common.ROOT + '\nimport lejit; lejit.start()\n')
        env = {k: v for k, v in os.environ.items()
               if not k.startswith('LEJIT_')}
        r = subprocess.run([python, '-c', PROGRAM], cwd=venv, env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           universal_newlines=True, timeout=300)
    out = r.stdout.strip()
    if r.returncode or out != 'True [] []':
        print('FAIL lejit.check() with LeJIT started by site: %s' % out[-2000:])
        return 1
    print('lejit.check() with LeJIT started by site: no differences')
    return 0


if __name__ == '__main__':
    sys.exit(main())
