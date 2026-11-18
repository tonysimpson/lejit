#! /usr/bin/env python

"""
Ni aims to provide a module that just makes Python faster.

Ni is based on a fork of Armin Rigo's Psyco, Armin and the other Psyco
developers moved onto PyPy so go check that out if you want a high performance
implementation of Python (http://pypy.org).
"""
from __future__ import print_function

import os, sys
import glob
import subprocess
from distutils.core import setup
from distutils.extension import Extension
from distutils.command.build_ext import build_ext
from distutils.errors import DistutilsError

####################################################################
# Override defaults using environment variables e.g. dev mode
# install with debug trace points:
# > NI_TRACE=1 pip install -e .
#
# If the following is set to 1, Ni is compiled by #including all .c files
# into ni.c (initialize.h, x64/iinitialize.h); platform.c and
# Python/pyruntime.c stay separate units. Everything else is static, so the
# compiler warns about unused functions, and recompiling after small changes
# is slower.
ALL_STATIC = int(os.environ.get('NI_ALL_STATIC', 0))

# Enable debugger trace points and compiler with debug options by 
# setting to 1
NI_TRACE = int(os.environ.get('NI_TRACE', 0))

# Extra checks enable with 1
ALL_CHECKS = int(os.environ.get('NI_ALL_CHECKS', 0))

# Code size per instruction, ni._codestats() (tools/codesize.py)
NI_CODESTATS = int(os.environ.get('NI_CODESTATS', 0))

# The generators (tools/metac/generate.py) need a newer Python than the one
# Ni is built for. NI_CPYTHON_SRC: the CPython source tree to generate from,
# instead of the cached release tarball (doc/BUILD.md).
NI_GEN_PYTHON = os.environ.get('NI_GEN_PYTHON', 'python3')

####################################################################

macros = [
    ('ALL_STATIC', str(ALL_STATIC)),
    ('NI_TRACE', str(NI_TRACE)),
    ('ALL_CHECKS', str(ALL_CHECKS)),
    ('NI_CODESTATS', str(NI_CODESTATS)),
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

PROCESSOR = autodetect()

def find_sources(processor, all_static):
    if all_static:
        # Python/pyruntime.c is built with Py_BUILD_CORE, on its own;
        # platform.c is kept apart from the Python headers
        return ['./ni/ni.c', './ni/platform.c', './ni/Python/pyruntime.c']
    else:
        result = glob.glob('./ni/*.c')
        result += glob.glob('./ni/Python/*.c')
        result += glob.glob('./ni/Modules/*.c')
        result += glob.glob('./ni/Objects/*.c')
        result += glob.glob('./ni/%s/*.c' % (processor,))
        return result

if NI_TRACE:
    extra_compile_args = ['-O0', '-g3', '-Wall', '-fno-stack-protector']
else:
    extra_compile_args = ['-O3']
# pyenv's CFLAGS demote these to warnings; a missing prototype makes a
# pointer result an int, truncated (a segfault, found adding
# psyco_current_frame), so keep them errors
extra_compile_args += ['-Werror=implicit-function-declaration',
                       '-Werror=int-conversion',
                       '-Werror=incompatible-pointer-types']
extra_link_args = []
sources = find_sources(PROCESSOR, ALL_STATIC==1)
# distutils rebuilds only when a file listed here or in sources is newer
# than ni.so: list every header and .c file (ni.c #includes the .c files)
depends = sorted(set(glob.glob('./ni/**/*.h', recursive=True) +
                     glob.glob('./ni/**/*.c', recursive=True)))
# switching NI_TRACE or NI_ALL_CHECKS must also rebuild: remember the
# options, and remove the built module when they change (distutils compares
# whole seconds, so a fresh stamp file alone is not reliably "newer")
config = repr((macros, extra_compile_args))
stamp = os.path.join('build', 'ni_config.stamp')
if not os.path.exists(stamp) or open(stamp).read() != config:
    for built in glob.glob('ni*.so') + glob.glob('build/lib*/ni*.so'):
        os.remove(built)
    os.makedirs('build', exist_ok=True)
    with open(stamp, 'w') as f:
        f.write(config)
processor_dir = os.path.join('./ni', PROCESSOR)
HERE = os.path.dirname(os.path.abspath(__file__))


class build_ext_generated(build_ext):
    """Generate the translated headers from the source of the CPython being
    built for (D67), into the build directory, before compiling."""

    def run(self):
        # "file.h" looks next to the includer first: an old copy there
        # would win over the fresh one
        old = glob.glob(os.path.join(HERE, 'ni', '**', '*_gen.h'), recursive=True)
        if old:
            raise DistutilsError('remove %s: generated headers now live in'
                                 ' the build directory' % ', '.join(old))
        gen_dir = os.path.join(self.build_temp, 'gen')
        cmd = [NI_GEN_PYTHON, os.path.join(HERE, 'tools', 'metac', 'generate.py'),
               '--python', sys.executable, '--out', gen_dir]
        print(' '.join(cmd))
        sys.stdout.flush()
        try:
            # a fixed string hash, so that no set order can vary the headers
            env = dict(os.environ, PYTHONHASHSEED='0')
            ok = subprocess.call(cmd, env=env) == 0
        except OSError as e:
            raise DistutilsError('cannot run the generators with %s (%s): set'
                                 ' NI_GEN_PYTHON to a Python 3.10 or later'
                                 % (NI_GEN_PYTHON, e))
        if not ok:
            raise DistutilsError('generating from CPython\'s source failed'
                                 ' (above); see doc/BUILD.md')
        gen = sorted(glob.glob(os.path.join(gen_dir, '*.h')))
        for ext in self.extensions:
            ext.include_dirs.append(gen_dir)
            ext.depends += gen
        build_ext.run(self)


CLASSIFIERS = [
    'Development Status :: 2 - Pre-Alpha',
    'Intended Audience :: Developers',
    'License :: OSI Approved :: MIT License',
    'Operating System :: OS Independent',
    'Programming Language :: Python',
    'Programming Language :: C',
    'Topic :: Software Development :: Compilers',
    'Topic :: Software Development :: Interpreters',
    ]

setup( 
    name             = "ni",
    version          = "0.1a2",
    description      = "Plugin JIT for CPython",
    maintainer       = "Tony Simpson",
    maintainer_email = "agjasimpson@gmail.com",
    url              = "http://github.com/tonysimpson/ni",
    license          = "MIT License",
    long_description = __doc__,
    packages         = ['ni'],
    ext_modules=[Extension(name = 'ni',
                           sources = sources,
                           depends = depends,
                           extra_compile_args = extra_compile_args,
                           extra_link_args = extra_link_args,
                           define_macros = macros,
                           debug = True,
                           include_dirs = [processor_dir],
                           libraries = ['ffi'],
    )],
    classifiers=CLASSIFIERS,
    cmdclass={'build_ext': build_ext_generated},
)
