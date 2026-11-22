from . import Block

INT_EXPRS = ['a + b', 'a - b', 'a * b', 'a // b', 'a % b', 'a << (b & 63)', 'a >> (b & 127)',
             'a & b', 'a | b', 'a ^ b', '-a', '~a', 'a ** 2', 'divmod(a, b)', 'a < b', 'a == b',
             'a * 3 - b', '(a + 1) // 7', 'pow(a, 3, 1000003)', 'int(a) // -3', 'a % -5',
             'a >= b', 'bool(a) + b']
INT_VALS = ['0', '1', '-1', '2', '-7', '255', '2**31 - 1', '2**31', '-2**31', '-2**31 - 1', '2**32',
            '2**62', '2**63 - 1', '2**63', '-2**63', '-2**63 - 1', '2**64', 'True', 'False', '10**20']
FLOAT_EXPRS = ['a + b', 'a - b', 'a * b', 'a / b', 'a // b', 'a % b', 'a ** 2', 'a < b', 'a == b',
               'a != b', 'int(a)', 'round(a)', 'math.floor(a)', 'a.is_integer()', '-a', 'a + 1',
               'divmod(a, b)', 'float(b) * a', 'math.fmod(a, b)', 'a >= b']
FLOAT_VALS = ['0.0', '-0.0', '1.5', '-2.5', '1e308', '-1e308', '5e-324', 'float("inf")', 'float("-inf")',
              'float("nan")', '1e16', '2**53 + 1', '3', '-1', '2**63', 'True']


def edge_block(name, decision, exprs, vals, extra=''):
    return Block(name, 'arith', decision,
                 'type switch and size past a limit: the operands move to other edges after the break',
                 extra + '''
P_vals = (S_a, S_b)
def P_break():
    global P_vals
    P_vals = (S_c, S_d, S_a)
def P_run(r):
    out = []
    for a in P_vals:
        for b in P_vals:
            for k in range(3):
                try:
                    if k == 0:
                        v = S_e1
                    elif k == 1:
                        v = S_e2
                    else:
                        v = S_e3
                except (ArithmeticError, ValueError, TypeError, AttributeError) as e:
                    v = type(e).__name__
                out.append(v)
    inner = NEXT(r)
    return out, inner
''', slots={'a': vals, 'b': vals, 'c': vals, 'd': vals, 'e1': exprs, 'e2': exprs, 'e3': exprs})


BLOCKS = [
    edge_block('int_edges',
               'translated int arithmetic, compares and shifts on machine words, falling back to '
               'longs past the word',
               INT_EXPRS, INT_VALS),
    edge_block('float_edges',
               'translated float arithmetic and conversions at nan, inf, -0.0 and the int edges',
               FLOAT_EXPRS, FLOAT_VALS, extra='import math\n'),
    Block('const_arith', 'folding',
          'arithmetic on constants folded at compile time, edge values included',
          'type switch: a run-time operand joins the folded one after the break',
          '''
P_x = S_x
def P_break():
    global P_x
    P_x = S_y
def P_run(r):
    out = []
    try:
        out.append((S_c1) + (S_c2))
        out.append((S_c1) * (S_c2))
        out.append((S_c1) // ((S_c2) or 1))
        out.append((S_c1) << 3)
        out.append((S_c1) + P_x)
        out.append(P_x * (S_c2))
    except (ArithmeticError, TypeError, ValueError) as e:
        out.append(type(e).__name__)
    return out, NEXT(r)
''', slots={'c1': INT_VALS, 'c2': INT_VALS, 'x': ['1', '2**63'], 'y': ['1.5', "'s'", '2**64', 'True']}),
]
