# C code reading frames through CPython's C API: an extension (ctypes here)
# calling it from a frame ceval entered, and from a compiled callee that
# has no frame of its own in CPython, sees that frame and its locals as
# they are, as plain CPython's code sees them.
import ctypes
import sys

api = ctypes.pythonapi


def _function(name, restype, *argtypes):
    f = getattr(api, name, None)
    if f is not None:
        f.restype = restype
        f.argtypes = argtypes
    return f


# results as addresses (ctypes takes a py_object result as a new
# reference)
_GetFrame = _function('PyEval_GetFrame', ctypes.c_void_p)
_GetLocals = _function('PyEval_GetLocals', ctypes.c_void_p)
_GetGlobals = _function('PyEval_GetGlobals', ctypes.c_void_p)
_GetBuiltins = _function('PyEval_GetBuiltins', ctypes.c_void_p)
_ThreadState = _function('PyThreadState_Get', ctypes.c_void_p)
_ThreadFrame = _function('PyThreadState_GetFrame', ctypes.c_void_p,
                         ctypes.c_void_p)
_GetBack = _function('PyFrame_GetBack', ctypes.c_void_p, ctypes.py_object)
_FrameLocals = _function('PyFrame_GetLocals', ctypes.c_void_p,
                         ctypes.py_object)
_GetVar = _function('PyFrame_GetVar', ctypes.c_void_p, ctypes.py_object,
                    ctypes.py_object)
_GetVarString = _function('PyFrame_GetVarString', ctypes.c_void_p,
                          ctypes.py_object, ctypes.c_char_p)
_EvalFrameLocals = _function('PyEval_GetFrameLocals', ctypes.c_void_p)
_EvalFrameGlobals = _function('PyEval_GetFrameGlobals', ctypes.c_void_p)
_EvalFrameBuiltins = _function('PyEval_GetFrameBuiltins', ctypes.c_void_p)
_DecRef = _function('Py_DecRef', None, ctypes.c_void_p)
_LineNumber = _function('PyFrame_GetLineNumber', ctypes.c_int,
                        ctypes.py_object)


def _borrowed(address):
    return ctypes.cast(address, ctypes.py_object).value


def _new(address):
    if not address:
        return None
    o = _borrowed(address)
    _DecRef(address)
    return o


def _names(f, n=3):
    out = []
    while f is not None and len(out) < n:
        out.append(f.f_code.co_name)
        f = f.f_back
    return out


def _shown(d):
    return sorted((k, v) for k, v in d.items() if isinstance(v, int))


# called by the harness (a frame ceval entered) and directly by compiled
# callers
def seen_here(a):
    x = a + 1
    f = _borrowed(_GetFrame())
    out = [_names(f), _shown(_borrowed(_GetLocals())),
           _borrowed(_GetGlobals()) is globals(),
           _borrowed(_GetBuiltins()) is f.f_builtins]
    if _ThreadFrame is not None:
        out.append(_names(_new(_ThreadFrame(_ThreadState()))))
        out.append(_names(_new(_GetBack(f)), 2))
    if _FrameLocals is not None:
        out.append(_shown(_new(_FrameLocals(f))))
    if _GetVar is not None:
        out.append((_new(_GetVar(f, 'x')), _new(_GetVarString(f, b'a'))))
    if _EvalFrameLocals is not None:
        out.append((_shown(_new(_EvalFrameLocals())),
                    _new(_EvalFrameGlobals()) is globals(),
                    _new(_EvalFrameBuiltins()) is f.f_builtins))
    return out, x


def direct_callee(n):
    y = n * 2
    return seen_here(y), y


def two_down(n):
    z = n + 5
    return direct_callee(z), z


# the line C reads of the innermost frame, at two places in one call: the
# frame it gets is where the call is each time
def _lines_here():
    first = _LineNumber(_borrowed(_GetFrame()))
    second = _LineNumber(_borrowed(_GetFrame()))
    return second - first


def lines_from_c(n):
    return [_lines_here() for i in range(n)]


CASES = [
    ('seen_here', "[(1,), (2,)]", {}),
    ('direct_callee', "[(1,), (3,)]", {}),
    ('two_down', "[(1,)]", {}),
    ('lines_from_c', [(3,)], {}),
]
