# A caller dies and a new one takes its code object's address (KNOWN_BUGS
# C30, CPython's test_timeit). A method inlined into the caller leaves the
# blocks compiled inside it in the method's own join points, and their code
# goes on in the caller's. The saved caller state named the caller only by
# its code object, borrowed, so a new caller at the same address with the
# same globals matched those blocks and jumped into the dead caller's code:
# here the 'B' caller's return value for an 'A' caller; in timeit, a
# promotion whose merge points had been freed, a segfault.

class Counter:
    def __init__(self):
        self.n = 0

    def bump(self):
        self.n += 1


SRC_A = '''
def f(c):
    c.bump()
    return 'A'
'''
SRC_B = '''
def f(c):
    c.bump()
    return 'B'
'''


def callers_at_one_address():
    # compiled fresh each time, so each f's code object takes the address
    # the previous (dead) one had
    c = Counter()
    out = []
    for i in range(20):
        ns = {}
        exec(SRC_A if i % 2 == 0 else SRC_B, globals(), ns)
        f = ns['f']
        del ns
        out.append(f(c))
        del f
    return ''.join(out), c.n


CASES = [
    ('callers_at_one_address', [()], {'interpreted': 'calls exec (C15)'}),
]
