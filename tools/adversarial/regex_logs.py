"""Work already in C: regular expressions matching, searching and substituting over access-log lines. Variant 1 has other addresses, methods, statuses and agents."""
import re

LINE = re.compile(r'(?P<ip>\d+\.\d+\.\d+\.\d+) - - \[(?P<day>\d+)/(?P<mon>\w+)/\d+:[\d:]+\] '
                  r'"(?P<method>[A-Z]+) (?P<path>\S+) HTTP/1\.[01]" (?P<status>\d{3}) (?P<size>\d+)')
IDS = re.compile(r'/\d+')
QUERY = re.compile(r'[?&](\w+)=([^&]*)')
BOT = re.compile(r'bot|crawl|spider', re.I)
MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun']


def make_log(n, variant):
    if variant:
        return make_other_log(n)
    lines = []
    for i in range(n):
        lines.append('10.0.%d.%d - - [%02d/%s/2024:12:%02d:%02d] "%s /shop/%d/item/%d?q=%s&page=%d HTTP/1.1" %d %d %s' % (
            i % 256, i * 7 % 256, 1 + i % 28, MONTHS[i % 6], i % 60, i * 3 % 60,
            ('GET', 'POST', 'PUT')[i % 3], i % 40, i % 1000, 'w%d' % (i % 13), i % 9,
            (200, 404, 500, 301)[i % 4], i * 37 % 5000, ('Mozilla', 'Googlebot', 'curl')[i % 3]))
    return lines


def make_other_log(n):
    lines = []
    for i in range(n):
        lines.append('192.168.%d.%d - - [%02d/%s/2023:%02d:%02d:07] "%s /api/v%d/user/%d/orders?sort=%s HTTP/1.0" %d %d %s' % (
            i * 3 % 256, i % 199, 1 + i * 5 % 28, MONTHS[(i * 5) % 6], i % 24, i * 7 % 60,
            ('DELETE', 'GET', 'PATCH', 'GET', 'HEAD')[i % 5], i % 3, i * 11 % 5000,
            ('asc', 'desc')[i % 2], (200, 204, 403, 404, 502)[i % 5], i * 91 % 20000,
            ('Spider/2', 'Wget', 'Mozilla/5.0', 'bingbot')[i % 4]))
        if i % 17 == 0:
            lines.append('malformed line %d' % i)
    return lines


def main(n, variant=0):
    lines = make_log(n, variant)
    routes = {}
    statuses = {}
    params = 0
    bots = 0
    size = 0
    for line in lines:
        m = LINE.match(line)
        if not m:
            continue
        route = IDS.sub('/:id', m.group('path').split('?')[0])
        routes[route] = routes.get(route, 0) + 1
        statuses[m.group('status')] = statuses.get(m.group('status'), 0) + 1
        params += len(QUERY.findall(m.group('path')))
        bots += BOT.search(line) is not None
        size += int(m.group('size'))
    return sorted(routes.items()), sorted(statuses.items()), params, bots, size


N = 30000

if __name__ == '__main__':
    print(main(N))
