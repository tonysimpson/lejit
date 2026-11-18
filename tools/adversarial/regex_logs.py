"""Work already in C: regular expressions matching, searching and substituting over access-log lines."""
import re

LINE = re.compile(r'(?P<ip>\d+\.\d+\.\d+\.\d+) - - \[(?P<day>\d+)/(?P<mon>\w+)/\d+:[\d:]+\] '
                  r'"(?P<method>[A-Z]+) (?P<path>\S+) HTTP/1\.[01]" (?P<status>\d{3}) (?P<size>\d+)')
IDS = re.compile(r'/\d+')
QUERY = re.compile(r'[?&](\w+)=([^&]*)')
BOT = re.compile(r'bot|crawl|spider', re.I)
MONTHS = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun']


def make_log(n):
    lines = []
    for i in range(n):
        lines.append('10.0.%d.%d - - [%02d/%s/2024:12:%02d:%02d] "%s /shop/%d/item/%d?q=%s&page=%d HTTP/1.1" %d %d %s' % (
            i % 256, i * 7 % 256, 1 + i % 28, MONTHS[i % 6], i % 60, i * 3 % 60,
            ('GET', 'POST', 'PUT')[i % 3], i % 40, i % 1000, 'w%d' % (i % 13), i % 9,
            (200, 404, 500, 301)[i % 4], i * 37 % 5000, ('Mozilla', 'Googlebot', 'curl')[i % 3]))
    return lines


def main(n):
    lines = make_log(n)
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
