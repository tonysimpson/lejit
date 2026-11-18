"""Generator bodies doing real work between yields: a log-processing pipeline of chained generators."""
LEVELS = ('DEBUG', 'INFO', 'WARNING', 'ERROR')


def make_lines(n):
    seed = 12345
    for i in range(n):
        seed = (seed * 1103515245 + 12345) & 0x7fffffff
        yield '%06d %s user=u%d path=/api/v%d/items/%d ms=%d' % (
            i, LEVELS[seed % 4], seed % 97, seed % 3, seed % 50, seed % 1000)


def parse(lines):
    for line in lines:
        num, level, rest = line.split(' ', 2)
        fields = {'n': int(num), 'level': level}
        for part in rest.split():
            key, _, value = part.partition('=')
            fields[key] = value
        yield fields


def only(records, levels):
    for r in records:
        if r['level'] in levels:
            yield r


def with_duration(records):
    for r in records:
        r['ms'] = int(r['ms'])
        r['version'] = int(r['path'].split('/')[2][1:])
        yield r


def windows(records, size):
    window = []
    for r in records:
        window.append(r)
        if len(window) == size:
            yield window
            window = []
    if window:
        yield window


def summarise(wins):
    for w in wins:
        total = slow = 0
        users = {}
        for r in w:
            total += r['ms']
            if r['ms'] > 800:
                slow += 1
            users[r['user']] = users.get(r['user'], 0) + 1
        top = max(sorted(users), key=users.get)
        yield len(w), total, slow, top


def main(n):
    acc = 0
    records = with_duration(only(parse(make_lines(n)), ('INFO', 'WARNING', 'ERROR')))
    for count, total, slow, top in summarise(windows(records, 50)):
        acc = (acc * 31 + count + total * 7 + slow + int(top[1:])) % 1000000007
    return acc


N = 40000

if __name__ == '__main__':
    print(main(N))
