"""Module-level code, which is never compiled: a script that does its work in top-level loops."""

SCRIPT = '''
accounts = {}
for k in range(200):
    accounts['acc%03d' % k] = 100 * (k % 7 + 1)

ledger = []
seed = 1
for step in range(steps):
    seed = (seed * 1103515245 + 12345) & 0x7fffffff
    src = 'acc%03d' % (seed % 200)
    dst = 'acc%03d' % ((seed >> 8) % 200)
    amount = seed % 50
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


def main(n):
    ns = {'steps': n}
    exec(CODE, ns)
    return ns['result']


N = 60000

if __name__ == '__main__':
    print(main(N))
