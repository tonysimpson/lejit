# The respawn replay check (ALL_CHECKS, dispatcher.c:respawn_check) must see
# the state the first compile saw. A virtual tuple of constants forced at a
# compiled call is a new constant tuple on every compile, and the callee's
# frame shares its source_known_t: psyco_unfix in the callee cleared its
# SkFlagPyObj, so the replay saw a plain constant and aborted (KNOWN_BUGS
# F51, F35; CPython's test_subclassinit test_errors). Checked build only.
import types


class Raises:
    def __enter__(self):
        return self

    def __exit__(self, t, v, tb):
        return t is not None and issubclass(t, TypeError)


def new_class_errors():
    class MyMeta(type):
        pass
    with Raises():
        class MyClass(metaclass=MyMeta, otherarg=1):
            pass
    with Raises():
        types.new_class("MyClass", (object,), dict(metaclass=MyMeta,
                                                   otherarg=1))
    return 1


CASES = [
    ('new_class_errors', [(), (), ()], {}),
]
