"""Code objects replaced: pure-Python modules reloaded and their fresh functions called a few times each."""
import calendar
import colorsys
import difflib
import fnmatch
import html
import importlib
import quopri
import shlex
import textwrap
import zlib

MODULES = [calendar, colorsys, difflib, fnmatch, html, quopri, shlex, textwrap]
TEXT = 'The quick brown fox jumps over the lazy dog. ' * 6
NAMES = ['main.py', 'util.py', 'README.md', 'setup.cfg', 'test_x.py', 'data.json']


def use(i):
    parts = [
        repr(colorsys.rgb_to_hsv(0.2, 0.4 + i % 3 * 0.1, 0.6)),
        textwrap.fill(TEXT, 30 + i % 5),
        repr(shlex.split('cp -r "my dir" dest%d --force' % i)),
        repr(fnmatch.filter(NAMES, '*.py')),
        repr(calendar.monthcalendar(2024, 1 + i % 12)),
        html.escape('<a href="x">%d & more</a>' % i),
        quopri.encodestring(('caf\xe9 %d' % i).encode('latin-1')).decode(),
        ''.join(difflib.unified_diff(['a\n', 'b\n', 'c\n'], ['a\n', 'B\n', 'c\n', str(i) + '\n'])),
    ]
    return zlib.crc32('\n'.join(parts).encode())


def main(n):
    out = []
    for i in range(n):
        for m in MODULES:
            importlib.reload(m)
        out.append(sum(use(i * 3 + k) for k in range(3)))
    return out


N = 20

if __name__ == '__main__':
    print(main(N))
