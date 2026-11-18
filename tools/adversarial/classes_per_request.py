"""Classes created and dropped per request: namedtuple, make_dataclass and type() factories, each with fresh code (watch memory). Variant 1 has other field names, counts and weights."""
import collections
import dataclasses


class Handler:
    def handle(self, row):
        return len(row)


SCHEMAS = [(('id', 'name', 'amount'), 'extra%d', 4, 5),
           (('key', 'label', 'value'), 'opt_%d', 3, 7)]


def schema_for(request, variant):
    base, extra, count, _ = SCHEMAS[variant]
    return list(base) + [extra % k for k in range(request % count)]


def serve(request, variant):
    fields = schema_for(request, variant)
    Row = collections.namedtuple('Row%d' % request, fields)
    Record = dataclasses.make_dataclass('Record%d' % request,
                                        [(f, object, dataclasses.field(default=0)) for f in fields],
                                        frozen=True)
    Special = type('Special%d' % request, (Handler,), {
        'weight': request % SCHEMAS[variant][3],
        'handle': lambda self, row: len(row) * self.weight + row[0],
    })
    h = Special()
    total = 0
    for k in range(10):
        values = [k, 'n%d' % k, k * 1.5] + [k] * (len(fields) - 3)
        row = Row(*values)
        rec = Record(*values)
        total += h.handle(row) + len(repr(rec)) + len(row._asdict())
        total += rec == Record(*values)
    return total


def main(n, variant=0):
    return sum(serve(request, variant) for request in range(n))


N = 300

if __name__ == '__main__':
    print(main(N))
