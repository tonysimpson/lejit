"""Types as Ni represents them (doc/GENERATOR_DESIGN.md "Rules"), as much
as the families moved so far need.

SINGLETONS: CPython's immortal objects, by the struct the preprocessed C
takes the address of, with Ni's function giving a new reference to its
constant vinfo. Their identity is known when compiling, and a decref of
one runs no Python.
"""
SINGLETONS = {
    '_Py_NoneStruct': 'psyco_vi_None',
    '_Py_TrueStruct': 'psyco_vi_Py_True',
    '_Py_FalseStruct': 'psyco_vi_Py_False',
    '_Py_NotImplementedStruct': 'psyco_vi_NotImplemented',
}
NOT_IMPLEMENTED = '_Py_NotImplementedStruct'
# the names the source writes them by, for messages
SINGLETON_NAMES = {'_Py_NoneStruct': 'Py_None', '_Py_TrueStruct': 'Py_True',
                   '_Py_FalseStruct': 'Py_False',
                   '_Py_NotImplementedStruct': 'Py_NotImplemented'}
