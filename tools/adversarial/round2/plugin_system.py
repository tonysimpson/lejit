"""Plugin host that execs plugins into fresh module namespaces, hot-reloads them into the same dict, and lets them shadow builtins and rebind their globals between calls: invalidation of global and builtin lookups specialised as constants (Paleczny, Vick and Click, The Java HotSpot Server Compiler, JVM 2001; Bolz et al., Runtime Feedback in a Meta-Tracing JIT, ICOOOLPS 2011)."""
import builtins
import types
import zlib

N = 800

PLUGINS = {
    'wordcount': '''
LIMIT = {limit}
SEEN = []

def on_text(text):
    words = text.split()
    SEEN.append(len(words))
    return 'words', min(len(words), LIMIT)

def configure(limit):
    global LIMIT
    LIMIT = limit

def report():
    return sorted(SEEN)[-3:], max(SEEN, default=0)
''',
    'shout': '''
PREFIX = '{prefix}'

def on_text(text):
    out = []
    for w in text.split():
        if len(w) > 3:
            out.append(str(w).upper())
    return 'shout', PREFIX + '-'.join(out[:4])

def shadow_len():
    real = __builtins__['len'] if isinstance(__builtins__, dict) else __builtins__.len
    def len(x):
        return real(x) * 10
    globals()['len'] = len

def unshadow_len():
    globals().pop('len', None)

def report():
    return PREFIX
''',
    'stats': '''
import math
TOTAL = 0

def on_text(text):
    global TOTAL
    vals = [ord(c) for c in text if c.isalpha()]
    TOTAL += sum(vals)
    return 'stats', round(math.sqrt(sum(v * v for v in vals) / max(len(vals), 1)), 3)

def report():
    return TOTAL
''',
}

SECOND_VERSION = {
    'wordcount': '''
LIMIT = LIMIT
SEEN = SEEN

def on_text(text):
    words = [w for w in text.split() if w.isalpha()]
    SEEN.append(len(words) * 2)
    return 'words2', min(len(words), LIMIT)

def configure(limit):
    global LIMIT
    LIMIT = limit * 2

def report():
    return sorted(SEEN)[-3:], len(SEEN)
''',
}


class Host:
    def __init__(self, variant):
        self.modules = {}
        self.hooks = []
        self.variant = variant

    def load(self, name, src):
        mod = self.modules.get(name)
        if mod is None:
            mod = self.modules[name] = types.ModuleType('plugin_' + name)
        exec(compile(src, '<plugin %s>' % name, 'exec'), mod.__dict__)
        return mod

    def register(self, name):
        self.hooks.append((name, self.modules[name].on_text))

    def dispatch(self, text):
        return [hook(text) for _, hook in self.hooks]


TEXTS = ['the quick brown fox jumps over the lazy dog',
         'pack my box with five dozen liquor jugs',
         'how vexingly quick daft zebras jump 42 times',
         'sphinx of black quartz judge my vow',
         'a b c d e f g',
         'jackdaws love my big sphinx of quartz 1999']


def run_session(i, variant):
    host = Host(variant)
    host.load('wordcount', PLUGINS['wordcount'].format(limit=5 + i % 3))
    host.load('shout', PLUGINS['shout'].format(prefix='>' * (1 + i % 2)))
    host.load('stats', PLUGINS['stats'])
    for name in (['wordcount', 'shout', 'stats'] if variant == 0 else ['stats', 'shout', 'wordcount']):
        host.register(name)
    log = []
    real_max = builtins.max
    try:
        for step in range(60):
            text = TEXTS[(step + i) % len(TEXTS)] + ' %d' % step
            log.append(host.dispatch(text))
            if step == 10:
                host.modules['wordcount'].configure(3)
            if variant == 1:
                if step == 15:
                    host.modules['shout'].shadow_len()
                if step == 25:
                    host.modules['shout'].unshadow_len()
                if step == 30:
                    host.load('wordcount', SECOND_VERSION['wordcount'])
                    host.modules['wordcount'].configure(4)
                if step == 40:
                    host.register('wordcount')
                if step == 45:
                    builtins.max = lambda *a, **k: real_max(*a, **k) + 1
                if step == 50:
                    builtins.max = real_max
                if step % 7 == 3:
                    host.modules['stats'].math = types.SimpleNamespace(sqrt=lambda v: v)
                if step % 7 == 5:
                    del host.modules['stats'].math
                    host.modules['stats'].__dict__['math'] = __import__('math')
    finally:
        builtins.max = real_max
    reports = [(name, host.modules[name].report()) for name in sorted(host.modules)]
    return log, reports


def digest(results, keep):
    crc = 0
    for r in results:
        crc = zlib.crc32(repr(r).encode(), crc)
    return len(results), crc, results[:keep], results[-keep:]


def main(n, variant=0):
    return digest([run_session(i, variant) for i in range(n)], 2)
