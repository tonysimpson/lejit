"""try/except on the hot path: exceptions as control flow while parsing, looking up and iterating, with finally and custom errors."""


class ValidationError(Exception):
    def __init__(self, field, value):
        super().__init__('%s: %r' % (field, value))
        self.field = field


def parse_cell(text):
    try:
        return int(text)
    except ValueError:
        pass
    try:
        return float(text)
    except ValueError:
        return text.strip() or None


def validate(row):
    if row['age'] is None or not isinstance(row['age'], int):
        raise ValidationError('age', row['age'])
    if isinstance(row['score'], str):
        raise ValidationError('score', row['score'])
    return row


def lookup(table, key):
    try:
        return table[key]
    except KeyError:
        return -1
    finally:
        table['_lookups'] = table.get('_lookups', 0) + 1


def drain(it):
    count = 0
    while True:
        try:
            v = next(it)
        except StopIteration:
            return count
        count += v is not None


TEXTS = ['12', '3.5', 'abc', '', ' 7 ', '-4', '1e3', 'x9', '0', '2.25']


def main(n):
    table = {'k%d' % k: k for k in range(0, 100, 3)}
    good = bad = hits = 0
    fields = {}
    for i in range(n):
        row = {'age': parse_cell(TEXTS[i % 10]), 'score': parse_cell(TEXTS[(i * 3) % 10])}
        try:
            validate(row)
            good += 1
        except ValidationError as e:
            bad += 1
            fields[e.field] = fields.get(e.field, 0) + 1
        hits += lookup(table, 'k%d' % (i % 100))
        if i % 10 == 0:
            hits += drain(iter([parse_cell(t) for t in TEXTS]))
    return good, bad, hits, sorted(fields.items()), table['_lookups']


N = 60000

if __name__ == '__main__':
    print(main(N))
