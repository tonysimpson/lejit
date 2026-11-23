# Generator bodies run compiled and resumed many times
# (eval_hook.c:LeGen_Run): the permutations nqueens uses, generators
# passed between threads, and one shared by several consumers.
import threading


def permutations(iterable, r=None):
    pool = tuple(iterable)
    n = len(pool)
    r = n if r is None else r
    indices = list(range(n))
    cycles = list(range(n - r + 1, n + 1))[::-1]
    yield tuple(pool[i] for i in indices[:r])
    while n:
        for i in reversed(range(r)):
            cycles[i] -= 1
            if cycles[i] == 0:
                indices[i:] = indices[i + 1:] + indices[i:i + 1]
                cycles[i] = n - i
            else:
                j = cycles[i]
                indices[i], indices[-j] = indices[-j], indices[i]
                yield tuple(pool[i] for i in indices[:r])
                break
        else:
            return


def n_queens(queen_count):
    cols = range(queen_count)
    for vec in permutations(cols):
        if (queen_count == len(set(vec[i] + i for i in cols))
                == len(set(vec[i] - i for i in cols))):
            yield vec


def queens(n):
    return list(n_queens(n)), sum(1 for _ in permutations(range(n), 2))


def numbers(n):
    total = 0
    for i in range(n):
        total += i
        yield total, [i] * (i % 3)


def threads(n):
    g = numbers(n * 200)
    out = []
    lock = threading.Lock()

    def take():
        while True:
            with lock:
                try:
                    out.append(next(g))
                except StopIteration:
                    return

    ts = [threading.Thread(target=take) for _ in range(4)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return sorted(out) == list(numbers(n * 200)), len(out)


def shared(n):
    g = numbers(n)
    a = [x for _, x in zip(range(n // 3), g)]
    b = list(map(lambda v: v[0], zip(g, range(n // 3))))
    c = list(g)
    return a, b, c


CASES = [
    ('queens', [(5,), (6,)], {}),
    ('threads', [(3,)], {}),
    ('shared', [(30,)], {}),
]
