# Lazy imports (PEP 810): a module's globals can hold a lazy import's proxy,
# which a load of the global replaces with the module it imports. Compiled
# code never takes the proxy for the global's value, imports when the load
# runs (not when it compiles), and gives CPython's errors, which say where
# the import was declared. The statement is written in exec's source, so
# that this file compiles where there is no such syntax.
import builtins
import sys

try:
    compile('lazy import colorsys', '<test>', 'exec')
    LAZY = True
except SyntaxError:
    LAZY = False


def relazy(src, module):
    # a module already imported is bound at once, not through a proxy
    sys.modules.pop(module, None)
    exec(src, globals())


if LAZY:
    relazy('lazy import colorsys as lc', 'colorsys')


def kind(name):
    return type(globals()[name]).__name__


def global_reified(n):
    # the first call compiles while lc is a proxy; each round makes a new
    # one, so the load sees a proxy where it last saw the module
    if not LAZY:
        return None
    out = []
    for i in range(n):
        out.append(kind('lc'))
        out.append(lc.__name__)  # noqa: F821
        out.append(kind('lc'))
        relazy('lazy import colorsys as lc', 'colorsys')
    return out


def where(tb):
    # the frame the import was declared in, as the cause's traceback says
    code = tb.tb_frame.f_code
    return code.co_name, code.co_filename.split('/')[-1], tb.tb_lineno


def cause_of(e):
    c = e.__cause__
    return (type(e).__name__, str(e), type(c).__name__, str(c),
            where(c.__traceback__))


def missing_reified(n):
    if not LAZY:
        return None
    out = []
    for i in range(n):
        relazy('lazy import no_such_module_here as missing',
               'no_such_module_here')
        try:
            missing  # noqa: F821
        except ImportError as e:
            out.append(cause_of(e))
    return out


HELD = []


def give_held(*args, **kwargs):
    return HELD[0]


def from_proxy(n):
    # an import that gives a proxy (here through a replaced __import__):
    # from-importing from it makes another proxy, which says where it was
    # made when it fails
    if not LAZY:
        return None
    out = []
    for i in range(n):
        relazy('lazy import wave as lw', 'wave')
        HELD[:] = [globals()['lw']]
        real = builtins.__import__
        builtins.__import__ = give_held
        try:
            from anything import nothing_here
        finally:
            builtins.__import__ = real
        try:
            nothing_here.attr
        except AttributeError as e:
            out.append(str(e))
        out.append((type(nothing_here).__name__, repr(nothing_here)))
        globals()['projected'] = nothing_here
        try:
            projected  # noqa: F821
        except ImportError as e:
            out.append(cause_of(e))
    return out


# each round imports a module afresh, and an import looks up names made
# afresh, which the type cache keeps
FRESH = {'clear_type_cache': 'imports look up new names each round'}

CASES = [
    ('global_reified', [(3,)], FRESH),
    ('missing_reified', [(2,)], FRESH),
    ('from_proxy', [(2,)], FRESH),
]
