"""Work already in C: str methods (split, strip, lower, replace, startswith, format, join, encode) normalising text records. Variant 1 has other records."""

RAW = [[
    '  Mr. John SMITH ; john.smith@Example.COM ; +44 (0)20 7946 0018 ; London, UK  ',
    'mrs jane doe;JANE.DOE@example.org;020-7946-0999;  Manchester , uk',
    'Dr  Alan   Turing ; alan@bletchley.example ; (01908) 640404 ; Milton Keynes,UK',
    ' ms. ada lovelace;ADA@analytical.example;+44 1234 567890;london,uk ',
], [
    'Prof. Karen SPARCK JONES;ksj@Cam.example.AC.UK;+44 (0)1223 763500;Cambridge,  uk',
    '  dr. tim berners-lee ; TIMBL@w3.example ; 0207 946 0123 ; london , UK',
    'sophie wilson;Sophie.Wilson@acorn.example;01223-245200;Cambridge,UK   ',
    'MR ROBIN MILNER ; r.milner@ed.example ; +44 131 650 1000 ; Edinburgh, uk',
    ' Ms Steve Shirley;steve@xansa.example;+44(0)1494 000000 ;Amersham,UK',
]]

TITLES = ('mr', 'mrs', 'ms', 'dr')


def clean(line):
    name, email, phone, place = [part.strip() for part in line.split(';')]
    words = name.replace('.', ' ').split()
    if words and words[0].lower() in TITLES:
        title = words[0].capitalize()
        words = words[1:]
    else:
        title = ''
    name = ' '.join(w.capitalize() for w in words)
    email = email.lower()
    user, _, domain = email.partition('@')
    digits = ''.join(ch for ch in phone if ch.isdigit())
    if digits.startswith('44'):
        digits = '0' + digits[2:].lstrip('0')
    city, _, country = place.partition(',')
    city = city.strip().title()
    country = country.strip().upper()
    return '{0:<4}{1:<16}{2:>14} {3}@{4} [{5}/{6}]'.format(
        title, name, digits, user, domain.replace('example', 'ex'), city, country)


def main(n, variant=0):
    out = []
    size = 0
    raw = RAW[variant]
    for i in range(n):
        line = raw[i % len(raw)].replace('0', str(i % 10), 1)
        rec = clean(line)
        size += len(rec.encode('utf-8')) + rec.count(' ') + rec.find('@')
        if rec.endswith('[London/UK]'):
            out.append(rec.upper().center(70, '.'))
    return size, len(out), out[-1] if out else ''


N = 40000

if __name__ == '__main__':
    print(main(N))
