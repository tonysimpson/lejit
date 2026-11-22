# Classes and their methods made afresh on every call, as a module reload
# makes them: the guards and caches their compiled attribute loads and
# stores share go with the last code that uses them, so the leak check
# sees nothing left per class.
SOURCE = '''
class Point:
    scale = 2

    def __init__(self, x, y):
        self.x = x
        self.y = y
        self.tag = 'p'

    def norm(self):
        return (self.x * self.x + self.y * self.y) * self.scale
'''


def _load():
    ns = {}
    exec(SOURCE, ns)
    return ns['Point']


def fresh_class(n):
    out = []
    for k in range(n):
        p = _load()(k, k + 1)
        p.x += 1
        out.append((p.norm(), p.tag, p.x))
    return out


CASES = [
    ('fresh_class', [(4,)], {'warmup': (40, 'owner scans and parked states fill up first')}),
]
