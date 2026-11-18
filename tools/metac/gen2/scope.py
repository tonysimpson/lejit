"""What the new translator attempts (doc/GENERATOR_DESIGN.md "Rules"), one
family at a time (Migration step 3).

A family is one CPython file and one header: its metas are the functions
of the file whose meta Ni declares in `decls` (conventions.psyco_name),
the functions filling the type slots `slots` names, which the header
registers (gen_FILE_init), and the file's functions those call. Ni's metas
of other CPython functions, which calls go to, are declared in NI_HEADERS.
`reps` names the representations (types.py) the family's C is read with.
`builtins` names a module's functions by their Python names: which C
function each is, and its ml_flags, come from the module's method table
(slots.builtin_functions); the header's gen_FILE_init(md) registers each
meta under those flags.

Slots, as (type, [slot]): a field of one of the type's method tables, or a
function-pointer field of the type itself.
"""
import collections

Family = collections.namedtuple('Family',
                                'name cfile decls includer header slots reps builtins')
Family.__new__.__defaults__ = ((), (), ())

FAMILIES = {
    'abstract': Family('abstract', 'Objects/abstract.c', 'ni/Objects/pabstract.h',
                       'pabstract.c', 'pabstract_gen.h'),
    'long': Family('long', 'Objects/longobject.c', None, 'plongobject.c', 'plongobject_gen.h',
                   [('PyLong_Type', ['nb_add', 'nb_subtract', 'nb_multiply', 'nb_negative',
                                     'nb_invert', 'nb_bool', 'nb_positive', 'nb_absolute',
                                     'nb_true_divide', 'nb_power', 'nb_float'])],
                   ('word', 'double')),
    'float': Family('float', 'Objects/floatobject.c', 'ni/Objects/pfloatobject.h',
                    'pfloatobject.c', 'pfloatslots_gen.h',
                    [('PyFloat_Type', ['nb_add', 'nb_subtract', 'nb_multiply', 'nb_remainder',
                                       'nb_negative', 'nb_positive', 'nb_absolute', 'nb_bool',
                                       'nb_true_divide', 'tp_hash'])],
                    ('double',)),
    # ceil and floor stay hand-written (pmath.c: an int result)
    'math': Family('math', 'Modules/mathmodule.c', None, 'pmath.c', 'pmathmodule_gen.h',
                   reps=('double',),
                   builtins=('acos', 'asin', 'atan', 'cos', 'cosh', 'exp', 'fabs', 'sin',
                             'sinh', 'sqrt', 'tan', 'tanh')),
}

NI_HEADERS = ['ni/Objects/pabstract.h', 'ni/Objects/pobject.h', 'ni/Objects/pfloatobject.h',
              'ni/Objects/plongobject.h', 'ni/Objects/piterobject.h',
              'ni/Objects/ptupleobject.h', 'ni/Objects/plistobject.h',
              'ni/Objects/pdictobject.h', 'ni/Objects/pboolobject.h',
              'ni/Objects/pstructmember.h', 'ni/Objects/pmethodobject.h']
