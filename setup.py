#! /usr/bin/env python

"""
LeJIT aims to provide a module that just makes Python faster.

LeJIT is based on a fork of Armin Rigo's Psyco, Armin and the other Psyco
developers moved onto PyPy so go check that out if you want a high performance
implementation of Python (http://pypy.org).
"""
from __future__ import print_function

import os, sys, sysconfig
import glob
import json
import shutil
import platform
import subprocess
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext


class BuildError(Exception):
    pass


def fallback_reason():
    """Why no C extension is built for this Python, or None. LeJIT compiles to
    x86-64 code for CPython on Linux; anywhere else only the Python package
    is installed, whose functions then do nothing, so that installing LeJIT
    never fails. LEJIT_DEV_FALLBACK=force installs it so anyway, to test it;
    LEJIT_DEV_FALLBACK=skip builds the C extension anyway, to develop a port."""
    choice = os.environ.get('LEJIT_DEV_FALLBACK', '')
    if choice not in ('', 'force', 'skip'):
        raise SystemExit('LEJIT_DEV_FALLBACK must be force or skip, not %r' % choice)
    if choice == 'force':
        return 'LEJIT_DEV_FALLBACK=force was set when it was installed'
    if choice == 'skip':
        return None
    impl = platform.python_implementation()
    if impl != 'CPython':
        return 'LeJIT runs only on CPython, not %s' % impl
    if not sys.platform.startswith('linux') or platform.machine() != 'x86_64' \
            or sys.maxsize <= 2**32:
        return 'LeJIT runs only on x86-64 Linux, not %s %s' % (sys.platform,
                                                           platform.machine())
    if sysconfig.get_config_var('Py_GIL_DISABLED'):
        return 'LeJIT runs only on CPython with the GIL, not a free-threaded build'
    if sys.version_info >= (3, 16):
        return 'LeJIT runs only on CPython 3.7 to 3.15, not %d.%d' % sys.version_info[:2]
    return None


FALLBACK = fallback_reason()

####################################################################
# Settings for developing LeJIT, from the environment:
# > LEJIT_DEV_TRACE=1 pip install -e .

# LEJIT_DEV_TRACE=1: debugger trace points and extra checks (a checked build)
LE_TRACE = ALL_CHECKS = int(os.environ.get('LEJIT_DEV_TRACE', 0))

# LEJIT_DEV_CODESTATS=1: code size per instruction, lejit._core._codestats()
LE_CODESTATS = int(os.environ.get('LEJIT_DEV_CODESTATS', 0))

# LEJIT_DEV_CPYTHON_SRC=DIR, or fetch: detect the features in CPython's source
# (the tree DIR, or the release fetched and cached once) instead of the
# installed interpreter; the source detector is the reference the installed
# one is checked against.
CPYTHON_SRC = os.environ.get('LEJIT_DEV_CPYTHON_SRC', '')
if CPYTHON_SRC not in ('', 'fetch') and not os.path.isdir(CPYTHON_SRC):
    raise SystemExit('LEJIT_DEV_CPYTHON_SRC must be fetch or a CPython source'
                     ' tree, not %r' % CPYTHON_SRC)

####################################################################

macros = [
    ('LE_TRACE', str(LE_TRACE)),
    ('ALL_CHECKS', str(ALL_CHECKS)),
    ('LE_CODESTATS', str(LE_CODESTATS)),
]

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

PROCESSOR = None if FALLBACK else autodetect()

def find_sources(processor):
    result = glob.glob('./src/*.c')
    result += glob.glob('./src/Python/*.c')
    result += glob.glob('./src/Modules/*.c')
    result += glob.glob('./src/Objects/*.c')
    result += glob.glob('./src/%s/*.c' % (processor,))
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

if LE_TRACE:
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
# what CPython's flags warn of that is no fault in LeJIT's code:
# - type-limits: an opcode the source lacks is defined as 256 and up,
#   so a byte compared with it is always false on that version
# - deprecated-declarations: compiled code calls the API the version has
#   (3.9's PyCFunction_Call); another function would be other code emitted
extra_compile_args += ['-Wno-type-limits', '-Wno-deprecated-declarations']
# any other warning is an error, so the tree stays clean
extra_compile_args += ['-Werror']
extra_link_args = []
sources = find_sources(PROCESSOR)
# distutils rebuilds only when a file listed here or in sources is newer
# than the module: list every header and .c file
depends = sorted(set(glob.glob('./src/**/*.h', recursive=True) +
                     glob.glob('./src/**/*.c', recursive=True)))
# switching LEJIT_DEV_TRACE must also rebuild: remember the
# options, and remove the built module when they change (distutils compares
# whole seconds, so a fresh stamp file alone is not reliably "newer").
# Each CPython version has its own stamp and module (its extension suffix),
# so building for one leaves the other's alone
config = repr((macros, extra_compile_args, extra_link_args))
version = '%d.%d' % sys.version_info[:2]
module = 'lejit/_core' + sysconfig.get_config_var('EXT_SUFFIX')
stamp = os.path.join('build', 'le_config-%s.stamp' % version)
# (--compile-commands compiles nothing, so it leaves them alone)
if not FALLBACK and \
        not any(a.startswith('--compile-commands') for a in sys.argv) and \
        (not os.path.exists(stamp) or open(stamp).read() != config):
    for built in [module] + glob.glob('build/lib*/' + module):
        if os.path.exists(built):
            os.remove(built)
    os.makedirs('build', exist_ok=True)
    with open(stamp, 'w') as f:
        f.write(config)
processor_dir = os.path.join('./src', PROCESSOR or '')
HERE = os.path.dirname(os.path.abspath(__file__))
# the files of tools/features lejit.runtime_features runs: check.py and the
# detectors it calls
CHECK_FILES = ['check.py', 'installed.py', 'installed_probes.py',
               'features.py', 'ctokens.py', 'strings.py']


class build_ext_generated(build_ext):
    """Detect the features of the CPython being built for, and take its
    messages from the interpreter, into the build directory's
    le_features_gen.h and le_strings_gen.h, before compiling.
    --compile-commands FILE compiles nothing, but writes the commands the
    build would run to FILE, as a compile_commands.json (for clang-tidy)."""

    user_options = build_ext.user_options + [
        ('compile-commands=', None, 'write the compile commands to this file'
                                    ' instead of compiling')]

    def initialize_options(self):
        build_ext.initialize_options(self)
        self.compile_commands = None

    def run(self):
        # "file.h" looks next to the includer first: an old copy there
        # would win over the fresh one
        old = glob.glob(os.path.join(HERE, 'src', '**', '*_gen.h'), recursive=True)
        if old:
            raise BuildError('remove %s: generated headers now live in'
                                 ' the build directory' % ', '.join(old))
        gen_dir = os.path.join(self.build_temp, 'gen')
        detector = 'features.py' if CPYTHON_SRC else 'installed.py'
        cmd = [sys.executable, os.path.join(HERE, 'tools', 'features', detector),
               '--python', sys.executable, '--out', gen_dir]
        if CPYTHON_SRC not in ('', 'fetch'):
            cmd += ['--src', CPYTHON_SRC]
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
        # the detectors lejit.runtime_features runs go next to the module, as
        # the package lejit._features
        pkg = os.path.join(os.path.dirname(self.get_ext_fullpath('lejit._core')),
                           '_features')
        os.makedirs(pkg, exist_ok=True)
        with open(os.path.join(pkg, '__init__.py'), 'w') as f:
            f.write('"""The detectors lejit.runtime_features runs: see'
                    ' check.py."""\n')
        for name in CHECK_FILES:
            shutil.copyfile(os.path.join(HERE, 'tools', 'features', name),
                            os.path.join(pkg, name))
        # a build outside the tree (build_ext --build-lib) needs the
        # package's API next to it too
        init = os.path.join(os.path.dirname(pkg), '__init__.py')
        source = os.path.join(HERE, 'lejit', '__init__.py')
        if os.path.abspath(init) != source:
            shutil.copyfile(source, init)

    def build_extension(self, ext):
        if not self.compile_commands:
            return build_ext.build_extension(self, ext)
        # record the compiler's commands instead of running them
        commands = []

        def record(cmd):
            if '-c' in cmd:
                commands.append({'directory': HERE, 'arguments': list(cmd),
                                 'file': cmd[cmd.index('-c') + 1]})
        self.compiler.spawn = record
        self.force = True
        build_ext.build_extension(self, ext)
        with open(self.compile_commands, 'w') as f:
            json.dump(commands, f, indent=1)
        print('wrote %s (%d files)' % (self.compile_commands, len(commands)))

    def get_libraries(self, ext):
        # 3.7's distutils links libpython into an extension when Python is
        # a shared library; 3.8 stopped. The interpreter provides those
        # symbols when it loads the module, and a manylinux wheel must not
        # name libpython, so leave it out on every version
        return [lib for lib in build_ext.get_libraries(self, ext)
                if not lib.startswith('python')]


# the package's name, version and licence are in pyproject.toml
if FALLBACK:
    print('lejit: %s; installing only the Python package, which does nothing'
          % FALLBACK)
    setup()
else:
    setup(
        ext_modules=[Extension(name = 'lejit._core',
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
