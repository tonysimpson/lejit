"""Closures with cells of their own and nonlocal writes: event emitters, counters, memoisers and accumulators. Variant 1 has other handlers, and float keys and means."""


def make_counter():
    count = 0

    def inc(k=1):
        nonlocal count
        count += k
        return count
    return inc


def make_emitter():
    handlers = []

    def on(f):
        handlers.append(f)
        return f

    def emit(value):
        total = 0
        for h in handlers:
            total += h(value)
        return total
    return on, emit


def unique_count(items):
    seen = set()

    def visit(x):
        if x in seen:
            return 0
        seen.add(x)
        return 1
    total = 0
    for x in items:
        total += visit(x)
    return total


def memoise(f):
    cache = {}

    def wrapper(n):
        if n not in cache:
            cache[n] = f(n)
        return cache[n]
    return wrapper


def running_mean():
    total = 0.0
    count = 0

    def add(x):
        nonlocal total, count
        total += x
        count += 1
        return total / count
    return add


def main(n, variant=0):
    inc = make_counter()
    on, emit = make_emitter()
    clicks = make_counter()
    on(lambda v: clicks())
    scale = 3 if variant == 0 else 4
    if variant == 0:
        on(lambda v: v % 7)
        on(lambda v: v * scale % 11)
        keys, step, mod = 300, 1, 17
    else:
        on(lambda v: v * scale % 13)
        on(lambda v: v // 5 % 3)
        on(lambda v: clicks(0) % 2)
        keys, step, mod = 250, 0.5, 23
    mean = running_mean()
    square = memoise(lambda k: k * k)
    acc = 0
    for i in range(n):
        acc += inc() + emit(i) + square(i % keys * step)
        m = mean(i % 100 * step)
        if i % 50 == 0:
            acc += unique_count([j % mod for j in range(i % 200)])
    return acc, inc(0), clicks(0), round(m, 6)


N = 60000

if __name__ == '__main__':
    print(main(N))
