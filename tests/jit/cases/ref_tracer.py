# A ref tracer (PyRefTracer_SetTracer, 3.13 on) is told of every object
# freed, also by compiled code, which calls tp_dealloc itself only while no
# tracer is set (x64/ipyencoding.c:decref_dealloc_calling); where the type
# is known only at run time, it tests the type's GC flag too, since
# _Py_Dealloc may defer the free of a type the GC tracks. The tracer here
# is a few bytes of machine code, since a Python one would be told of the
# objects its own call makes: it counts in WATCH[1] the destroy events of
# the object whose address is in WATCH[0]. Early 3.13 releases (3.13.2)
# don't report what ceval's own Py_DECREF frees, which later ones (3.13.14,
# 3.14) do, so the results expected are every free reported.
import ctypes
import mmap

TRACER = bytes([
    0x83, 0xFE, 0x01,        # cmp esi, PyRefTracer_DESTROY
    0x75, 0x0C,              # jne done
    0x48, 0x8B, 0x02,        # mov rax, [rdx]
    0x48, 0x39, 0xF8,        # cmp rax, rdi
    0x75, 0x04,              # jne done
    0x48, 0xFF, 0x42, 0x08,  # inc qword [rdx + 8]
    0x31, 0xC0,              # done: xor eax, eax
    0xC3,                    # ret
])
SET = getattr(ctypes.pythonapi, 'PyRefTracer_SetTracer', None)
WATCH = (ctypes.c_ssize_t * 2)()
if SET is not None:
    SET.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    _code = mmap.mmap(-1, mmap.PAGESIZE,
                      prot=mmap.PROT_READ | mmap.PROT_WRITE | mmap.PROT_EXEC)
    _code.write(TRACER)
    FN = ctypes.addressof(ctypes.c_char.from_buffer(_code))
    ARG = ctypes.addressof(WATCH)


class Thing:
    pass


def destroys_seen(n):
    # a Thing, a list and a float (whose types compiled code knows, the
    # float's one the GC doesn't track), each freed by compiled code, with
    # the tracer set in every other round
    if SET is None:
        return True
    out = []
    for i in range(n):
        traced = i % 2
        if traced:
            SET(FN, ARG)
        x = Thing()
        WATCH[0] = id(x)
        WATCH[1] = 0
        x = None
        y = [i]
        WATCH[0] = id(y)
        y = None
        z = i * 1.5
        WATCH[0] = id(z)
        z = None
        if traced:
            SET(None, None)
        out.append(WATCH[1])
    return out == [i % 2 * 3 for i in range(n)]


def unknown_type_destroys_seen(n):
    # a float, a Thing and a list taken off a list, so that compiled code
    # knows none of their types, each freed by compiled code, with the
    # tracer set in every other round
    if SET is None:
        return True
    out = []
    for i in range(n):
        traced = i % 2
        pool = [i * 1.5, Thing(), [i]]
        if traced:
            SET(FN, ARG)
        seen = []
        while pool:
            x = pool.pop()
            WATCH[0] = id(x)
            WATCH[1] = 0
            x = None
            seen.append(WATCH[1])
        if traced:
            SET(None, None)
        out.append(seen)
    return out == [[i % 2] * 3 for i in range(n)]


def unknown_type_deep_chain(n):
    # a list nested deeper than the C stack could free recursively, freed
    # where compiled code doesn't know its type: _Py_Dealloc's trashcan
    # frees it in steps
    out = []
    for i in range(n):
        chain = [i]
        for _ in range(200000):
            chain = [chain]
        pool = [chain]
        chain = None
        x = pool.pop()
        x = None
        out.append(len(pool))
    return out


CASES = [
    ('destroys_seen', [(6,)], {'expected': [True]}),
    ('unknown_type_destroys_seen', [(6,)], {'expected': [True]}),
    ('unknown_type_deep_chain', [(3,)], {}),
]
