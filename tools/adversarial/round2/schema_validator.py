"""Recursive schema validator that reports failures by raising deep inside nested documents, wraps them with raise from, reads __context__ and sys.exc_info() in finally blocks, and lets __exit__ suppress some errors: exceptions as ordinary control flow through compiled frames (Antonov, Optimization Killers, V8 Crankshaft; Paleczny, Vick and Click, The Java HotSpot Server Compiler, JVM 2001, on uncommon traps)."""
import sys
import zlib

N = 1500


class Invalid(Exception):
    def __init__(self, path, msg):
        super().__init__('%s: %s' % ('/'.join(map(str, path)) or '<root>', msg))
        self.path = path


class Collect:
    def __init__(self, sink, path, suppress):
        self.sink, self.path, self.suppress = sink, path, suppress

    def __enter__(self):
        return self

    def __exit__(self, et, ev, tb):
        if et is not None and issubclass(et, Invalid) and self.suppress:
            depth = 0
            while tb is not None:
                depth += 1
                tb = tb.tb_next
            self.sink.append('suppressed %s (tb %d)' % (ev, depth))
            return True
        return False


def check_type(value, expected, path):
    if expected == 'int':
        if type(value) is bool or not isinstance(value, int):
            raise Invalid(path, 'expected int, got %s' % type(value).__name__)
    elif expected == 'str':
        if not isinstance(value, str):
            raise Invalid(path, 'expected str, got %s' % type(value).__name__)
    elif expected == 'number':
        try:
            float(value)
        except (TypeError, ValueError) as e:
            raise Invalid(path, 'not a number') from e


def validate(value, schema, path, sink, strict):
    kind = schema['type']
    trail = []
    try:
        if kind in ('int', 'str', 'number'):
            check_type(value, kind, path)
            if 'min' in schema and value < schema['min']:
                raise Invalid(path, 'below %s' % schema['min'])
            return value
        if kind == 'list':
            if not isinstance(value, (list, tuple)):
                raise Invalid(path, 'expected list')
            out = []
            for i, item in enumerate(value):
                with Collect(sink, path, not strict):
                    out.append(validate(item, schema['items'], path + [i], sink, strict))
            return out
        if kind == 'object':
            if not isinstance(value, dict):
                raise Invalid(path, 'expected object')
            out = {}
            for key, sub in schema['fields'].items():
                if key not in value:
                    if sub.get('required'):
                        raise Invalid(path + [key], 'missing')
                    continue
                try:
                    out[key] = validate(value[key], sub, path + [key], sink, strict)
                except Invalid as e:
                    if sub.get('fallback') is not None:
                        out[key] = sub['fallback']
                        trail.append('fallback %s' % key)
                    else:
                        raise Invalid(path, 'in field %s' % key) from e
            extra = sorted(set(value) - set(schema['fields']))
            if extra and strict:
                raise Invalid(path, 'unexpected %s' % ','.join(extra))
            return out
        raise Invalid(path, 'unknown schema type %s' % kind)
    finally:
        et = sys.exc_info()[0]
        if trail or (et is not None and len(path) == 1):
            sink.append('leaving %s with %s %s' % (path, et.__name__ if et else None, trail))


SCHEMA = {'type': 'object', 'fields': {
    'id': {'type': 'int', 'required': True},
    'name': {'type': 'str', 'required': True},
    'score': {'type': 'number', 'fallback': 0},
    'tags': {'type': 'list', 'items': {'type': 'str'}},
    'owner': {'type': 'object', 'fields': {
        'uid': {'type': 'int', 'required': True, 'min': 0},
        'groups': {'type': 'list', 'items': {'type': 'object', 'fields': {
            'gid': {'type': 'int', 'required': True},
            'role': {'type': 'str', 'fallback': 'member'}}}}}},
}}


def make_doc(k, variant):
    doc = {'id': k, 'name': 'doc%d' % k, 'score': k * 1.5,
           'tags': ['t%d' % (k % 5), 't%d' % (k % 3)],
           'owner': {'uid': k % 17, 'groups': [{'gid': g, 'role': 'r%d' % g} for g in range(k % 4)]}}
    if variant == 1:
        m = k % 9
        if m == 0:
            doc['score'] = 'high'
        elif m == 1:
            doc['tags'].append(k)
        elif m == 2:
            doc['owner']['uid'] = -k
        elif m == 3:
            doc['owner']['groups'].append({'gid': 'g%d' % k, 'role': None})
        elif m == 4:
            del doc['name']
        elif m == 5:
            doc['id'] = True
        elif m == 6:
            doc['owner']['groups'].append({'role': 'orphan'})
        elif m == 7:
            doc['extra'] = k
        doc['tags'] = tuple(doc['tags'])
    return doc


def chain_of(e):
    out = []
    while e is not None:
        out.append('%s(%s)' % (type(e).__name__, e))
        e = e.__cause__ or e.__context__
    return out


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    results = []
    for k in range(n * 20):
        sink = []
        strict = k % 2 == 0
        try:
            r = validate(make_doc(k, variant), SCHEMA, [], sink, strict)
            results.append(('ok', r, sink))
        except Invalid as e:
            results.append(('invalid', chain_of(e), sink))
    return digest(results, 30)
