"""Classes created and dropped per request: namedtuple, make_dataclass and type() factories, each with fresh code (watch memory)."""
import collections
import dataclasses


class Handler:
    def handle(self, row):
        return len(row)


def schema_for(request):
    return ['id', 'name', 'amount'] + ['extra%d' % k for k in range(request % 4)]


def serve(request):
    fields = schema_for(request)
    Row = collections.namedtuple('Row%d' % request, fields)
    Record = dataclasses.make_dataclass('Record%d' % request,
                                        [(f, object, dataclasses.field(default=0)) for f in fields],
                                        frozen=True)
    Special = type('Special%d' % request, (Handler,), {
        'weight': request % 5,
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


def main(n):
    return sum(serve(request) for request in range(n))


N = 300

if __name__ == '__main__':
    print(main(N))
