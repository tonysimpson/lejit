"""Settings the test runners share.

Run as a script, prints build_temp() for the running interpreter.
"""
import os, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


# setuptools' build_temp, relative to the repo root
BUILD_TEMP_CODE = ("from setuptools.dist import Distribution\n"
                   "b = Distribution().get_command_obj('build')\n"
                   "b.finalize_options(); print(b.build_temp)")


def build_temp(python=sys.executable):
    out = subprocess.check_output([python, '-c', BUILD_TEMP_CODE], cwd=ROOT)
    return os.path.join(ROOT, out.decode().strip())


if __name__ == '__main__':
    print(build_temp())
