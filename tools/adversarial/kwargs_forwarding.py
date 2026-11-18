"""**kwargs and *args forwarded through layers of wrappers into keyword-only parameters: an HTTP client's option plumbing."""
import functools


def request(method, url, *, timeout=10, retries=0, headers=None, params=None, verify=True, **extra):
    h = dict(headers or {})
    h.setdefault('User-Agent', 'client/1.0')
    query = '&'.join('%s=%s' % kv for kv in sorted((params or {}).items()))
    line = '%s %s?%s t=%d r=%d v=%d h=%d x=%d' % (
        method, url, query, timeout, retries, verify, len(h), len(extra))
    return len(line) + sum(len(k) for k in extra)


def with_retries(func):
    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        kwargs.setdefault('retries', 3)
        return func(*args, **kwargs)
    return wrapper


def logged(func):
    calls = []

    @functools.wraps(func)
    def wrapper(*args, **kwargs):
        calls.append(len(kwargs))
        return func(*args, **kwargs)
    wrapper.calls = calls
    return wrapper


@logged
@with_retries
def send(method, url, **kwargs):
    return request(method, url, **kwargs)


class Session:
    def __init__(self, **defaults):
        self.defaults = defaults

    def request(self, method, url, **kwargs):
        merged = dict(self.defaults)
        merged.update(kwargs)
        return send(method, url, **merged)

    def get(self, url, **kwargs):
        return self.request('GET', url, **kwargs)

    def post(self, url, data=None, **kwargs):
        kwargs.setdefault('headers', {'Content-Type': 'application/json'})
        return self.request('POST', url, params=data, **kwargs)


def main(n):
    del send.calls[:]
    s = Session(timeout=5, verify=False, trace_id='abc')
    total = 0
    for i in range(n):
        url = '/api/items/%d' % (i % 50)
        total += s.get(url, params={'page': i % 7, 'size': 20})
        total += s.post(url, data={'id': i}, timeout=i % 9, flag=i % 2 == 0)
        total += send('DELETE', url, **{'retries': i % 4, 'extra%d' % (i % 3): 1})
    return total, len(send.calls), sum(send.calls)


N = 20000

if __name__ == '__main__':
    print(main(N))
