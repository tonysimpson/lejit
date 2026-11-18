"""Functions using locals(), vars(), eval or exec, which are left to the interpreter: reports, formulas and settings."""


def summarise(rows):
    total = 0
    count = 0
    biggest = 0
    for qty, price in rows:
        amount = qty * price
        total += amount
        count += 1
        if amount > biggest:
            biggest = amount
    average = total / count
    return '{count} rows, total {total:.2f}, average {average:.3f}, max {biggest:.2f}'.format(**locals())


def formula(expr, x, y):
    return eval(expr)


class Settings:
    def __init__(self):
        self.width = 80
        self.depth = 3


def apply(settings, text):
    exec(text, {}, vars(settings))
    return settings.width + settings.depth


def checkpoint(a, b):
    total = 0
    for k in range(50):
        total += (a * k + b) % 7
    snapshot = dict(locals())
    return total + len(snapshot)


EXPRS = ['x * y + 1', 'x ** 2 - y', '(x + y) % 7', 'max(x, y) - min(x, y)', 'x // (y + 1)']


def main(n):
    out = 0
    text = ''
    for i in range(n):
        rows = [(1 + (i + k) % 5, 1.25 * ((i * k) % 9)) for k in range(40)]
        text = summarise(rows)
        out += len(text)
        out += formula(EXPRS[i % len(EXPRS)], i % 17, i % 5)
        s = Settings()
        out += apply(s, 'width = width + %d\ndepth = depth * 2' % (i % 3))
        out += checkpoint(i, i % 4)
    return out, text


N = 3000

if __name__ == '__main__':
    print(main(N))
