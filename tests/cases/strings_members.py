# A T_STRING_INPLACE struct member is a str in CPython 3; Ni built bytes
# (KNOWN_BUGS F9, Objects/pstructmember.c). CPython 3.7 has no such member
# that compiled code reaches (_testcapi's type has no version tag, so its
# attributes stay generic), so this builds a heap type with one through
# ctypes and PyType_FromSpec.
import ctypes


class PyMemberDef(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char_p), ('type', ctypes.c_int),
                ('offset', ctypes.c_ssize_t), ('flags', ctypes.c_int),
                ('doc', ctypes.c_char_p)]


class PyType_Slot(ctypes.Structure):
    _fields_ = [('slot', ctypes.c_int), ('pfunc', ctypes.c_void_p)]


class PyType_Spec(ctypes.Structure):
    _fields_ = [('name', ctypes.c_char_p), ('basicsize', ctypes.c_int),
                ('itemsize', ctypes.c_int), ('flags', ctypes.c_uint),
                ('slots', ctypes.POINTER(PyType_Slot))]


T_STRING_INPLACE, READONLY, Py_tp_members = 13, 1, 72
Py_TPFLAGS_DEFAULT = 1 << 18
OFFSET, SIZE = 16, 16
_members = (PyMemberDef * 2)(
    PyMemberDef(b'text', T_STRING_INPLACE, OFFSET, READONLY, None))
_slots = (PyType_Slot * 2)(
    PyType_Slot(Py_tp_members, ctypes.cast(_members, ctypes.c_void_p)))
_spec = PyType_Spec(b'strings_members.Inplace', OFFSET + SIZE, 0,
                    Py_TPFLAGS_DEFAULT, _slots)
ctypes.pythonapi.PyType_FromSpec.restype = ctypes.py_object
ctypes.pythonapi.PyType_FromSpec.argtypes = (ctypes.POINTER(PyType_Spec),)
Inplace = ctypes.pythonapi.PyType_FromSpec(ctypes.byref(_spec))


def make(text):
    o = Inplace()
    data = text.encode()[:SIZE - 1]
    ctypes.memmove(id(o) + OFFSET, data, len(data))
    return o


def inplace(objs):
    return [(o.text, type(o.text).__name__) for o in objs]


CASES = [
    ('inplace', '[([make("abc"), make("h\xe9llo"), make("")],)]', {}),
]
