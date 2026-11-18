"""Module-level code, which is never compiled: a script that does its work in top-level loops. Variant 1 has another seed and number of accounts, and fractional amounts."""

SCRIPT = '''
accounts = {}
for k in range(naccounts):
    accounts['acc%03d' % k] = 100 * (k % 7 + 1)

ledger = []
seed = seed0
for step in range(steps):
    seed = (seed * 1103515245 + 12345) & 0x7fffffff
    src = 'acc%03d' % (seed % naccounts)
    dst = 'acc%03d' % ((seed >> 8) % naccounts)
    amount = seed % 50 * unit
    if src != dst and accounts[src] >= amount:
        accounts[src] -= amount
        accounts[dst] += amount
        ledger.append((src, dst, amount))

richest = max(sorted(accounts), key=accounts.get)
moved = 0
for src, dst, amount in ledger:
    moved += amount
result = (len(ledger), moved, richest, accounts[richest], sum(accounts.values()))
'''

CODE = compile(SCRIPT, '<script>', 'exec')


VARIANTS = [{'seed0': 1, 'naccounts': 200, 'unit': 1}, {'seed0': 4242, 'naccounts': 150, 'unit': 0.25}]


def main(n, variant=0):
    ns = dict(VARIANTS[variant], steps=n)
    exec(CODE, ns)
    return ns['result']


N = 60000

if __name__ == '__main__':
    print(main(N))
