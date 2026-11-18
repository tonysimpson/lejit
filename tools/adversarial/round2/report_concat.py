"""Report builder that grows local strings with += and s = s + piece, relying on CPython resizing a string in place when the variable holds its only reference, with non-ASCII pieces widening the string's storage in one variant: losing a reference-count-dependent fast path turns linear building into quadratic (PEP 8, Programming Recommendations, on string concatenation; PEP 393 flexible string representation)."""
N = 20


def row(k, cols, variant):
    cells = []
    for c in range(cols):
        v = (k * 31 + c * 17) % 1000
        if variant == 1 and (k + c) % 97 == 0:
            cells.append('%d\u20ac' % v)
        elif variant == 1 and (k + c) % 251 == 0:
            cells.append('\U0001d11e%d' % v)
        else:
            cells.append('%d' % v)
    return cells


def render_table(rows, cols, variant):
    out = ''
    out += '<table>\n'
    for k in range(rows):
        line = '<tr>'
        for cell in row(k, cols, variant):
            line += '<td>' + cell + '</td>'
        line += '</tr>\n'
        out += line
    out = out + '</table>\n'
    return out


def render_text(rows, variant):
    text = ''
    for k in range(rows):
        text = text + 'line %06d: ' % k
        text += 'ok' if k % 3 else 'check'
        if variant == 1 and k % 113 == 5:
            text += ' \u00e9t\u00e9'
        text += '\n'
    return text


def csv_report(rows, variant):
    body = ''
    total = 0
    for k in range(rows):
        cells = row(k, 5, variant)
        body += ','.join(cells) + '\r\n'
        total += sum(int(c.strip('\u20ac\U0001d11e')) for c in cells)
    return body, total


def main(n, variant=0):
    pieces = []
    for r in range(n):
        size = 4000 + 500 * (r % 8)
        t = render_table(size // 4, 8, variant)
        x = render_text(size * 2, variant)
        c, total = csv_report(size, variant)
        pieces.append((len(t), t[:120], t[-60:], len(x), x[-80:], len(c), total,
                       max(map(ord, t)), max(map(ord, x)), t.count('\u20ac'), x.count('\n')))
    return pieces
