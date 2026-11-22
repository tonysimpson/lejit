# bytes len() and b[i] (Objects/pbytesobject.c): the length is ob_size and
# an int index gives the byte as a virtual int, with no C call. Constants
# fold; slices and other keys stay in C.
DATA = b'\x00\x7f\x80\xff'


class B(bytes):
    pass


def lengths(xs):
    return [len(x) for x in xs]


def items(xs):
    out = []
    for x in xs:
        for i in range(-len(x) - 2, len(x) + 2):
            try:
                out.append((i, x[i]))
            except IndexError as e:
                out.append((i, str(e)))
    return out


def checksum(x):
    t = 0
    for i in range(len(x)):
        t = (t * 31 + x[i]) & 0xffff
    return t


def runs(nt):
    # pyflate's bwt run-length decode loop
    i = 0
    out = 0
    while i < len(nt):
        if i < len(nt) - 4 and nt[i] == nt[i + 1] == nt[i + 2] == nt[i + 3]:
            out += nt[i + 4] + 4
            i += 5
        else:
            out += 1
            i += 1
    return out


def odd_keys(x):
    out = [x[True], x[False], x[1:3], x[::-1]]
    for k in (2 ** 70, -2 ** 70, 1.0, None):
        try:
            out.append(x[k])
        except (IndexError, TypeError) as e:
            out.append((type(e).__name__, str(e)))
    return out


def subclass(x):
    b = B(x)
    return len(b), b[0], b[-1]


def constants(i):
    return len(DATA), DATA[0], DATA[-1], DATA[2] + 1, DATA[i]


def bytearrays(x):
    return len(x), x[0], x[-1]


CASES = [
    ('lengths', [([b'', b'a', DATA, b'x' * 1000],)], {}),
    ('items', [([b'abc', DATA, b''],)], {}),
    ('checksum', [(DATA * 50,), (b'',)], {}),
    ('runs', [(b'aaaaa\x03bcddddd\x00e',), (b'',)], {}),
    ('odd_keys', [(b'abc',)], {}),
    ('subclass', [(b'abc',), (DATA,)], {}),
    ('constants', [(0,), (3,), (-4,), (4,)], {}),
    ('bytearrays', '[(bytearray(b"xyz"),), (b"xyz",)]', {}),
]
