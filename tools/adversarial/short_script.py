"""Compile cost in a short script: a sales report that parses, groups and prints once, then exits."""
DATA = """region,product,month,units,price
North,Widget,2024-01,12,2.50
North,Gadget,2024-01,3,10.00
South,Widget,2024-01,7,2.50
South,Gizmo,2024-02,9,4.75
East,Gadget,2024-02,5,10.00
East,Widget,2024-02,14,2.40
West,Gizmo,2024-03,2,4.75
West,Widget,2024-03,20,2.30
North,Gizmo,2024-03,4,4.80
South,Gadget,2024-03,6,9.50
"""


def parse(text):
    lines = text.strip().splitlines()
    header = lines[0].split(',')
    rows = []
    for line in lines[1:]:
        row = dict(zip(header, line.split(',')))
        row['units'] = int(row['units'])
        row['price'] = float(row['price'])
        rows.append(row)
    return rows


def revenue(row):
    return row['units'] * row['price']


def group_by(rows, key):
    groups = {}
    for row in rows:
        groups.setdefault(row[key], []).append(row)
    return groups


def summary(rows):
    total = sum(revenue(r) for r in rows)
    units = sum(r['units'] for r in rows)
    return {'revenue': round(total, 2), 'units': units,
            'average': round(total / units, 3) if units else 0.0}


def best(groups):
    return max(sorted(groups), key=lambda k: summary(groups[k])['revenue'])


def table(groups):
    out = ['%-8s %6s %9s %8s' % ('', 'units', 'revenue', 'average')]
    for name in sorted(groups):
        s = summary(groups[name])
        out.append('%-8s %6d %9.2f %8.3f' % (name, s['units'], s['revenue'], s['average']))
    return '\n'.join(out)


def monthly_growth(rows):
    months = group_by(rows, 'month')
    keys = sorted(months)
    totals = [summary(months[k])['revenue'] for k in keys]
    return [round((b - a) / a * 100, 1) for a, b in zip(totals, totals[1:])]


def main(n):
    out = []
    for _ in range(n):
        rows = parse(DATA)
        regions = group_by(rows, 'region')
        products = group_by(rows, 'product')
        out = [table(regions), table(products),
               'best region: %s' % best(regions),
               'best product: %s' % best(products),
               'growth: %s' % monthly_growth(rows)]
    return '\n'.join(out)


N = 1

if __name__ == '__main__':
    print(main(N))
