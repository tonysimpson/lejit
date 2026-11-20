#! /usr/bin/env python

"""
Ni aims to provide a module that just makes Python faster.

Ni is based on a fork of Armin Rigo's Psyco, Armin and the other Psyco
developers moved onto PyPy so go check that out if you want a high performance
implementation of Python (http://pypy.org).
"""
from __future__ import print_function

import os, sys, sysconfig
import glob
import subprocess
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext


class BuildError(Exception):
    pass

####################################################################
# Override defaults using environment variables e.g. dev mode
# install with debug trace points:
# > NI_TRACE=1 pip install -e .
#
# Enable debugger trace points and compiler with debug options by 
# setting to 1
NI_TRACE = int(os.environ.get('NI_TRACE', 0))

# Extra checks enable with 1
ALL_CHECKS = int(os.environ.get('NI_ALL_CHECKS', 0))

# Code size per instruction, ni._codestats()
NI_CODESTATS = int(os.environ.get('NI_CODESTATS', 0))

# NI_FEATURES=source: detect the features in CPython's source (fetched and
# cached once, or NI_CPYTHON_SRC) instead of the installed interpreter; the
# source detector is the reference the installed one is checked against.
NI_FEATURES = os.environ.get('NI_FEATURES', 'installed')
if NI_FEATURES not in ('installed', 'source'):
    raise SystemExit('NI_FEATURES must be installed or source, not %r' % NI_FEATURES)

# NI_SANITIZE=address or address,undefined: build with gcc's sanitizers
# Such a build goes in build/asan
# (--build-lib), never in place, with its own config stamp.
NI_SANITIZE = os.environ.get('NI_SANITIZE', '')

# NI_COVERAGE=1: build with gcov into build/cov, never in place
NI_COVERAGE = int(os.environ.get('NI_COVERAGE', 0))

####################################################################

macros = [
    ('NI_TRACE', str(NI_TRACE)),
    ('ALL_CHECKS', str(ALL_CHECKS)),
    ('NI_CODESTATS', str(NI_CODESTATS)),
]
if NI_SANITIZE:
    macros.append(('NI_SANITIZE', '"%s"' % NI_SANITIZE))   # ni.build_info

class ProcessorAutodetectError(Exception):
    pass

def autodetect():
    platform = sys.platform.lower()
    if platform.startswith('win'):   # assume an Intel Windows
        mach = 'win'
    else:
        mach = os.popen('uname -m', 'r').read().strip()
    if not mach:
        raise ProcessorAutodetectError("cannot run 'uname -m'")
    try:
        return {
                'x86_64': 'x64',
                }[mach]
    except KeyError:
        raise ProcessorAutodetectError("unsupported processor '%s'" % mach)

PROCESSOR = autodetect()

def find_sources(processor):
    result = glob.glob('./ni/*.c')
    result += glob.glob('./ni/Python/*.c')
    result += glob.glob('./ni/Modules/*.c')
    result += glob.glob('./ni/Objects/*.c')
    result += glob.glob('./ni/%s/*.c' % (processor,))
    return result

def cpython_warnings():
    """The warning options CPython is built with, as the target's build
    configuration holds them: distutils passes CFLAGS, but not the
    *_NODIST flags CPython keeps for its own code (-Wextra and its
    exceptions among them)."""
    out = []
    for var in ('CFLAGS', 'CFLAGS_NODIST', 'PY_CFLAGS_NODIST'):
        for f in (sysconfig.get_config_var(var) or '').split():
            if f.startswith('-W') and not f.startswith(('-Wl,', '-Wa,', '-Wp,')) \
                    and f not in out:
                out.append(f)
    return out

if NI_TRACE:
    extra_compile_args = ['-O0', '-g3', '-fno-stack-protector']
else:
    extra_compile_args = ['-O3']
extra_compile_args += cpython_warnings()
# pyenv's CFLAGS demote these to warnings; a missing prototype makes a
# pointer result an int, truncated (a segfault, found adding
# psyco_current_frame), so keep them errors
extra_compile_args += ['-Werror=implicit-function-declaration',
                       '-Werror=int-conversion',
                       '-Werror=incompatible-pointer-types']
# what CPython's flags warn of that is no fault in Ni's code:
# - type-limits: an opcode the source lacks is defined as 256 and up,
#   so a byte compared with it is always false on that version
# - deprecated-declarations: compiled code calls the API the version has
#   (3.9's PyCFunction_Call); another function would be other code emitted
extra_compile_args += ['-Wno-type-limits', '-Wno-deprecated-declarations']
# any other warning is an error, so the tree stays clean
extra_compile_args += ['-Werror']
extra_link_args = []
if NI_SANITIZE:
    if '--inplace' in sys.argv or '-i' in sys.argv:
        raise SystemExit('NI_SANITIZE: build with --build-lib build/asan,'
                         ' not --inplace')
    # after the options above, so this -O1 wins
    extra_compile_args += ['-fsanitize=' + NI_SANITIZE, '-O1', '-g',
                           '-fno-omit-frame-pointer']
    # the encoder writes instructions' immediates and displacements at any
    # byte address, by design (x86 allows it)
    if 'undefined' in NI_SANITIZE:
        extra_compile_args += ['-fno-sanitize=alignment']
    extra_link_args += ['-fsanitize=' + NI_SANITIZE]
if NI_COVERAGE:
    if '--inplace' in sys.argv or '-i' in sys.argv:
        raise SystemExit('NI_COVERAGE: build with --build-lib build/cov,'
                         ' not --inplace')
    extra_compile_args += ['--coverage', '-O0']
    extra_link_args += ['--coverage']
sources = find_sources(PROCESSOR)
# distutils rebuilds only when a file listed here or in sources is newer
# than ni.so: list every header and .c file
depends = sorted(set(glob.glob('./ni/**/*.h', recursive=True) +
                     glob.glob('./ni/**/*.c', recursive=True)))
# switching NI_TRACE or NI_ALL_CHECKS must also rebuild: remember the
# options, and remove the built module when they change (distutils compares
# whole seconds, so a fresh stamp file alone is not reliably "newer").
# Each CPython version has its own stamp and module (its extension suffix),
# so building for one leaves the other's alone
config = repr((macros, extra_compile_args, extra_link_args))
version = '%d.%d' % sys.version_info[:2]
module = 'ni' + sysconfig.get_config_var('EXT_SUFFIX')
if NI_SANITIZE:
    stamp_dir, built_glob = 'build/asan', ['build/asan/' + module]
elif NI_COVERAGE:
    stamp_dir, built_glob = 'build/cov', ['build/cov/' + module]
else:
    stamp_dir, built_glob = 'build', [module, 'build/lib*/' + module]
stamp = os.path.join(stamp_dir, 'ni_config-%s.stamp' % version)
if not os.path.exists(stamp) or open(stamp).read() != config:
    for built in [b for g in built_glob for b in glob.glob(g)]:
        os.remove(built)
    os.makedirs(stamp_dir, exist_ok=True)
    with open(stamp, 'w') as f:
        f.write(config)
processor_dir = os.path.join('./ni', PROCESSOR)
HERE = os.path.dirname(os.path.abspath(__file__))


class build_ext_generated(build_ext):
    """Detect the features of the CPython being built for, and take its
    messages from the interpreter, into the build directory's
    ni_features_gen.h and ni_strings_gen.h, before compiling."""

    def run(self):
        # "file.h" looks next to the includer first: an old copy there
        # would win over the fresh one
        old = glob.glob(os.path.join(HERE, 'ni', '**', '*_gen.h'), recursive=True)
        if old:
            raise BuildError('remove %s: generated headers now live in'
                                 ' the build directory' % ', '.join(old))
        gen_dir = os.path.join(self.build_temp, 'gen')
        detector = 'features.py' if NI_FEATURES == 'source' else 'installed.py'
        cmd = [sys.executable, os.path.join(HERE, 'tools', 'features', detector),
               '--python', sys.executable, '--out', gen_dir]
        print(' '.join(cmd))
        sys.stdout.flush()
        if subprocess.call(cmd) != 0:
            raise BuildError('detecting the features of CPython'
                                 ' failed (above)')
        cmd = [sys.executable, os.path.join(HERE, 'tools', 'features', 'strings.py'),
               '--python', sys.executable, '--out', gen_dir]
        print(' '.join(cmd))
        sys.stdout.flush()
        if subprocess.call(cmd) != 0:
            raise BuildError('taking CPython\'s messages from the interpreter'
                                 ' failed (above)')
        gen = sorted(glob.glob(os.path.join(gen_dir, '*.h')))
        for ext in self.extensions:
            ext.include_dirs.append(gen_dir)
            ext.depends += gen
        build_ext.run(self)

    def get_libraries(self, ext):
        # 3.7's distutils links libpython into an extension when Python is
        # a shared library; 3.8 stopped. The interpreter provides those
        # symbols when it loads the module, and a manylinux wheel must not
        # name libpython, so leave it out on every version
        return [lib for lib in build_ext.get_libraries(self, ext)
                if not lib.startswith('python')]


# the package's name, version and licence are in pyproject.toml
setup(
    ext_modules=[Extension(name = 'ni',
                           sources = sources,
                           depends = depends,
                           extra_compile_args = extra_compile_args,
                           extra_link_args = extra_link_args,
                           define_macros = macros,
                           include_dirs = [processor_dir],
                           libraries = ['ffi', 'unwind'],
    )],
    cmdclass={'build_ext': build_ext_generated},
)
