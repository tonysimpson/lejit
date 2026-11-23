# A property on a ModuleType subclass is a data descriptor, so it wins over
# the module's __dict__, as for any other object: for a module made as the
# subclass, and for one whose __class__ becomes it. CPython 3.14's
# specialised attribute load reads the module's __dict__ instead once the
# load is warm, so these cases check LeJIT against the language rule
# ('expected'), not against plain CPython's output.
import types


class Sub(types.ModuleType):
    @property
    def answer(self):
        return 42


def made_as_subclass(n):
    out = set()
    for i in range(n):
        m = Sub('m%d' % i)
        m.__dict__['answer'] = 'dict'
        out.add(m.answer)
    return sorted(out, key=repr)


def class_changed(n):
    before, after = set(), set()
    for i in range(n):
        m = types.ModuleType('m%d' % i)
        m.answer = 'dict'
        before.add(m.answer)
        m.__class__ = Sub
        after.add(m.answer)
    return sorted(before, key=repr), sorted(after, key=repr)


def one_module(n):
    m = types.ModuleType('one')
    m.answer = 'dict'
    out = []
    for i in range(n):
        out.append(m.answer)
        m.__class__ = Sub if i % 2 == 0 else types.ModuleType
    return out


CASES = [
    ('made_as_subclass', [(1,), (50,)], {'expected': [[42], [42]]}),
    ('class_changed', [(1,), (50,)],
     {'expected': [(['dict'], [42]), (['dict'], [42])]}),
    ('one_module', [(6,)],
     {'expected': [['dict', 42, 'dict', 42, 'dict', 42]]}),
]
