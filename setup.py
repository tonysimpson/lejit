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
from distutils.core import setup
from distutils.extension import Extension

####################################################################
# Override defaults using environment variables e.g. dev mode
# install with debug trace points:
# > NI_TRACE=1 pip install -e .
#
# If the following is set to 1, Psyco is compiled by #including all .c
# files into ni.c.
# It provides a version of _psyco.so whose only exported (non-static)
# symbol is init_psyco(). It also seems that the GDB debugger doesn't locate
# too well non-static symbols in shared libraries. Recompiling after minor
# changes is faster if ALL_STATIC=0.
ALL_STATIC = int(os.environ.get('NI_ALL_STATIC', 0))

# Enable debugger trace points and compiler with debug options by 
# setting to 1
NI_TRACE = int(os.environ.get('NI_TRACE', 0))

# Extra checks enable with 1
ALL_CHECKS = int(os.environ.get('NI_ALL_CHECKS', 0))

####################################################################

macros = [
    ('ALL_STATIC', str(ALL_STATIC)),
    ('NI_TRACE', str(NI_TRACE)),
    ('ALL_CHECKS', str(ALL_CHECKS)),
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
        return ['./ni/ni.c']
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
extra_link_args = []
sources = find_sources(PROCESSOR, ALL_STATIC==1)
processor_dir = os.path.join('./ni', PROCESSOR)

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
                           extra_compile_args = extra_compile_args,
                           extra_link_args = extra_link_args,
                           define_macros = macros,
                           debug = True,
                           include_dirs = [processor_dir],
                           libraries = ['ffi'],
    )],
    classifiers=CLASSIFIERS,
)
