# globals() in compiled code is the function's own globals (KNOWN_BUGS F47:
# the real builtin reads the nearest frame, and a compiled function called
# straight from compiled code has none, so a method called from another
# module's compiled code got that module's globals; CPython's test_typing
# and test_funcattrs, whose test methods unittest now calls compiled)
import sys
import types

MARK = 'globals_builtin'


def own_globals():
    return globals()['MARK'], globals() is sys.modules[__name__].__dict__


class Runner:
    def run(self, m):
        for i in range(3):
            r = m()
        return r


def _test(self):
    return globals()['MARK']


# a method whose globals are not this module's
Other = type('Other', (), {'test': types.FunctionType(
    _test.__code__, {'MARK': 'other', '__builtins__': __builtins__})})


def called_by_compiled_method():
    return Runner().run(Other().test)


CASES = [
    ('own_globals', [()], {}),
    ('called_by_compiled_method', [()], {}),
]
