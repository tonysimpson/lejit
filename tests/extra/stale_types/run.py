"""Runs each stale-type check here plain and with LeJIT; the outputs must be
the same. A check whose syntax this Python lacks is skipped.

    python3.X tests/extra/stale_types/run.py
"""
import glob, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))


def run(path, mode):
    p = subprocess.run([sys.executable, path, mode], cwd=HERE, timeout=120,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode(errors='replace')


def main():
    failed = ran = 0
    for path in sorted(glob.glob(os.path.join(HERE, '*.py'))):
        name = os.path.basename(path)
        if name in ('hdr.py', 'run.py'):
            continue
        try:
            compile(open(path).read(), path, 'exec')
        except SyntaxError:
            continue
        ran += 1
        plain, jit = run(path, 'plain'), run(path, 'lejit')
        if plain != jit:
            failed += 1
            print('FAIL %s: plain exit %d, LeJIT exit %d' % (name, plain[0], jit[0]))
            for a, b in zip(plain[1].splitlines() + [''], jit[1].splitlines() + ['']):
                if a != b:
                    print('  plain: %s\n  LeJIT: %s' % (a, b))
                    break
    print('%d of %d differ' % (failed, ran))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
