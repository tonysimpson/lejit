"""What the new translator attempts (doc/GENERATOR_DESIGN.md "Rules"), one
family at a time (Migration step 3).

A family is one CPython file and one header: its metas are the functions
of the file whose meta Ni declares in `decls` (conventions.psyco_name),
and the file's functions those call. Ni's metas of other CPython
functions, which calls go to, are declared in NI_HEADERS.
"""
import collections

Family = collections.namedtuple('Family', 'name cfile decls includer header')

FAMILIES = {
    'abstract': Family('abstract', 'Objects/abstract.c', 'ni/Objects/pabstract.h',
                       'pabstract.c', 'pabstract_gen.h'),
}

NI_HEADERS = ['ni/Objects/pabstract.h', 'ni/Objects/pobject.h', 'ni/Objects/pfloatobject.h',
              'ni/Objects/plongobject.h', 'ni/Objects/piterobject.h',
              'ni/Objects/ptupleobject.h', 'ni/Objects/plistobject.h',
              'ni/Objects/pdictobject.h', 'ni/Objects/pboolobject.h',
              'ni/Objects/pstructmember.h', 'ni/Objects/pmethodobject.h']
