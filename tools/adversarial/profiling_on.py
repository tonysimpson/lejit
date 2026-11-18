"""Profiling on: cProfile around ordinary application code, with the call counts checked."""
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


def simulate(accounts, steps):
    ok = 0
    for s in range(steps):
        for k, acc in enumerate(accounts):
            acc.deposit((s * 7 + k) % 50)
            ok += acc.withdraw((s * 13 + k) % 70)
            if s % 10 == 0:
                interest(acc, 2)
    return ok


def main(n):
    accounts = [Account('o%d' % k, 100) for k in range(30)]
    prof = cProfile.Profile()
    prof.enable()
    ok = simulate(accounts, n)
    prof.disable()
    stats = pstats.Stats(prof)
    counts = sorted((func[2], cc) for func, (cc, nc, tt, ct, callers) in stats.stats.items()
                    if not func[2].startswith('<'))
    return ok, sum(a.balance for a in accounts), counts


N = 600

if __name__ == '__main__':
    print(main(N))
