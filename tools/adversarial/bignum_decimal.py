"""Work already in C: big-integer arithmetic and decimal money sums, with only a thin Python loop around them."""
import decimal


def rsa_like(bits, rounds):
    p = (1 << bits) - 159
    q = (1 << (bits - 3)) - 21
    n = p * q
    e = 65537
    acc = 0
    for m in range(2, rounds + 2):
        c = pow(m * 123456789, e, n)
        acc ^= c
    return acc % 1000000007


def factorial_digits(k):
    f = 1
    for i in range(2, k):
        f *= i
    s = str(f)
    return len(s), sum(int(d) for d in s[:200])


def ledger(n):
    ctx = decimal.Context(prec=28, rounding=decimal.ROUND_HALF_EVEN)
    total = decimal.Decimal(0)
    vat = decimal.Decimal('0.20')
    cent = decimal.Decimal('0.01')
    for i in range(n):
        net = decimal.Decimal(i % 997) / decimal.Decimal(7)
        gross = ctx.multiply(net, 1 + vat).quantize(cent)
        total = ctx.add(total, gross)
    return str(total)


def main(n):
    return rsa_like(1024, n), factorial_digits(n * 4), ledger(n * 40)


N = 400

if __name__ == '__main__':
    print(main(N))
