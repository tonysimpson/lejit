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
function-pointer field of the type itself; (type, [slot], file) for a type
another file defines, whose slots this file's functions fill.
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
    'tuple': Family('tuple', 'Objects/tupleobject.c', None, 'ptupleobject.c',
                    'ptupleobject_gen.h',
                    [('PyTuple_Type', ['sq_length', 'sq_item', 'sq_contains', 'sq_concat',
                                       'sq_repeat', 'tp_richcompare'])],
                    ('known_length',)),
    'list': Family('list', 'Objects/listobject.c', None, 'plistobject.c', 'plistobject_gen.h',
                   [('PyList_Type', ['sq_length'])]),
    'dict': Family('dict', 'Objects/dictobject.c', None, 'pdictobject.c', 'pdictobject_gen.h',
                   [('PyDict_Type', ['mp_length'])]),
    'bytes': Family('bytes', 'Objects/bytesobject.c', None, 'pbytesobject.c',
                    'pbytesobject_gen.h', [('PyBytes_Type', ['sq_length'])]),
    'unicode': Family('unicode', 'Objects/unicodeobject.c', None, 'punicodeobject.c',
                      'punicodeobject_gen.h',
                      [('PyUnicode_Type', ['sq_length', 'sq_concat', 'sq_contains',
                                           'tp_richcompare'])]),
    # ceil and floor stay hand-written (pmath.c: an int result)
    'math': Family('math', 'Modules/mathmodule.c', None, 'pmath.c', 'pmathmodule_gen.h',
                   reps=('double',),
                   builtins=('acos', 'asin', 'atan', 'cos', 'cosh', 'exp', 'fabs', 'sin',
                             'sinh', 'sqrt', 'tan', 'tanh')),
    # PyMember_GetOne, which member_get calls (declared in pstructmember.h)
    'structmember': Family('structmember', 'Python/structmember.c', 'ni/Objects/pstructmember.h',
                           'pstructmember.c', 'pstructmember_gen.h', reps=('double',)),
    'descr': Family('descr', 'Objects/descrobject.c', None, 'pdescrobject.c',
                    'pdescrobject_gen.h', [('PyMemberDescr_Type', ['tp_descr_get']),
                                           ('PyMethodDescr_Type', ['tp_descr_get'])]),
    # object.c's functions in the slots of types other files define
    'object': Family('object', 'Objects/object.c', None, 'pobject.c', 'pobject_gen.h',
                     [('PySeqIter_Type', ['tp_iter'], 'Objects/iterobject.c'),
                      ('PyRangeIter_Type', ['tp_iter'], 'Objects/rangeobject.c')]),
    # iter, next, chr and ord stay C calls (METACOMPILER failures log)
    'bltin': Family('bltin', 'Python/bltinmodule.c', None, 'pbltinmodule.c',
                    'pbltinmodule_gen.h',
                    builtins=('abs', 'callable', 'delattr', 'divmod', 'getattr', 'hasattr',
                              'hash', 'id', 'isinstance', 'issubclass', 'len', 'pow',
                              'setattr')),
}

NI_HEADERS = ['ni/Objects/pabstract.h', 'ni/Objects/pobject.h', 'ni/Objects/pfloatobject.h',
              'ni/Objects/plongobject.h', 'ni/Objects/piterobject.h',
              'ni/Objects/ptupleobject.h', 'ni/Objects/plistobject.h',
              'ni/Objects/pdictobject.h', 'ni/Objects/pboolobject.h',
              'ni/Objects/pstructmember.h', 'ni/Objects/pmethodobject.h',
              'ni/Python/pbltinmodule.h']

# The argument binding (binding.py): the C function making and running a
# frame whose binding of the arguments, between the two, is generated;
# Ni declares the binder (conventions.psyco_name of the function) in
# `decls`, and the header is included by `includer`.
Binding = collections.namedtuple('Binding', 'cfile function decls includer header')
BINDING = Binding('Python/ceval.c', '_PyEval_EvalCodeWithName', 'ni/psyfunc.h', 'psyfunc.c',
                  'psyfunc_gen.h')

# The opcode cases (opcodes.py): the cases of CEVAL_FUNCTION's switch on
# the opcode become cases of pycompiler.c's opcode switch.
CEVAL = Family('ceval', 'Python/ceval.c', None, 'pycompiler.c', 'pycompiler_gen.h')
CEVAL_FUNCTION = '_PyEval_EvalFrameDefault'
