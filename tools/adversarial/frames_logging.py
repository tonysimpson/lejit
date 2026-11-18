"""Frame introspection: logging with caller names and line numbers, warnings with stacklevel, tracebacks and sys._getframe."""
import io
import logging
import sys
import traceback
import warnings

stream = io.StringIO()
log = logging.getLogger('orders')
log.propagate = False
handler = logging.StreamHandler(stream)
handler.setFormatter(logging.Formatter('%(levelname)s %(funcName)s:%(lineno)d %(message)s'))
log.addHandler(handler)
log.setLevel(logging.INFO)


def caller_name():
    return sys._getframe(1).f_code.co_name


def deprecated_total(order):
    warnings.warn('use total()', DeprecationWarning, stacklevel=2)
    return sum(order)


def check(order):
    if sum(order) > 100:
        raise ValueError('order too large: %d' % sum(order))
    return caller_name()


def handle(i):
    order = [i % 13, i % 17, i % 29, i % 41 * 2]
    log.info('order %d has %d lines', i, len(order))
    try:
        who = check(order)
    except ValueError:
        tb = traceback.extract_tb(sys.exc_info()[2])
        who = '%s@%d' % (tb[-1].name, tb[-1].lineno)
        log.warning('rejected %d', i)
    return who, deprecated_total(order)


def main(n):
    stream.seek(0)
    stream.truncate()
    names = {}
    total = 0
    with warnings.catch_warnings(record=True) as caught:
        warnings.simplefilter('always')
        for i in range(n):
            who, t = handle(i)
            names[who] = names.get(who, 0) + 1
            total += t
    lines = sorted(set((w.lineno, str(w.message)) for w in caught))
    text = stream.getvalue().splitlines()
    return total, sorted(names.items()), lines, len(text), sorted(set(l.split(' ')[1] for l in text))


N = 5000

if __name__ == '__main__':
    print(main(N))
