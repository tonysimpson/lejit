"""Profiling on: cProfile around ordinary application code, with the call counts checked. Variant 1 has other accounts, amounts and interest."""
import cProfile
import pstats


class Account:
    def __init__(self, owner, balance):
        self.owner = owner
        self.balance = balance

    def deposit(self, amount):
        self.balance += amount
        return self.balance

    def withdraw(self, amount):
        if amount > self.balance:
            return False
        self.balance -= amount
        return True


def interest(acc, rate):
    return acc.deposit(acc.balance * rate // 100)


def simulate(accounts, steps, a, b, every, rate):
    ok = 0
    for s in range(steps):
        for k, acc in enumerate(accounts):
            acc.deposit((s * 7 + k) % a)
            ok += acc.withdraw((s * 13 + k) % b)
            if s % every == 0:
                interest(acc, rate)
    return ok


VARIANTS = [(30, 100, 50, 70, 10, 2), (24, 250, 90, 85, 7, 3)]


def main(n, variant=0):
    count, start, a, b, every, rate = VARIANTS[variant]
    accounts = [Account('o%d' % k, start) for k in range(count)]
    prof = cProfile.Profile()
    prof.enable()
    ok = simulate(accounts, n, a, b, every, rate)
    prof.disable()
    stats = pstats.Stats(prof)
    counts = sorted((func[2], cc) for func, (cc, nc, tt, ct, callers) in stats.stats.items()
                    if not func[2].startswith('<'))
    return ok, sum(a.balance for a in accounts), counts


N = 600

if __name__ == '__main__':
    print(main(N))
