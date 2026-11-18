"""Web framework of class-based views composed from mixins at start-up with type(), dispatching through cooperative super() chains, where later requests reassign __bases__, replace methods on base classes and swap instances' __class__: method-lookup caches and class-hierarchy assumptions invalidated under running code (Paleczny, Vick and Click, The Java HotSpot Server Compiler, JVM 2001, class hierarchy analysis; Hoelzle, Chambers and Ungar, ECOOP 1991)."""
import zlib

N = 5000


class Response:
    __slots__ = ('status', 'body', 'headers')

    def __init__(self, status, body, headers=()):
        self.status, self.body, self.headers = status, body, list(headers)

    def as_tuple(self):
        return self.status, self.body, tuple(self.headers)


class BaseView:
    name = 'base'

    def __init__(self, app):
        self.app = app
        self.trail = []

    def dispatch(self, req):
        self.trail.append('BaseView')
        handler = getattr(self, req['method'].lower(), None)
        if handler is None:
            return Response(405, 'method not allowed')
        return handler(req)

    def render(self, data):
        return '%s:%s' % (self.name, data)


class AuthMixin:
    def dispatch(self, req):
        self.trail.append('Auth')
        if not req.get('user'):
            return Response(401, 'login required')
        return super().dispatch(req)


class CacheMixin:
    def dispatch(self, req):
        self.trail.append('Cache')
        key = (type(self).__name__, req['method'], req['path'])
        hit = self.app.cache.get(key)
        if hit is not None:
            return Response(hit.status, hit.body, hit.headers + ['X-Cache: hit'])
        resp = super().dispatch(req)
        if req['method'] == 'GET' and resp.status == 200:
            self.app.cache[key] = resp
        return resp


class JSONMixin:
    def render(self, data):
        if isinstance(data, dict):
            inner = ','.join('"%s":%s' % (k, self.render_value(v)) for k, v in data.items())
            return '{' + inner + '}'
        return super().render(data)

    def render_value(self, v):
        if isinstance(v, str):
            return '"%s"' % v
        if isinstance(v, (list, tuple)):
            return '[' + ','.join(self.render_value(x) for x in v) + ']'
        return str(v)


class AuditMixin:
    def dispatch(self, req):
        self.trail.append('Audit')
        resp = super().dispatch(req)
        self.app.audit.append((type(self).__name__, req['path'], resp.status))
        return resp


class ThrottleMixin:
    def dispatch(self, req):
        self.trail.append('Throttle')
        user = req.get('user') or 'anon'
        n = self.app.hits[user] = self.app.hits.get(user, 0) + 1
        if n % 7 == 0:
            return Response(429, 'slow down')
        return super().dispatch(req)


class ItemHandlers:
    def get(self, req):
        item = self.app.items.get(req['path'])
        if item is None:
            return Response(404, 'no ' + req['path'])
        return Response(200, self.render(item))

    def post(self, req):
        self.app.items[req['path']] = dict(req.get('data', {}), owner=req.get('user'))
        for key in [k for k in self.app.cache if k[2] == req['path']]:
            del self.app.cache[key]
        return Response(201, self.render({'created': req['path']}))


ROUTES = [
    ('Items', (AuditMixin, CacheMixin, JSONMixin, ItemHandlers, BaseView)),
    ('Admin', (AuthMixin, AuditMixin, JSONMixin, ItemHandlers, BaseView)),
    ('Public', (ThrottleMixin, CacheMixin, ItemHandlers, BaseView)),
    ('Plain', (ItemHandlers, BaseView)),
]


class App:
    def __init__(self):
        self.cache = {}
        self.audit = []
        self.hits = {}
        self.items = {}
        self.views = {}
        for name, bases in ROUTES:
            self.views[name] = type(name + 'View', bases, {'name': name.lower()})

    def handle(self, route, req):
        view = self.views[route](self)
        resp = view.dispatch(req)
        return resp.as_tuple() + (tuple(view.trail), type(view).__mro__[1].__name__)


def request(k):
    method = ['GET', 'GET', 'POST', 'GET', 'DELETE'][k % 5]
    req = {'method': method, 'path': '/item/%d' % (k % 6)}
    if k % 3:
        req['user'] = 'u%d' % (k % 4)
    if method == 'POST':
        req['data'] = {'v': k, 'tags': ['t%d' % (k % 3), k % 5]}
    return req


def patched_render(self, data):
    return '<%s>' % (data,)


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    app = App()
    out = []
    routes = [r for r, _ in ROUTES]
    for k in range(n * 20):
        route = routes[(k * 3 + k // 5) % len(routes)]
        if variant == 1:
            if k % 40 == 10:
                app.views['Plain'].__bases__ = (AuditMixin, ItemHandlers, BaseView)
            elif k % 40 == 30:
                app.views['Plain'].__bases__ = (ItemHandlers, BaseView)
            if k % 25 == 12:
                app.views['Items'].render = patched_render
            elif k % 25 == 20:
                del app.views['Items'].render
            if k % 9 == 4:
                view = app.views[route](app)
                view.__class__ = app.views['Admin' if route != 'Admin' else 'Public']
                resp = view.dispatch(request(k))
                out.append(('swapped', type(view).__name__) + resp.as_tuple() + (tuple(view.trail),))
                continue
        out.append(app.handle(route, request(k)))
    return digest(out, 40), app.audit[-20:], sorted(app.hits.items()), sorted(app.items.items())
