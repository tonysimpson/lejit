# Module-level code iterating its own globals in a comprehension, as
# capstone's __init__.py does: compiling the comprehension mustn't add to
# the dict being iterated, as a warning raised while compiling would by
# putting a __warningregistry__ there (pycompiler.c:codestats_flush, in a
# LEJIT_DEV_CODESTATS=1 build), nor warn at all.
import warnings

run = exec  # reached by another name, the caller is still compiled

CODE = compile('''
X_A = 1
X_B = 2
names = {v: k for k, v in locals().items() if k.startswith('X_')}
''', '<module_locals>', 'exec')


def module_comprehension(k):
    g = {'__name__': 'module_locals_%d' % k}
    with warnings.catch_warnings():
        warnings.simplefilter('error')
        run(CODE, g)
    return g['names'], sorted(g)


CASES = [
    ('module_comprehension', [(1,), (2,)], {}),
]
