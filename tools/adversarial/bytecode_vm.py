"""One large function with many branches and a changing stack: the dispatch loop of a small stack-machine interpreter. Variant 1 runs the program with other constants, so other branches and float sums."""

PUSH, LOAD, STORE, ADD, SUB, MUL, DIV, MOD, NEG, DUP, SWAP, POP, JMP, JZ, JNZ, LT, EQ, \
    CALL, RET, PRINT, CAT, LEN, LIST, APPEND, INDEX, HALT, NOT, FLOAT, STR, GT = range(30)


def run(code, consts, nlocals):
    stack = []
    frames = []
    local = [0] * nlocals
    out = []
    pc = 0
    steps = 0
    while True:
        op, arg = code[pc]
        pc += 1
        steps += 1
        if op == PUSH:
            stack.append(consts[arg])
        elif op == LOAD:
            stack.append(local[arg])
        elif op == STORE:
            local[arg] = stack.pop()
        elif op == ADD:
            b = stack.pop()
            stack[-1] = stack[-1] + b
        elif op == SUB:
            b = stack.pop()
            stack[-1] = stack[-1] - b
        elif op == MUL:
            b = stack.pop()
            stack[-1] = stack[-1] * b
        elif op == DIV:
            b = stack.pop()
            stack[-1] = stack[-1] / b
        elif op == MOD:
            b = stack.pop()
            stack[-1] = stack[-1] % b
        elif op == NEG:
            stack[-1] = -stack[-1]
        elif op == NOT:
            stack[-1] = not stack[-1]
        elif op == DUP:
            stack.append(stack[-1])
        elif op == SWAP:
            stack[-1], stack[-2] = stack[-2], stack[-1]
        elif op == POP:
            stack.pop()
        elif op == JMP:
            pc = arg
        elif op == JZ:
            if not stack.pop():
                pc = arg
        elif op == JNZ:
            if stack.pop():
                pc = arg
        elif op == LT:
            b = stack.pop()
            stack[-1] = stack[-1] < b
        elif op == GT:
            b = stack.pop()
            stack[-1] = stack[-1] > b
        elif op == EQ:
            b = stack.pop()
            stack[-1] = stack[-1] == b
        elif op == CALL:
            frames.append((pc, local))
            local = [stack.pop()] + [0] * (nlocals - 1)
            pc = arg
        elif op == RET:
            pc, local = frames.pop()
        elif op == PRINT:
            out.append(str(stack.pop()))
        elif op == CAT:
            b = stack.pop()
            stack[-1] = str(stack[-1]) + str(b)
        elif op == LEN:
            stack[-1] = len(stack[-1])
        elif op == LIST:
            stack.append([])
        elif op == APPEND:
            b = stack.pop()
            stack[-1].append(b)
        elif op == INDEX:
            b = stack.pop()
            stack[-1] = stack[-1][b]
        elif op == FLOAT:
            stack[-1] = float(stack[-1])
        elif op == STR:
            stack[-1] = str(stack[-1])
        elif op == HALT:
            return out, steps, stack
        else:
            raise ValueError(op)


def program(limit, variant):
    consts = [[0, 1, limit, 3, 2.5, 'n=', 7, 2], [0, 1, limit, 5, 0.75, 'total=', 4, 3]][variant]
    code = [
        (PUSH, 0), (STORE, 0),
        (PUSH, 0), (STORE, 1),
        (LIST, 0), (STORE, 2),
        (LOAD, 0), (PUSH, 2), (LT, 0), (JZ, 44),
        (LOAD, 0), (PUSH, 3), (MOD, 0), (JNZ, 21),
        (LOAD, 1), (LOAD, 0), (FLOAT, 0), (PUSH, 4), (DIV, 0), (ADD, 0), (STORE, 1),
        (LOAD, 0), (PUSH, 7), (MOD, 0), (JZ, 33),
        (LOAD, 1), (LOAD, 0), (PUSH, 6), (MUL, 0), (ADD, 0), (STORE, 1), (JMP, 39), (HALT, 0),
        (LOAD, 2), (LOAD, 0), (STR, 0), (LEN, 0), (APPEND, 0), (POP, 0),
        (LOAD, 0), (PUSH, 1), (ADD, 0), (STORE, 0), (JMP, 6),
        (PUSH, 5), (LOAD, 1), (CAT, 0), (PRINT, 0),
        (LOAD, 2), (LEN, 0), (PRINT, 0),
        (HALT, 0),
    ]
    return code, consts


def main(n, variant=0):
    code, consts = program(n, variant)
    out, steps, stack = run(code, consts, 4)
    return out, steps, len(stack)


N = 10000

if __name__ == '__main__':
    print(main(N))
