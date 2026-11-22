# Builtins that read the calling frame's locals can't see a compiled
# function's locals, so such functions are left to the interpreter
# (as for CPython's test_collections' eval('p.z', locals())).


def uses_locals(x):
    y = x * 2
    return sorted(locals().items())


def uses_eval(x):
    p = (x, x + 1)
    return eval('p[1] + x')


def uses_eval_locals(x):
    p = x + 1
    return eval('p', locals())


def uses_vars(x):
    z = x
    return sorted(vars())


def uses_dir(x):
    w = x
    return dir()


def uses_exec(x):
    out = []
    exec('out.append(x * 3)')
    return out


class Thing:
    vars = 5
    eval = 7


# attributes with those names don't matter: still compiled
def attribute_names(t):
    return t.vars + t.eval


CASES = [
    ('uses_locals', [(1,)], {'interpreted': 'reads its own locals'}),
    ('uses_eval', [(1,)], {'interpreted': 'reads its own locals'}),
    ('uses_eval_locals', [(1,)], {'interpreted': 'reads its own locals'}),
    ('uses_vars', [(1,)], {'interpreted': 'reads its own locals'}),
    ('uses_dir', [(1,)], {'interpreted': 'reads its own locals'}),
    ('uses_exec', [(1,)], {'interpreted': 'reads its own locals'}),
    ('attribute_names', "[(Thing(),)]", {}),
]
