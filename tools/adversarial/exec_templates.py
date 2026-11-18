"""Fresh code objects on every request: a template engine that compiles each page to Python source and execs it (watch memory)."""
import re

TOKEN = re.compile(r'(\{\{.*?\}\}|\{%.*?%\})')

PAGE = """<h1>{{ title }}</h1>
<p>Request {{ request }} for {{ user.name }} ({{ user.city }})</p>
{% for item in items %}<li>{{ item.name }}: {{ item.price }} x {{ item.qty }}</li>
{% endfor %}{% if total %}<p>Total {{ total }}</p>{% endif %}
<footer>build REQUEST_ID</footer>
"""


class Item:
    def __init__(self, name, price, qty):
        self.name = name
        self.price = price
        self.qty = qty


class User:
    def __init__(self, name, city):
        self.name = name
        self.city = city


def compile_template(text, names):
    lines = ['def render(ctx):', '    out = []', '    append = out.append']
    lines.extend('    %s = ctx[%r]' % (name, name) for name in names)
    depth = 1
    for part in TOKEN.split(text):
        pad = '    ' * depth
        if part.startswith('{{'):
            lines.append('%sappend(str(%s))' % (pad, part[2:-2].strip()))
        elif part.startswith('{%'):
            words = part[2:-2].split()
            if words[0] in ('for', 'if'):
                lines.append('%s%s:' % (pad, ' '.join(words)))
                depth += 1
            else:
                depth -= 1
        elif part:
            lines.append('%sappend(%r)' % (pad, part))
    lines.append('    return "".join(out)')
    src = '\n'.join(lines)
    ns = {}
    code = compile(src, '<template>', 'exec')
    exec(code, ns)
    return ns['render']


def serve(request):
    template = PAGE.replace('REQUEST_ID', str(request))
    items = [Item('thing%d' % k, round(1.25 * (k + request % 5), 2), 1 + k % 3)
             for k in range(request % 6 + 1)]
    ctx = {'title': 'Basket', 'request': request, 'user': User('u%d' % request, 'Leeds'),
           'items': items, 'total': round(sum(it.price * it.qty for it in items), 2)}
    render = compile_template(template, sorted(ctx))
    out = ''
    for _ in range(3):
        out = render(ctx)
    return out


def quick_formula(request):
    return eval('%d * x + %d' % (request % 9, request % 4), {'x': request})


def main(n):
    size = 0
    last = ''
    for request in range(n):
        last = serve(request)
        size += len(last)
        size += quick_formula(request)
    return size, last


N = 400

if __name__ == '__main__':
    print(main(N))
