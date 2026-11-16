# Builtins that read the calling frame's locals can't see a compiled
# function's locals, so such functions are left to the interpreter
# (KNOWN_BUGS C15; CPython's test_collections used eval('p.z', locals())).


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
    ('uses_locals', [(1,)], {'interpreted': 'C15'}),
    ('uses_eval', [(1,)], {'interpreted': 'C15'}),
    ('uses_eval_locals', [(1,)], {'interpreted': 'C15'}),
    ('uses_vars', [(1,)], {'interpreted': 'C15'}),
    ('uses_dir', [(1,)], {'interpreted': 'C15'}),
    ('uses_exec', [(1,)], {'interpreted': 'C15'}),
    ('attribute_names', "[(Thing(),)]", {}),
]
