"""Work already in C: json dumps and loads of nested API documents, with a little Python between. Variant 1 has more items, whole-number prices and fractional quantities."""
import json


def item(i, k, variant):
    if variant == 0:
        return {'sku': 'S%04d' % (i * k % 9973), 'qty': k, 'price': round(1.5 * k + i % 7, 2)}
    return {'sku': 'B-%05d' % (i * k % 99991), 'qty': k / 4, 'price': 120 * k + i % 13}


def document(i, variant):
    return {
        'id': i,
        'user': {'name': 'user%d' % i, 'email': 'u%d@example.org' % i, 'active': i % 3 != 0},
        'items': [item(i, k, variant) for k in range(1, 6 if variant == 0 else 8)],
        'tags': ['t%d' % (i % 11), 'priority' if i % 5 == 0 else 'normal'],
        'meta': {'created': '2024-01-%02dT10:00:00Z' % (1 + i % 28), 'version': 3, 'notes': None},
    }


def main(n, variant=0):
    blob = json.dumps([document(i, variant) for i in range(n)], sort_keys=True)
    docs = json.loads(blob)
    total = 0.0
    active = 0
    for d in docs:
        total += sum(it['qty'] * it['price'] for it in d['items'])
        active += d['user']['active']
    pretty = json.dumps(docs[:max(1, n // 10)], indent=2, sort_keys=True)
    again = json.loads(pretty)
    return len(blob), len(pretty), round(total, 2), active, again[-1]['items'][0]['sku']


N = 6000

if __name__ == '__main__':
    print(main(N))
