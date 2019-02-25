import os, sys
import glob

from setuptools import setup
from setuptools.extension import Extension


def find_sources(processor, all_static):
    if all_static:
        return ['./lejit/lejit.c']
    else:
        result = glob.glob('./lejit/*.c')
        return result


extra_compile_args = ['-O3']

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
    name             = "lejit",
    version          = "0.1a1",
    description      = "Plugin JIT for CPython",
    maintainer       = "Tony Simpson",
    maintainer_email = "agjasimpson@gmail.com",
    url              = "http://github.com/tonysimpson/lejit",
    license          = "MIT License",
    ext_modules=[Extension(name = 'lejit',
                           sources = ['./lejit/lejit.c', './lejit/mergepoints.c',],
                           include_dirs=['./lejit/cpython/Include'],
                           extra_compile_args = extra_compile_args)], 
    classifiers=CLASSIFIERS,
)
