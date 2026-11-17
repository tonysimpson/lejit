# Conditional jumps whose target block already exists (KNOWN_BUGS C35).
# A JUMP_IF_FALSE_OR_POP in the second version of a loop body jumps to the
# join that the first version compiled: psyco_unify's code there is a single
# JMP, and x64/idispatcher.h:make_code_conditional rewrites it as one Jcc to
# the JMP's target (IS_A_SINGLE_JUMP). Ni's IS_A_JUMP read the JMP one byte
# late, so it recognised a JMP only when its displacement's low byte was
# 0xE9, and then took a target from the wrong bytes: the Jcc went outside
# the address space. Whether a JMP has that byte depends on how far its
# target is, so this case compiles a few thousand such sites at different
# distances (the expression statements are padding); about one in 256 hits.


def _source(k, chains=32):
    lines = ['def h(items):', '    r = items[0]', '    for o in items:']
    for c in range(chains):
        lines.append('        r = o[0] and o[1] and o[2]')
        lines += ['        o[0] + 1'] * ((k * 7 + c * 3) % 5)
        lines += ['        o[1] == 2'] * ((k + c) % 3)
    lines.append('    return r')
    return '\n'.join(lines)


def _build(n=160):
    out = []
    for k in range(n):
        ns = {}
        exec(_source(k), ns)
        out.append(ns['h'])
    return out


FUNCS = _build()
DATA = [(1, 0, 1), (0, 1, 1), (1, 1, 0), (0, 0, 0), (1, 1, 1)] * 2


def run_all(items):
    out = []
    for f in FUNCS:
        out.append(f(items))
    return out


CASES = [
    ('run_all', '[(DATA,)]', {}),
]
