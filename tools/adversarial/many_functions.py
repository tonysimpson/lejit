"""Compile cost before pay-off: four hundred business rules, each a distinct function called a few times."""
import random

TEMPLATES = [
    "def {name}(rec):\n"
    "    v = rec['age'] * {a} + {b}\n"
    "    return v % {m} < {c}\n",
    "def {name}(rec):\n"
    "    s = rec['name']\n"
    "    return s[{a} % len(s)] in '{chars}'\n",
    "def {name}(rec):\n"
    "    t = 0\n"
    "    for tag in rec['tags']:\n"
    "        t += len(tag) * {a}\n"
    "    return t > {b}\n",
    "def {name}(rec):\n"
    "    b = rec['balance']\n"
    "    if b < {a}:\n"
    "        return False\n"
    "    elif b > {b} * 10:\n"
    "        return rec['score'] > {c}\n"
    "    return True\n",
    "def {name}(rec):\n"
    "    return len([ch for ch in rec['city'] if ch > '{ch}']) >= {c}\n",
    "def {name}(rec):\n"
    "    limits = {{'lo': {a}, 'hi': {b}}}\n"
    "    return limits['lo'] <= rec['score'] <= limits['hi'] + limits['lo']\n",
    "def {name}(rec):\n"
    "    ratio = rec['balance'] / (rec['age'] + {a})\n"
    "    return round(ratio, 1) > {c} / 10.0\n",
    "def {name}(rec):\n"
    "    words = rec['name'].lower().split('{ch}')\n"
    "    return len(words) == {c} % 3 + 1 or '{chars}' in rec['city']\n",
]


def build(count, seed):
    rnd = random.Random(seed)
    parts = []
    for k in range(count):
        parts.append(TEMPLATES[k % len(TEMPLATES)].format(
            name='rule_%d' % k, a=rnd.randrange(1, 9), b=rnd.randrange(10, 90),
            c=rnd.randrange(0, 7), m=rnd.randrange(3, 11),
            ch=rnd.choice('aeiost'), chars=''.join(rnd.sample('abcdefghij', 3))))
    ns = {}
    exec(compile(''.join(parts), '<rules>', 'exec'), ns)
    return [ns['rule_%d' % k] for k in range(count)]


RULES = build(400, 1)
NAMES = ['Ada Lovelace', 'Alan Turing', 'Grace Hopper', 'Edsger Dijkstra',
         'Barbara Liskov', 'Donald Knuth', 'Frances Allen']
CITIES = ['London', 'Cambridge', 'Edinburgh', 'Leeds', 'Bristol', 'Oxford']


def record(i):
    return {'age': 18 + i * 7 % 60, 'name': NAMES[i % len(NAMES)],
            'tags': ['t%d' % j for j in range(i % 4)], 'balance': i * 37 % 1000,
            'score': i * 13 % 100, 'city': CITIES[i % len(CITIES)]}


def main(n):
    passed = []
    for i in range(n):
        rec = record(i)
        passed.append(sum(1 for rule in RULES if rule(rec)))
    return passed


N = 3

if __name__ == '__main__':
    print(main(N))
