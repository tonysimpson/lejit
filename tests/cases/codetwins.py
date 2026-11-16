# Equal but distinct code objects, as when a module is imported twice
# (test_json imports json with and without _json). Ni looked code objects
# up by value, so near the recursion limit the comparison of their nested
# co_consts raised RecursionError inside the lookup, which was swallowed,
# and the recursion ran on to a fatal "Cannot recover from stack overflow"
# (KNOWN_BUGS C13). A twin with other globals was also never compiled.

SRC = '''
def gen(n):
    k = ((((((1,),),),),),)
    yield n
    yield from gen(n + 1)

def scale(x):
    return x * FACTOR
'''

ns1 = {'FACTOR': 2}
ns2 = {'FACTOR': 3}
exec(SRC, ns1)
exec(SRC, ns2)


def endless_generator_recursion():
    out = []
    for ns in (ns1, ns2, ns1):
        try:
            for x in ns['gen'](0):
                pass
        except RecursionError:
            out.append('RecursionError')
    return out


def twin_globals():
    return [ns1['scale'](5), ns2['scale'](5), ns1['scale'](1)]


CASES = [
    ('endless_generator_recursion', "[()]", {}),
    ('twin_globals', "[()]", {}),
]
