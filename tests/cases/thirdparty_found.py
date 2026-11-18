# Differences other projects' test suites found (tools/thirdparty), each cut
# down to the smallest code that shows it, with the project it came from.
import builtins


# sympy's lambdify fills a namespace with exec('from mpmath import *', {},
# namespace). Code run with locals other than its globals must not be
# compiled: the compiler's IMPORT_STAR and DELETE_NAME use the globals.
# exec is reached through a variable so that these functions compile (C15)
_exec = builtins.exec


def exec_import_star():
    g, ns = {}, {}
    _exec('from math import *', g, ns)
    return 'fabs' in ns, 'fabs' in g


def exec_delete_name():
    g, ns = {'x': 1}, {'x': 2}
    _exec('del x', g, ns)
    return sorted(g), sorted(ns)


CASES = [
    ('exec_import_star', "[()]", {}),
    ('exec_delete_name', "[()]", {}),
]
