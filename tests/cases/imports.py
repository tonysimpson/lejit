# from-imports inside functions: IMPORT_FROM is ceval.c:import_from
# (KNOWN_BUGS F44: Ni raised Python 2's "cannot import name x", without the
# name and path attributes, and had no sys.modules fallback)
import sys
import types

PKG = types.ModuleType('ni_test_pkg')
sys.modules['ni_test_pkg'] = PKG
# a submodule that is in sys.modules but not yet an attribute of its
# package, as in a circular relative import
sys.modules['ni_test_pkg.sub'] = types.ModuleType('ni_test_pkg.sub')


def from_import_missing(k):
    try:
        from os import does_not_exist_here
    except ImportError as e:
        return str(e).split(' (')[0], e.name, e.path == __import__('os').__file__
    return 'imported'


def from_import_fallback(k):
    from ni_test_pkg import sub
    return sub.__name__


def from_import_present(k):
    from os import path
    return path.__name__


CASES = [
    ('from_import_missing', [(1,)], {}),
    ('from_import_fallback', [(1,)], {}),
    ('from_import_present', [(1,)], {}),
]
