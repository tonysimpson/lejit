from . import Block

BLOCKS = [
    Block('merge_cap', 'merge',
          'a loop head sees a local holding more distinct constants than the cap (6 '
          'blocks), which generalises it and marks it forgotten',
          'size past a limit: more constants, then a type change, reach the loop head',
          '''
P_m = 2
def P_break():
    global P_m
    P_m = S_m
def P_run(r):
    acc = 0
    k = 0
    seen = []
    for i in range(S_n):
        j = (i + r) % P_m
        if j == 0:
            k = 0
        elif j == 1:
            k = 1
        elif j == 2:
            k = 2
        elif j == 3:
            k = 3
        elif j == 4:
            k = 4
        elif j == 5:
            k = 5
        elif j == 6:
            k = 6
        elif j == 7:
            k = 7.5
        else:
            k = 'big'
        if type(k) is str:
            seen.append(k)
        else:
            acc = acc + k * 3
    return acc, len(seen), NEXT(r)
''', slots={'m': ['7', '8', '9', '12'], 'n': ['20', '40']}),

    Block('loop_type_change', 'merge',
          'an accumulator whose type changes inside a loop, generalised at the loop '
          'head',
          'type switch in the middle of the iteration',
          '''
P_seq = S_seq
def P_break():
    global P_seq
    P_seq = S_seq2
def P_run(r):
    acc = 0
    try:
        for v in P_seq:
            acc = acc + v
    except TypeError:
        acc = ('TypeError', acc)
    return acc, NEXT(r)
''', slots={'seq': ['[1, 2, 3]', '(1, 2.5, 3)', '[True, 1]'],
            'seq2': ['[1, 2**64, -2**64]', '[1, 1.5, True, 2**63]', "[1, 'a']", '[0.0, -0.0, 1e308, 1e308]',
                     '[2**63 - 1, 1]', '[1j, 2]']}),

    Block('branch_join', 'merge',
          'two arms join with the same value in two forms, a constant and a computed '
          'int',
          'the arm taken switches after the break',
          '''
P_flag = 0
def P_break():
    global P_flag
    P_flag = 1
def P_run(r):
    if P_flag:
        z = r - r
    else:
        z = 0
    lst = ['a', 'b']
    inner = NEXT(r)
    y = z + S_c
    return lst[z], z == 0, (z,)[z], y, inner
''', slots={'c': ['0', '1', '2**63', '0.0']}),

    Block('while_overflow', 'arith',
          'a counter loop whose int stays a machine word, until it passes 2**63 or '
          '2**31',
          'size past a limit: the window moves across the edge after the break',
          '''
P_win = S_win
def P_break():
    global P_win
    P_win = S_win2
def P_run(r):
    start, stop, step = P_win
    i = start
    n = 0
    while (i < stop) if step > 0 else (i > stop):
        i += step
        n += 1
        if n > 40:
            break
    return i, n, NEXT(r)
''', slots={'win': ['(0, 5, 1)', '(10, 0, -3)', '(0, 1, 0.25)'],
            'win2': ['(2**63 - 3, 2**63 + 2, 1)', '(2**31 - 2, 2**31 + 2, 1)', '(-2**63 + 2, -2**63 - 3, -1)',
                     '(2**62, 2**64, 2**62)', '(0, 1e308, 1e307)']}),
]
