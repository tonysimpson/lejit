"""Settings the test runners share.

Run as a script, prints build_temp() for the running interpreter.
"""
import hashlib, os, random, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def take_seed(args):
    """Removes --seed N from args and returns N, or a new random seed: the
    one seed that orders a test run."""
    if '--seed' not in args:
        return random.randrange(1 << 32)
    k = args.index('--seed')
    seed = int(args[k + 1])
    del args[k:k + 2]
    return seed


def rank(seed, name):
    """Where name goes in a run ordered by seed: the same on every machine
    and every Python, and independent of what else the run holds."""
    return hashlib.sha256(('%d %s' % (seed, name)).encode()).hexdigest()


# setuptools' build_temp, relative to the repo root
BUILD_TEMP_CODE = ("from setuptools.dist import Distribution\n"
                   "b = Distribution().get_command_obj('build')\n"
                   "b.finalize_options(); print(b.build_temp)")


def build_temp(python=sys.executable):
    out = subprocess.check_output([python, '-c', BUILD_TEMP_CODE], cwd=ROOT)
    return os.path.join(ROOT, out.decode().strip())


if __name__ == '__main__':
    print(build_temp())
