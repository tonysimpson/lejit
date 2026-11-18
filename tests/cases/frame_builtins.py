# A frame's builtins are fixed when it starts (frameobject.c: the caller's
# f_builtins when the globals are the same, else globals['__builtins__']),
# so replacing __builtins__ in the module's globals doesn't change what a
# running function, or a same-module callee, sees (CPython's
# test_dynamic test_cannot_replace_builtins_dict_*).
import types


def foo():
    return len([1, 2, 3])


def swap_mid_call():
    # the restoring line must find globals in the frame's builtins, not in
    # the temporary dict, or it raises NameError
    g = globals()
    old = g['__builtins__']
    g['__builtins__'] = {'len': lambda x: 7}
    try:
        r = [foo(), len('ab'), isinstance(1, int)]
    finally:
        globals()['__builtins__'] = old
    return r + [foo()]


def swap_in_loop(n):
    # the swap happens while a loop reading builtins is running
    g = globals()
    old = g['__builtins__']
    out = []
    try:
        for i in range(n):
            if i == n // 2:
                g['__builtins__'] = {'len': lambda x: 7}
            out.append(len([i]))
    finally:
        g['__builtins__'] = old
    return out


def class_and_import_mid_call():
    # LOAD_BUILD_CLASS and IMPORT_NAME read the frame's builtins too
    g = globals()
    old = g['__builtins__']
    g['__builtins__'] = {}
    try:
        class C:
            pass
        import os
        r = [C.__name__, os.sep]
    finally:
        g['__builtins__'] = old
    return r


SRC = '''
def custom():
    return len([1, 2, 3])
def call_custom():
    return custom()
'''
# code exec'd with a builtins dict of its own
NS = {'__builtins__': {'len': lambda x: 7}}
exec(SRC, NS)


def custom_builtins_dict():
    return [NS['custom'](), NS['call_custom'](), foo()]


def no_builtins():
    # globals without __builtins__ get CPython's minimal {'None': None}
    f = types.FunctionType(foo.__code__, {})
    try:
        return f()
    except NameError as e:
        return str(e)


CASES = [
    ('swap_mid_call', [(), ()], {}),
    ('swap_in_loop', [(10,), (11,)], {}),
    ('class_and_import_mid_call', [()], {}),
    ('custom_builtins_dict', [()], {}),
    ('no_builtins', [()], {}),
]
