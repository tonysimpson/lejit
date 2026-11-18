"""Template engine compiling each template to Python source with compile() and exec(), most run a handful of times on polymorphic contexts: compiled code discarded before it repays its cost (Barrett et al., Virtual Machine Warmup Blows Hot and Cold, OOPSLA 2017)."""
import re
from collections import namedtuple
from decimal import Decimal

N = 150


class Markup(str):
    __slots__ = ()

    def __html__(self):
        return self

    def __add__(self, other):
        return Markup(str.__add__(self, escape(other)))


def escape(v):
    h = getattr(v, '__html__', None)
    if h is not None:
        return h()
    return Markup(str(v).replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;')
                  .replace('"', '&quot;'))


def lookup(obj, name):
    try:
        return getattr(obj, name)
    except AttributeError:
        pass
    try:
        return obj[name]
    except (KeyError, TypeError, IndexError):
        try:
            return obj[int(name)]
        except (ValueError, KeyError, TypeError, IndexError):
            return None


def f_money(v):
    if v is None:
        return '-'
    if isinstance(v, Decimal):
        return '%s' % v.quantize(Decimal('0.01'))
    return '%.2f' % v


FILTERS = {
    'upper': lambda v: str(v).upper(),
    'title': lambda v: str(v).title(),
    'length': lambda v: len(v) if v is not None else 0,
    'default': lambda v: '(none)' if v is None or v == '' else v,
    'money': f_money,
    'join': lambda v: ', '.join(str(x) for x in v),
    'safe': lambda v: Markup(v),
    'first': lambda v: next(iter(v), None) if v is not None else None,
}

TOKEN = re.compile(r'(\{\{.*?\}\}|\{%.*?%\})', re.S)


def compile_expr(expr):
    parts = [p.strip() for p in expr.split('|')]
    head = parts[0].split('.')
    code = 'ctx.get(%r)' % head[0]
    for attr in head[1:]:
        code = '_lookup(%s, %r)' % (code, attr)
    for f in parts[1:]:
        code = '_filters[%r](%s)' % (f, code)
    return code


def compile_template(src, name):
    lines = ['def render(ctx, _lookup, _filters, _escape):',
             '    _out = []', '    _w = _out.append']
    depth = 1
    loop = 0
    for tok in TOKEN.split(src):
        ind = '    ' * depth
        if tok.startswith('{{'):
            lines.append(ind + '_w(_escape(%s))' % compile_expr(tok[2:-2]))
        elif tok.startswith('{%'):
            words = tok[2:-2].split()
            if words[0] == 'for':
                loop += 1
                lines.append(ind + 'for _v%d in (%s or ()):' % (loop, compile_expr(words[3])))
                lines.append(ind + '    ctx = dict(ctx); ctx[%r] = _v%d' % (words[1], loop))
                depth += 1
            elif words[0] == 'if':
                lines.append(ind + 'if %s:' % compile_expr(' '.join(words[1:])))
                depth += 1
            elif words[0] == 'else':
                lines.append('    ' * (depth - 1) + 'else:')
            elif words[0] in ('endfor', 'endif'):
                lines.append(ind + 'pass')
                depth -= 1
        elif tok:
            lines.append(ind + '_w(%r)' % tok)
    lines.append('    return "".join(_out)')
    ns = {}
    exec(compile('\n'.join(lines), '<template %s>' % name, 'exec'), ns)
    return ns['render']


class Engine:
    def __init__(self):
        self.cache = {}
        self.compiles = 0

    def render(self, src, ctx):
        fn = self.cache.get(src)
        if fn is None:
            self.compiles += 1
            fn = self.cache[src] = compile_template(src, len(self.cache))
        return fn(ctx, lookup, FILTERS, escape)


FRAGMENTS = [
    '<h1>{{ title|title }}</h1>',
    '<p class="by">{{ user.name|default }} ({{ user.email }})</p>',
    '{% for item in items %}<li>{{ item.name }}: {{ item.price|money }}</li>{% endfor %}',
    '{% if user.admin %}<a href="/admin">admin</a>{% else %}<span>guest</span>{% endif %}',
    '<footer>{{ tags|join }} / {{ tags|length }} tags</footer>',
    '{% for row in rows %}<tr>{% for c in row %}<td>{{ c }}</td>{% endfor %}</tr>{% endfor %}',
    '<div>{{ note|safe }}</div><i>{{ comment }}</i>',
    '<b>{{ items|first }}</b>',
]

User = namedtuple('User', 'name email admin')


class Product:
    def __init__(self, name, price):
        self.name = name
        self._price = price

    @property
    def price(self):
        return self._price

    def __str__(self):
        return 'Product(%s)' % self.name


def make_context(i, variant):
    if variant == 0:
        user = {'name': 'user%d' % i, 'email': 'u%d@example.org' % i, 'admin': i % 3 == 0}
        items = [{'name': 'thing %d' % j, 'price': float(j * 3 + i)} for j in range(6)]
        rows = [[j * k for k in range(4)] for j in range(5)]
        note, comment, tags = '<em>note</em>', 'plain', ['a', 'b', 'c']
    else:
        kind = i % 3
        if kind == 0:
            user = User(Markup('<b>u%d</b>' % i), None, True)
        elif kind == 1:
            user = {'name': '', 'email': 'x<y>@z', 'admin': 0}
        else:
            user = None
        items = [Product('p&%d' % j, Decimal(j * 3 + i) / 7 if j % 2 else None) for j in range(6)]
        rows = [('a<%d' % j, j / 3, None, Markup('<i>%d</i>' % j)) for j in range(5)]
        note, comment, tags = Markup('<em>%d</em>' % i), '<script>%d</script>' % i, ('x', 1, 2.5)
    return {'title': 'page number %d' % i, 'user': user, 'items': items, 'rows': rows,
            'note': note, 'comment': comment, 'tags': tags}


def main(n, variant=0):
    engine = Engine()
    pages = []
    for i in range(n * 40):
        t = i // 3
        k = (t * 7 + variant) % len(FRAGMENTS)
        body = ''.join(FRAGMENTS[(k + j * (variant + 1)) % len(FRAGMENTS)] for j in range(3 + t % 4))
        src = '<!-- t%d -->' % t + body
        ctx = make_context(i, variant)
        pages.append(engine.render(src, ctx))
        if i % 5 == 0:
            pages.append(engine.render(src, make_context(i + 1, 1 - variant)))
    return engine.compiles, pages[-3:], [len(p) for p in pages], sum(map(len, pages))
