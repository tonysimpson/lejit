"""Generator expressions handed to consumers that are not fused: str.join, sorted, dict, Counter, and min and max with a key."""
import collections
import heapq

PRODUCTS = [('p%04d' % i, (i * 37) % 101, round((i * 7919) % 1000 / 7.0, 2), 'cat%d' % (i % 9))
            for i in range(1000)]


def report(products, limit):
    csv = '\n'.join(','.join(str(f) for f in p) for p in products if p[1] < limit)
    prices = sorted(p[2] for p in products if p[1] < limit)
    by_name = dict((p[0], p[2]) for p in products)
    per_cat = collections.Counter(p[3] for p in products if p[2] > 50)
    cheapest = min((p for p in products if p[1] > limit), key=lambda p: p[2], default=None)
    dearest = max((p for p in products if p[1] < limit), key=lambda p: (p[2], p[0]))
    top = heapq.nlargest(5, (p[2] for p in products))
    names = ' '.join(sorted(p[0][-2:] for p in products if p[3] == 'cat3'))
    return (len(csv), prices[len(prices) // 2], by_name['p0500'], sorted(per_cat.items()),
            cheapest, dearest, top, len(names))


def main(n):
    acc = []
    for i in range(n):
        acc.append(report(PRODUCTS, 20 + i % 60))
    return acc[-1], len(acc), sum(a[0] for a in acc)


N = 150

if __name__ == '__main__':
    print(main(N))
