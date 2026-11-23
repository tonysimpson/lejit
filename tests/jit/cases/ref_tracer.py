# A ref tracer (PyRefTracer_SetTracer, 3.13 on) is told of every object
# freed, also by compiled code, which calls tp_dealloc itself only while no
# tracer is set (x64/ipyencoding.c:decref_dealloc_calling). The tracer here
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
    # a Thing and a list (whose type compiled code knows), each freed by
    # compiled code, with the tracer set in every other round
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
        if traced:
            SET(None, None)
        out.append(WATCH[1])
    return out == [i % 2 * 2 for i in range(n)]


CASES = [
    ('destroys_seen', [(6,)], {'expected': [True]}),
]
