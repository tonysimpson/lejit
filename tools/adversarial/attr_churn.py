"""Instance dicts that never settle: optional attributes set in varying order, deleted, read with getattr defaults and __getattr__ fallbacks. Variant 1 starts elsewhere in the bit patterns and stores bytes and floats instead of str and int."""

FIELDS = ['email', 'phone', 'city', 'company', 'title', 'twitter', 'notes', 'score']


class Contact:
    def __init__(self, name):
        self.name = name

    def __getattr__(self, attr):
        if attr in FIELDS:
            return None
        raise AttributeError(attr)


class Loose:
    pass


def build(i, variant):
    c = Contact('c%d' % i)
    order = FIELDS[i % len(FIELDS):] + FIELDS[:i % len(FIELDS)]
    for k, f in enumerate(order):
        if (i >> k) & 1:
            setattr(c, f, ('%s-%d' % (f, i)) if variant == 0 else ('%s:%x' % (f, i)).encode())
    if i % 3 == 0:
        c.tmp = i
    if i % 7 == 0:
        c.__dict__.clear()
        c.name = 'reset%d' % i
    return c


def score(c):
    s = len(c.name)
    for f in FIELDS:
        v = getattr(c, f)
        if v is not None:
            s += len(v)
    if c.email and c.phone:
        s += 10
    if hasattr(c, 'tmp'):
        s += c.tmp % 5
        del c.tmp
    s += len(getattr(c, 'tmp', 'gone'))
    return s


def loose(i, variant):
    o = Loose()
    for k in range(i % 9):
        setattr(o, 'k%d' % ((i * 7 + k) % 12), k if variant == 0 else k * 0.5)
    o.__dict__.pop('k3', None)
    return sum(o.__dict__.values()) + len(o.__dict__)


def main(n, variant=0):
    total = 0
    start = 0 if variant == 0 else 4099
    for i in range(start, start + n):
        total += score(build(i, variant)) + loose(i, variant)
    return total


N = 30000

if __name__ == '__main__':
    print(main(N))
