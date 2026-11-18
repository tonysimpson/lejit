# Member descriptors through the generated member_get and PyMember_GetOne
# (pdescrobject_gen.h, pstructmember_gen.h): one heap type, made with
# ctypes and PyType_FromSpec, with a member of every T_* type, each in its
# own 8-byte slot; READONLY ones; T_OBJECT and T_OBJECT_EX holding NULL
# (None, and AttributeError); __slots__ (T_OBJECT_EX) on a class and on a
# subclass, whose descriptor's d_type is the base.
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


READONLY, Py_tp_members = 1, 72
Py_TPFLAGS_DEFAULT = 1 << 18
# (name, T_ code, ctypes type to write the slot with, READONLY)
MEMBERS = [
    ('m_short', 0, ctypes.c_short, 0), ('m_int', 1, ctypes.c_int, 0),
    ('m_long', 2, ctypes.c_long, 0), ('m_float', 3, ctypes.c_float, 0),
    ('m_double', 4, ctypes.c_double, 0), ('m_string', 5, ctypes.c_void_p, READONLY),
    ('m_object', 6, ctypes.c_void_p, 0), ('m_char', 7, ctypes.c_char, 0),
    ('m_byte', 8, ctypes.c_byte, 0), ('m_ubyte', 9, ctypes.c_ubyte, 0),
    ('m_uint', 10, ctypes.c_uint, 0), ('m_ushort', 11, ctypes.c_ushort, 0),
    ('m_ulong', 12, ctypes.c_ulong, 0), ('m_inplace', 13, ctypes.c_char * 8, READONLY),
    ('m_bool', 14, ctypes.c_char, 0), ('m_object_ex', 16, ctypes.c_void_p, 0),
    ('m_longlong', 17, ctypes.c_longlong, 0), ('m_ulonglong', 18, ctypes.c_ulonglong, 0),
    ('m_ssize', 19, ctypes.c_ssize_t, READONLY), ('m_none', 20, ctypes.c_void_p, READONLY),
    ('m_int_ro', 1, ctypes.c_int, READONLY), ('m_object_ro', 6, ctypes.c_void_p, READONLY),
]
BASE = 16
_members = (PyMemberDef * (len(MEMBERS) + 1))(*[
    PyMemberDef(n.encode(), t, BASE + 8 * k, fl, None)
    for k, (n, t, _, fl) in enumerate(MEMBERS)])
_slots = (PyType_Slot * 2)(
    PyType_Slot(Py_tp_members, ctypes.cast(_members, ctypes.c_void_p)))
_spec = PyType_Spec(b'gen_members.All', BASE + 8 * len(MEMBERS), 0,
                    Py_TPFLAGS_DEFAULT, _slots)
ctypes.pythonapi.PyType_FromSpec.restype = ctypes.py_object
ctypes.pythonapi.PyType_FromSpec.argtypes = (ctypes.POINTER(PyType_Spec),)
All = ctypes.pythonapi.PyType_FromSpec(ctypes.byref(_spec))
OFFSET = {n: BASE + 8 * k for k, (n, _, _, _) in enumerate(MEMBERS)}
CTYPE = {n: c for n, _, c, _ in MEMBERS}
_keep = []          # what the objects' members point to, alive for good


def put(o, name, value):
    addr = id(o) + OFFSET[name]
    if CTYPE[name] is ctypes.c_void_p and isinstance(value, bytes):
        buf = ctypes.create_string_buffer(value)
        _keep.append(buf)
        value = ctypes.addressof(buf)
    elif CTYPE[name] is ctypes.c_void_p and value is not None and \
            name in ('m_object', 'm_object_ex', 'm_object_ro'):
        _keep.append(value)
        ctypes.pythonapi.Py_IncRef(ctypes.py_object(value))
        value = id(value)
    if isinstance(CTYPE[name], type) and issubclass(CTYPE[name], ctypes.Array):
        ctypes.memmove(addr, value, len(value))
        return
    CTYPE[name].from_address(addr).value = value


def make(**values):
    o = All()
    for n, v in values.items():
        put(o, n, v)
    return o


FULL = make(m_short=-12345, m_int=-2000000000, m_long=-(1 << 62), m_float=1.5,
            m_double=-2.25e300, m_string=b'h\xc3\xa9llo', m_object=[1, 2],
            m_char=b'z', m_byte=-100, m_ubyte=250, m_uint=4000000000,
            m_ushort=65000, m_ulong=(1 << 64) - 1, m_inplace=b'inpl\0',
            m_bool=b'\x01', m_object_ex='ex', m_longlong=-(1 << 63),
            m_ulonglong=(1 << 64) - 2, m_ssize=(1 << 62), m_int_ro=7,
            m_object_ro=(3,))
EMPTY = make()      # NULL pointers, zero numbers, '' in place
NAMES = [n for n, _, _, _ in MEMBERS if n != 'm_object_ex']


def numbers(o):
    return (o.m_short, o.m_int, o.m_long, o.m_float, o.m_double, o.m_byte,
            o.m_ubyte, o.m_uint, o.m_ushort, o.m_ulong, o.m_bool,
            o.m_longlong, o.m_ulonglong, o.m_ssize, o.m_int_ro)


def numbers_loop(o, n):
    t = 0
    for i in range(n):
        t += o.m_short + o.m_int + o.m_byte + o.m_ubyte + o.m_ushort + o.m_uint
    return t


def texts(o):
    return (o.m_string, o.m_char, o.m_inplace, type(o.m_inplace).__name__,
            o.m_none, o.m_object, o.m_object_ro)


def object_ex(o):
    try:
        return o.m_object_ex
    except AttributeError as e:
        return 'AttributeError', str(e)


def all_attrs(o):
    return [getattr(o, n) for n in NAMES]


def readonly(o):
    out = []
    for n in ('m_string', 'm_inplace', 'm_ssize', 'm_none', 'm_int_ro', 'm_object_ro'):
        try:
            setattr(o, n, 1)
            out.append(n)
        except (AttributeError, TypeError) as e:
            out.append(type(e).__name__)
    return out, o.m_int_ro, o.m_object_ro


def write_read(o, v):
    o.m_int = v
    o.m_object = v
    return o.m_int, o.m_object


class Slotted:
    __slots__ = ('a', 'b')


class SubSlotted(Slotted):
    __slots__ = ('c',)


def slotted(objs):
    out = []
    for o in objs:
        try:
            out.append((o.a, o.b))
        except AttributeError as e:
            out.append(('AttributeError', str(e)))
    return out


def _slotted(cls, **kw):
    o = cls()
    for k, v in kw.items():
        setattr(o, k, v)
    return o


def sub_slotted(objs):
    out = []
    for o in objs:
        try:
            out.append((o.a, o.c))
        except AttributeError as e:
            out.append(('AttributeError', str(e)))
    return out


def slot_deleted(o):
    o.a = 1
    first = o.a
    del o.a
    try:
        return first, o.a
    except AttributeError as e:
        return first, 'AttributeError', str(e)


CASES = [
    ('numbers', '[(FULL,), (EMPTY,)]', {}),
    ('numbers_loop', '[(FULL, 20), (EMPTY, 3)]', {}),
    ('texts', '[(FULL,), (EMPTY,)]', {}),
    ('object_ex', '[(FULL,), (EMPTY,), (FULL,)]', {}),
    ('all_attrs', '[(FULL,), (EMPTY,)]', {}),
    ('readonly', '[(make(m_int_ro=3),)]', {}),
    ('write_read', '[(make(), 5), (make(), -6)]', {}),
    ('slotted', '[([_slotted(Slotted, a=1, b="x"), _slotted(Slotted, a=2), '
                '_slotted(Slotted, a=[3], b=None)],)]', {}),
    ('sub_slotted', '[([_slotted(SubSlotted, a=1, b=2, c=3), _slotted(SubSlotted, c=4), '
                    '_slotted(SubSlotted, a=5)],)]', {}),
    ('slot_deleted', '[(Slotted(),), (SubSlotted(),)]', {}),
]
