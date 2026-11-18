"""Hot code whose types change between phases: one numeric kernel run on ints, then floats, Fractions, Decimals and complex numbers."""
import decimal
import fractions


def horner(coeffs, x):
    acc = coeffs[0] * 0
    for c in coeffs:
        acc = acc * x + c
    return acc


def matmul(a, b):
    n = len(a)
    return [[sum(a[i][k] * b[k][j] for k in range(n)) for j in range(n)] for i in range(n)]


def mean_var(xs):
    m = sum(xs) / len(xs)
    return m, sum((x - m) * (x - m) for x in xs) / len(xs)


def kernel(conv, size):
    coeffs = [conv(k % 7 - 3) for k in range(size)]
    xs = [conv(k % 5) / conv(4) for k in range(size)]
    poly = [horner(coeffs, x) for x in xs]
    m = [[conv((i * 3 + j) % 5) for j in range(4)] for i in range(4)]
    for _ in range(3):
        m = matmul(m, m)
        m = [[v / conv(10) for v in row] for row in m]
    return poly[-1], mean_var(xs), m[0][0]


PHASES = [int, float, fractions.Fraction, decimal.Decimal, complex]


def show(v):
    if isinstance(v, tuple):
        return '(' + ', '.join(show(x) for x in v) + ')'
    if isinstance(v, complex):
        return '(%.6g%+.6gj)' % (v.real, v.imag)
    if isinstance(v, fractions.Fraction):
        return '%.6g' % float(v)
    return '%.6g' % v


def main(n):
    out = []
    for conv in PHASES:
        out.append(show(kernel(conv, n)))
    return out


N = 120

if __name__ == '__main__':
    print(main(N))
