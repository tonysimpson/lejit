"""Subclasses of built-in types whose overrides the fast paths must respect: a case-insensitive dict, a money int, a lazy list and a tuple record. Variant 1 uses other headers and window sizes, and floats in the windows and points."""


class CaseDict(dict):
    def __setitem__(self, key, value):
        super().__setitem__(key.lower(), value)

    def __getitem__(self, key):
        return super().__getitem__(key.lower())

    def __contains__(self, key):
        return super().__contains__(key.lower())

    def get(self, key, default=None):
        return super().get(key.lower(), default)


class Pence(int):
    def __add__(self, other):
        return Pence(int(self) + int(other))

    __radd__ = __add__

    def __mul__(self, k):
        return Pence(int(self) * k)

    def __str__(self):
        return '\xa3%d.%02d' % divmod(int(self), 100)


class Window(list):
    def __init__(self, items, size):
        super().__init__(items)
        self.size = size

    def __iter__(self):
        return iter(list.__getitem__(self, slice(-self.size, None)))

    def __len__(self):
        return min(self.size, list.__len__(self))

    def __getitem__(self, i):
        if isinstance(i, int) and i < 0:
            i += list.__len__(self)
        return list.__getitem__(self, i)


class Point(tuple):
    def __new__(cls, x, y):
        return super().__new__(cls, (x, y))

    def __iter__(self):
        yield tuple.__getitem__(self, 1)
        yield tuple.__getitem__(self, 0)

    @property
    def x(self):
        return self[0]


HEADERS = [['Content-Type', 'ACCEPT', 'user-agent', 'X-Request-Id', 'Cache-Control'],
           ['content-type', 'Accept', 'Authorization', 'x-request-id', 'ETag', 'If-None-Match']]


def main(n, variant=0):
    total = Pence(0)
    hits = 0
    swapped = 0
    headers = HEADERS[variant]
    for i in range(n):
        h = CaseDict()
        for k, name in enumerate(headers):
            h[name] = k + i % 3
        hits += ('content-type' in h) + h['Accept'] + h.get('X-REQUEST-ID', 0)
        price = Pence(199 + i % 50 if variant == 0 else 1250 - i % 70)
        total = total + price * (1 + i % 3)
        total = 5 + total
        if variant == 0:
            w = Window(range(i % 20 + 5), 4)
            p = Point(i % 7, i % 11)
        else:
            w = Window([k * 0.25 for k in range(i % 13 + 6)], 5)
            p = Point(i % 9 * 1.5, i % 4 - 0.5)
        hits += sum(w) + len(w) + w[-1]
        a, b = w[-2:]
        hits += a - b
        x, y = p
        swapped += x - y + p.x + len(list(p))
    return str(total), hits, swapped, type(total).__name__


N = 20000

if __name__ == '__main__':
    print(main(N))
