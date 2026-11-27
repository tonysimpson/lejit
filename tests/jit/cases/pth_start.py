# LeJIT started by a .pth file in site-packages: a script run then has its
# own module code compiled, as it starts after LeJIT. The child is a venv's
# Python with the .pth in its site-packages; with LeJIT the script answers
# only if its module code compiled, so its answer is plain CPython's.
import os
import subprocess
import sys
import tempfile

ROOT = sys.path[1]   # the repo root, inserted by tests/jit/child.py

SCRIPT = '''
import sys
total = 0
for i in range(1000):
    total += i * i % 7
if 'lejit' in sys.modules:
    info = sys.modules['lejit']._core._code_info(sys._getframe().f_code)
    print(total if info == 'compiled' else 'module code: %s' % info)
else:
    print(total)
'''

_cache = {}


def script_run_with_pth():
    if 'result' not in _cache:
        with tempfile.TemporaryDirectory() as d:
            env_dir = os.path.join(d, 'env')
            subprocess.run([sys.executable, '-m', 'venv', '--without-pip',
                            env_dir], check=True)
            py = os.path.join(env_dir, 'bin', 'python')
            if 'lejit' in sys.modules:
                site = subprocess.run(
                    [py, '-c', 'import sysconfig; '
                     'print(sysconfig.get_path("purelib"))'],
                    check=True, stdout=subprocess.PIPE,
                    universal_newlines=True).stdout.strip()
                with open(os.path.join(site, 'lejit-start.pth'), 'w') as f:
                    f.write('%s\nimport lejit; lejit.start()\n' % ROOT)
            script = os.path.join(d, 'script.py')
            with open(script, 'w') as f:
                f.write(SCRIPT)
            p = subprocess.run([py, script], stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT,
                               universal_newlines=True)
            _cache['result'] = p.returncode, p.stdout.strip()
    return _cache['result']


CASES = [
    ('script_run_with_pth', [()],
     {'leaks': 'runs child processes',
      'checked': 'asks the checked build whether the script compiled'}),
]
