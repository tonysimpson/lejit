"""Work already in C: big-integer arithmetic and decimal money sums, with only a thin Python loop around them. Variant 1 uses other key sizes, rates and precision."""
import decimal
import sys

# CPython 3.7.14 and later limit int-to-str conversion; the factorials here pass it.
if hasattr(sys, 'set_int_max_str_digits'):
    sys.set_int_max_str_digits(0)


def rsa_like(bits, rounds, dp, dq):
    p = (1 << bits) - dp
    q = (1 << (bits - 3)) - dq
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


def ledger(n, prec, rounding, rate):
    ctx = decimal.Context(prec=prec, rounding=rounding)
    total = decimal.Decimal(0)
    vat = decimal.Decimal(rate)
    cent = decimal.Decimal('0.01')
    for i in range(n):
        net = decimal.Decimal(i % 997) / decimal.Decimal(7)
        gross = ctx.multiply(net, 1 + vat).quantize(cent)
        total = ctx.add(total, gross)
    return str(total)


PARAMS = [(1024, 159, 21, 4, 28, decimal.ROUND_HALF_EVEN, '0.20'),
          (896, 87, 45, 5, 34, decimal.ROUND_HALF_UP, '0.175')]


def main(n, variant=0):
    bits, dp, dq, fk, prec, rounding, rate = PARAMS[variant]
    return (rsa_like(bits, n, dp, dq), factorial_digits(n * fk),
            ledger(n * 40, prec, rounding, rate))


N = 400

if __name__ == '__main__':
    print(main(N))
