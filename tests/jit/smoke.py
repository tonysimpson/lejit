"""Exercises a spread of the stdlib; output must be the same with and
without LeJIT.

    python3.X tests/jit/smoke.py plain|lejit
"""
import sys, os
if len(sys.argv) > 1 and sys.argv[1] == 'lejit':
    sys.path.insert(0, os.environ.get('LEJIT_MODULE_DIR') or
                os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
    import lejit
    lejit.start()
import json, re, collections, textwrap, string, fractions, heapq, bisect, functools, itertools, base64, difflib, pprint, statistics, decimal, datetime, csv, io
print(json.loads(json.dumps({'a': [1, 2.5, None]})))
print(re.findall(r'\d+', 'a1b22c333'))
print(collections.Counter('abracadabra').most_common(2))
print(fractions.Fraction(3, 4) + fractions.Fraction(1, 6))
h = [5, 1, 4]; heapq.heapify(h); print([heapq.heappop(h) for _ in range(3)])
print(base64.b64encode(b'hello'), list(difflib.ndiff(['a', 'b'], ['a', 'c'])))
print(statistics.mean([1, 2, 3, 4]), statistics.median([3, 1, 2]))
print(decimal.Decimal('1.1') + decimal.Decimal('2.2'))
print(datetime.date(2026, 11, 16).isoformat(), datetime.timedelta(days=3) * 2)
r = list(csv.reader(io.StringIO('a,b\n1,"x,y"\n'))); print(r)
pprint.pprint({'k': list(range(12))}, width=40)
print(sorted(itertools.permutations('abc', 2))[:3], functools.reduce(lambda a, b: a * b, range(1, 8)))
print(textwrap.fill('the quick brown fox jumps over the lazy dog ' * 3, 30))
import string, random, operator, enum, argparse, shlex, fnmatch, glob, posixpath, urllib.parse, html, email.utils, calendar, inspect, ast, tokenize, configparser
print(string.Template('$a-$b').substitute(a=1, b=2), shlex.split('a "b c" d'), fnmatch.fnmatch('x.py', '*.py'))
print(posixpath.join('a', 'b', '../c'), posixpath.normpath('a/b/../c'), urllib.parse.urlparse('http://x.org/p?q=1').query)
print(html.escape('<a&b>'), calendar.monthrange(2026, 2), ast.dump(ast.parse('x+1').body[0])[:40])
r = random.Random(42); print([r.randint(0, 9) for _ in range(5)], sorted({3: 'c', 1: 'a'}.items(), key=operator.itemgetter(0)))
class Colour(enum.Enum):
    RED = 1
    GREEN = 2
print(Colour(2), list(Colour))
p = argparse.ArgumentParser(); p.add_argument('--n', type=int, default=3); print(p.parse_args(['--n', '5']))
c = configparser.ConfigParser(); c.read_string('[s]\nk = v\n'); print(c['s']['k'])
