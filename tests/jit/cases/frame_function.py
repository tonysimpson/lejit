# The functions of a compiled frame's code, as gc.get_referrers finds them
# while the frame runs: only the real one, as trace names a caller's class
# by it (Trace.runfunc). The frame read here is one LeJIT makes.
import gc
import inspect
import sys


def functions_of_caller():
    code = sys._getframe(1).f_code
    return len([o for o in gc.get_referrers(code) if inspect.isfunction(o)])


def caller(a):
    return functions_of_caller() + a


def outer(a):
    return caller(a)


CASES = [
    ('outer', "[(0,), (1,)]", {}),
]
