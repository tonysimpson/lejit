# _warnings.warn walks the frames in C. LeJIT replaces it at start; a
# _warnings imported again afterwards (test.support.import_fresh_module)
# has the original C function, which compiled code calls with the compiled
# frames current all the same (pmethodobject.c:PsycoCFunction_Call), so
# stacklevel finds the callee's frame, called directly or by keyword.
import importlib
import sys
import warnings


def fresh_warn():
    saved = sys.modules.pop('_warnings', None)
    try:
        return importlib.import_module('_warnings').warn
    finally:
        if saved is not None:
            sys.modules['_warnings'] = saved


WARN = [None]


def inner(message, stacklevel=1):
    WARN[0](message, stacklevel=stacklevel)


def inner_positional(message, stacklevel=1):
    WARN[0](message, UserWarning, stacklevel)


def where(n):
    WARN[0] = fresh_warn()
    out = []
    for i in range(n):
        with warnings.catch_warnings(record=True) as w:
            warnings.simplefilter('always')
            inner('a', stacklevel=1)
            inner('b', 1)
            inner_positional('c', stacklevel=1)
            inner('d', stacklevel=2)
        out.append([(x.filename.rsplit('/', 1)[-1], x.lineno) for x in w])
    return out


CASES = [
    ('where', [(3,)],
     {'warmup': (40, 'compiling goes on until round 34 on 3.12')}),
]
