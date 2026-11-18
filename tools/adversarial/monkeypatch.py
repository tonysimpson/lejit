"""Guards that keep failing: methods, instance attributes and module globals swapped while running (mock patching, feature flags). Variant 1 patches on another schedule, with other keys."""
import types
from unittest import mock


class Service:
    retries = 2

    def fetch(self, key):
        return len(key) * 3

    def transform(self, value):
        return value + 1


def fast_fetch(self, key):
    return len(key)


def slow_fetch(self, key):
    return sum(map(ord, key)) % 50


def scale(v):
    return v * 2


def scale_alt(v):
    return v * 3


ORIGINAL_FETCH = Service.fetch
ORIGINAL_SCALE = scale
KEYS = [['user:%d' % i for i in range(40)], ['session/%04x' % (i * 977) for i in range(36)]]
SCHEDULES = [(3, 2, 5, 4, 25), (4, 3, 7, 5, 18)]


def handle(svc, keys):
    t = 0
    for k in keys:
        t += scale(svc.transform(svc.fetch(k))) + svc.retries
    return t


def main(n, variant=0):
    g = globals()
    svc = Service()
    keys = KEYS[variant]
    fetches = (fast_fetch, slow_fetch, ORIGINAL_FETCH, fast_fetch)
    nfetch, nscale, ntransform, nretries, nmock = SCHEDULES[variant]
    total = 0
    try:
        for i in range(n):
            Service.fetch = fetches[i % nfetch]
            g['scale'] = scale_alt if i % nscale else ORIGINAL_SCALE
            if i % ntransform == 0:
                svc.transform = types.MethodType(lambda self, v: v - 1, svc)
            else:
                svc.__dict__.pop('transform', None)
            Service.retries = i % nretries
            total += handle(svc, keys)
            if i % nmock == 0:
                with mock.patch.object(Service, 'fetch', lambda self, key: 7):
                    total += handle(svc, keys[:5])
    finally:
        Service.fetch = ORIGINAL_FETCH
        g['scale'] = ORIGINAL_SCALE
        Service.retries = 2
    return total


N = 3000

if __name__ == '__main__':
    print(main(N))
