"""Function objects made and dropped in loops: lambdas, nested defs and closures with defaults created per item. Variant 1 runs on floats with other constants."""


def make_filters(thresholds):
    return [lambda x, t=t: x > t for t in thresholds]


def pipeline(data, ops):
    for op in ops:
        data = [op(x) for x in data]
    return data


def compose(*fs):
    def composed(x):
        for f in reversed(fs):
            x = f(x)
        return x
    return composed


def main(n, variant=0):
    total = 0
    data = list(range(40)) if variant == 0 else [k * 0.75 - 3 for k in range(36)]
    for i in range(n):
        scale = i % 7 + 1 if variant == 0 else i % 5 + 0.5
        ops = [lambda x: x * scale, lambda x: x + i % 11, (lambda k: lambda x: x % k)(i % 13 + 2)]

        def keyfn(x, s=scale):
            return (x % s, -x)

        out = sorted(pipeline(data, ops), key=keyfn)
        filters = make_filters(range(0, 12, 3))
        total += sum(1 for x in out if all(f(x) for f in filters[:2]))
        f = compose(lambda x: x + 1, lambda x: x * 2, ops[2])
        total += f(i) + out[0] + out[-1]
    return total


N = 4000

if __name__ == '__main__':
    print(main(N))
